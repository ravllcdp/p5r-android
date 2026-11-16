// Quick resume (docs/QUICK-RESUME.md): stop-the-world for the guest threads, the base of a save state.
//
// freeze() raises g_freeze. A guest thread parks at the entry of its next svc or HLE call (nce_dispatch), where its
// registers are already in its CPU, and the call it was about to make is noted (code and x0..x7), so a restored
// thread can make it again. A thread inside a long wait (WaitScope) counts as still. freeze() returns when every
// started guest thread is parked or waiting; thaw() lets them go.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nx/runtime.h>

namespace nx {
struct EventState;  // kernel.h
struct KThread;
}

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

// A thread that leaves a wait while frozen must not run: it holds here (still waiting, with the kernel lock released
// by the wait) until thaw(). hold() is the same for a place where no lock is held (a sleep).
void hold_waiting(std::unique_lock<std::recursive_mutex>& lk, KThread* self);
void hold();

// Noted on every call a guest thread makes into the runtime (nce_dispatch): what it is about to do, for a restored
// thread to do it again.
void note_call(CPU& c, uint32_t code, int depth);

// A runtime function that runs as a thread's whole life and keeps no state (the movie pump) says so on entry, with its
// own name, and is registered by that name: a restored process starts it again from its saved entry registers.
void register_loop(const char* name, CPU& c);
void register_loop_function(const char* name, void (*fn)(CPU&));
void (*loop_function(const std::string& name))(CPU&);

// State a runtime part keeps outside guest memory (the audio queue): saved as bytes under its name, handed back to it
// before the guest threads resume in a restored process.
void register_state(const char* name, std::function<std::vector<uint8_t>()> save,
                    std::function<void(const uint8_t*, size_t)> restore);

// Events that live in the runtime (vsync, audio frame): a restored handle must point at this process's, not a copy.
void register_event(const char* name, std::shared_ptr<EventState> state);

// At boot: true if a restore was asked for (snap.restore in the data root) and a state is there to restore.
bool restore_requested();
// Puts a saved state back and runs the main thread on the calling thread until it returns. False (nothing touched)
// if the state does not fit this process; boot then goes on normally.
bool restore_and_run();

// Automatic saving (a thread started with the watcher): every few minutes (the first line of snap.auto in the data root:
// seconds, 0 = never, default 600) and whenever the app is paused (home, app switcher, screen off), when the game holds
// still anyway. set_paused is called from the platform's lifecycle events.
void set_paused(bool paused);
// Asks the watcher for a save as soon as it can (not within 20 s of another): the system is short of memory and may end
// the process within seconds. `why` must stay valid (a literal).
void request_save(const char* why);

// Debug trigger: a file named snap.freeze in the data root (its text: how many ms to stay frozen, default 3000)
// freezes, holds and thaws, and logs how long each step took. Replaced by the overlay button in a later step.
void start_watcher();

}  // namespace nx::snap
