// wxl-graphics-shadow: one frame's compute work, recorded into wxl-graphics-extend's block.
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

#include <cstdint>

namespace wxl::gfx::shadow::gpu
{
    /// Before the block (the compute pass's wants): the device, the pipelines and the images this frame
    /// needs, at the last frame's size. False when nothing can run.
    bool Prepare(const WXL_GfxVulkanApi* api, bool masks, bool debug);

    /// The frame: uploads, the maps' conversion and mips, the public block, then the masks (when
    /// wanted) and the debug view (copied into debugTexture, an A16B16G16R16F D3D9 texture, when given).
    bool Record(const WXL_GfxVulkanApi* api, const WXL_GfxVkFrame& vk, bool masks, void* debugTexture);

    /// Every map converted again from scratch (settings that change the moments changed).
    void ResetMaps();
}
