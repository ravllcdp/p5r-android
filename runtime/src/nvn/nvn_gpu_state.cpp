// Quick resume: what the GPU holds. Texture data reaches the host images through copy commands (buffer to texture) and
// rendering, so the guest's memory does not have it: a restored process would sample empty images. Every image the
// textures own is read back into the save state and written into the new process's images.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <set>
#include <vector>

#include "nvn_vk.h"

namespace nx::nvn {

using namespace vk;

bool vk_ensure_ready();
VkQueue vk_queue();
uint32_t vk_queue_family();

namespace {

struct Region {
    VkImageAspectFlags aspect;
    uint32_t level, w, h, d;
    uint64_t offset, size;
};

// The block (texels wide, high; bytes) of a format for one aspect; false when unknown.
bool block_of(VkFormat f, VkImageAspectFlags aspect, uint32_t& bw, uint32_t& bh, uint32_t& bytes) {
    bw = bh = 1;
    if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) return bytes = 1, true;
    if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
        switch (f) {
            case VK_FORMAT_D16_UNORM: case VK_FORMAT_D16_UNORM_S8_UINT: return bytes = 2, true;
            case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: case VK_FORMAT_X8_D24_UNORM_PACK32:
            case VK_FORMAT_D24_UNORM_S8_UINT: return bytes = 4, true;
            default: return false;
        }
    }
    const int v = int(f);
    if (v >= 131 && v <= 146) {  // BC1..BC7
        bw = bh = 4;
        const bool eight = v <= 134 || v == 139 || v == 140;  // BC1 (4 formats), BC4 (2)
        return bytes = eight ? 8 : 16, true;
    }
    if (v >= 147 && v <= 156) {  // ETC2, EAC
        bw = bh = 4;
        const bool eight = v <= 150 || v == 153 || v == 154;
        return bytes = eight ? 8 : 16, true;
    }
    static const uint8_t astc[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                        {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    int idx = -1;
    if (v >= 157 && v <= 184) idx = (v - 157) / 2;
    else if (v >= 1000066000 && v <= 1000066013) idx = v - 1000066000;
    if (idx >= 0) return bw = astc[idx][0], bh = astc[idx][1], bytes = 16, true;
    bytes = vk_block_bytes(f);
    return bytes != 0;
}

// The layout of an image's bytes in a staging buffer: level by level, aspect by aspect, tightly packed, each at a
// multiple of 48 (a multiple of every texel block size the copies need).
bool plan(const GpuImage& g, std::vector<Region>& regs, uint64_t& total) {
    std::vector<VkImageAspectFlags> aspects;
    if ((g.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && (g.aspect & VK_IMAGE_ASPECT_STENCIL_BIT))
        aspects = {VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_ASPECT_STENCIL_BIT};
    else
        aspects = {g.aspect};
    const bool is3d = g.view_type == VK_IMAGE_VIEW_TYPE_3D;
    total = 0;
    for (uint32_t l = 0; l < g.levels; l++) {
        for (VkImageAspectFlags a : aspects) {
            uint32_t bw, bh, bytes;
            if (!block_of(g.format, a, bw, bh, bytes)) return false;
            Region r{};
            r.aspect = a, r.level = l;
            r.w = std::max(1u, g.width >> l), r.h = std::max(1u, g.height >> l);
            r.d = is3d ? std::max(1u, g.depth >> l) : 1;
            const uint64_t layers = is3d ? 1 : g.layers;
            r.size = uint64_t((r.w + bw - 1) / bw) * ((r.h + bh - 1) / bh) * bytes * r.d * layers;
            r.offset = (total + 47) / 48 * 48;
            total = r.offset + r.size;
            regs.push_back(r);
        }
    }
    return true;
}

struct Staging {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64_t size = 0;
    uint8_t* ptr = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    bool init() {
        VkDevice d = vk_device();
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = vk_queue_family();
        if (p_vkCreateCommandPool(d, &pi, nullptr, &pool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (p_vkAllocateCommandBuffers(d, &ai, &cb) != VK_SUCCESS) return false;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        return p_vkCreateFence(d, &fi, nullptr, &fence) == VK_SUCCESS;
    }
    bool reserve(uint64_t need) {
        if (need <= size) return true;
        VkDevice d = vk_device();
        release_buffer();
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = need;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (p_vkCreateBuffer(d, &bi, nullptr, &buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        p_vkGetBufferMemoryRequirements(d, buffer, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex =
            memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        void* p = nullptr;
        if (p_vkAllocateMemory(d, &ai, nullptr, &memory) != VK_SUCCESS ||
            p_vkBindBufferMemory(d, buffer, memory, 0) != VK_SUCCESS ||
            p_vkMapMemory(d, memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS)
            return false;
        ptr = static_cast<uint8_t*>(p);
        size = need;
        return true;
    }
    void release_buffer() {
        VkDevice d = vk_device();
        if (ptr) p_vkUnmapMemory(d, memory);
        if (buffer) p_vkDestroyBuffer(d, buffer, nullptr);
        if (memory) p_vkFreeMemory(d, memory, nullptr);
        buffer = VK_NULL_HANDLE, memory = VK_NULL_HANDLE, ptr = nullptr, size = 0;
    }
    void destroy() {
        VkDevice d = vk_device();
        release_buffer();
        if (fence) p_vkDestroyFence(d, fence, nullptr);
        if (pool) p_vkDestroyCommandPool(d, pool, nullptr);
    }
    void begin() {
        p_vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        p_vkBeginCommandBuffer(cb, &bi);
    }
    bool submit_and_wait() {
        p_vkEndCommandBuffer(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        VkDevice d = vk_device();
        p_vkResetFences(d, 1, &fence);
        return p_vkQueueSubmit(vk_queue(), 1, &si, fence) == VK_SUCCESS &&
               p_vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    }
};

VkBufferImageCopy copy_of(const GpuImage& g, const Region& r) {
    VkBufferImageCopy c{};
    c.bufferOffset = r.offset;
    c.imageSubresource = {r.aspect, r.level, 0, g.view_type == VK_IMAGE_VIEW_TYPE_3D ? 1u : g.layers};
    c.imageExtent = {r.w, r.h, r.d};
    return c;
}

struct Header {
    uint64_t guest;
    uint32_t format, width, height, levels, layers, depth, aspect, layout, view_type;
    uint8_t rendered, uploaded, texels;
    uint8_t pad[5];
    uint64_t data_size;
};

}  // namespace

bool gpu_state_save(const std::function<bool(const void*, size_t)>& out) {
    wait_submits_so_far(kWaitFinish);
    worker_finish();
    gpu_wait(gpu_serial());
    std::vector<std::pair<uint64_t, std::shared_ptr<GpuImage>>> images;
    {
        std::shared_lock lk(objects_lock());
        std::set<GpuImage*> seen;
        for (auto& [guest, obj] : objects()) {
            auto* t = dynamic_cast<Texture*>(obj.get());
            if (!t || !t->image || !t->image->image || t->samples > 1) continue;
            if (t->image->layout == VK_IMAGE_LAYOUT_UNDEFINED) continue;
            if (seen.insert(t->image.get()).second) images.emplace_back(guest, t->image);
        }
    }
    Staging st;
    if (!st.init()) return false;
    const char magic[8] = {'P', '5', 'R', 'G', 'P', 'U', '0', '1'};
    const uint32_t count_pos = 0;
    (void)count_pos;
    uint32_t n = uint32_t(images.size());
    bool ok = out(magic, 8) && out(&n, 4);
    uint64_t bytes_total = 0;
    for (auto& [guest, img] : images) {
        GpuImage& g = *img;
        std::vector<Region> regs;
        uint64_t total = 0;
        Header h{};
        h.guest = guest, h.format = uint32_t(g.format), h.width = g.width, h.height = g.height, h.levels = g.levels;
        h.layers = g.layers, h.depth = g.depth, h.aspect = g.aspect, h.layout = uint32_t(g.layout);
        h.view_type = uint32_t(g.view_type);
        h.rendered = g.rendered, h.uploaded = g.uploaded, h.texels = g.texels;
        if (!ok) break;
        if (!plan(g, regs, total) || !st.reserve(total)) {
            h.data_size = 0;  // an image whose format this does not know: nothing saved, it starts empty
            ok = out(&h, sizeof h);
            continue;
        }
        const VkImageLayout back = g.layout;
        st.begin();
        transition(st.cb, g, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        for (const Region& r : regs) {
            VkBufferImageCopy c = copy_of(g, r);
            p_vkCmdCopyImageToBuffer(st.cb, g.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st.buffer, 1, &c);
        }
        transition(st.cb, g, back);
        if (!st.submit_and_wait()) {
            st.destroy();
            return false;
        }
        h.data_size = total;
        bytes_total += total;
        ok = out(&h, sizeof h) && out(st.ptr, total);
    }
    st.destroy();
    log("snap: gpu: %u images, %llu MB", n, (unsigned long long)(bytes_total >> 20));
    return ok;
}

bool gpu_state_restore(CPU&, const std::function<bool(void*, size_t)>& in) {
    char magic[8];
    uint32_t n = 0;
    if (!in(magic, 8) || std::memcmp(magic, "P5RGPU01", 8) != 0 || !in(&n, 4)) return false;
    if (!vk_ensure_ready()) {
        log("snap: restore: gpu: Vulkan did not start");
        return false;
    }
    Staging st;
    if (!st.init()) return false;
    uint32_t put = 0, skipped = 0;
    std::vector<uint8_t> scratch;
    for (uint32_t k = 0; k < n; k++) {
        Header h;
        if (!in(&h, sizeof h)) return false;
        scratch.resize(h.data_size);
        if (h.data_size && !in(scratch.data(), h.data_size)) return false;
        GpuImage* g = h.data_size ? image_of(h.guest) : nullptr;
        if (!g || !g->image || g->format != VkFormat(h.format) || g->width != h.width || g->height != h.height ||
            g->levels != h.levels || g->layers != h.layers || g->depth != h.depth || g->aspect != h.aspect) {
            skipped++;
            continue;
        }
        std::vector<Region> regs;
        uint64_t total = 0;
        if (!plan(*g, regs, total) || total != h.data_size || !st.reserve(total)) {
            skipped++;
            continue;
        }
        std::memcpy(st.ptr, scratch.data(), total);
        st.begin();
        transition(st.cb, *g, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        for (const Region& r : regs) {
            VkBufferImageCopy c = copy_of(*g, r);
            p_vkCmdCopyBufferToImage(st.cb, st.buffer, g->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
        transition(st.cb, *g, VkImageLayout(h.layout));
        if (!st.submit_and_wait()) {
            st.destroy();
            return false;
        }
        g->rendered = h.rendered, g->uploaded = h.uploaded, g->texels = h.texels;
        put++;
    }
    st.destroy();
    log("snap: restore: gpu: %u images back, %u skipped", put, skipped);
    return true;
}

// Debug (snap.shot in the data root): the window's textures and the screen-sized targets, to <data root>/frames/*.ppm.
void gpu_debug_shot() {
    static uint64_t shot = 0;
    std::vector<uint64_t> list;
    {
        std::shared_lock lk(objects_lock());
        for (auto& [guest, obj] : objects()) {
            if (auto* w = dynamic_cast<Window*>(obj.get()))
                for (uint64_t t : w->textures) list.push_back(t);
        }
        for (auto& [guest, obj] : objects()) {
            auto* t = dynamic_cast<Texture*>(obj.get());
            if (t && t->image && (t->width == 1920 || t->width == 1280) && (t->height == 1080 || t->height == 720) &&
                list.size() < 14)
                list.push_back(guest);
        }
    }
    ++shot;
    const uint64_t serial = worker_run([list] {
        for (uint64_t t : list) debug_dump_add(t);
        debug_dump_textures(1000000 + 0);
    });
    if (serial) worker_wait(serial);
    log("snap: shot %llu: %zu textures asked", (unsigned long long)shot, list.size());
}

}  // namespace nx::nvn
