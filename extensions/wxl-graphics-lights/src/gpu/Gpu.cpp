// wxl-graphics-lights: the device functions, the images, the pipelines, one dispatch, and the per-span GPU
// timers (Vulkan timestamps, read a slot's worth of frames late, never waited for).
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
#include "Shaders.hpp"
#include "../core/Extension.hpp"

#include <algorithm>
#include <vector>

namespace
{
    namespace gl = wxl::gfx::lights;
    using namespace wxl::gfx::lights::gpu;

    constexpr uint32_t kShadowMax = 16;   // wxl-graphics-shadow bindings a pipeline takes at most

    Device            g_dev;
    bool              g_loadWarned = false;
    FrameBindings     g_frame;
    WXL_GfxVkImage    g_neutral2D{}, g_neutral3D{};
    bool              g_neutralFailed = false;
    bool              g_createWarned = false;
    bool              g_setWarned = false;
    std::vector<WXL_GfxVkImage*> g_fresh;   // made since the last block: cleared before use

    struct Pipe
    {
        WXL_GfxVkPipeline p{};
        uint32_t generation = 0;   // the device it was made on (0: not tried)
        uint32_t shadowCount = 0;  // wxl-graphics-shadow bindings in its layout
        bool     failed = false;
    };
    Pipe g_pipes[LIGHTS_PIPE_COUNT];

    constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    void ForgetResources()
    {
        // The device took its objects: only the handles are dropped.
        g_neutral2D = WXL_GfxVkImage{};
        g_neutral3D = WXL_GfxVkImage{};
        for (Pipe& p : g_pipes) p = Pipe{};
        g_neutralFailed = false;
        g_createWarned = false;
        g_fresh.clear();
    }

    // --- timers --------------------------------------------------------------------------------------

    constexpr uint32_t kSlots  = WXL_GFX_VK_FRAMES_IN_FLIGHT;
    constexpr uint32_t kStamps = 16;

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
    float    g_spanMs[kSpanCount] = { -1.0f, -1.0f, -1.0f, -1.0f };
    float    g_totalMs = -1.0f;

    bool CreateStamps()
    {
        if (g_stampsTried) return g_stampsOk;
        g_stampsTried = true;
        const Device& d = g_dev;
        if (!d.vkCreateQueryPool || !d.vkCmdResetQueryPool || !d.vkCmdWriteTimestamp || !d.vkGetQueryPoolResults
            || !d.vkGetPhysicalDeviceQueueFamilyProperties || !(d.timestampPeriod > 0.0f))
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
        if (g_dev.vkGetQueryPoolResults(g_dev.device, s.pool, 0, s.count, sizeof(uint64_t) * s.count, stamps, sizeof(uint64_t),
                                        VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            return;
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

    /// The bindings of the base layout (shaders/compute/shared.h).
    uint32_t BaseLayout(WXL_GfxVkBinding* out)
    {
        for (uint32_t b = 0; b < LIGHTS_B_COUNT; ++b)
        {
            out[b].binding = b;
            out[b].count = 1;
            out[b].type = b == LIGHTS_B_CONSTANTS ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                        : b < LIGHTS_B_SAMP_POINT_CLAMP ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                        : b < LIGHTS_B_OUT0 ? VK_DESCRIPTOR_TYPE_SAMPLER
                        : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        return LIGHTS_B_COUNT;
    }

    void MakePipeline(const WXL_GfxVulkanApi* api, uint32_t pipe)
    {
        Pipe& p = g_pipes[pipe];
        const gl::shaders::Module& m = gl::shaders::Get(pipe);
        p.generation = g_dev.generation;
        p.failed = true;
        if (!m.words) return;   // not compiled into this build
        WXL_GfxVkBinding layout[LIGHTS_B_COUNT + kShadowMax] = {};
        uint32_t count = BaseLayout(layout);
        if (pipe == LIGHTS_PIPE_FIELD_SHADOW)
        {
            const WXL_GraphicsShadowApi* shadow = gl::Shadow();
            if (!shadow) return;
            const uint32_t n = shadow->BindingCount();
            if (n == 0 || n > kShadowMax || shadow->DescribeBindings(LIGHTS_B_COUNT, layout + count, kShadowMax) != n) return;
            count += n;
            p.shadowCount = n;
        }
        WXL_GfxVkPipelineDesc d{};
        d.structSize = sizeof d;
        d.name = m.name;
        d.spirv = m.words;
        d.spirvBytes = m.bytes;
        d.bindings = layout;
        d.bindingCount = count;
        d.pushConstantBytes = LIGHTS_PUSH_BYTES;
        if (!api->CreateComputePipeline(&d, &p.p) || p.p.pipeline == VK_NULL_HANDLE)
        {
            LIGHTS_LOG_WARN("gpu: pipeline %s refused (see the graphics-extend log)", m.name);
            p.p = WXL_GfxVkPipeline{};
            return;
        }
        p.failed = false;
        LIGHTS_LOG_INFO("gpu: pipeline %s ready (%u bindings)", m.name, count);
    }
}

namespace wxl::gfx::lights::gpu
{
    Device& Dev() { return g_dev; }
    FrameBindings& Frame() { return g_frame; }

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
        ForgetResources();
        ForgetTimers();
        if (!g_dev.LoadAll(ctx))
        {
            if (!g_loadWarned)
            {
                g_loadWarned = true;
                LIGHTS_LOG_WARN("gpu: the Vulkan device functions could not be loaded; the surfaces and the field cannot run");
            }
            return false;
        }
        g_loadWarned = false;
        LIGHTS_LOG_INFO("gpu: Vulkan device generation %u", ctx.generation);
        return true;
    }

    bool MakeImage(const WXL_GfxVulkanApi* api, WXL_GfxVkImage& out, const char* name, VkImageType type, VkFormat format,
                   uint32_t w, uint32_t h, uint32_t d)
    {
        if (out.image != VK_NULL_HANDLE) return true;
        WXL_GfxVkImageDesc desc{};
        desc.structSize = sizeof desc;
        desc.name = name;
        desc.type = type;
        desc.format = format;
        desc.width = std::max(w, 1u);
        desc.height = std::max(h, 1u);
        desc.depth = std::max(d, 1u);
        desc.mipLevels = 1;
        desc.usage = kUsage;
        out = WXL_GfxVkImage{};
        if (!api->CreateImage(&desc, &out) || out.image == VK_NULL_HANDLE)
        {
            out = WXL_GfxVkImage{};
            if (!g_createWarned)
            {
                g_createWarned = true;
                LIGHTS_LOG_WARN("gpu: image %s (%ux%ux%u, format %d) refused (logged once)", name, desc.width, desc.height,
                                desc.depth, int(format));
            }
            return false;
        }
        g_fresh.push_back(&out);
        return true;
    }

    void DropImage(const WXL_GfxVulkanApi* api, WXL_GfxVkImage& img)
    {
        if (img.image != VK_NULL_HANDLE)
        {
            g_fresh.erase(std::remove(g_fresh.begin(), g_fresh.end(), &img), g_fresh.end());
            api->DestroyImage(&img);
        }
        img = WXL_GfxVkImage{};
    }

    bool EnsureNeutral(const WXL_GfxVulkanApi* api)
    {
        if (g_neutral2D.image && g_neutral3D.image) return true;
        if (g_neutralFailed) return false;
        const bool ok = MakeImage(api, g_neutral2D, "lights.neutral2d", VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1, 1)
                     && MakeImage(api, g_neutral3D, "lights.neutral3d", VK_IMAGE_TYPE_3D, VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1, 1);
        if (!ok)
        {
            g_neutralFailed = true;
            LIGHTS_LOG_WARN("gpu: the stand-in images were refused; the compute passes stay inert on this device");
        }
        return ok;
    }

    const WXL_GfxVkImage& Neutral2D() { return g_neutral2D; }
    const WXL_GfxVkImage& Neutral3D() { return g_neutral3D; }

    void ClearFresh(const WXL_GfxVulkanApi* api, VkCommandBuffer cmd)
    {
        if (g_fresh.empty()) return;
        const VkClearColorValue zero{};
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = VK_REMAINING_MIP_LEVELS;
        range.layerCount = VK_REMAINING_ARRAY_LAYERS;
        for (WXL_GfxVkImage* i : g_fresh)
            if (i->image != VK_NULL_HANDLE) Dev().vkCmdClearColorImage(cmd, i->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        g_fresh.clear();
        api->CmdBarrier(cmd);
    }

    void EnsurePipelines(const WXL_GfxVulkanApi* api)
    {
        const uint32_t generation = Dev().generation;
        if (gl::shaders::Count() != LIGHTS_PIPE_COUNT)
        {
            static bool said = false;
            if (!said)
            {
                said = true;
                LIGHTS_LOG_ERROR("gpu: the shader table holds %u modules, shared.h lists %u; rebuild the shaders", gl::shaders::Count(),
                                 unsigned(LIGHTS_PIPE_COUNT));
            }
            return;
        }
        for (uint32_t i = 0; i < LIGHTS_PIPE_COUNT; ++i)
        {
            Pipe& p = g_pipes[i];
            // The point-shadow variant follows the shadow service: made once it is there.
            const bool retryShadow = i == LIGHTS_PIPE_FIELD_SHADOW && p.failed && p.generation == generation && gl::Shadow()
                                  && gl::shaders::Get(i).words && p.shadowCount == 0;
            if (p.generation == generation && !retryShadow) continue;
            MakePipeline(api, i);
        }
    }

    bool HasPipeline(uint32_t pipe)
    {
        return pipe < LIGHTS_PIPE_COUNT && g_pipes[pipe].p.pipeline != VK_NULL_HANDLE && g_pipes[pipe].generation == Dev().generation;
    }

    void DropPipelines(const WXL_GfxVulkanApi* api)
    {
        for (Pipe& p : g_pipes)
        {
            if (p.p.pipeline != VK_NULL_HANDLE && api) api->DestroyPipeline(&p.p);
            p = Pipe{};
        }
    }

    bool Dispatch(uint32_t pipe, const Bind& bind, const PushData& push, uint32_t gx, uint32_t gy, uint32_t gz, uint32_t volumes)
    {
        const FrameBindings& f = g_frame;
        if (!HasPipeline(pipe) || !f.cmd) return false;
        if (gx == 0 || gy == 0 || gz == 0) return true;
        const WXL_GfxVkPipeline& p = g_pipes[pipe].p;
        VkDescriptorSet set = f.api->AllocDescriptorSet(p.setLayout);
        if (set == VK_NULL_HANDLE)
        {
            if (!g_setWarned)
            {
                g_setWarned = true;
                LIGHTS_LOG_WARN("gpu: the frame's descriptor pool is exhausted; part of the lighting is skipped this frame");
            }
            return false;
        }
        wxl::gfx::vk::DescriptorWriter<LIGHTS_B_COUNT> w(set);
        w.Uniform(LIGHTS_B_CONSTANTS, f.constants);
        // Every slot gets an image: the shaders declare the ones they read, a stand-in fills the rest.
        for (uint32_t i = 0; i < LIGHTS_TEX_SLOTS; ++i)
        {
            const WXL_GfxVkImage* img = bind.tex[i];
            if ((!img || img->image == VK_NULL_HANDLE) && i == LIGHTS_T_NOISE) img = f.noise;
            if (!img || img->image == VK_NULL_HANDLE) img = (volumes >> i) & 1u ? &g_neutral3D : &g_neutral2D;
            if (!img || img->view == VK_NULL_HANDLE) continue;
            w.Image(LIGHTS_B_TEX0 + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, img->view, img->layout);
        }
        w.SamplerOnly(LIGHTS_B_SAMP_POINT_CLAMP, f.samplers[0]);
        w.SamplerOnly(LIGHTS_B_SAMP_LINEAR_CLAMP, f.samplers[1]);
        for (uint32_t i = 0; i < LIGHTS_OUT_SLOTS; ++i)
        {
            const WXL_GfxVkImage* img = bind.out[i];
            if (!img) img = (volumes >> (16 + i)) & 1u ? &g_neutral3D : &g_neutral2D;
            if (!img || img->view == VK_NULL_HANDLE) continue;
            w.Image(LIGHTS_B_OUT0 + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, img->view, VK_IMAGE_LAYOUT_GENERAL);
        }
        w.Apply(Dev());
        if (pipe == LIGHTS_PIPE_FIELD_SHADOW)
        {
            const WXL_GraphicsShadowApi* shadow = gl::Shadow();
            if (!shadow || !shadow->WriteBindings(set, LIGHTS_B_COUNT)) return false;
        }

        const Device& d = Dev();
        d.vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        d.vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
        d.vkCmdPushConstants(f.cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, LIGHTS_PUSH_BYTES, &push);
        d.vkCmdDispatch(f.cmd, gx, gy, gz);
        f.api->CmdBarrier(f.cmd);
        return true;
    }

    // --- timers --------------------------------------------------------------------------------------

    const char* const kSpanNames[kSpanCount] = { "field", "halo", "surfaces", "copy" };

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

    float SpanMs(int span)
    {
        if (span < 0) return g_totalMs;
        return span < kSpanCount ? g_spanMs[span] : -1.0f;
    }

    bool TimersSupported() { return g_stampsOk; }
}
