// wxl-graphics-lights: light families -- colour temperature, scene-referred intensity, soft core,
// flicker and angular profile per kind of lamp -- and the blackbody colour of a temperature.
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

#include "wxl/GraphicsLightsSourcesApi.h"

#include <cstdint>

// docs/design.md, section 3. A family's intensity is in scene units at one yard: the irradiance a
// surface facing the light receives there, 1 being the engine's white.
namespace wxl::gfx::lights::families
{
    constexpr uint32_t kCount = WXL_GFX_LIGHT_FAMILY_COUNT;

    struct Family
    {
        const char* name;
        float       kelvin;       // 0: a tint, the light keeps its own hue
        float       tint[3];      // gamma colour of a tint family (the green lamp); zero otherwise
        float       intensity;    // scene units at one yard
        float       softRadius;   // yards: the glowing source
        uint8_t     flicker;      // WXL_GFX_LIGHT_FLICKER_*
        uint8_t     profile;      // WXL_GFX_LIGHT_PROFILE_*
        float       hot;          // share of the hot core a flame takes near its source
    };

    /// The family table, indexed by WXL_GFX_LIGHT_FAMILY_*; intensities scaled by the user's settings.
    const Family& Get(uint32_t family);

    /// Overrides a family's intensity (the panel); scale 1 restores the default.
    float& IntensityScale(uint32_t family);

    /// The family named in the model table ("torch", ...); WXL_GFX_LIGHT_FAMILY_TINT when unknown.
    uint32_t ByName(const char* name);

    const char* Name(uint32_t family);

    /// Linear sRGB of a blackbody at kelvin (1000..25000), luminance 1.
    void Blackbody(float kelvin, float rgb[3]);

    /// A linear colour moved towards white of the same luminance by share (chromatic adaptation).
    void Adapt(float rgb[3], float share);

    /// A gamma working colour to linear, normalised to luminance 1; white when black.
    void TintOf(const float gamma[3], float rgb[3]);

    /// Whether a working colour reads as a warm flame or lamp (red over green over blue).
    bool Warm(const float gamma[3]);

    /// Rec. 709 luminance.
    inline float Luma(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
}
