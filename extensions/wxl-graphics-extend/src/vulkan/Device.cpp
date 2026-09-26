// wxl-graphics-extend: the Vulkan device behind DXVK's D3D9 device -- detection, handles, functions.
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

#include "Device.hpp"
#include "Caps.hpp"
#include "Frame.hpp"
#include "Resources.hpp"
#include "../core/Extension.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    namespace device = wxl::gfx::vulkan::device;
    namespace frames = wxl::gfx::vulkan::frames;
    namespace resources = wxl::gfx::vulkan::resources;

    bool            g_enabled = true;
    void*           g_lastDevice = nullptr;   // the pointer last probed, whatever it turned out to be
    device::State   g_state = device::State::None;
    device::Context g_ctx;
    uint32_t        g_generation = 0;
    char            g_reason[96] = "";

    /**
     * @brief vkGetInstanceProcAddr from the loader DXVK is using.
     *
     * On Windows that is vulkan-1.dll; under Wine, DXVK may talk to winevulkan.dll directly, in which
     * case vulkan-1.dll (the same dispatch, one layer up) is loaded here as a last resort. Nothing is
     * linked: a client without any Vulkan loader never gets here, since the interop answered first.
     */
    PFN_vkGetInstanceProcAddr LoaderEntry(HMODULE module)
    {
        // Through void*: a FARPROC is not the entry's type, and a direct cast between the two is
        // what -Wcast-function-type rightly flags.
        void* raw = reinterpret_cast<void*>(GetProcAddress(module, "vkGetInstanceProcAddr"));
        return reinterpret_cast<PFN_vkGetInstanceProcAddr>(raw);
    }

    PFN_vkGetInstanceProcAddr FindLoader(const char*& source)
    {
        static const char* const kNames[] = { "vulkan-1.dll", "winevulkan.dll" };
        for (const char* name : kNames)
        {
            if (HMODULE h = GetModuleHandleA(name))
            {
                if (PFN_vkGetInstanceProcAddr fn = LoaderEntry(h))
                {
                    source = name;
                    return fn;
                }
            }
        }
        if (HMODULE h = LoadLibraryA("vulkan-1.dll"))
        {
            if (PFN_vkGetInstanceProcAddr fn = LoaderEntry(h))
            {
                source = "vulkan-1.dll (loaded)";
                return fn;
            }
        }
        source = "none";
        return nullptr;
    }

    void SetReason(const char* why)
    {
        std::snprintf(g_reason, sizeof g_reason, "%s", why ? why : "");
    }

    /// Everything on the current device goes, then the device reference: the modules first, while
    /// the interop reference still keeps the D3D9 device -- and with it the VkDevice -- alive.
    void Teardown()
    {
        if (g_ctx.interop)
        {
            const bool clean = frames::Destroy();
            resources::Destroy(clean);
            g_ctx.interop->Release();
        }
        g_ctx = device::Context{};
        g_state = device::State::None;
        g_reason[0] = '\0';
    }

    /// The interop answered but the device cannot be used: no reference is kept, nothing exists.
    void Unusable(ID3D9VkInteropDevice* interop, const char* why)
    {
        GFX_LOG_WARN("vulkan: DXVK interop found but unusable: %s; Vulkan passes inert", why);
        interop->Release();
        g_ctx = device::Context{};
        g_state = device::State::Unusable;
        SetReason(why);
    }

    void Probe(void* d3dDevice)
    {
        auto* unknown = static_cast<IUnknown*>(d3dDevice);
        ID3D9VkInteropDevice* interop = nullptr;
        if (FAILED(unknown->QueryInterface(__uuidof(ID3D9VkInteropDevice), reinterpret_cast<void**>(&interop))) || !interop)
        {
            g_state = device::State::Native;
            GFX_LOG_INFO("vulkan: native D3D9: no DXVK interop, Vulkan passes inert");
            return;
        }

        device::Context c;
        c.interop = interop;
        interop->GetVulkanHandles(&c.instance, &c.physical, &c.device);
        interop->GetSubmissionQueue(&c.queue, &c.queueIndex, &c.queueFamily);
        if (!c.instance || !c.physical || !c.device || !c.queue)
        {
            Unusable(interop, "the interop returned a null Vulkan handle");
            return;
        }

        const char* loader = nullptr;
        c.getInstanceProcAddr = FindLoader(loader);
        if (!c.getInstanceProcAddr)
        {
            Unusable(interop, "no Vulkan loader module (vulkan-1.dll) in the process");
            return;
        }
        c.getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(c.getInstanceProcAddr(c.instance, "vkGetDeviceProcAddr"));
        if (!c.getDeviceProcAddr)
        {
            Unusable(interop, "vkGetDeviceProcAddr not found through the loader");
            return;
        }
        if (const char* missing = wxl::gfx::vulkan::LoadFunctions(c.fn, c.instance, c.device, c.getInstanceProcAddr, c.getDeviceProcAddr))
        {
            char why[96];
            std::snprintf(why, sizeof why, "%s not found on the device", missing);
            Unusable(interop, why);
            return;
        }

        c.fn.vkGetPhysicalDeviceProperties(c.physical, &c.properties);
        c.fn.vkGetPhysicalDeviceMemoryProperties(c.physical, &c.memory);

        uint32_t familyCount = 0;
        c.fn.vkGetPhysicalDeviceQueueFamilyProperties(c.physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        if (familyCount) c.fn.vkGetPhysicalDeviceQueueFamilyProperties(c.physical, &familyCount, families.data());
        c.timestampValidBits = c.queueFamily < familyCount ? families[c.queueFamily].timestampValidBits : 0;
        wxl::gfx::vulkan::caps::Query(c, d3dDevice, c.caps);

        if (++g_generation == 0) ++g_generation;   // never 0: consumers compare against their own
        c.generation = g_generation;

        g_ctx = c;
        g_state = device::State::Dxvk;

        if (!frames::Create())
        {
            // The slots that were made go with the (still alive) device; the interop reference last.
            Teardown();
            g_state = device::State::Unusable;
            SetReason("the frame slots could not be created (see the log)");
            GFX_LOG_WARN("vulkan: DXVK interop found but the frame slots could not be created; Vulkan passes inert");
            return;
        }

        const uint32_t api = c.properties.apiVersion;
        GFX_LOG_INFO("vulkan: DXVK interop: %s, Vulkan %u.%u.%u, driver 0x%08X, queue family %u (index %u), timestamps %s (%.1f ns), loader %s, generation %u",
                     c.properties.deviceName, VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api),
                     c.properties.driverVersion, c.queueFamily, c.queueIndex, c.timestampValidBits ? "yes" : "no",
                     double(c.properties.limits.timestampPeriod), loader, c.generation);
        wxl::gfx::vulkan::caps::Log(g_ctx);
    }
}

namespace wxl::gfx::vulkan::device
{
    void SetEnabled(bool enabled) { g_enabled = enabled; }

    void OnDevice(void* d3dDevice)
    {
        if (d3dDevice == g_lastDevice) return;
        // A new device: the old one, if it was DXVK's, is still alive through our interop reference
        // (the pointer could not have been reused otherwise), so its objects are destroyed properly.
        Teardown();
        g_lastDevice = d3dDevice;
        if (!d3dDevice) return;
        if (!g_enabled)
        {
            g_state = State::Disabled;
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                GFX_LOG_INFO("vulkan: disabled (WXL_GFX_VULKAN=0), Vulkan passes inert");
            }
            return;
        }
        Probe(d3dDevice);
    }

    bool Available() { return g_state == State::Dxvk; }

    const Context& Ctx() { return g_ctx; }

    State Current() { return g_state; }

    const char* Reason() { return g_reason; }

    void MarkUnusable(const char* why)
    {
        if (g_state != State::Dxvk) return;
        g_state = State::Unusable;
        SetReason(why);
        GFX_LOG_ERROR("vulkan: %s; Vulkan passes inert until the next device", why);
    }

    void SettleD3D9(void* anyImage)
    {
        if (g_state != State::Dxvk || !g_ctx.interop) return;
        ID3D9VkInteropTexture* texture = nullptr;
        if (anyImage && SUCCEEDED(static_cast<IUnknown*>(anyImage)->QueryInterface(__uuidof(ID3D9VkInteropTexture),
                                                                                   reinterpret_cast<void**>(&texture))) && texture)
        {
            // GENERAL to GENERAL records no barrier: only the render pass ending (2.x spillRenderPass(false),
            // 3.x endCurrentPass(false)), and with it every image's return to its default layout.
            VkImageSubresourceRange all{};
            all.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            all.levelCount = VK_REMAINING_MIP_LEVELS;
            all.layerCount = VK_REMAINING_ARRAY_LAYERS;
            g_ctx.interop->TransitionTextureLayout(texture, &all, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            texture->Release();
        }
        g_ctx.interop->FlushRenderingCommands();
    }

    int32_t MemoryType(uint32_t typeBits, VkMemoryPropertyFlags required)
    {
        const VkPhysicalDeviceMemoryProperties& m = g_ctx.memory;
        for (uint32_t i = 0; i < m.memoryTypeCount && i < VK_MAX_MEMORY_TYPES; ++i)
            if ((typeBits & (1u << i)) && (m.memoryTypes[i].propertyFlags & required) == required) return int32_t(i);
        return -1;
    }

    uint32_t ApiVersion() { return g_ctx.properties.apiVersion; }
}
