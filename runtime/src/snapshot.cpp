#include "snapshot.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cinttypes>
#include <cstring>
#include <vector>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "kernel_state.h"
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
        if (perms[0] != 'r') continue;
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

void start_watcher() {
    std::thread([] {
        const std::string trigger = plat::data_root() + "/snap.freeze";
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            const std::string dump_trigger = plat::data_root() + "/snap.dump";
            if (access(dump_trigger.c_str(), F_OK) == 0) {
                std::remove(dump_trigger.c_str());
                const std::string out = plat::data_root() + "/snap/mem.bin";
                ::mkdir((plat::data_root() + "/snap").c_str(), 0700);
                const auto t0 = std::chrono::steady_clock::now();
                if (freeze(8000)) {
                    const auto t1 = std::chrono::steady_clock::now();
                    const uint64_t e0 = wake_epoch();
                    DumpStats st;
                    const bool ok = dump_memory(out, st);
                    const uint64_t e1 = wake_epoch();
                    const auto t2 = std::chrono::steady_clock::now();
                    thaw();
                    auto ms = [](auto a, auto b) { return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count(); };
                    log("snap: dump %s: %" PRIu64 " regions, %" PRIu64 " MB in RAM + %" PRIu64 " MB in swap = %" PRIu64
                        " MB written; freeze %lld ms, dump %lld ms (total frozen %lld ms); threads that left a wait during the dump: %" PRIu64,
                        ok ? "ok" : "FAILED", st.regions, st.present_pages * 4096 >> 20, st.swapped_pages * 4096 >> 20,
                        st.bytes >> 20, ms(t0, t1), ms(t1, t2), ms(t1, t2), e1 - e0);
                }
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
