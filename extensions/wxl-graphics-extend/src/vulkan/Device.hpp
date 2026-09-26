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

#pragma once

#include "DxvkInterop.hpp"
#include "Functions.hpp"

#include <cstdint>

// One D3D9 device pointer is probed once (QueryInterface for DXVK's interop); afterwards OnDevice is
// a pointer compare. While the interop answered, the context below is valid and Available() is
// true: the frame slots (Frame.hpp) and the service's objects (Resources.hpp) live on its device.
// A new device pointer tears every one of them down first, in that order, then starts over.
namespace wxl::gfx::vulkan::device
{
    struct Context
    {
        uint32_t                         generation = 0;   // never 0 once probed; bumps per Vulkan device
        VkInstance                       instance = VK_NULL_HANDLE;
        VkPhysicalDevice                 physical = VK_NULL_HANDLE;
        VkDevice                         device = VK_NULL_HANDLE;
        VkQueue                          queue = VK_NULL_HANDLE;
        uint32_t                         queueFamily = 0;
        uint32_t                         queueIndex = 0;
        PFN_vkGetInstanceProcAddr        getInstanceProcAddr = nullptr;
        PFN_vkGetDeviceProcAddr          getDeviceProcAddr = nullptr;
        VkPhysicalDeviceProperties       properties{};
        VkPhysicalDeviceMemoryProperties memory{};
        uint32_t                         timestampValidBits = 0;   // of the queue family; 0 = no timestamps
        ID3D9VkInteropDevice*            interop = nullptr;        // one reference, held while the device is ours
        Functions                        fn;
        WXL_GfxVkCaps                    caps{};                   // versions, features, subgroups (Caps.hpp)
    };

    enum class State
    {
        None,       // no device seen yet
        Disabled,   // WXL_GFX_VULKAN=0
        Native,     // the device answered no interop: native D3D9 (or a DXVK too old to have it)
        Dxvk,       // Available
        Unusable,   // DXVK, but something failed (see Reason): inert until the next device
    };

    /// From Install, before any device: WXL_GFX_VULKAN.
    void SetEnabled(bool enabled);

    /// The scheduler's device, every world pass. Cheap after the first call per pointer.
    void OnDevice(void* d3dDevice);

    bool           Available();
    const Context& Ctx();       ///< valid while Available
    State          Current();
    const char*    Reason();    ///< why Unusable; "" otherwise

    /// Something the device can never recover from (VK_ERROR_DEVICE_LOST): Available() turns false
    /// until the next device pointer, and nothing is submitted any more.
    void MarkUnusable(const char* why);

    /**
     * @brief Before a block reads any D3D9 image: every image is back in its default layout, and the
     *        D3D9 work so far is submitted.
     *
     * A same-layout TransitionTextureLayout on anyImage (any DXVK texture or surface; null skips it)
     * makes DXVK end its render pass for good rather than suspend it, which returns the images it
     * left elsewhere -- the world's render targets -- to the layout GetVulkanImageInfo reports. The
     * flush after it may move images to new memory, so image handles are read after this call.
     */
    void SettleD3D9(void* anyImage);

    /// The first memory type in typeBits with every flag of required; -1 when none.
    int32_t MemoryType(uint32_t typeBits, VkMemoryPropertyFlags required);

    /// The physical device's API version, for the log and the context (the usable one is Ctx().caps).
    uint32_t ApiVersion();
}
