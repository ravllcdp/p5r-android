#include "snapshot.h"

#include <unistd.h>

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

void start_watcher() {
    std::thread([] {
        const std::string trigger = plat::data_root() + "/snap.freeze";
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
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
