// wxl-graphics-extend: the Vulkan side -- the published table, the scheduler's hooks, the panel.
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

#include "Vulkan.hpp"
#include "Block.hpp"
#include "Caps.hpp"
#include "Device.hpp"
#include "Frame.hpp"
#include "Resources.hpp"
#include "../core/Extension.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

namespace
{
    namespace device = wxl::gfx::vulkan::device;
    namespace frames = wxl::gfx::vulkan::frames;
    namespace block = wxl::gfx::vulkan::block;
    namespace resources = wxl::gfx::vulkan::resources;

    void DrawPanel();   // the panel section's body, below the table

    /**
     * @brief Runs one call so that no exception leaves it (Module.cpp's rule for the C ABI, and the
     *        event path's: the scheduler calls in from an event handler, the panel from the overlay).
     */
    template <class R, class F>
    R Guarded(const char* what, R fallback, F&& call) noexcept
    {
        try
        {
            return call();
        }
        catch (const std::bad_alloc&)
        {
            GFX_LOG_ERROR("vulkan: %s: out of memory", what);
        }
        catch (...)
        {
            GFX_LOG_ERROR("vulkan: %s: unexpected exception", what);
        }
        return fallback;
    }

    template <class F>
    void GuardedVoid(const char* what, F&& call) noexcept
    {
        Guarded(what, 0, [&] { call(); return 0; });
    }

    /// One line, formatted here: no varargs cross the ABI.
    void Textf(const char* fmt, ...)
    {
        char buf[1024];   // the capability lines run to several hundred characters
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, args);
        va_end(args);
        wxl::gfx::g_api->UiText(buf);
    }

    /// WXL_GFX_NEED_* as words, as the scheduler's panel spells them.
    const char* Needs(uint32_t bits, char* buf, size_t cap)
    {
        static const struct { uint32_t bit; const char* word; } kWords[] = {
            { WXL_GFX_NEED_DEPTH, "depth" }, { WXL_GFX_NEED_NORMALS, "normals" },
            { WXL_GFX_NEED_ALBEDO, "albedo" }, { WXL_GFX_NEED_HDR, "hdr" }, { WXL_GFX_NEED_RUN, "run" },
        };
        size_t n = 0;
        buf[0] = '\0';
        for (const auto& w : kWords)
        {
            if (!(bits & w.bit) || n >= cap) continue;
            const int wrote = std::snprintf(buf + n, cap - n, "%s%s", n ? " " : "", w.word);
            if (wrote > 0) n += size_t(wrote);
        }
        if (!buf[0]) std::snprintf(buf, cap, "none");
        return buf;
    }

    void Ms(float ms, char* buf, size_t cap)
    {
        if (ms >= 0.0f) std::snprintf(buf, cap, "%.2f ms", double(ms));
        else            std::snprintf(buf, cap, "-");
    }

    // --- the C ABI: every entry __cdecl and exception-guarded --------------------------------------

    int __cdecl ApiAvailable(void) { return device::Available() ? 1 : 0; }

    int __cdecl ApiGetContext(WXL_GfxVkContext* out)
    {
        if (!out || !device::Available()) return 0;
        const device::Context& c = device::Ctx();
        WXL_GfxVkContext full{};
        full.generation          = c.generation;
        full.instance            = c.instance;
        full.physicalDevice      = c.physical;
        full.device              = c.device;
        full.queue               = c.queue;
        full.queueFamily         = c.queueFamily;
        full.queueIndex          = c.queueIndex;
        full.apiVersion          = c.properties.apiVersion;
        full.getInstanceProcAddr = c.getInstanceProcAddr;
        full.getDeviceProcAddr   = c.getDeviceProcAddr;
        full.limits              = c.properties.limits;
        // The caller's struct may be an older, shorter version: only what fits is written.
        const uint32_t asked = out->structSize;
        if (asked != 0 && asked < sizeof(uint32_t)) return 0;
        const uint32_t n = (asked == 0 || asked > sizeof full) ? uint32_t(sizeof full) : asked;
        full.structSize = n;
        std::memcpy(out, &full, n);
        return 1;
    }

    const char* __cdecl ApiStatus(void)
    {
        static char line[256];
        switch (device::Current())
        {
        case device::State::None:     std::snprintf(line, sizeof line, "Vulkan: not probed yet (no world pass seen)"); break;
        case device::State::Disabled: std::snprintf(line, sizeof line, "Vulkan: disabled (WXL_GFX_VULKAN=0)"); break;
        case device::State::Native:   std::snprintf(line, sizeof line, "Vulkan: native D3D9, no DXVK interop: compute passes inert"); break;
        case device::State::Unusable: std::snprintf(line, sizeof line, "Vulkan: DXVK found but unusable: %s", device::Reason()); break;
        default:
        {
            const device::Context& c = device::Ctx();
            char ms[24], usable[16], dev[24];
            Ms(frames::BlockGpuMs(), ms, sizeof ms);
            std::snprintf(line, sizeof line, "DXVK %u.x: %s | Vulkan %s (device %s) | queue family %u | block %s", c.caps.dxvkGeneration,
                          c.properties.deviceName, wxl::gfx::vulkan::caps::Version(c.caps.apiVersion, false, usable, sizeof usable),
                          wxl::gfx::vulkan::caps::Version(c.caps.deviceApiVersion, true, dev, sizeof dev), c.queueFamily, ms);
            break;
        }
        }
        return line;
    }

    uint32_t __cdecl ApiAddComputePass(const WXL_GfxVkPassDesc* desc)
    {
        return Guarded("AddComputePass", 0u, [&] { return block::AddComputePass(desc); });
    }

    float __cdecl ApiComputePassGpuMs(uint32_t passId)
    {
        return passId ? frames::PassGpuMs(passId) : -1.0f;
    }

    int __cdecl ApiImportTexture(void* d3dResource, WXL_GfxVkImage* out)
    {
        return Guarded("ImportTexture", 0, [&] { return block::ImportTexture(d3dResource, out); });
    }

    int __cdecl ApiCopyToTexture(const WXL_GfxVkImage* source, void* d3dTexture)
    {
        return Guarded("CopyToTexture", 0, [&] { return block::CopyToTexture(source, d3dTexture); });
    }

    void __cdecl ApiCmdBarrier(VkCommandBuffer cmd)
    {
        GuardedVoid("CmdBarrier", [&] { block::CmdBarrier(cmd); });
    }

    int __cdecl ApiAllocUniform(VkDeviceSize size, WXL_GfxVkAlloc* out)
    {
        if (!out) return 0;
        *out = WXL_GfxVkAlloc{};
        return Guarded("AllocUniform", 0, [&] { return frames::AllocUniform(size, *out) ? 1 : 0; });
    }

    int __cdecl ApiAllocStaging(VkDeviceSize size, WXL_GfxVkAlloc* out)
    {
        if (!out) return 0;
        *out = WXL_GfxVkAlloc{};
        return Guarded("AllocStaging", 0, [&] { return frames::AllocStaging(size, *out) ? 1 : 0; });
    }

    int __cdecl ApiUploadImage(const WXL_GfxVkImage* image, const void* data, size_t bytes)
    {
        return Guarded("UploadImage", 0, [&] { return resources::UploadImage(image, data, bytes); });
    }

    VkDescriptorSet __cdecl ApiAllocDescriptorSet(VkDescriptorSetLayout layout)
    {
        return Guarded("AllocDescriptorSet", VkDescriptorSet(VK_NULL_HANDLE), [&] { return frames::AllocDescriptorSet(layout); });
    }

    int __cdecl ApiCreateImage(const WXL_GfxVkImageDesc* desc, WXL_GfxVkImage* out)
    {
        return Guarded("CreateImage", 0, [&] { return resources::CreateImage(desc, out); });
    }

    void __cdecl ApiDestroyImage(const WXL_GfxVkImage* image)
    {
        GuardedVoid("DestroyImage", [&] { resources::DestroyImage(image); });
    }

    int __cdecl ApiCreateComputePipeline(const WXL_GfxVkPipelineDesc* desc, WXL_GfxVkPipeline* out)
    {
        return Guarded("CreateComputePipeline", 0, [&] { return resources::CreateComputePipeline(desc, out); });
    }

    void __cdecl ApiDestroyPipeline(const WXL_GfxVkPipeline* pipeline)
    {
        GuardedVoid("DestroyPipeline", [&] { resources::DestroyPipeline(pipeline); });
    }

    VkSampler __cdecl ApiSampler(uint32_t which)
    {
        return Guarded("Sampler", VkSampler(VK_NULL_HANDLE), [&] { return resources::Sampler(which); });
    }

    int __cdecl ApiSharedTexture(uint32_t which, WXL_GfxVkImage* out)
    {
        return Guarded("SharedTexture", 0, [&] { return resources::SharedTexture(which, out); });
    }

    int __cdecl ApiGetCaps(WXL_GfxVkCaps* out)
    {
        if (!out || !device::Available()) return 0;
        // The caller's struct may be an older, shorter version: only what fits is written.
        const uint32_t asked = out->structSize;
        if (asked != 0 && asked < sizeof(uint32_t)) return 0;
        WXL_GfxVkCaps full = device::Ctx().caps;
        const uint32_t n = (asked == 0 || asked > sizeof full) ? uint32_t(sizeof full) : asked;
        full.structSize = n;
        std::memcpy(out, &full, n);
        return 1;
    }

    VkFormatFeatureFlags2 __cdecl ApiFormatFeatures(VkFormat format)
    {
        return Guarded("FormatFeatures", VkFormatFeatureFlags2(0), [&] { return wxl::gfx::vulkan::caps::FormatFeatures(format); });
    }

    const WXL_GfxVulkanApi kTable = {
        .structSize            = sizeof(WXL_GfxVulkanApi),
        .apiVersion            = WXL_GRAPHICS_VULKAN_API_VERSION,
        .Available             = &ApiAvailable,
        .GetContext            = &ApiGetContext,
        .Status                = &ApiStatus,
        .AddComputePass        = &ApiAddComputePass,
        .ComputePassGpuMs      = &ApiComputePassGpuMs,
        .ImportTexture         = &ApiImportTexture,
        .CopyToTexture         = &ApiCopyToTexture,
        .CmdBarrier            = &ApiCmdBarrier,
        .AllocUniform          = &ApiAllocUniform,
        .AllocStaging          = &ApiAllocStaging,
        .UploadImage           = &ApiUploadImage,
        .AllocDescriptorSet    = &ApiAllocDescriptorSet,
        .CreateImage           = &ApiCreateImage,
        .DestroyImage          = &ApiDestroyImage,
        .CreateComputePipeline = &ApiCreateComputePipeline,
        .DestroyPipeline       = &ApiDestroyPipeline,
        .Sampler               = &ApiSampler,
        .SharedTexture         = &ApiSharedTexture,
        .GetCaps               = &ApiGetCaps,
        .FormatFeatures        = &ApiFormatFeatures,
    };
}

namespace wxl::gfx::vulkan
{
    void Install()
    {
        const bool enabled = ConfigBool("WXL_GFX_VULKAN", true);
        device::SetEnabled(enabled);
        GFX_LOG_INFO("vulkan: %s; probing the device for DXVK's interop on the first world pass", enabled ? "enabled" : "disabled (WXL_GFX_VULKAN=0)");
    }

    const WXL_GfxVulkanApi* Table() { return &kTable; }

    void OnDevice(void* d3dDevice)
    {
        GuardedVoid("OnDevice", [&] { device::OnDevice(d3dDevice); });
    }

    uint32_t PollWants()
    {
        return Guarded("PollWants", 0u, [&] { return block::PollWants(); });
    }

    void RunComputeBlock(const WXL_GfxFrame& frame)
    {
        GuardedVoid("RunComputeBlock", [&] { block::Run(frame); });
    }

    void OnDeviceLost()
    {
        // DXVK keeps the VkDevice through a D3D9 reset; what goes is every DEFAULT-pool texture the
        // blocks in flight imported, so those blocks are waited for (and their references released)
        // before the engine releases them. Imports are per block: nothing else refers to D3D9.
        GuardedVoid("OnDeviceLost", [&] { frames::WaitAll(); });
    }

    void PanelSection()
    {
        GuardedVoid("PanelSection", [] { DrawPanel(); });
    }
}

namespace
{
    void DrawPanel()
    {
        const WXL_Api* api = wxl::gfx::g_api;
        switch (device::Current())
        {
        case device::State::None:     api->UiText("Not probed yet (enter the world)"); return;
        case device::State::Disabled: api->UiText("Disabled (WXL_GFX_VULKAN=0)"); return;
        case device::State::Native:   api->UiText("Native D3D9: no DXVK interop, compute passes inert"); return;
        case device::State::Unusable: Textf("DXVK found but unusable: %s", device::Reason()); return;
        default: break;
        }

        const device::Context& c = device::Ctx();
        const WXL_GfxVkCaps& k = c.caps;
        char usable[16], dev[24], words[640];
        Textf("DXVK %u.x: %s | Vulkan %s usable (device %s) | SPIR-V %u.%u | driver %s %s | queue family %u (index %u) | generation %u",
              k.dxvkGeneration, c.properties.deviceName, wxl::gfx::vulkan::caps::Version(k.apiVersion, false, usable, sizeof usable),
              wxl::gfx::vulkan::caps::Version(k.deviceApiVersion, true, dev, sizeof dev), (k.spirvVersion >> 16) & 0xFF,
              (k.spirvVersion >> 8) & 0xFF, k.driverName, k.driverInfo, c.queueFamily, c.queueIndex, c.generation);
        Textf("Subgroups: %u lanes (%u..%u) | %s", k.subgroupSize, k.minSubgroupSize, k.maxSubgroupSize,
              wxl::gfx::vulkan::caps::SubgroupNames(k.subgroupOperations, words, sizeof words));
        Textf("Enabled: %s", wxl::gfx::vulkan::caps::CapNames(k.enabled, words, sizeof words));
        Textf("Hardware only (not enabled by DXVK): %s", wxl::gfx::vulkan::caps::CapNames(k.supported & ~k.enabled, words, sizeof words));

        frames::Stats fs{};
        frames::GetStats(fs);
        char ms[24];
        Ms(frames::BlockGpuMs(), ms, sizeof ms);
        Textf("Block GPU: %s | timestamps %s | last frame: %s, %u imports | %u in flight", ms,
              frames::TimestampsSupported() ? "on" : "off (unsupported, or WXL_GFX_PROFILE=0)",
              block::LastBlockRan() ? "block submitted" : "no block", block::LastImports(), fs.inFlight);

        const size_t passCount = block::PassCount();
        if (!passCount) api->UiText("Compute passes: none registered.");
        for (size_t i = 0; i < passCount; ++i)
        {
            block::PassInfo p{};
            if (!block::GetPassInfo(i, p)) break;
            char wants[48], gpu[24];
            Ms(p.gpuMs, gpu, sizeof gpu);
            Textf("[%d] %s | wants %s | %s | %s", p.order, p.name ? p.name : "?", Needs(p.lastWants, wants, sizeof wants),
                  p.ran ? "ran" : "idle", gpu);
        }

        uint32_t images = 0, pipelines = 0;
        uint64_t imageBytes = 0;
        resources::Stats(images, imageBytes, pipelines);
        Textf("Service images: %u (%.1f MB) | pipelines: %u | objects awaiting the GPU: %u", images,
              double(imageBytes) / (1024.0 * 1024.0), pipelines, fs.deferred);
        Textf("Rings: uniform peak %u KB of %u MB | staging peak %u KB of %u MB | descriptor sets peak %u of %u",
              unsigned(fs.uniformPeak >> 10), unsigned(fs.uniformCapacity >> 20), unsigned(fs.stagingPeak >> 10),
              unsigned(fs.stagingCapacity >> 20), fs.setsPeak, fs.setsCapacity);
    }
}
