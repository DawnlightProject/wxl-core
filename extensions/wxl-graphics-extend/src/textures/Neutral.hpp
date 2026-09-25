// wxl-graphics-extend: the shared stand-in textures (white, black, flat normal, white cube and volume)
// and the blue-noise texture.
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

#include <cstdint>

// Every texture is D3DPOOL_MANAGED (it survives a device reset), created on first use and kept for
// the process. The blue noise is baked on a worker thread started at load: 128 x 128, four
// independent void-and-cluster rank maps (Ulichney), every value 0..255 equally often.
namespace wxl::gfx::textures
{
    /// Starts the blue-noise bake on a worker thread. Called once from WXL_Load.
    void StartBlueNoiseBake();

    /// WXL_GFX_TEX_* as IDirect3DBaseTexture9*, borrowed; null when unavailable or still baking.
    void* Texture(void* device, uint32_t which);

    /// For the panel: whether the bake finished and how long it took.
    bool BlueNoiseReady(uint32_t& bakeMs);
}
