// wxl-graphics-shadow: the baked terrain horizon (5.tools/forever-bake, Textures\Forever\Horizon) of a 4 x 4
// block of ADT tiles around the camera, resident in two wrapping Vulkan images for shadow.hlsli (the
// terrain's shadow at any distance and sun angle), with each tile's heights kept on the CPU for the
// terrain caster.
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

namespace wxl::gfx::shadow::horizon
{
    constexpr int   kTile   = 128;                    // texels per tile edge, the bake's size
    constexpr int   kBlock  = 4;                      // tiles per block edge: the images wrap over it
    constexpr int   kSlices = 16;                     // azimuths
    constexpr float kTileSize = 533.0f + 1.0f / 3.0f; // yards per ADT tile

    /// Before the block (a wants callback): follows the Vulkan device and makes the images.
    void Prepare(const WXL_GfxVulkanApi* api);

    /// Follows the camera's tile and the map: requests the block's files; inside a record callback
    /// with a cmd, uploads what arrived (a few tiles a frame). Without a cmd (no Vulkan) only the CPU
    /// heights are kept, for the caster.
    void Update(const WXL_GfxVulkanApi* api, VkCommandBuffer cmd, const float eye[3]);

    /// The volume and height images, or null while not made.
    const WXL_GfxVkImage* Volume();
    const WXL_GfxVkImage* Heights();

    int  OriginX();
    int  OriginY();
    bool Ready();

    /// A resident tile's heights, kTile x kTile floats (row r along -X, column c along -Y: texel (c, r)
    /// at world (x0 - r T / 128, y0 - c T / 128)), or null.
    const float* TileHeights(int cx, int cy);

    /// Bumped whenever a tile's heights arrive or leave (the caster rebuilds then).
    uint32_t Generation();

    /// A new Vulkan device: every handle is gone, every tile uploads again.
    void Forget();

    const char* Status();
}
