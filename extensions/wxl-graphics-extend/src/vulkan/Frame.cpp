// wxl-graphics-extend: the frames in flight -- per-slot command buffer, fence, pools, host rings,
// timestamps and deferred destruction; the submission on DXVK's queue.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "Frame.hpp"
#include "Device.hpp"
#include "Ring.hpp"
#include "../core/Extension.hpp"
#include "../frame/Scheduler.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
    namespace device = wxl::gfx::vulkan::device;
    namespace frames = wxl::gfx::vulkan::frames;
    using wxl::gfx::vulkan::Ring;
    using wxl::gfx::vulkan::ResultName;

    /// A block that takes longer than this has hung the GPU: skipping frames beats freezing the client.
    constexpr uint64_t     kFenceTimeoutNs = 2'000'000'000ull;
    constexpr VkDeviceSize kUniformBytes   = VkDeviceSize(4) << 20;
    constexpr VkDeviceSize kStagingBytes   = VkDeviceSize(16) << 20;
    constexpr VkDeviceSize kStagingAlign   = 16;   // a multiple of every texel size UploadImage takes
    constexpr uint32_t     kMaxSets        = 512;
    constexpr uint32_t     kMaxTimers      = 30;   // compute passes timed per block; the rest read -1
    constexpr uint32_t     kQueryCount     = 2 + 2 * kMaxTimers;   // queries 0, 1: the block; 2 + 2i, 3 + 2i: pass i

    struct Slot
    {
        VkCommandPool    pool = VK_NULL_HANDLE;
        VkCommandBuffer  cmd = VK_NULL_HANDLE;
        VkFence          fence = VK_NULL_HANDLE;
        VkDescriptorPool descriptors = VK_NULL_HANDLE;
        VkQueryPool      queries = VK_NULL_HANDLE;
        VkBuffer         buffer = VK_NULL_HANDLE;
        VkDeviceMemory   memory = VK_NULL_HANDLE;
        uint8_t*         mapped = nullptr;
        Ring             uniform;
        Ring             staging;
        bool             pending = false;   // submitted; its fence not yet seen signaled
        uint64_t         seq = 0;           // the block recorded in it
        bool             blockTimed = false;
        uint32_t         timers = 0;
        uint32_t         timerPass[kMaxTimers] = {};
        uint32_t         sets = 0;
    };

    Slot     g_slots[frames::kSlots];
    uint32_t g_next = 0;          // the slot the next block records in
    uint64_t g_seq = 0;           // the block being recorded, or the last one begun
    uint64_t g_done = 0;          // every block up to this number is known done
    bool     g_created = false;
    bool     g_recording = false;
    Slot*    g_current = nullptr;

    bool         g_timestamps = false;
    uint64_t     g_timestampMask = 0;
    double       g_nsPerTick = 0.0;
    VkDeviceSize g_uniformAlign = 16;

    /// One object waiting for the GPU: destroyed once block `seq` is known done.
    struct Deferred
    {
        enum Kind { View, Image, Pipeline, Resource } kind;
        uint64_t              seq;
        VkImageView           view = VK_NULL_HANDLE;
        VkImage               image = VK_NULL_HANDLE;
        VkDeviceMemory        memory = VK_NULL_HANDLE;
        VkPipeline            pipeline = VK_NULL_HANDLE;
        VkPipelineLayout      layout = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        IUnknown*             resource = nullptr;
    };
    std::vector<Deferred> g_deferred;

    struct PassTime
    {
        uint32_t id;
        float    ms;
    };
    std::vector<PassTime> g_passMs;   // a handful of passes: a linear search
    float    g_blockMs = -1.0f;
    uint32_t g_setsPeak = 0;

    void Smooth(float& value, float sample)
    {
        value = value < 0.0f ? sample : value * 0.9f + sample * 0.1f;
    }

    void SmoothPass(uint32_t id, float sample)
    {
        for (PassTime& p : g_passMs)
            if (p.id == id) { Smooth(p.ms, sample); return; }
        g_passMs.push_back(PassTime{ id, sample });
    }

    /// The Vulkan objects of an entry go only when the GPU is known done with them; a D3D9
    /// reference is released either way, since that is the D3D9 device's business, not the GPU's.
    void Dispose(const Deferred& d, bool destroyObjects)
    {
        if (d.kind == Deferred::Resource)
        {
            if (d.resource) d.resource->Release();
            return;
        }
        if (!destroyObjects) return;
        const device::Context& c = device::Ctx();
        switch (d.kind)
        {
        case Deferred::View:
            if (d.view) c.fn.vkDestroyImageView(c.device, d.view, nullptr);
            break;
        case Deferred::Image:
            if (d.view)   c.fn.vkDestroyImageView(c.device, d.view, nullptr);
            if (d.image)  c.fn.vkDestroyImage(c.device, d.image, nullptr);
            if (d.memory) c.fn.vkFreeMemory(c.device, d.memory, nullptr);
            break;
        case Deferred::Pipeline:
            if (d.pipeline)  c.fn.vkDestroyPipeline(c.device, d.pipeline, nullptr);
            if (d.layout)    c.fn.vkDestroyPipelineLayout(c.device, d.layout, nullptr);
            if (d.setLayout) c.fn.vkDestroyDescriptorSetLayout(c.device, d.setLayout, nullptr);
            break;
        default:
            break;
        }
    }

    void FlushDeferred(uint64_t upTo, bool destroyObjects)
    {
        size_t kept = 0;
        for (size_t i = 0; i < g_deferred.size(); ++i)
        {
            if (g_deferred[i].seq <= upTo) Dispose(g_deferred[i], destroyObjects);
            else g_deferred[kept++] = g_deferred[i];
        }
        g_deferred.resize(kept);
    }

    void Defer(Deferred d)
    {
        d.seq = g_seq;
        g_deferred.push_back(d);
    }

    /// The timestamps of a retired slot: available for certain, since its fence signaled.
    void ReadTimestamps(Slot& s)
    {
        if (!g_timestamps || !s.blockTimed) return;
        const device::Context& c = device::Ctx();
        const uint32_t count = 2 + 2 * s.timers;
        uint64_t data[2 * kQueryCount];   // value, availability per query
        const VkResult r = c.fn.vkGetQueryPoolResults(c.device, s.queries, 0, count, sizeof(uint64_t) * 2 * count, data,
                                                      2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (r != VK_SUCCESS && r != VK_NOT_READY) return;
        auto pairMs = [&](uint32_t first, float& ms) {
            if (!data[2 * first + 1] || !data[2 * first + 3]) return false;
            const uint64_t ticks = (data[2 * first + 2] - data[2 * first]) & g_timestampMask;
            ms = float(double(ticks) * g_nsPerTick * 1e-6);
            return true;
        };
        float ms = 0.0f;
        if (pairMs(0, ms)) Smooth(g_blockMs, ms);
        for (uint32_t i = 0; i < s.timers; ++i)
            if (pairMs(2 + 2 * i, ms)) SmoothPass(s.timerPass[i], ms);
        s.blockTimed = false;
    }

    /// The slot's block is known done: what it held goes, and so does everything older.
    void Retire(Slot& s)
    {
        ReadTimestamps(s);
        s.pending = false;
        if (s.seq > g_done) g_done = s.seq;
        FlushDeferred(g_done, true);
    }

    /// False on a timeout; lost says the device itself is gone.
    bool WaitSlot(Slot& s, bool& lost)
    {
        const device::Context& c = device::Ctx();
        const VkResult r = c.fn.vkWaitForFences(c.device, 1, &s.fence, VK_TRUE, kFenceTimeoutNs);
        if (r == VK_SUCCESS) return true;
        lost = r != VK_TIMEOUT;
        return false;
    }

    bool CreateSlot(Slot& s, uint32_t index)
    {
        const device::Context& c = device::Ctx();
        VkDevice dev = c.device;
        VkResult r = VK_SUCCESS;
        auto fail = [&](const char* what) {
            GFX_LOG_WARN("vulkan: frame slot %u: %s failed (%s)", index, what, ResultName(r));
            return false;
        };

        VkCommandPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;   // reset whole each reuse
        pool.queueFamilyIndex = c.queueFamily;
        if ((r = c.fn.vkCreateCommandPool(dev, &pool, nullptr, &s.pool)) != VK_SUCCESS) return fail("vkCreateCommandPool");

        VkCommandBufferAllocateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cb.commandPool = s.pool;
        cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb.commandBufferCount = 1;
        if ((r = c.fn.vkAllocateCommandBuffers(dev, &cb, &s.cmd)) != VK_SUCCESS) return fail("vkAllocateCommandBuffers");

        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if ((r = c.fn.vkCreateFence(dev, &fence, nullptr, &s.fence)) != VK_SUCCESS) return fail("vkCreateFence");

        const VkDescriptorPoolSize sizes[] = {
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024 },
            { VK_DESCRIPTOR_TYPE_SAMPLER, 512 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256 },
        };
        VkDescriptorPoolCreateInfo dp{};
        dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dp.maxSets = kMaxSets;
        dp.poolSizeCount = uint32_t(sizeof sizes / sizeof sizes[0]);
        dp.pPoolSizes = sizes;
        if ((r = c.fn.vkCreateDescriptorPool(dev, &dp, nullptr, &s.descriptors)) != VK_SUCCESS) return fail("vkCreateDescriptorPool");

        if (g_timestamps)
        {
            VkQueryPoolCreateInfo qp{};
            qp.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qp.queryCount = kQueryCount;
            if ((r = c.fn.vkCreateQueryPool(dev, &qp, nullptr, &s.queries)) != VK_SUCCESS) return fail("vkCreateQueryPool");
        }

        VkBufferCreateInfo buffer{};
        buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.size = kUniformBytes + kStagingBytes;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if ((r = c.fn.vkCreateBuffer(dev, &buffer, nullptr, &s.buffer)) != VK_SUCCESS) return fail("vkCreateBuffer");

        VkMemoryRequirements req{};
        c.fn.vkGetBufferMemoryRequirements(dev, s.buffer, &req);
        const int32_t type = device::MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type < 0)
        {
            GFX_LOG_WARN("vulkan: frame slot %u: no host-visible coherent memory type for the rings", index);
            return false;
        }
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = uint32_t(type);
        if ((r = c.fn.vkAllocateMemory(dev, &alloc, nullptr, &s.memory)) != VK_SUCCESS) return fail("vkAllocateMemory");
        if ((r = c.fn.vkBindBufferMemory(dev, s.buffer, s.memory, 0)) != VK_SUCCESS) return fail("vkBindBufferMemory");
        void* mapped = nullptr;
        if ((r = c.fn.vkMapMemory(dev, s.memory, 0, VK_WHOLE_SIZE, 0, &mapped)) != VK_SUCCESS) return fail("vkMapMemory");
        s.mapped = static_cast<uint8_t*>(mapped);
        s.uniform.Init(0, kUniformBytes);
        s.staging.Init(kUniformBytes, kStagingBytes);
        return true;
    }

    void DestroySlot(Slot& s, bool destroyObjects)
    {
        if (destroyObjects)
        {
            const device::Context& c = device::Ctx();
            VkDevice dev = c.device;
            if (s.mapped)      c.fn.vkUnmapMemory(dev, s.memory);
            if (s.buffer)      c.fn.vkDestroyBuffer(dev, s.buffer, nullptr);
            if (s.memory)      c.fn.vkFreeMemory(dev, s.memory, nullptr);
            if (s.queries)     c.fn.vkDestroyQueryPool(dev, s.queries, nullptr);
            if (s.descriptors) c.fn.vkDestroyDescriptorPool(dev, s.descriptors, nullptr);
            if (s.fence)       c.fn.vkDestroyFence(dev, s.fence, nullptr);
            if (s.pool)        c.fn.vkDestroyCommandPool(dev, s.pool, nullptr);   // frees its command buffer
        }
        s = Slot{};
    }

    void WarnNotRecording(const char* what)
    {
        static bool warned = false;
        if (warned) return;
        warned = true;
        GFX_LOG_WARN("vulkan: %s: only inside a record callback; ignored (this is logged once)", what);
    }
}

namespace wxl::gfx::vulkan::frames
{
    bool Create()
    {
        const device::Context& c = device::Ctx();
        const VkPhysicalDeviceLimits& l = c.properties.limits;
        // The scheduler's profiling switch (WXL_GFX_PROFILE) governs the compute timers too.
        g_timestamps = c.timestampValidBits != 0 && wxl::gfx::frame::ProfilingEnabled();
        g_timestampMask = c.timestampValidBits >= 64 ? ~0ull : ((1ull << c.timestampValidBits) - 1);
        g_nsPerTick = l.timestampPeriod;
        // One buffer serves uniform and storage descriptors alike: align to the stricter of the two.
        g_uniformAlign = std::max<VkDeviceSize>({ 16, l.minUniformBufferOffsetAlignment, l.minStorageBufferOffsetAlignment });
        g_next = 0;
        g_done = g_seq;
        g_blockMs = -1.0f;
        g_setsPeak = 0;
        g_passMs.clear();
        for (uint32_t i = 0; i < kSlots; ++i)
        {
            if (!CreateSlot(g_slots[i], i))
            {
                g_created = true;   // so that Destroy tears down the part that was made
                return false;
            }
        }
        g_created = true;
        GFX_LOG_INFO("vulkan: %u frame slots: %u MB uniform + %u MB staging each, %u descriptor sets, timestamps %s",
                     kSlots, unsigned(kUniformBytes >> 20), unsigned(kStagingBytes >> 20), kMaxSets, g_timestamps ? "on" : "off");
        return true;
    }

    bool Destroy()
    {
        bool clean = true;
        if (g_created)
        {
            for (uint32_t k = 0; k < kSlots; ++k)
            {
                Slot& s = g_slots[(g_next + k) % kSlots];   // oldest first: the same order they were submitted
                if (!s.pending) continue;
                bool lost = false;
                if (WaitSlot(s, lost)) Retire(s);
                else
                {
                    clean = false;
                    s.pending = false;
                }
            }
        }
        if (!clean)
            GFX_LOG_WARN("vulkan: the GPU did not finish the compute blocks in time: the old device's Vulkan objects are left to it");
        FlushDeferred(~0ull, clean);
        for (Slot& s : g_slots) DestroySlot(s, clean);
        g_created = false;
        g_recording = false;
        g_current = nullptr;
        g_next = 0;
        g_done = g_seq;
        g_passMs.clear();
        g_blockMs = -1.0f;
        g_setsPeak = 0;
        return clean;
    }

    void WaitAll()
    {
        if (!g_created || !device::Available()) return;
        for (uint32_t k = 0; k < kSlots; ++k)
        {
            Slot& s = g_slots[(g_next + k) % kSlots];
            if (!s.pending) continue;
            bool lost = false;
            if (WaitSlot(s, lost)) { Retire(s); continue; }
            if (lost) device::MarkUnusable("vkWaitForFences returned an error before the device reset");
            else
            {
                static bool warned = false;
                if (!warned)
                {
                    warned = true;
                    GFX_LOG_WARN("vulkan: a compute block did not finish within 2 s before the device reset");
                }
            }
            return;   // the younger slots are no further along
        }
    }

    void Poll()
    {
        if (!g_created || g_recording || !device::Available()) return;
        const device::Context& c = device::Ctx();
        for (uint32_t k = 0; k < kSlots; ++k)
        {
            Slot& s = g_slots[(g_next + k) % kSlots];   // oldest first, and only in order
            if (!s.pending) continue;
            if (c.fn.vkGetFenceStatus(c.device, s.fence) != VK_SUCCESS) return;
            Retire(s);
        }
    }

    bool Begin()
    {
        if (!device::Available() || !g_created || g_recording) return false;
        const device::Context& c = device::Ctx();
        Slot& s = g_slots[g_next];
        if (s.pending)
        {
            bool lost = false;
            if (!WaitSlot(s, lost))
            {
                if (lost) device::MarkUnusable("vkWaitForFences returned an error (device lost?)");
                else
                {
                    static bool warned = false;
                    if (!warned)
                    {
                        warned = true;
                        GFX_LOG_WARN("vulkan: a compute block did not finish within 2 s: this frame's block is skipped (logged once)");
                    }
                }
                return false;
            }
            Retire(s);
        }

        VkResult r = c.fn.vkResetFences(c.device, 1, &s.fence);
        if (r == VK_SUCCESS) r = c.fn.vkResetCommandPool(c.device, s.pool, 0);
        if (r == VK_SUCCESS) r = c.fn.vkResetDescriptorPool(c.device, s.descriptors, 0);
        s.uniform.Reset();
        s.staging.Reset();
        s.timers = 0;
        s.sets = 0;
        s.blockTimed = false;

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (r == VK_SUCCESS) r = c.fn.vkBeginCommandBuffer(s.cmd, &begin);
        if (r != VK_SUCCESS)
        {
            if (r == VK_ERROR_DEVICE_LOST) device::MarkUnusable("VK_ERROR_DEVICE_LOST while beginning a block");
            else
            {
                static bool warned = false;
                if (!warned)
                {
                    warned = true;
                    GFX_LOG_WARN("vulkan: could not begin a compute block (%s); skipped (logged once)", ResultName(r));
                }
            }
            return false;
        }
        if (g_timestamps) c.fn.vkCmdResetQueryPool(s.cmd, s.queries, 0, kQueryCount);

        s.seq = ++g_seq;
        g_current = &s;
        g_recording = true;
        return true;
    }

    bool Submit()
    {
        if (!g_recording || !g_current) return false;
        Slot& s = *g_current;
        g_recording = false;
        g_current = nullptr;
        const device::Context& c = device::Ctx();

        VkResult r = c.fn.vkEndCommandBuffer(s.cmd);
        if (r != VK_SUCCESS)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                GFX_LOG_WARN("vulkan: vkEndCommandBuffer failed (%s): the block is dropped (logged once)", ResultName(r));
            }
            return false;
        }
        if (!device::Available()) return false;

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &s.cmd;

        // The D3D9 work the block reads (the world pass) was flushed before the block recorded
        // (device::SettleD3D9); the queue is now ours for one submission and nothing D3D9 may be
        // called until it is handed back.
        c.interop->LockSubmissionQueue();
        r = c.fn.vkQueueSubmit(c.queue, 1, &submit, s.fence);
        c.interop->ReleaseSubmissionQueue();

        if (r != VK_SUCCESS)
        {
            if (r == VK_ERROR_DEVICE_LOST) device::MarkUnusable("vkQueueSubmit returned VK_ERROR_DEVICE_LOST");
            else
            {
                static bool warned = false;
                if (!warned)
                {
                    warned = true;
                    GFX_LOG_WARN("vulkan: vkQueueSubmit failed (%s): the block is dropped (logged once)", ResultName(r));
                }
            }
            return false;   // the fence stays unsignaled and the slot free: reused without a wait
        }
        s.pending = true;
        g_next = (g_next + 1) % kSlots;
        return true;
    }

    void Abandon()
    {
        // The command buffer stays in the recording state; the pool reset on the slot's next use
        // takes care of it. What the block deferred is flushed by a later block's retire.
        g_recording = false;
        g_current = nullptr;
    }

    bool            Recording() { return g_recording; }
    VkCommandBuffer Cmd() { return g_current ? g_current->cmd : VK_NULL_HANDLE; }
    uint32_t        SlotIndex() { return g_current ? uint32_t(g_current - g_slots) : 0; }

    bool AllocUniform(VkDeviceSize size, WXL_GfxVkAlloc& out)
    {
        if (!g_recording) { WarnNotRecording("AllocUniform"); return false; }
        if (!size) return false;
        Slot& s = *g_current;
        uint64_t offset = 0;
        if (!s.uniform.Alloc(size, g_uniformAlign, offset))
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                GFX_LOG_WARN("vulkan: AllocUniform: the %u MB uniform ring is full this frame (logged once)", unsigned(kUniformBytes >> 20));
            }
            return false;
        }
        out.buffer = s.buffer;
        out.offset = offset;
        out.size = size;
        out.mapped = s.mapped + offset;
        return true;
    }

    bool AllocStaging(VkDeviceSize size, WXL_GfxVkAlloc& out, VkDeviceSize alignment)
    {
        if (!g_recording) { WarnNotRecording("AllocStaging"); return false; }
        if (!size || alignment < kStagingAlign) return false;
        Slot& s = *g_current;
        uint64_t offset = 0;
        if (!s.staging.Alloc(size, alignment, offset))
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                GFX_LOG_WARN("vulkan: AllocStaging: the %u MB staging ring is full this frame (logged once)", unsigned(kStagingBytes >> 20));
            }
            return false;
        }
        out.buffer = s.buffer;
        out.offset = offset;
        out.size = size;
        out.mapped = s.mapped + offset;
        return true;
    }

    VkDescriptorSet AllocDescriptorSet(VkDescriptorSetLayout layout)
    {
        if (!g_recording) { WarnNotRecording("AllocDescriptorSet"); return VK_NULL_HANDLE; }
        if (!layout) return VK_NULL_HANDLE;
        Slot& s = *g_current;
        const device::Context& c = device::Ctx();
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = s.descriptors;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult r = c.fn.vkAllocateDescriptorSets(c.device, &alloc, &set);
        if (r != VK_SUCCESS)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                GFX_LOG_WARN("vulkan: AllocDescriptorSet failed (%s): the frame's pool is out (logged once)", ResultName(r));
            }
            return VK_NULL_HANDLE;
        }
        if (++s.sets > g_setsPeak) g_setsPeak = s.sets;
        return set;
    }

    void TimestampBlockBegin()
    {
        if (!g_timestamps || !g_recording) return;
        Slot& s = *g_current;
        device::Ctx().fn.vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.queries, 0);
        s.blockTimed = true;
    }

    void TimestampBlockEnd()
    {
        if (!g_timestamps || !g_recording || !g_current->blockTimed) return;
        Slot& s = *g_current;
        device::Ctx().fn.vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.queries, 1);
    }

    int TimerBegin(uint32_t passId)
    {
        if (!g_timestamps || !g_recording || !g_current->blockTimed) return -1;
        Slot& s = *g_current;
        if (s.timers >= kMaxTimers) return -1;
        const uint32_t i = s.timers++;
        s.timerPass[i] = passId;
        device::Ctx().fn.vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.queries, 2 + 2 * i);
        return int(i);
    }

    void TimerEnd(int timer)
    {
        if (timer < 0 || !g_timestamps || !g_recording) return;
        Slot& s = *g_current;
        device::Ctx().fn.vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.queries, 3 + 2 * uint32_t(timer));
    }

    void HoldResource(IUnknown* resource)
    {
        if (!resource) return;
        resource->AddRef();
        Deferred d{};
        d.kind = Deferred::Resource;
        d.resource = resource;
        Defer(d);
    }

    void DeferImageView(VkImageView view)
    {
        if (!view) return;
        Deferred d{};
        d.kind = Deferred::View;
        d.view = view;
        Defer(d);
    }

    void DeferImage(VkImage image, VkImageView view, VkDeviceMemory memory)
    {
        Deferred d{};
        d.kind = Deferred::Image;
        d.image = image;
        d.view = view;
        d.memory = memory;
        Defer(d);
    }

    void DeferPipeline(VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSetLayout setLayout)
    {
        Deferred d{};
        d.kind = Deferred::Pipeline;
        d.pipeline = pipeline;
        d.layout = layout;
        d.setLayout = setLayout;
        Defer(d);
    }

    float PassGpuMs(uint32_t passId)
    {
        for (const PassTime& p : g_passMs)
            if (p.id == passId) return p.ms;
        return -1.0f;
    }

    float BlockGpuMs() { return g_blockMs; }

    bool TimestampsSupported() { return g_timestamps; }

    void GetStats(Stats& out)
    {
        out = Stats{};
        out.uniformCapacity = kUniformBytes;
        out.stagingCapacity = kStagingBytes;
        out.setsCapacity = kMaxSets;
        out.setsPeak = g_setsPeak;
        out.deferred = uint32_t(g_deferred.size());
        for (const Slot& s : g_slots)
        {
            out.uniformPeak = std::max(out.uniformPeak, s.uniform.Peak());
            out.stagingPeak = std::max(out.stagingPeak, s.staging.Peak());
            if (s.pending) ++out.inFlight;
        }
    }
}
