#include "snapshot.h"

#include <fcntl.h>
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
    g_freeze.store(true);
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
            g_freeze.store(false);
            g_thaw_cv.notify_all();
            return false;
        }
        g_parked_cv.wait_for(lk, std::chrono::milliseconds(2));
    }
}

void thaw() {
    {
        std::lock_guard lk(g_m);
        g_freeze.store(false);
    }
    g_thaw_cv.notify_all();
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
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        log("snap: cannot create %s", tmp.c_str());
        return false;
    }
    int pm = ::open("/proc/self/pagemap", O_RDONLY);
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (pm < 0 || !maps) {
        log("snap: cannot read /proc/self/{pagemap,maps}");
        ::close(fd);
        if (pm >= 0) ::close(pm);
        if (maps) std::fclose(maps);
        return false;
    }
    bool ok = write_all(fd, "P5RSNAP0", 8);
    const uint64_t n = wins.size();
    ok = ok && write_all(fd, &n, 8);
    for (const Window& w : wins) ok = ok && write_all(fd, &w.lo, 8);
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
            st.rw.emplace_back(a, b);
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
                ok = write_all(fd, &addr, 8) && write_all(fd, &len, 8) &&
                     write_all(fd, reinterpret_cast<const void*>(addr), len);
                st.bytes += len;
                i = j;
            }
        }
    }
    const uint64_t zero[2] = {0, 0};
    ok = ok && write_all(fd, zero, 16);
    std::fclose(maps);
    ::close(pm);
    if (ok) ok = ::fsync(fd) == 0;
    ::close(fd);
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
    uint32_t threads = 0, events = 0, sessions = 0, shared = 0, transfers = 0, other = 0;
    std::string other_names;
    std::string body;
    uint32_t count = 0;
    Out ob;
    for (auto& kv : ke.objects) {
        const uint32_t handle = kv.first;
        const auto& obj = kv.second;
        if (auto t = std::dynamic_pointer_cast<KThread>(obj)) {
            threads++;
            const bool live = t->started && !t->exited;
            const bool is_main = t->entry == 0;
            if (live && t->snap_depth != (is_main ? 2 : 1)) {
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
            ob.put<uint32_t>(t->snap_code);
            for (int i = 0; i < 8; i++) ob.put<uint64_t>(t->snap_args[i]);
            ob.raw(&t->ctx, sizeof(CPU));
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
                ob.put<uint32_t>(kObjSession);
                ob.put<uint32_t>(handle);
                ob.put<uint8_t>(domain);
                ob.str(name);
                if (!ipc_service_known(name)) {
                    log("snap: session %x is to '%s', which is an object made by another service (not saved yet)", handle, name.c_str());
                    other++;
                }
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
    log("snap: state: %u threads, %u events, %u sessions, %u shared memories, %u transfer memories, %u objects not saved%s",
        threads, events, sessions, shared, transfers, other, other_names.empty() ? "" : (" (handles" + other_names + ")").c_str());
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
static bool capture(const std::string& dir) {
    ::mkdir(dir.c_str(), 0700);
    for (int attempt = 1; attempt <= 25; attempt++) {
        const auto t0 = std::chrono::steady_clock::now();
        if (!freeze(8000)) return false;
        const auto t1 = std::chrono::steady_clock::now();
        const uint64_t e0 = wake_epoch();
        DumpStats st;
        bool ok = dump_memory(dir + "/mem.bin", st);
        ok = ok && save_state(dir + "/state.bin", st);
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

bool restore_requested() {
    const std::string root = plat::data_root();
    return access((root + "/snap.restore").c_str(), F_OK) == 0 && access((root + "/snap/state.bin").c_str(), F_OK) == 0 &&
           access((root + "/snap/mem.bin").c_str(), F_OK) == 0;
}

bool restore_and_run() {
    const std::string root = plat::data_root();
    std::remove((root + "/snap.restore").c_str());  // a state that crashes must not trap the next start
    const auto t0 = std::chrono::steady_clock::now();
    std::string blob;
    {
        FILE* f = std::fopen((root + "/snap/state.bin").c_str(), "rb");
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
        int fd = ::open((root + "/snap/mem.bin").c_str(), O_RDONLY);
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
                ssize_t n = ::read(fd, dst, left > (64u << 20) ? (64u << 20) : left);
                if (n <= 0) { ok = false; break; }
                dst += n, left -= uint64_t(n), total += uint64_t(n);
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
    const auto t1 = std::chrono::steady_clock::now();
    log("snap: restore: %u threads, %u events, %u sessions, %u shared, %u transfer memories in %lld ms; starting %zu threads and the main one",
        restored[0], restored[1], restored[2], restored[3], restored[4],
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(), live.size());
    for (auto& t : live) kernel_start_restored(t.get());
    kernel_run_restored(main_thread.get());
    return true;
}

void start_watcher() {
    std::thread([] {
        const std::string trigger = plat::data_root() + "/snap.freeze";
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            const std::string dump_trigger = plat::data_root() + "/snap.dump";
            if (access(dump_trigger.c_str(), F_OK) == 0) {
                std::remove(dump_trigger.c_str());
                if (!capture(plat::data_root() + "/snap")) log("snap: could not save a state");
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
