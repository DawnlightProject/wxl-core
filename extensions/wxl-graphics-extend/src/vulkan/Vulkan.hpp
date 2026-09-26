// wxl-graphics-extend: the Vulkan side -- DXVK's device through its D3D9 interop, the per-frame compute block.
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

#include "wxl/GraphicsVulkanApi.h"

#include <cstddef>
#include <cstdint>

// The published table (GraphicsVulkanApi.h) and the scheduler's hooks into it. Everything here is
// inert on native D3D9: OnDevice finds no interop and every other call returns at once.
namespace wxl::gfx::vulkan
{
    /// Reads the config (WXL_GFX_VULKAN, default 1). Called once from WXL_Load.
    void Install();

    /// The table Module.cpp publishes as WXL_GRAPHICS_VULKAN_API_NAME.
    const WXL_GfxVulkanApi* Table();

    /**
     * @brief Called by the scheduler at the start of every world pass with the D3D9 device.
     *
     * Probes the DXVK interop once per device pointer (QueryInterface for ID3D9VkInteropDevice) and
     * loads the Vulkan functions; afterwards a pointer compare.
     */
    void OnDevice(void* device);

    /// Union of the compute passes' wants for the coming world pass; 0 when not Available.
    uint32_t PollWants();

    /**
     * @brief Records and submits the compute block: called by the scheduler once the world is drawn and
     *        the frame is built, before any D3D9 pass draws. Nothing when no compute pass wants.
     */
    void RunComputeBlock(const WXL_GfxFrame& frame);

    /// Called before a device reset: drops imports and anything tied to DEFAULT-pool D3D9 resources.
    void OnDeviceLost();

    /// The panel's "Vulkan" section (drawn inside the service panel; Ui* calls only).
    void PanelSection();
}
