// wxl-graphics-lights: the surfaces -- deferred HDR lighting of the G-buffer at full resolution, in Vulkan
// compute (shaders/compute/surface.cs.hlsl), copied into a D3D9 texture for the composite.
// docs/design.md, section 6.
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

#include "../gpu/Gpu.hpp"

#include "wxl/GraphicsVulkanApi.h"

#include <cstdint>

namespace wxl::gfx::lights::surface
{
    struct Settings
    {
        int   burley = 1;             // Burley diffuse (0 Lambert)
        float specular = 1.0f;        // GGX strength
        float wetness = 0.0f;         // wet ground on upward-facing outdoor surfaces
        float roughness = 1.0f;       // every roughness times this
        float emissive = 1.0f;        // the lamp heads' glow
        float fogDims = 1.0f;         // share of the fog's extinction between a lamp and a surface
        float outdoorIndoors = 0.12f; // an outdoor light reaching into a room
        float indoorOutdoors = 0.2f;  // a room's light reaching outside every room
        int   sun = 1;                // the sun and moon shadow the engine lacks
        float deepen = 0.6f;          // the engine's cascades deepened past its own shading
        float prefilter = 3.0f;       // cookie texels a footprint may span before the pattern gives way
        int   view = 0;               // LIGHTS_VIEW_*
        uint32_t isolate = 0;         // LIGHTS_ISO_*
        float fieldSlice = 0.3f;      // the field slice the Field view shows, 0..1 in depth
    };

    Settings& Config();

    /// Reads WXL_GFX_LIGHTS_SURFACE_*. Called once at load.
    void Install();

    /// Fills the rows of the constant buffer the surfaces own.
    void FillConstants(gpu::Constants& c);

    /// Makes the output image and its D3D9 copy at the frame's size; call before the block's first clear.
    bool Ensure(const WXL_GfxVulkanApi* api, const WXL_GfxFrame& frame);

    /**
     * @brief Records the surface pass and its copy into the D3D9 texture.
     * @param masks  the shadow service's masks this frame, or null.
     * @param halo   the integrated halo (3D), or null.
     * @param field  the field's inscatter (3D, for the Field view), or null.
     */
    bool Record(const WXL_GfxVkFrame& vk, const WXL_GfxVkImage* sources, const WXL_GfxVkImage* clusters,
                const WXL_GfxVkImage* cookies, const WXL_GfxVkImage* masks, const WXL_GfxVkImage* halo,
                const WXL_GfxVkImage* field);

    /// The D3D9 texture (A16B16G16R16F) holding frame `frameIndex`'s light; null when that frame has none.
    void* LightTexture(uint32_t frameIndex);

    /// Before a device reset: the D3D9 texture goes.
    void OnDeviceLost();

    /// The Surfaces tab of the panel.
    void Panel();

    /// The views and isolates of the Debug tab.
    void DebugPanel();

    const char* Status();
}
