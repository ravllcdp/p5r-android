// Quick resume: a journal of the NVN calls that make and set up GPU objects (pools, buffers, textures, windows,
// programs, descriptor pools). The objects live in this process (host side, keyed by the guest struct's address), so a
// save state only has the guest structs; the journal is what builds the host objects again in a fresh process.
// Calls that take a pointer to a builder or a list copy those bytes at the time of the call (the game's own copy may be
// on its stack by now). A Finalize or a new Initialize of an object drops what the journal held for it.
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <nx/runtime.h>

#include "nvn.h"

namespace nx::nvn {

uint64_t gpu_next_get();
void gpu_next_set(uint64_t v);
void gpu_force_next(uint64_t v);

namespace {

enum Kind { kInit, kFinal, kSet, kSlot, kAdd };  // kSlot: one entry per (name, x0, x1); kAdd: stays (cumulative)

struct Spec {
    Kind kind;
    int blob_arg;       // which argument is a pointer to copy; -1 none
    uint32_t blob_size;  // bytes; 0 with blob_count_arg: count * stride
    int count_arg;
    uint32_t stride;
    bool registration;  // replayed after everything else (it names textures and samplers made later)
};

const std::map<std::string, Spec>& specs() {
    static const std::map<std::string, Spec> m = {
        {"nvnMemoryPoolInitialize", {kInit, 1, 0x100, -1, 0, false}},
        {"nvnMemoryPoolFinalize", {kFinal, -1, 0, -1, 0, false}},
        {"nvnMemoryPoolMapVirtual", {kAdd, 2, 0, 1, 0x28, false}},
        {"nvnBufferInitialize", {kInit, 1, 0x100, -1, 0, false}},
        {"nvnBufferFinalize", {kFinal, -1, 0, -1, 0, false}},
        {"nvnTextureInitialize", {kInit, 1, 0x100, -1, 0, false}},
        {"nvnTextureFinalize", {kFinal, -1, 0, -1, 0, false}},
        {"nvnWindowInitialize", {kInit, 1, 0x100, -1, 0, false}},
        {"nvnWindowFinalize", {kFinal, -1, 0, -1, 0, false}},
        {"nvnWindowSetCrop", {kSlot, -1, 0, -1, 0, false}},
        {"nvnWindowSetNumActiveTextures", {kSlot, -1, 0, -1, 0, false}},
        {"nvnWindowSetPresentInterval", {kSlot, -1, 0, -1, 0, false}},
        {"nvnSamplerInitialize", {kInit, 1, 0x100, -1, 0, false}},
        {"nvnSamplerFinalize", {kFinal, -1, 0, -1, 0, false}},
        {"nvnSyncInitialize", {kInit, -1, 0, -1, 0, false}},
        {"nvnSyncFinalize", {kFinal, -1, 0, -1, 0, false}},
        {"nvnCommandBufferInitialize", {kInit, -1, 0, -1, 0, false}},
        {"nvnCommandBufferFinalize", {kFinal, -1, 0, -1, 0, false}},
        {"nvnProgramInitialize", {kInit, -1, 0, -1, 0, false}},
        {"nvnProgramFinalize", {kFinal, -1, 0, -1, 0, false}},
        {"nvnProgramSetShaders", {kSlot, 2, 0, 1, 0x10, false}},
        {"nvnProgramSetShadersExt", {kSlot, 2, 0, 1, 0x18, false}},
        {"nvnSamplerPoolInitialize", {kInit, -1, 0, -1, 0, false}},
        {"nvnTexturePoolInitialize", {kInit, -1, 0, -1, 0, false}},
        {"nvnSamplerPoolFinalize", {kFinal, -1, 0, -1, 0, false}},
        {"nvnTexturePoolFinalize", {kFinal, -1, 0, -1, 0, false}},
        {"nvnTexturePoolRegisterTexture", {kSlot, 3, 0x80, -1, 0, true}},
        {"nvnTexturePoolRegisterImage", {kSlot, 3, 0x80, -1, 0, true}},
        {"nvnSamplerPoolRegisterSampler", {kSlot, -1, 0, -1, 0, true}},
        {"nvnSamplerPoolRegisterSamplerBuilder", {kSlot, 2, 0x100, -1, 0, true}},
        {"nvnDeviceSetDepthMode", {kSlot, -1, 0, -1, 0, false}},
        {"nvnDeviceSetWindowOriginMode", {kSlot, -1, 0, -1, 0, false}},
        {"nvnDeviceRegisterFastClearColor", {kAdd, -1, 0, -1, 0, false}},
        {"nvnDeviceRegisterFastClearColori", {kAdd, -1, 0, -1, 0, false}},
        {"nvnDeviceRegisterFastClearColorui", {kAdd, -1, 0, -1, 0, false}},
        {"nvnDeviceRegisterFastClearDepth", {kAdd, -1, 0, -1, 0, false}},
    };
    return m;
}

struct Entry {
    std::string name;
    uint64_t args[8] = {};
    int blob_arg = -1;
    std::vector<uint8_t> blob;
    uint64_t gpu = 0;  // memory pools: the GPU address they were given
    std::vector<uint64_t> window_textures;  // windows: the textures the builder listed (an array the game may have reused)
    bool registration = false;
};

std::mutex g_lock;
std::map<uint64_t, Entry> g_entries;                    // sequence -> entry
std::unordered_map<uint64_t, std::set<uint64_t>> g_by_obj;  // guest object -> its sequences
std::map<std::string, uint64_t> g_slots;                // "name/x0/x1" -> sequence
uint64_t g_seq = 0;

std::string slot_key(uint64_t obj, const char* name, uint64_t x1) {
    if (!std::strncmp(name, "nvnWindow", 9) || !std::strncmp(name, "nvnDevice", 9) || !std::strncmp(name, "nvnProgram", 10))
        return std::to_string(obj) + "/" + name + "/0";
    return std::to_string(obj) + "/" + name + "/" + std::to_string(x1);
}

void drop_object(uint64_t obj) {
    auto it = g_by_obj.find(obj);
    if (it == g_by_obj.end()) return;
    for (uint64_t s : it->second) g_entries.erase(s);
    g_by_obj.erase(it);
    // slots of that object: names start with the object in the key "obj/name/x1"
    const std::string prefix = std::to_string(obj) + "/";
    for (auto s = g_slots.lower_bound(prefix); s != g_slots.end() && s->first.compare(0, prefix.size(), prefix) == 0;)
        s = g_slots.erase(s);
}

}  // namespace

bool journal_wants(const char* name) { return specs().count(name) != 0; }

void journal_note(const char* name, const uint64_t* args, uint64_t /*result*/) {
    auto sp = specs().find(name);
    if (sp == specs().end()) return;
    const Spec& s = sp->second;
    Entry e;
    e.name = name;
    std::memcpy(e.args, args, sizeof e.args);
    e.registration = s.registration;
    if (s.blob_arg >= 0 && args[s.blob_arg]) {
        uint64_t n = s.blob_size ? s.blob_size : args[s.count_arg] * s.stride;
        if (n > 0x10000) n = 0x10000;
        e.blob_arg = s.blob_arg;
        e.blob.resize(n);
        std::memcpy(e.blob.data(), reinterpret_cast<const void*>(args[s.blob_arg]), n);
    }
    if (!std::strcmp(name, "nvnMemoryPoolInitialize")) {
        if (auto* p = get<MemoryPool>(args[0])) e.gpu = p->gpu;
    }
    if (!std::strcmp(name, "nvnWindowInitialize")) {
        if (auto* w = get<Window>(args[0])) e.window_textures = w->textures;
    }
    std::lock_guard lk(g_lock);
    const uint64_t obj = args[0];
    if (s.kind == kFinal) {
        drop_object(obj);
        return;
    }
    if (s.kind == kInit) drop_object(obj);
    if (s.kind == kSlot) {
        const std::string key = slot_key(obj, name, args[1]);
        auto old = g_slots.find(key);
        if (old != g_slots.end()) {
            g_entries.erase(old->second);
            g_by_obj[obj].erase(old->second);
        }
        g_slots[key] = g_seq + 1;
    }
    const uint64_t seq = ++g_seq;
    g_entries[seq] = std::move(e);
    g_by_obj[obj].insert(seq);
}

// ---- saving and putting back ------------------------------------------------------------------------------------

std::vector<uint8_t> journal_export() {
    std::lock_guard lk(g_lock);
    std::vector<uint8_t> out;
    auto put = [&](const void* p, size_t n) { out.insert(out.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
    const uint64_t next = gpu_next_get(), n = g_entries.size();
    put(&next, 8);
    put(&n, 8);
    for (auto& [seq, e] : g_entries) {
        const uint32_t len = uint32_t(e.name.size());
        put(&len, 4);
        put(e.name.data(), len);
        put(e.args, sizeof e.args);
        const int32_t ba = e.blob_arg;
        put(&ba, 4);
        const uint32_t bn = uint32_t(e.blob.size());
        put(&bn, 4);
        put(e.blob.data(), bn);
        put(&e.gpu, 8);
        const uint8_t reg = e.registration;
        put(&reg, 1);
        const uint32_t wn = uint32_t(e.window_textures.size());
        put(&wn, 4);
        put(e.window_textures.data(), wn * 8);
    }
    return out;
}

bool journal_replay(CPU& c, const uint8_t* p, size_t size) {
    const uint8_t* end = p + size;
    auto get_ = [&](void* d, size_t n) {
        if (size_t(end - p) < n) return false;
        std::memcpy(d, p, n);
        p += n;
        return true;
    };
    uint64_t next = 0, n = 0;
    if (!get_(&next, 8) || !get_(&n, 8)) return false;
    std::vector<Entry> list(n);
    for (Entry& e : list) {
        uint32_t len = 0, bn = 0;
        int32_t ba = 0;
        uint8_t reg = 0;
        if (!get_(&len, 4) || size_t(end - p) < len) return false;
        e.name.assign(reinterpret_cast<const char*>(p), len);
        p += len;
        if (!get_(e.args, sizeof e.args) || !get_(&ba, 4) || !get_(&bn, 4) || size_t(end - p) < bn) return false;
        e.blob_arg = ba;
        e.blob.assign(p, p + bn);
        p += bn;
        if (!get_(&e.gpu, 8) || !get_(&reg, 1)) return false;
        e.registration = reg;
        uint32_t wn = 0;
        if (!get_(&wn, 4) || size_t(end - p) < size_t(wn) * 8) return false;
        e.window_textures.resize(wn);
        if (wn) get_(e.window_textures.data(), size_t(wn) * 8);
    }
    // The replay calls the functions directly, not through the thunks that feed the journal, so a restored process would
    // start with an empty journal and a save made in it would only hold what happened after the restore (the objects of
    // the first session gone: black picture on the next restore). The restored entries become this process's journal.
    {
        std::lock_guard lk(g_lock);
        g_entries.clear(), g_by_obj.clear(), g_slots.clear();
        g_seq = 0;
        for (Entry& e : list) {
            const uint64_t seq = ++g_seq;
            const uint64_t obj = e.args[0];
            auto sp = specs().find(e.name);
            if (sp != specs().end() && sp->second.kind == kSlot) g_slots[slot_key(obj, e.name.c_str(), e.args[1])] = seq;
            g_by_obj[obj].insert(seq);
            g_entries[seq] = e;
        }
    }
    const uint64_t buf = kernel_alloc(0x11000);
    size_t done = 0, failed = 0;
    for (int phase = 0; phase < 2; phase++) {
        for (Entry& e : list) {
            if (e.registration != (phase == 1)) continue;
            HleFn fn = hle_lookup(e.name.c_str());
            if (!fn) {
                failed++;
                continue;
            }
            uint64_t a[8];
            std::memcpy(a, e.args, sizeof a);
            if (e.blob_arg >= 0) {
                std::memcpy(reinterpret_cast<void*>(buf), e.blob.data(), e.blob.size());
                a[e.blob_arg] = buf;
            }
            if (e.gpu) gpu_force_next(e.gpu);
            for (int k = 0; k < 8; k++) c.x[k] = a[k];
            fn(c);
            if (!e.window_textures.empty() && e.name == "nvnWindowInitialize")
                if (auto* w = get<Window>(a[0])) w->textures = e.window_textures;
            done++;
        }
    }
    gpu_next_set(next);
    log("snap: restore: %zu NVN calls replayed (%zu without a function here)", done, failed);
    return true;
}

}  // namespace nx::nvn
