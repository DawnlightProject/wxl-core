// wxl-graphics-extend: what DXVK's device offers a compute pass -- versions, features, subgroups.
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

namespace wxl::gfx::vulkan::device { struct Context; }

// Read once per device: the hardware's features from the physical device, then the subset the
// running DXVK generation turns on (Caps.cpp keeps that table, taken from DXVK's own sources). The
// generation comes from the D3D9 factory: ID3D9VkExtInterface answers there since DXVK 3.0.
namespace wxl::gfx::vulkan::caps
{
    /// Fills out for the device in c; d3dDevice is the IDirect3DDevice9 the interop came from.
    void Query(const device::Context& c, void* d3dDevice, WXL_GfxVkCaps& out);

    /// The startup lines: versions, subgroups, what is enabled and what the hardware has besides.
    void Log(const device::Context& c);

    /// A format's optimal-tiling features (VkFormatProperties3); 0 when not Available.
    VkFormatFeatureFlags2 FormatFeatures(VkFormat format);

    /// WXL_GFX_VK_CAP_* bits as words, for the log and the panel; "none" when empty.
    const char* CapNames(uint64_t bits, char* buf, size_t cap);

    /// VK_SUBGROUP_FEATURE_* bits as words.
    const char* SubgroupNames(VkSubgroupFeatureFlags ops, char* buf, size_t cap);

    /// "1.3" (patch false) or "1.4.351".
    const char* Version(uint32_t v, bool patch, char* buf, size_t cap);
}
