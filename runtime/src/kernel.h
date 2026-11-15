// Kernel objects shared by kernel.cpp and ipc.cpp.
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <vector>
#include <string>

#include <nx/runtime.h>

namespace nx {

struct KObject {
    virtual ~KObject() = default;
    virtual bool signaled() const { return false; }
    virtual void consume() {}
};

struct KThread : KObject {
    std::weak_ptr<KThread> self;
    uint32_t handle = 0;
    uint64_t id = 0;
    CPU ctx{};
    uint64_t tls = 0;
    uint64_t entry = 0;
    int64_t host_tid = 0;
    uint64_t last_yield = 0;  // svcSleepThread(0) back-off
    uint32_t yields = 0;
    uint64_t last_tick_call = 0;  // nn::os::GetSystemTick back-off
    uint32_t tick_spins = 0;
    int prio = 44, core = 0;
    std::atomic<const char*> hle{nullptr};  // the HLE function it is in (for the stall report), null in guest code
    bool started = false, exited = false, cancel = false;
    // wait state (under the kernel lock)
    bool wake = false;
    uint32_t wait_result = 0;
    uint64_t wait_mutex = 0, wait_key = 0, wait_addr = 0;
    uint32_t wait_tag = 0;
    std::condition_variable_any cv;  // this thread's own wake-up (with the kernel lock)
    // Quick resume (snapshot.cpp): 1 while parked at the entry of a call, wait depth, and the call it was about to make.
    std::atomic<int> snap_state{0};
    std::atomic<int> snap_wait{0};
    uint32_t snap_code = 0;
    uint64_t snap_args[8] = {};
    // A thread that sits in a runtime loop with no state of its own (the movie pump): its registers on entering the loop and
    // the loop's name, so a restored process starts the loop again.
    std::string loop_name;
    CPU* loop_ctx = nullptr;
    int snap_depth = 0;  // nested nce_enter frames on its host thread when it made that call
    bool signaled() const override { return exited; }
};

struct EventState {
    bool signaled = false;
};

struct KEvent : KObject {
    std::shared_ptr<EventState> state;
    bool signaled() const override { return state->signaled; }
};

struct KSharedMemory : KObject {
    uint64_t size = 0;
    uint64_t host = 0;  // backing memory the service writes to (kernel region)
    std::string name;
};

struct KTransferMemory : KObject {
    uint64_t addr = 0, size = 0;
};

// Quick resume (snapshot.cpp, kernel.cpp): a thread as it was frozen, and the handle table around it.
struct KThreadImage {
    uint32_t handle = 0;
    uint64_t id = 0, tls = 0, entry = 0;
    int32_t prio = 44, core = 0;
    uint32_t code = 0;
    uint64_t args[8] = {};
    CPU ctx{};
    std::string loop_name;
};
struct KernelExport {
    uint32_t next_handle = 0;
    uint64_t next_thread_id = 0, heap_size = 0;
    std::vector<std::pair<uint32_t, std::shared_ptr<KObject>>> objects;
};
KernelExport kernel_export();
void kernel_set_counters(uint32_t next_handle, uint64_t next_thread_id, uint64_t heap_size);
std::shared_ptr<KThread> kernel_adopt_thread(const KThreadImage& im);
void kernel_adopt_object(uint32_t handle, std::shared_ptr<KObject> o);
void kernel_start_restored(KThread* t);  // on a new host thread
// `before` runs on the thread first, once its identity is set and before it resumes (the NVN objects are built there).
void kernel_run_restored(KThread* t, void (*before)(KThread*, void*) = nullptr, void* arg = nullptr);    // on the calling thread (the main thread's)

uint32_t handle_add(std::shared_ptr<KObject> o);
std::shared_ptr<KObject> handle_get(uint32_t h);
void handle_close(uint32_t h);
void notify_all();
KThread* self_thread();
void backtrace(CPU& c);
uint64_t heap_size();  // svcSetHeapSize's current size

// svc_process.cpp
uint64_t get_info(uint32_t type, uint32_t handle, uint64_t sub, uint32_t& rc);
uint32_t set_thread_core_mask(uint32_t handle, int32_t ideal, uint64_t mask);
uint32_t map_transfer_memory(uint32_t handle, uint64_t addr, uint64_t size);
uint32_t unmap_transfer_memory(uint32_t handle, uint64_t addr, uint64_t size);
// Initial static TLS of main (its .tdata), copied to tp+0x10 of every thread created afterwards.
void set_tls_template(uint64_t start, uint64_t end);
void init_tls_block(uint64_t block);

// ipc.cpp
bool ipc_session_info(const KObject& o, std::string& name, bool& domain);
std::shared_ptr<KObject> ipc_make_session(const std::string& name, bool domain);
bool ipc_service_known(const std::string& name);
uint32_t ipc_connect(const char* port, uint64_t* out_handle);
uint32_t ipc_request(CPU& c, uint32_t handle, uint64_t buf, uint64_t size);
uint32_t map_shared_memory(KSharedMemory& s, uint64_t addr, uint64_t size);

}  // namespace nx
