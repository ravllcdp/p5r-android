// Quick resume (docs/QUICK-RESUME.md): stop-the-world for the guest threads, the base of a save state.
//
// freeze() raises g_freeze. A guest thread parks at the entry of its next svc or HLE call (nce_dispatch), where its
// registers are already in its CPU, and the call it was about to make is noted (code and x0..x7), so a restored
// thread can make it again. A thread inside a long wait (WaitScope) counts as still. freeze() returns when every
// started guest thread is parked or waiting; thaw() lets them go.
#pragma once

#include <atomic>
#include <cstdint>

#include <nx/runtime.h>

namespace nx::snap {

extern std::atomic<bool> g_freeze;

// From nce_dispatch, only when g_freeze is set.
void park(CPU& c, uint32_t code);

// True when every guest thread is parked or waiting before the timeout; false (and thawed) otherwise, with the threads
// that did not stop in the log.
bool freeze(int timeout_ms);
void thaw();

// How many times a thread left a wait while frozen (a dump is only consistent if this did not change during it).
uint64_t wake_epoch();

// Marks the calling guest thread as still while it waits (a kernel wait that can last long).
struct WaitScope {
    WaitScope();
    ~WaitScope();
    WaitScope(const WaitScope&) = delete;
    WaitScope& operator=(const WaitScope&) = delete;
};

// Debug trigger: a file named snap.freeze in the data root (its text: how many ms to stay frozen, default 3000)
// freezes, holds and thaws, and logs how long each step took. Replaced by the overlay button in a later step.
void start_watcher();

}  // namespace nx::snap
