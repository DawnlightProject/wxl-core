// wxl-graphics-shadow: the device functions and the per-pass GPU timers (Vulkan timestamps, read a slot's
// worth of frames late, never waited for).
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
#include "../core/Extension.hpp"

#include <cstring>

namespace
{
    using namespace wxl::gfx::shadow::gpu;

    Device g_dev;
    bool   g_loadWarned = false;

    constexpr uint32_t kSlots  = WXL_GFX_VK_FRAMES_IN_FLIGHT;
    constexpr uint32_t kStamps = 24;

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
    float    g_spanMs[kSpanCount] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
    float    g_totalMs = -1.0f;

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
            g_spanMs[i] = g_spanMs[i] < 0.0f ? frame[i] : g_spanMs[i] * 0.92f + frame[i] * 0.08f;
            total += g_spanMs[i];
        }
        g_totalMs = total;
    }
}

namespace wxl::gfx::shadow::gpu
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
        if (!g_dev.LoadAll(ctx))
        {
            if (!g_loadWarned)
            {
                g_loadWarned = true;
                SHADOW_LOG_WARN("gpu: the Vulkan device functions could not be loaded; shadows cannot run");
            }
            return false;
        }
        g_loadWarned = false;
        SHADOW_LOG_INFO("gpu: Vulkan device generation %u", ctx.generation);
        return true;
    }

    const char* const kSpanNames[kSpanCount] = { "uploads", "convert", "mips", "mask", "upsample", "debug" };

    void ForgetTimers()
    {
        for (StampSet& s : g_stamps) s = StampSet{};
        g_stampsTried = false;
        g_stampsOk = false;
        g_open = -1;
    }

    bool BeginTimers(VkCommandBuffer cmd, uint32_t slot)
    {
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

    float SpanMs(int span) { return span >= 0 && span < kSpanCount ? g_spanMs[span] : -1.0f; }
    bool  TimersSupported() { return g_stampsOk; }
    float TotalMs() { return g_totalMs; }
}
