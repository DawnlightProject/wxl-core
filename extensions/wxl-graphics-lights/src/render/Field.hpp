// wxl-graphics-lights: the lamp light field for the air (GraphicsLightsFieldApi.h) and, without the fog,
// its integration into a thin halo. docs/design.md, section 8.3.
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

namespace wxl::gfx::lights::field
{
    struct Settings
    {
        int   enabled = 1;          // compute the field when the fog or the halo wants it
        int   shadows = 1;          // the shadow service's point shadows in the air
        float thinning = 1.0f;      // share of the fog's extinction between a lamp and the air it lights
        int   halo = 1;             // without the fog: a thin halo from the field
        float haloDensity = 0.012f; // extinction per yard of that haze
        float haloStrength = 1.0f;
        float phase[3] = { 0.6f, -0.2f, 0.25f };   // g forward, g back, blend (SetPhase)
    };

    Settings& Config();

    /// Reads WXL_GFX_LIGHTS_FIELD_*; publishes wxl.graphics-lights.field.
    void Install();

    /// Called from the render wants: whether the field is computed this frame (wanted by a consumer
    /// this frame or the last, or needed for the halo).
    bool Wanted();

    /// Whether this frame draws the halo (the fog is not active).
    bool HaloWanted();

    void FillConstants(gpu::Constants& c);

    /// Makes the images; call before the block's first clear. False when refused.
    bool Ensure(const WXL_GfxVulkanApi* api);

    /**
     * @brief Records the field (and the halo when asked).
     * @return true when the field was written this frame.
     */
    bool Record(const WXL_GfxVkFrame& vk, const WXL_GfxVkImage* sources, const WXL_GfxVkImage* clusters,
                const WXL_GfxVkImage* cookies, bool halo);

    /// This frame's images for the surfaces: the inscatter and the integrated halo (null when absent).
    const WXL_GfxVkImage* Inscatter();
    const WXL_GfxVkImage* Halo();

    /// No field this frame (the record did not run): Get returns 0.
    void Invalidate();

    void Panel();
    const char* Status();
}
