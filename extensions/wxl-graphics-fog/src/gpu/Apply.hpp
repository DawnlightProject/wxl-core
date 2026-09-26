// wxl-graphics-fog: the D3D9 side -- the textures the block's results are copied into, and the
// composite (shaders/apply.ps.hlsl) that lays the fog over the scene.
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

#include "wxl/GraphicsExtendApi.h"

#include <cstdint>

namespace wxl::gfx::fog::gpu::apply
{
    constexpr uint32_t kRegisters = 9;   ///< c0..c8 of shaders/apply.ps.hlsl

    struct Input
    {
        uint32_t frameIndex = ~0u;
        float    c[kRegisters][4] = {};
    };

    /// The D3D9 textures (DEFAULT pool) the block copies into, at half resolution; made again after a
    /// reset or a resize. False when the device refused them.
    bool  EnsureTargets(void* device, uint32_t halfW, uint32_t halfH);
    void* FogTexture();     ///< A16B16G16R16F: light, transmittance (the back layer)
    void* FrontTexture();   ///< A16B16G16R16F: the front layer
    void* AuxTexture();     ///< A32B32G32R32F: distances
    void* DebugTexture();   ///< A16B16G16R16F: the debug views

    /// Composites over render target 0; false when nothing was drawn.
    bool Draw(const WXL_GfxFrame& frame, const Input& in);

    /// Before a reset: every DEFAULT-pool texture and the queries go; the compiled shader stays.
    void Release();
}
