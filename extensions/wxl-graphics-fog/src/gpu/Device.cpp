// wxl-graphics-fog: the device functions, the per-pass GPU timers (Vulkan timestamps, read a slot's
// worth of frames late, never waited for) and the camera probe's read-back buffer.
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

#include "Gpu.hpp"

#include <algorithm>
#include <cstring>

namespace wxl::gfx::fog::gpu
{
    void ForgetResources();   // Resources.cpp
}

namespace
{
    using namespace wxl::gfx::fog;
    using namespace wxl::gfx::fog::gpu;

    Device g_dev;
    bool   g_loadWarned = false;

    // --- timers ---------------------------------------------------------------------------------------------

    constexpr uint32_t kSlots  = WXL_GFX_VK_FRAMES_IN_FLIGHT;
    constexpr uint32_t kStamps = 32;

    struct StampSet
    {
        VkQueryPool pool = VK_NULL_HANDLE;
        int         marked[kStamps] = {};
        uint32_t    count = 0;
        bool        written = false;
    };
    StampSet g_stamps[kSlots];
    bool     g_stampsTried = false, g_stampsOk = false;
    uint64_t g_stampMask = ~0ull;
    int      g_open = -1;
    float    g_spanMs[kSpanCount];
    float    g_vulkanMs = -1.0f;
    bool     g_spansInit = false;

    void InitSpans()
    {
        if (g_spansInit) return;
        g_spansInit = true;
        for (float& v : g_spanMs) v = -1.0f;
    }

    bool CreateStamps()
    {
        if (g_stampsTried) return g_stampsOk;
        g_stampsTried = true;
        const Device& d = g_dev;
        if (!d.vkCreateQueryPool || !d.vkDestroyQueryPool || !d.vkCmdResetQueryPool || !d.vkCmdWriteTimestamp
            || !d.vkGetQueryPoolResults || !d.vkGetPhysicalDeviceQueueFamilyProperties || !(d.timestampPeriod > 0.0f))
            return false;
        uint32_t families = 0;
        d.vkGetPhysicalDeviceQueueFamilyProperties(d.physicalDevice, &families, nullptr);
        if (d.queueFamily >= families || families > 64) return false;
        VkQueueFamilyProperties props[64] = {};
        d.vkGetPhysicalDeviceQueueFamilyProperties(d.physicalDevice, &families, props);
        const uint32_t bits = props[d.queueFamily].timestampValidBits;
        if (bits == 0) return false;
        g_stampMask = bits >= 64 ? ~0ull : ((1ull << bits) - 1ull);
        for (StampSet& s : g_stamps)
        {
            VkQueryPoolCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            ci.queryCount = kStamps;
            if (d.vkCreateQueryPool(d.device, &ci, nullptr, &s.pool) != VK_SUCCESS)
            {
                s.pool = VK_NULL_HANDLE;
                for (StampSet& t : g_stamps)
                    if (t.pool != VK_NULL_HANDLE) { d.vkDestroyQueryPool(d.device, t.pool, nullptr); t.pool = VK_NULL_HANDLE; }
                return false;
            }
        }
        g_stampsOk = true;
        return true;
    }

    void ReadStamps(StampSet& s)
    {
        if (!s.written) return;
        s.written = false;
        uint64_t stamps[kStamps] = {};
        const VkResult r = g_dev.vkGetQueryPoolResults(g_dev.device, s.pool, 0, s.count, sizeof(uint64_t) * s.count, stamps,
                                                       sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (r != VK_SUCCESS) return;
        float frame[kSpanCount] = {};
        for (uint32_t i = 1; i < s.count; ++i)
        {
            if (s.marked[i] < 0 || s.marked[i] >= kSpanCount) continue;
            const uint64_t ticks = (stamps[i] - stamps[i - 1]) & g_stampMask;
            frame[s.marked[i]] += float(double(ticks) * double(g_dev.timestampPeriod) * 1e-6);
        }
        float total = 0.0f;
        for (int i = 0; i < kSpanCount; ++i)
        {
            if (i == kSpanApply) continue;
            g_spanMs[i] = g_spanMs[i] < 0.0f ? frame[i] : g_spanMs[i] * 0.92f + frame[i] * 0.08f;
            total += g_spanMs[i];
        }
        g_vulkanMs = total;
    }

    // --- probe ---------------------------------------------------------------------------------------------

    constexpr VkDeviceSize kProbeBytes = 32;
    VkBuffer       g_probeBuffer = VK_NULL_HANDLE;
    VkDeviceMemory g_probeMemory = VK_NULL_HANDLE;
    void*          g_probeMapped = nullptr;
    bool           g_probeCoherent = true;
    bool           g_probeTried = false;
    bool           g_probeWritten[kSlots] = {};
    ProbeResult    g_probe;

    bool CreateProbe()
    {
        if (g_probeTried) return g_probeBuffer != VK_NULL_HANDLE;
        g_probeTried = true;
        const Device& d = g_dev;
        if (!d.vkGetPhysicalDeviceMemoryProperties) return false;
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = kProbeBytes * kSlots;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (d.vkCreateBuffer(d.device, &bi, nullptr, &g_probeBuffer) != VK_SUCCESS)
        {
            g_probeBuffer = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryRequirements req{};
        d.vkGetBufferMemoryRequirements(d.device, g_probeBuffer, &req);
        VkPhysicalDeviceMemoryProperties props{};
        d.vkGetPhysicalDeviceMemoryProperties(d.physicalDevice, &props);
        const VkMemoryPropertyFlags wants[2] = {
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        };
        uint32_t type = ~0u;
        VkMemoryPropertyFlags got = 0;
        for (const VkMemoryPropertyFlags want : wants)
        {
            if (want == VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT && !d.vkInvalidateMappedMemoryRanges) continue;
            for (uint32_t i = 0; i < props.memoryTypeCount && type == ~0u; ++i)
                if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
                {
                    type = i;
                    got = props.memoryTypes[i].propertyFlags;
                }
            if (type != ~0u) break;
        }
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (type == ~0u || d.vkAllocateMemory(d.device, &ai, nullptr, &g_probeMemory) != VK_SUCCESS
            || d.vkBindBufferMemory(d.device, g_probeBuffer, g_probeMemory, 0) != VK_SUCCESS
            || d.vkMapMemory(d.device, g_probeMemory, 0, VK_WHOLE_SIZE, 0, &g_probeMapped) != VK_SUCCESS)
        {
            if (g_probeMemory != VK_NULL_HANDLE) d.vkFreeMemory(d.device, g_probeMemory, nullptr);
            d.vkDestroyBuffer(d.device, g_probeBuffer, nullptr);
            g_probeBuffer = VK_NULL_HANDLE;
            g_probeMemory = VK_NULL_HANDLE;
            g_probeMapped = nullptr;
            return false;
        }
        g_probeCoherent = (got & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        std::memset(g_probeMapped, 0, size_t(kProbeBytes * kSlots));
        return true;
    }
}

namespace wxl::gfx::fog::gpu
{
    Device& Dev() { return g_dev; }

    bool Device::LoadAll(const WXL_GfxVkContext& ctx)
    {
        if (!Load(ctx)) return false;
        physicalDevice = ctx.physicalDevice;
        queueFamily = ctx.queueFamily;
        timestampPeriod = ctx.limits.timestampPeriod;
        auto dev = [&ctx](const char* name) { return ctx.getDeviceProcAddr(ctx.device, name); };
        auto inst = [&ctx](const char* name) -> PFN_vkVoidFunction {
            return ctx.getInstanceProcAddr && ctx.instance ? ctx.getInstanceProcAddr(ctx.instance, name) : nullptr;
        };
        vkCreateQueryPool = reinterpret_cast<PFN_vkCreateQueryPool>(dev("vkCreateQueryPool"));
        vkDestroyQueryPool = reinterpret_cast<PFN_vkDestroyQueryPool>(dev("vkDestroyQueryPool"));
        vkCmdResetQueryPool = reinterpret_cast<PFN_vkCmdResetQueryPool>(dev("vkCmdResetQueryPool"));
        vkCmdWriteTimestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp>(dev("vkCmdWriteTimestamp"));
        vkGetQueryPoolResults = reinterpret_cast<PFN_vkGetQueryPoolResults>(dev("vkGetQueryPoolResults"));
        vkInvalidateMappedMemoryRanges = reinterpret_cast<PFN_vkInvalidateMappedMemoryRanges>(dev("vkInvalidateMappedMemoryRanges"));
        vkGetPhysicalDeviceMemoryProperties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(inst("vkGetPhysicalDeviceMemoryProperties"));
        vkGetPhysicalDeviceQueueFamilyProperties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(inst("vkGetPhysicalDeviceQueueFamilyProperties"));
        return true;
    }

    bool FollowDevice(const WXL_GfxVulkanApi* api)
    {
        WXL_GfxVkContext ctx{};
        ctx.structSize = sizeof ctx;
        if (!api || !api->GetContext(&ctx) || ctx.generation == 0) return false;
        if (ctx.generation == g_dev.generation) return true;
        // A new device: every handle of the old one is gone.
        ForgetResources();
        ForgetTimers();
        ForgetProbe();
        if (!g_dev.LoadAll(ctx))
        {
            if (!g_loadWarned)
            {
                g_loadWarned = true;
                FOG_LOG_WARN("gpu: the Vulkan device functions could not be loaded; the fog cannot run");
            }
            return false;
        }
        g_loadWarned = false;
        FOG_LOG_INFO("gpu: Vulkan device generation %u", ctx.generation);
        return true;
    }

    // --- timers ---------------------------------------------------------------------------------------------

    const char* const kSpanNames[kSpanCount] = {
        "terrain", "rivers", "wakes", "L0", "L1", "L2", "L3", "light", "lamps", "near", "far", "temporal", "copy", "apply",
    };

    void ForgetTimers()
    {
        for (StampSet& s : g_stamps) s = StampSet{};
        g_stampsTried = false;
        g_stampsOk = false;
        g_open = -1;
    }

    bool BeginTimers(VkCommandBuffer cmd, uint32_t slot)
    {
        InitSpans();
        g_open = -1;
        if (!CreateStamps() || slot >= kSlots) return false;
        StampSet& s = g_stamps[slot];
        ReadStamps(s);
        g_dev.vkCmdResetQueryPool(cmd, s.pool, 0, kStamps);
        g_dev.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.pool, 0);
        s.marked[0] = -1;
        s.count = 1;
        g_open = int(slot);
        return true;
    }

    void MarkTimer(VkCommandBuffer cmd, Span span)
    {
        if (g_open < 0) return;
        StampSet& s = g_stamps[g_open];
        if (s.count >= kStamps) return;
        g_dev.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.pool, s.count);
        s.marked[s.count] = int(span);
        ++s.count;
    }

    void EndTimers()
    {
        if (g_open < 0) return;
        g_stamps[g_open].written = true;
        g_open = -1;
    }

    float SpanMs(int span)
    {
        InitSpans();
        if (span < 0) return g_vulkanMs;
        return span < kSpanCount ? g_spanMs[span] : -1.0f;
    }

    void SetApplyMs(float ms)
    {
        InitSpans();
        g_spanMs[kSpanApply] = ms;
    }

    bool TimersSupported() { return g_stampsOk; }

    // --- probe ---------------------------------------------------------------------------------------------

    void ForgetProbe()
    {
        g_probeBuffer = VK_NULL_HANDLE;
        g_probeMemory = VK_NULL_HANDLE;
        g_probeMapped = nullptr;
        g_probeTried = false;
        for (bool& w : g_probeWritten) w = false;
        g_probe = ProbeResult{};
    }

    VkBuffer ProbeBuffer(uint32_t slot, VkDeviceSize& offset)
    {
        offset = 0;
        if (slot >= kSlots || !CreateProbe()) return VK_NULL_HANDLE;
        offset = kProbeBytes * slot;
        g_probeWritten[slot] = true;
        return g_probeBuffer;
    }

    void PollProbe(uint32_t slot)
    {
        if (slot >= kSlots || !g_probeWritten[slot] || !g_probeMapped) return;
        g_probeWritten[slot] = false;
        if (!g_probeCoherent && g_dev.vkInvalidateMappedMemoryRanges)
        {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = g_probeMemory;
            range.offset = 0;
            range.size = VK_WHOLE_SIZE;
            g_dev.vkInvalidateMappedMemoryRanges(g_dev.device, 1, &range);
        }
        float v[8];
        std::memcpy(v, static_cast<const uint8_t*>(g_probeMapped) + kProbeBytes * slot, sizeof v);
        if (v[7] < 0.5f) return;
        ProbeResult& p = g_probe;
        p.valid = true;
        p.extinction = v[0];
        p.fog = v[1];
        p.indoor = v[2];
        p.smoke = v[3];
        p.visibility = v[4];
        p.sun = v[5];
        p.lamps = v[6];
    }

    const ProbeResult& Probe() { return g_probe; }
}
