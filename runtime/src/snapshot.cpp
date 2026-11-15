#include "snapshot.h"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cinttypes>
#include <map>
#include <cstring>
#include <vector>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "ipc.h"
#include "kernel_state.h"
#include "nce/nce.h"
#include "platform.h"

namespace nx::snap {

std::atomic<bool> g_freeze{false};

namespace {

std::mutex g_m;
std::condition_variable g_parked_cv;  // a thread parked (the freezer listens)
std::condition_variable g_thaw_cv;    // the freeze ended (parked threads listen)
std::atomic<uint64_t> g_woke_frozen{0};

bool still(const KThread& t) { return t.snap_state.load() == 1 || t.snap_wait.load() > 0; }

}  // namespace

void park(CPU& c, uint32_t code) {
    KThread* t = kstate::t_self;
    if (!t) return;
    t->snap_code = code;
    for (int i = 0; i < 8; i++) t->snap_args[i] = c.x[i];
    std::unique_lock lk(g_m);
    t->snap_state.store(1);
    g_parked_cv.notify_all();
    g_thaw_cv.wait(lk, [] { return !g_freeze.load(); });
    t->snap_state.store(0);
}

void hold() {
    if (!g_freeze.load()) return;
    KThread* t = kstate::t_self;
    if (!t) return;
    std::unique_lock lk(g_m);
    if (!g_freeze.load()) return;
    t->snap_state.store(1);
    g_parked_cv.notify_all();
    g_thaw_cv.wait(lk, [] { return !g_freeze.load(); });
    t->snap_state.store(0);
}

void hold_waiting(std::unique_lock<std::recursive_mutex>& lk, KThread* self) {
    // g_freeze is set and cleared under the kernel lock for this: a thaw cannot slip between the test and the wait.
    while (g_freeze.load()) self->cv.wait(lk);
}

WaitScope::WaitScope() {
    if (KThread* t = kstate::t_self) t->snap_wait.fetch_add(1);
}

WaitScope::~WaitScope() {
    if (KThread* t = kstate::t_self) {
        t->snap_wait.fetch_sub(1);
        if (g_freeze.load()) g_woke_frozen.fetch_add(1);
    }
}

bool freeze(int timeout_ms) {
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(timeout_ms);
    {
        std::lock_guard k(kstate::g_k);
        g_freeze.store(true);
    }
    std::unique_lock lk(g_m);
    uint64_t epoch = g_woke_frozen.load();
    for (;;) {
        int guests = 0, parked = 0, waiting = 0;
        std::string loose;
        {
            std::lock_guard al(kstate::g_all_lock);
            for (auto& w : kstate::g_all) {
                auto t = w.lock();
                if (!t || !t->started || t->exited) continue;
                guests++;
                if (t->snap_state.load() == 1) parked++;
                else if (t->snap_wait.load() > 0) waiting++;
                else {
                    const char* h = t->hle.load();
                    char buf[96];
                    std::snprintf(buf, sizeof buf, " [thread %x in %s]", t->handle, h ? h : "guest code");
                    loose += buf;
                }
            }
        }
        // A waiting thread that wakes while frozen runs guest code until its next call, so "all still" only counts once
        // no thread has left a wait since the last look.
        if (parked + waiting == guests && g_woke_frozen.load() != epoch) {
            epoch = g_woke_frozen.load();
            g_parked_cv.wait_for(lk, std::chrono::milliseconds(2));
            continue;
        }
        if (parked + waiting == guests) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            log("snap: frozen in %lld ms (%d guest threads: %d parked, %d waiting)", (long long)ms, guests, parked, waiting);
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            log("snap: freeze timed out after %d ms (%d of %d guest threads still moving:%s)", timeout_ms, guests - parked - waiting,
                guests, loose.c_str());
            lk.unlock();
            thaw();
            return false;
        }
        g_parked_cv.wait_for(lk, std::chrono::milliseconds(2));
    }
}

void thaw() {
    {
        std::lock_guard k(kstate::g_k);
        std::lock_guard lk(g_m);
        g_freeze.store(false);
    }
    g_thaw_cv.notify_all();
    std::lock_guard al(kstate::g_all_lock);
    for (auto& w : kstate::g_all)
        if (auto t = w.lock()) t->cv.notify_all();
}


uint64_t wake_epoch() { return g_woke_frozen.load(); }

namespace {

struct Window {
    uint64_t lo, hi;
    const char* name;
};

struct DumpStats {
    uint64_t regions = 0, present_pages = 0, swapped_pages = 0, bytes = 0;
    std::vector<std::pair<uint64_t, uint64_t>> rw;  // every writable mapping inside the windows (committed memory)
};

bool write_all(int fd, const void* p, size_t n) {
    const char* c = static_cast<const char*>(p);
    while (n) {
        ssize_t w = ::write(fd, c, n);
        if (w <= 0) return false;
        c += w, n -= size_t(w);
    }
    return true;
}

}  // namespace

// Output that goes to the file in aligned blocks, straight to storage (O_DIRECT) when the file system allows it: a
// gigabyte through the page cache is memory pressure, and Android answers that by killing the app.
namespace {

struct Sink {
    int fd = -1;
    bool direct = false;
    char* buf = nullptr;
    static constexpr size_t kCap = 8u << 20;
    size_t used = 0;
    uint64_t total = 0;

    bool open_file(const std::string& path) {
        direct = true;
        fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0600);
        if (fd < 0) {
            direct = false;
            fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        }
        if (fd < 0) return false;
        if (posix_memalign(reinterpret_cast<void**>(&buf), 4096, kCap) != 0) {
            ::close(fd);
            fd = -1;
            return false;
        }
        return true;
    }
    bool flush_block(size_t n) {
        if (!write_all(fd, buf, n)) return false;
        if (!direct) {
            ::fdatasync(fd);
            ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        }
        return true;
    }
    bool put(const void* p, size_t n) {
        const char* c = static_cast<const char*>(p);
        total += n;
        while (n) {
            const size_t k = n < kCap - used ? n : kCap - used;
            std::memcpy(buf + used, c, k);
            used += k, c += k, n -= k;
            if (used == kCap) {
                if (!flush_block(kCap)) return false;
                used = 0;
            }
        }
        return true;
    }
    // The tail, padded to a block, then the file cut to its real length.
    bool finish() {
        bool ok = true;
        if (used) {
            const size_t padded = (used + 4095) & ~size_t(4095);
            std::memset(buf + used, 0, padded - used);
            ok = flush_block(padded);
        }
        if (ok) ok = ::ftruncate(fd, off_t(total)) == 0 && ::fsync(fd) == 0;
        ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        ::close(fd);
        std::free(buf);
        fd = -1;
        buf = nullptr;
        return ok;
    }
    void abandon() {
        if (fd >= 0) ::close(fd);
        std::free(buf);
        fd = -1;
        buf = nullptr;
    }
};

}  // namespace

// Every writable mapping inside the guest windows: the memory that is committed, whether or not it has been touched.
static bool collect_rw(DumpStats& st) {
    const std::vector<Window> wins = {{kModuleBase, kModuleBase + 0x40000000ull, "modules"},
                                      {kHeapBase, kHeapBase + kHeapMax, "heap"},
                                      {kAliasBase, kAliasBase + kAliasSize, "alias"},
                                      {kStackBase, kStackBase + kStackSize, "stack"},
                                      {kKernelBase, kKernelBase + kKernelSize, "kernel"}};
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return false;
    char line[512];
    while (std::fgets(line, sizeof line, maps)) {
        uint64_t lo = 0, hi = 0;
        char perms[8] = {};
        if (std::sscanf(line, "%" SCNx64 "-%" SCNx64 " %7s", &lo, &hi, perms) != 3) continue;
        if (perms[0] != 'r' || perms[1] != 'w') continue;
        for (const Window& w : wins) {
            const uint64_t a = lo > w.lo ? lo : w.lo, b = hi < w.hi ? hi : w.hi;
            if (a < b) st.rw.emplace_back(a, b);
        }
    }
    std::fclose(maps);
    return true;
}

// The guest's memory to a file: every page of the guest windows that the kernel says is in RAM or in swap (untouched
// pages are zero and are left out). Format: "P5RSNAP0", the layout (u64 count, then the window bases), then runs of
// {u64 addr, u64 length, bytes} and a final {0, 0}. Guest threads must be frozen.
static bool dump_memory(const std::string& path, DumpStats& st) {
    const std::vector<Window> wins = {{kModuleBase, kModuleBase + 0x40000000ull, "modules"},
                                      {kHeapBase, kHeapBase + kHeapMax, "heap"},
                                      {kAliasBase, kAliasBase + kAliasSize, "alias"},
                                      {kStackBase, kStackBase + kStackSize, "stack"},
                                      {kKernelBase, kKernelBase + kKernelSize, "kernel"}};
    const std::string tmp = path + ".tmp";
    Sink out;
    if (!out.open_file(tmp)) {
        log("snap: cannot create %s", tmp.c_str());
        return false;
    }
    int pm = ::open("/proc/self/pagemap", O_RDONLY);
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (pm < 0 || !maps) {
        log("snap: cannot read /proc/self/{pagemap,maps}");
        out.abandon();
        if (pm >= 0) ::close(pm);
        if (maps) std::fclose(maps);
        return false;
    }
    bool ok = out.put("P5RSNAP0", 8);
    const uint64_t n = wins.size();
    ok = ok && out.put(&n, 8);
    for (const Window& w : wins) ok = ok && out.put(&w.lo, 8);
    const uint64_t kPage = 4096;
    std::vector<uint64_t> entries;
    char line[512];
    while (ok && std::fgets(line, sizeof line, maps)) {
        uint64_t lo = 0, hi = 0;
        char perms[8] = {};
        if (std::sscanf(line, "%" SCNx64 "-%" SCNx64 " %7s", &lo, &hi, perms) != 3) continue;
        if (perms[0] != 'r' || perms[1] != 'w') continue;  // code and read-only data come back with the boot
        for (const Window& w : wins) {
            const uint64_t a = lo > w.lo ? lo : w.lo, b = hi < w.hi ? hi : w.hi;
            if (a >= b) continue;
            st.regions++;
            const uint64_t pages = (b - a) / kPage;
            entries.resize(pages);
            if (::pread(pm, entries.data(), pages * 8, off_t((a / kPage) * 8)) != ssize_t(pages * 8)) {
                log("snap: pagemap read failed at %" PRIx64, a);
                ok = false;
                break;
            }
            uint64_t i = 0;
            while (ok && i < pages) {
                const bool here = (entries[i] >> 63) & 1 || (entries[i] >> 62) & 1;
                if (!here) {
                    i++;
                    continue;
                }
                uint64_t j = i;
                while (j < pages && (((entries[j] >> 63) & 1) || ((entries[j] >> 62) & 1))) {
                    ((entries[j] >> 63) & 1) ? st.present_pages++ : st.swapped_pages++;
                    j++;
                }
                const uint64_t addr = a + i * kPage, len = (j - i) * kPage;
                ok = out.put(&addr, 8) && out.put(&len, 8) && out.put(reinterpret_cast<const void*>(addr), len);
                st.bytes += len;
                i = j;
            }
        }
    }
    const uint64_t zero[2] = {0, 0};
    ok = ok && out.put(zero, 16);
    std::fclose(maps);
    ::close(pm);
    if (ok) ok = out.finish();
    else out.abandon();
    if (ok) ok = ::rename(tmp.c_str(), path.c_str()) == 0;
    else ::unlink(tmp.c_str());
    return ok;
}

// ---- the registry of the runtime's own events -----------------------------------------------------------------

namespace {

std::map<std::string, std::shared_ptr<EventState>>& event_registry() {
    static std::map<std::string, std::shared_ptr<EventState>> m;
    return m;
}

}  // namespace

namespace {
std::map<std::string, void (*)(CPU&)>& loop_registry() {
    static std::map<std::string, void (*)(CPU&)> m;
    return m;
}
}  // namespace

void register_loop_function(const char* name, void (*fn)(CPU&)) { loop_registry()[name] = fn; }

void (*loop_function(const std::string& name))(CPU&) {
    auto it = loop_registry().find(name);
    return it == loop_registry().end() ? nullptr : it->second;
}

void register_loop(const char* name, CPU& c) {
    KThread* t = kstate::t_self;
    if (!t) return;
    t->loop_name = name;
    if (!t->loop_ctx) t->loop_ctx = new CPU(c);
    else *t->loop_ctx = c;
}

namespace {
struct StateHook {
    std::function<std::vector<uint8_t>()> save;
    std::function<void(const uint8_t*, size_t)> restore;
};
std::map<std::string, StateHook>& state_hooks() {
    static std::map<std::string, StateHook> m;
    return m;
}
}  // namespace

void register_state(const char* name, std::function<std::vector<uint8_t>()> save,
                    std::function<void(const uint8_t*, size_t)> restore) {
    state_hooks()[name] = {std::move(save), std::move(restore)};
}

void register_event(const char* name, std::shared_ptr<EventState> state) { event_registry()[name] = std::move(state); }

void note_call(CPU& c, uint32_t code, int depth) {
    KThread* t = kstate::t_self;
    if (!t) return;
    t->snap_code = code;
    for (int i = 0; i < 8; i++) t->snap_args[i] = c.x[i];
    t->snap_depth = depth;
}

// ---- the state file -------------------------------------------------------------------------------------------

namespace {

constexpr char kStateMagic[8] = {'P', '5', 'R', 'S', 'T', 'A', 'T', '1'};

struct Out {
    std::string b;
    void raw(const void* p, size_t n) { b.append(static_cast<const char*>(p), n); }
    template <typename T>
    void put(const T& v) { raw(&v, sizeof v); }
    void str(const std::string& s) {
        put<uint32_t>(uint32_t(s.size()));
        raw(s.data(), s.size());
    }
};

struct In {
    const char* p;
    const char* end;
    bool ok = true;
    void raw(void* d, size_t n) {
        if (size_t(end - p) < n) { ok = false; std::memset(d, 0, n); return; }
        std::memcpy(d, p, n);
        p += n;
    }
    template <typename T>
    T get() { T v{}; raw(&v, sizeof v); return v; }
    std::string str() {
        const uint32_t n = get<uint32_t>();
        if (!ok || size_t(end - p) < n) { ok = false; return {}; }
        std::string s(p, n);
        p += n;
        return s;
    }
};

enum ObjKind : uint32_t { kObjThread = 1, kObjEvent = 2, kObjSession = 3, kObjShared = 4, kObjTransfer = 5 };

std::vector<uint64_t> layout() {
    return {kModuleBase, kHeapBase, kAliasBase, kAliasSize, kStackBase, kKernelBase, kThunkBase};
}

std::string registry_name(const std::shared_ptr<EventState>& s) {
    for (auto& kv : event_registry())
        if (kv.second == s) return kv.first;
    return {};
}

}  // namespace

// The kernel side of a save: threads (registers and the call each was about to make), the handle table, the memory map.
// False if a thread is somewhere that cannot be put back (inside a runtime function that called into the game).
static bool save_state(const std::string& path, const DumpStats& mem) {
    KernelExport ke = kernel_export();
    Out o;
    o.raw(kStateMagic, 8);
    for (uint64_t v : layout()) o.put(v);
    o.put<uint64_t>(ke.heap_size);
    o.put<uint64_t>(kernel_next_get());
    o.put<uint32_t>(ke.next_handle);
    o.put<uint64_t>(ke.next_thread_id);
    const auto vm = vm_export();
    o.put<uint32_t>(uint32_t(vm.size()));
    for (const VmInfo& b : vm) o.put(b);
    o.put<uint32_t>(uint32_t(mem.rw.size()));
    for (auto& r : mem.rw) {
        o.put<uint64_t>(r.first);
        o.put<uint64_t>(r.second);
    }

    bool clean = true;
    uint32_t threads = 0, events = 0, sessions = 0, shared = 0, transfers = 0, other = 0, unknown_sessions = 0;
    std::string other_names;
    std::string body;
    std::vector<std::pair<uint32_t, std::shared_ptr<KObject>>> session_objs;
    uint32_t count = 0;
    Out ob;
    for (auto& kv : ke.objects) {
        const uint32_t handle = kv.first;
        const auto& obj = kv.second;
        if (auto t = std::dynamic_pointer_cast<KThread>(obj)) {
            threads++;
            const bool live = t->started && !t->exited;
            const bool is_main = t->entry == 0;
            const bool hle_loop = !t->loop_name.empty() && t->loop_ctx;
            if (live && !hle_loop && t->snap_depth != (is_main ? 2 : 1)) {
                log("snap: thread %x (%s) is %d calls deep inside the runtime, cannot be saved here", handle,
                    is_main ? "main" : "worker", t->snap_depth);
                clean = false;
            }
            ob.put<uint32_t>(kObjThread);
            ob.put<uint32_t>(handle);
            ob.put<uint64_t>(t->id);
            ob.put<uint64_t>(t->tls);
            ob.put<uint64_t>(t->entry);
            ob.put<int32_t>(t->prio);
            ob.put<int32_t>(t->core);
            ob.put<uint8_t>(t->started);
            ob.put<uint8_t>(t->exited);
            ob.put<uint32_t>(hle_loop ? 0xFFFFFFFEu : t->snap_code);
            if (hle_loop) ob.str(t->loop_name);
            for (int i = 0; i < 8; i++) ob.put<uint64_t>(hle_loop ? 0 : t->snap_args[i]);
            ob.raw(hle_loop ? static_cast<const void*>(t->loop_ctx) : static_cast<const void*>(&t->ctx), sizeof(CPU));
        } else if (auto e = std::dynamic_pointer_cast<KEvent>(obj)) {
            events++;
            ob.put<uint32_t>(kObjEvent);
            ob.put<uint32_t>(handle);
            ob.put<uint8_t>(e->state && e->state->signaled);
            ob.str(registry_name(e->state));
        } else if (auto sm = std::dynamic_pointer_cast<KSharedMemory>(obj)) {
            shared++;
            ob.put<uint32_t>(kObjShared);
            ob.put<uint32_t>(handle);
            ob.put<uint64_t>(sm->size);
            ob.put<uint64_t>(sm->host);
            ob.str(sm->name);
        } else if (auto tm = std::dynamic_pointer_cast<KTransferMemory>(obj)) {
            transfers++;
            ob.put<uint32_t>(kObjTransfer);
            ob.put<uint32_t>(handle);
            ob.put<uint64_t>(tm->addr);
            ob.put<uint64_t>(tm->size);
        } else {
            std::string name;
            bool domain = false;
            if (ipc_session_info(*obj, name, domain)) {
                sessions++;
                session_objs.emplace_back(handle, obj);
                ob.put<uint32_t>(kObjSession);
                ob.put<uint32_t>(handle);
                ob.put<uint8_t>(domain);
                ob.str(name);
                if (!ipc_service_known(name)) unknown_sessions++;
            } else {
                other++;
                other_names += " " + std::to_string(handle);
                continue;
            }
        }
        count++;
    }
    o.put<uint32_t>(count);
    o.raw(ob.b.data(), ob.b.size());
    const auto thunks = hle_thunks_export();
    o.put<uint32_t>(uint32_t(thunks.size()));
    for (const ThunkRecord& t : thunks) {
        o.str(t.name);
        o.put<uint8_t>(uint8_t(t.kind));
    }
    {
        const std::vector<uint8_t> j = nvn::journal_export();
        o.put<uint32_t>(uint32_t(j.size()));
        o.raw(j.data(), j.size());
    }
    {
        const std::vector<uint8_t> g = ipc_graph_export(session_objs);
        o.put<uint32_t>(uint32_t(g.size()));
        o.raw(g.data(), g.size());
    }
    {
        o.put<uint32_t>(uint32_t(state_hooks().size()));
        for (auto& [name, hook] : state_hooks()) {
            const std::vector<uint8_t> b = hook.save();
            o.str(name);
            o.put<uint32_t>(uint32_t(b.size()));
            o.raw(b.data(), b.size());
        }
    }
    log("snap: state: %u threads, %u events, %u sessions (%u of them stubs or objects made by another service), %u shared "
        "memories, %u transfer memories, %u objects not saved%s",
        threads, events, sessions, unknown_sessions, shared, transfers, other,
        other_names.empty() ? "" : (" (handles" + other_names + ")").c_str());
    if (!clean) return false;
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(o.b.data(), 1, o.b.size(), f) == o.b.size();
    std::fclose(f);
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) return false;
    log("snap: state file %zu bytes", o.b.size());
    return true;
}

// Freeze, dump the memory and the kernel state, thaw. Tries again when a thread was in a place that cannot be saved or
// left a wait during the dump.
// What the GPU holds (images), to its own file.
static bool dump_gpu(const std::string& path) {
    const std::string tmp = path + ".tmp";
    Sink out;
    if (!out.open_file(tmp)) return false;
    bool ok = nvn::gpu_state_save([&](const void* p, size_t n) { return out.put(p, n); });
    if (ok) ok = out.finish();
    else out.abandon();
    if (ok) ok = ::rename(tmp.c_str(), path.c_str()) == 0;
    else ::unlink(tmp.c_str());
    return ok;
}

static bool capture(const std::string& dir) {
    ::mkdir(dir.c_str(), 0700);
    for (int attempt = 1; attempt <= 25; attempt++) {
        const auto t0 = std::chrono::steady_clock::now();
        if (!freeze(8000)) return false;
        const auto t1 = std::chrono::steady_clock::now();
        const uint64_t e0 = wake_epoch();
        DumpStats st;
        // The cheap checks first: a state that cannot be saved must not cost a gigabyte of writes.
        bool ok = collect_rw(st) && save_state(dir + "/state.bin", st);
        ok = ok && dump_memory(dir + "/mem.bin", st);
        ok = ok && dump_gpu(dir + "/gpu.bin");
        const uint64_t e1 = wake_epoch();
        const auto t2 = std::chrono::steady_clock::now();
        thaw();
        auto ms = [](auto a, auto b) { return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count(); };
        if (ok && e0 == e1) {
            log("snap: saved (attempt %d): %" PRIu64 " MB of memory in %" PRIu64 " regions; frozen %lld ms", attempt,
                st.bytes >> 20, st.regions, ms(t1, t2));
            return true;
        }
        log("snap: attempt %d failed (%s), trying again", attempt, ok ? "a thread left a wait during the dump" : "see above");
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        (void)t0;
    }
    return false;
}

// A saved state is a folder with three files. A new one is written beside it and swapped in only when it is whole, so
// a game killed in the middle of saving leaves the last good state alone (and one killed between the two renames
// leaves it as snap.old, which restoring also accepts).
static const char* const kStateFiles[] = {"state.bin", "mem.bin", "gpu.bin"};

static bool state_complete(const std::string& dir) {
    for (const char* f : kStateFiles)
        if (access((dir + "/" + f).c_str(), F_OK) != 0) return false;
    return true;
}

static void remove_state_dir(const std::string& dir) {
    for (const char* f : kStateFiles) {
        ::unlink((dir + "/" + f).c_str());
        ::unlink((dir + "/" + f + ".tmp").c_str());
    }
    ::rmdir(dir.c_str());
}

static std::string usable_state_dir() {
    const std::string base = plat::internal_dir() + "/snap";
    if (state_complete(base)) return base;
    if (state_complete(base + ".old")) return base + ".old";
    return {};
}

static bool capture_safely(const std::string& dir) {
    const std::string fresh = dir + ".new", old = dir + ".old";
    remove_state_dir(fresh);
    if (!capture(fresh)) {
        remove_state_dir(fresh);
        return false;
    }
    if (state_complete(dir)) {
        remove_state_dir(old);
        if (::rename(dir.c_str(), old.c_str()) != 0) {
            log("snap: could not set the previous state aside");
            remove_state_dir(fresh);
            return false;
        }
    }
    if (::rename(fresh.c_str(), dir.c_str()) != 0) {
        log("snap: could not put the new state in place");
        return false;
    }
    remove_state_dir(old);
    return true;
}

bool restore_requested() {
    return access((plat::data_root() + "/snap.restore").c_str(), F_OK) == 0 && !usable_state_dir().empty();
}

bool restore_and_run() {
    const std::string root = plat::data_root(), dir = usable_state_dir();
    if (dir.empty()) return false;
    std::remove((root + "/snap.restore").c_str());  // a state that crashes must not trap the next start
    const auto t0 = std::chrono::steady_clock::now();
    std::string blob;
    {
        FILE* f = std::fopen((dir + "/state.bin").c_str(), "rb");
        if (!f) return false;
        char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) blob.append(buf, n);
        std::fclose(f);
    }
    In in{blob.data(), blob.data() + blob.size()};
    char magic[8];
    in.raw(magic, 8);
    if (!in.ok || std::memcmp(magic, kStateMagic, 8) != 0) {
        log("snap: restore: not a state file");
        return false;
    }
    const std::vector<uint64_t> mine = layout();
    for (size_t i = 0; i < mine.size(); i++) {
        const uint64_t v = in.get<uint64_t>();
        if (v != mine[i]) {
            log("snap: restore: this process lays memory out differently (item %zu: saved %llx, now %llx)", i,
                (unsigned long long)v, (unsigned long long)mine[i]);
            return false;
        }
    }
    const uint64_t heap_size = in.get<uint64_t>();
    const uint64_t kernel_next = in.get<uint64_t>();
    const uint32_t next_handle = in.get<uint32_t>();
    const uint64_t next_thread_id = in.get<uint64_t>();
    std::vector<VmInfo> vm(in.get<uint32_t>());
    for (VmInfo& b : vm) b = in.get<VmInfo>();
    std::vector<std::pair<uint64_t, uint64_t>> rw(in.get<uint32_t>());
    for (auto& r : rw) {
        r.first = in.get<uint64_t>();
        r.second = in.get<uint64_t>();
    }
    if (!in.ok) {
        log("snap: restore: the state file is cut short");
        return false;
    }

    // Memory: the writable mappings of the saved process, then its pages.
    for (auto& r : rw)
        if (!commit(r.first, r.second - r.first)) log("snap: restore: cannot make %llx-%llx writable", (unsigned long long)r.first, (unsigned long long)r.second);
    {
        int fd = ::open((dir + "/mem.bin").c_str(), O_RDONLY);
        if (fd < 0) { log("snap: restore: no memory file"); return false; }
        char head[8];
        uint64_t nwin = 0;
        bool ok = ::read(fd, head, 8) == 8 && std::memcmp(head, "P5RSNAP0", 8) == 0 && ::read(fd, &nwin, 8) == 8;
        if (ok) ::lseek(fd, off_t(nwin * 8), SEEK_CUR);
        uint64_t total = 0;
        while (ok) {
            uint64_t hdr[2];
            if (::read(fd, hdr, 16) != 16) { ok = false; break; }
            if (hdr[0] == 0 && hdr[1] == 0) break;
            char* dst = reinterpret_cast<char*>(hdr[0]);
            uint64_t left = hdr[1];
            while (left) {
                ssize_t n = ::read(fd, dst, left > (16u << 20) ? (16u << 20) : left);
                if (n <= 0) { ok = false; break; }
                dst += n, left -= uint64_t(n), total += uint64_t(n);
                // The file's pages must not pile up next to the memory being filled: a gigabyte of both is what
                // Android answers with a kill. Drop the ones read so far every 64 MB.
                if ((total & ((64u << 20) - 1)) < uint64_t(n)) ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
            }
        }
        ::close(fd);
        if (!ok) {
            log("snap: restore: the memory file is damaged. The process is half restored and cannot go on.");
            plat::exit_process(1);
        }
        log("snap: restore: %llu MB of memory back", (unsigned long long)(total >> 20));
    }
    vm_import(vm);
    kernel_next_set(kernel_next);

    // Kernel objects.
    const uint32_t n_objects = in.get<uint32_t>();
    std::vector<std::shared_ptr<KThread>> live;
    std::shared_ptr<KThread> main_thread;
    uint32_t restored[6] = {};
    for (uint32_t i = 0; i < n_objects && in.ok; i++) {
        const uint32_t kind = in.get<uint32_t>();
        const uint32_t handle = in.get<uint32_t>();
        if (kind == kObjThread) {
            KThreadImage im;
            im.handle = handle;
            im.id = in.get<uint64_t>();
            im.tls = in.get<uint64_t>();
            im.entry = in.get<uint64_t>();
            im.prio = in.get<int32_t>();
            im.core = in.get<int32_t>();
            const bool started = in.get<uint8_t>(), exited = in.get<uint8_t>();
            im.code = in.get<uint32_t>();
            if (im.code == 0xFFFFFFFEu) im.loop_name = in.str();
            for (int k = 0; k < 8; k++) im.args[k] = in.get<uint64_t>();
            in.raw(&im.ctx, sizeof(CPU));
            auto t = kernel_adopt_thread(im);
            t->started = started;
            t->exited = exited;
            restored[0]++;
            if (started && !exited) {
                if (im.entry == 0) main_thread = t;
                else live.push_back(t);
            }
        } else if (kind == kObjEvent) {
            const bool signaled = in.get<uint8_t>();
            const std::string name = in.str();
            auto e = std::make_shared<KEvent>();
            auto it = event_registry().find(name);
            e->state = (!name.empty() && it != event_registry().end()) ? it->second : std::make_shared<EventState>();
            if (name.empty()) e->state->signaled = signaled;
            kernel_adopt_object(handle, e);
            restored[1]++;
        } else if (kind == kObjSession) {
            const bool domain = in.get<uint8_t>();
            const std::string name = in.str();
            kernel_adopt_object(handle, ipc_make_session(name, domain));
            restored[2]++;
        } else if (kind == kObjShared) {
            auto sm = std::make_shared<KSharedMemory>();
            sm->size = in.get<uint64_t>();
            sm->host = in.get<uint64_t>();
            sm->name = in.str();
            kernel_adopt_object(handle, sm);
            restored[3]++;
        } else if (kind == kObjTransfer) {
            auto tm = std::make_shared<KTransferMemory>();
            tm->addr = in.get<uint64_t>();
            tm->size = in.get<uint64_t>();
            kernel_adopt_object(handle, tm);
            restored[4]++;
        } else {
            log("snap: restore: unknown object kind %u", kind);
            in.ok = false;
        }
    }
    if (!in.ok || !main_thread) {
        log("snap: restore: state file damaged or without a main thread. The process is half restored and cannot go on.");
        plat::exit_process(1);
    }
    kernel_set_counters(next_handle, next_thread_id, heap_size);
    {
        std::vector<ThunkRecord> thunks(in.get<uint32_t>());
        for (ThunkRecord& t : thunks) {
            t.name = in.str();
            t.kind = in.get<uint8_t>();
        }
        if (!in.ok || !hle_thunks_restore(thunks)) {
            log("snap: restore: the table of runtime functions does not match. The process is half restored and cannot go on.");
            plat::exit_process(1);
        }
        log("snap: restore: %zu runtime function addresses back", thunks.size());
    }
    std::vector<uint8_t> journal(in.get<uint32_t>());
    if (in.ok && !journal.empty()) in.raw(journal.data(), journal.size());
    std::vector<uint8_t> graph(in.get<uint32_t>());
    if (in.ok && !graph.empty()) in.raw(graph.data(), graph.size());
    std::vector<std::pair<std::string, std::vector<uint8_t>>> hook_blobs(in.ok ? in.get<uint32_t>() : 0);
    for (auto& [name, b] : hook_blobs) {
        name = in.str();
        b.resize(in.get<uint32_t>());
        if (in.ok && !b.empty()) in.raw(b.data(), b.size());
    }
    if (!in.ok) {
        log("snap: restore: the NVN journal is damaged. The process is half restored and cannot go on.");
        plat::exit_process(1);
    }
    const auto t1 = std::chrono::steady_clock::now();
    log("snap: restore: %u threads, %u events, %u sessions, %u shared, %u transfer memories in %lld ms; starting %zu threads and the main one",
        restored[0], restored[1], restored[2], restored[3], restored[4],
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(), live.size());
    struct Ctx {
        std::string dir;
        std::vector<uint8_t>* journal;
        std::vector<uint8_t>* graph;
        std::vector<std::pair<std::string, std::vector<uint8_t>>>* hooks;
        std::vector<std::shared_ptr<KThread>>* live;
    } ctx{dir, &journal, &graph, &hook_blobs, &live};
    kernel_run_restored(
        main_thread.get(),
        [](KThread* t, void* p) {
            Ctx* x = static_cast<Ctx*>(p);
            // The GPU objects first, on this thread (they call guest code), then the others may run.
            CPU c = t->ctx;
            c.x[31] -= 0x2000;
            if (!ipc_graph_import(c, x->graph->data(), x->graph->size()))
                log("snap: restore: the service objects did not all read; going on without them");
            if (!nvn::journal_replay(c, x->journal->data(), x->journal->size())) {
                log("snap: restore: the NVN journal does not read. The process is half restored and cannot go on.");
                plat::exit_process(1);
            }
            if (FILE* gf = std::fopen((x->dir + "/gpu.bin").c_str(), "rb")) {
                const bool good = nvn::gpu_state_restore(c, [&](void* p, size_t n) {
                    const bool r = std::fread(p, 1, n, gf) == n;
                    ::posix_fadvise(fileno(gf), 0, 0, POSIX_FADV_DONTNEED);
                    return r;
                });
                std::fclose(gf);
                if (!good) log("snap: restore: the GPU images did not all read; the picture may start empty");
            } else {
                log("snap: restore: no GPU file; images start empty");
            }
            for (auto& [name, b] : *x->hooks) {
                auto it = state_hooks().find(name);
                if (it != state_hooks().end()) it->second.restore(b.data(), b.size());
                else log("snap: restore: saved state '%s' has no taker in this build", name.c_str());
            }
            start_watcher();
            for (auto& th : *x->live) kernel_start_restored(th.get());
        },
        &ctx);
    return true;
}

void start_watcher() {
    std::thread([] {
        const std::string trigger = plat::data_root() + "/snap.freeze";
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            const std::string dump_trigger = plat::data_root() + "/snap.dump";
            const std::string shot_trigger = plat::data_root() + "/snap.shot";
            if (access(shot_trigger.c_str(), F_OK) == 0) {
                std::remove(shot_trigger.c_str());
                nvn::gpu_debug_shot();
                continue;
            }
            if (access(dump_trigger.c_str(), F_OK) == 0) {
                std::remove(dump_trigger.c_str());
                if (!capture_safely(plat::internal_dir() + "/snap")) log("snap: could not save a state");
                continue;
            }
            if (access(trigger.c_str(), F_OK) != 0) continue;
            int hold = 3000;
            if (FILE* f = std::fopen(trigger.c_str(), "r")) {
                int v = 0;
                if (std::fscanf(f, "%d", &v) == 1 && v > 0) hold = v;
                std::fclose(f);
            }
            std::remove(trigger.c_str());
            log("snap: trigger found, hold %d ms", hold);
            const uint64_t woke0 = g_woke_frozen.load();
            if (freeze(8000)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(hold));
                log("snap: held %d ms, threads that left a wait while frozen: %llu", hold,
                    (unsigned long long)(g_woke_frozen.load() - woke0));
                thaw();
                log("snap: thawed");
            }
        }
    }).detach();
}

}  // namespace nx::snap
