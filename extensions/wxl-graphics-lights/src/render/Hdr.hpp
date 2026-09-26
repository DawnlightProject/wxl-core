// wxl-graphics-lights: the HDR chain's D3D9 side -- the composite that expands the engine's image into
// scene-referred light and adds the lamps, the interim resolve, and the resolve contract
// (GraphicsLightsResolveApi.h). docs/design.md, section 2.
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
#include "wxl/GraphicsLightsResolveApi.h"

#include <cstdint>

namespace wxl::gfx::lights::hdr
{
    struct Settings
    {
        int   hdr      = 1;      // draw the world in FP16 and resolve at the end
        float knee     = 0.6f;   // where the curve leaves the identity, linear
        float exposure = 1.0f;   // the interim resolve's exposure
        int   dither   = 1;      // blue-noise dither on 8-bit output
    };

    Settings& Config();

    constexpr float kGamma = 2.2f;   // the FP16 scene's encoding, and the engine's display gamma

    /// Reads WXL_GFX_LIGHTS_HDR_*; publishes wxl.graphics-lights.resolve.
    void Install();

    /**
     * @brief The composite over render target 0 (the FP16 scene, or an 8-bit target without HDR).
     * @param light  IDirect3DTexture9* the surface pass copied its result into (rgb added light, a sun
     *               factor), or null: the engine's image is then only expanded.
     * @param view   a debug view: the light texture holds a colour to show as is.
     * @return whether it drew.
     */
    bool Composite(const WXL_GfxFrame& frame, void* light, bool view);

    /// The interim resolve; a no-op when the frame was not expanded, is already resolved or post claimed it.
    void Resolve(const WXL_GfxFrame& frame, bool view);

    /// Whether the resolve was claimed by another pass (post).
    bool Claimed();

    /// GPU milliseconds of the composite and the resolve, -1 when unknown.
    float CompositeMs();
    float ResolveMs();

    const char* Status();

    /// Before a device reset: the scene copy and the timer queries go; the shaders stay.
    void OnDeviceLost();
}
