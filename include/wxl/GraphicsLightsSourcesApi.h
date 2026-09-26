// wxl-graphics-lights' light sources in HDR: every lamp of this frame's list with its family, colour
// temperature, scene-referred intensity, reach, soft core, flicker and room gate, and the sun, the moon
// and the sky as light definitions. Published by the wxl-graphics-lights extension as
// WXL_Api::GetInterface(WXL_GRAPHICS_LIGHTS_SOURCES_API_NAME, WXL_GRAPHICS_LIGHTS_SOURCES_API_VERSION).
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

#ifndef WXL_GRAPHICS_LIGHTS_SOURCES_API_H
#define WXL_GRAPHICS_LIGHTS_SOURCES_API_H

#include <stdint.h>

// Index for index, the list is WXL_GraphicsLightsApi::Current's: source i is light i, with the same
// id. It is published in the same begin phase, so the same demand (WXL_GraphicsLightsApi::Want)
// governs both. See extensions/wxl-graphics-lights/docs/design.md, sections 3 and 7.
//
// Scene units. Linear, scene-referred: 1 is the engine's white. `intensity` is the irradiance at one
// yard on a surface facing the light, so a lamp gives intensity * window(d / reach) / (d^2 + softRadius^2)
// at distance d, window(x) = saturate(1 - x^4)^2. The fade, the flicker and the global lamp gain are
// already folded into it.
//
// Threads. Render thread only.

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_GRAPHICS_LIGHTS_SOURCES_API_NAME    "wxl.graphics-lights.sources"
#define WXL_GRAPHICS_LIGHTS_SOURCES_API_VERSION 1

#define WXL_GFX_LIGHT_SOURCE_ROWS 10   ///< texel rows per light in SourceTexture

#define WXL_GFX_LIGHT_FAMILY_TINT        0u   ///< its own hue, no colour temperature
#define WXL_GFX_LIGHT_FAMILY_CANDLE      1u
#define WXL_GFX_LIGHT_FAMILY_CHANDELIER  2u
#define WXL_GFX_LIGHT_FAMILY_TORCH       3u
#define WXL_GFX_LIGHT_FAMILY_FIRE        4u
#define WXL_GFX_LIGHT_FAMILY_CAMPFIRE    5u
#define WXL_GFX_LIGHT_FAMILY_BRAZIER     6u
#define WXL_GFX_LIGHT_FAMILY_HEARTH      7u
#define WXL_GFX_LIGHT_FAMILY_LANTERN     8u
#define WXL_GFX_LIGHT_FAMILY_STREETLAMP  9u
#define WXL_GFX_LIGHT_FAMILY_WALLLIGHT   10u
#define WXL_GFX_LIGHT_FAMILY_GREENLAMP   11u
#define WXL_GFX_LIGHT_FAMILY_ENGINE_FIRE 12u  ///< a warm M2 light of the engine
#define WXL_GFX_LIGHT_FAMILY_ENGINE_LAMP 13u  ///< a warm WMO light of the engine
#define WXL_GFX_LIGHT_FAMILY_GIVEN       14u  ///< handed in through SetGiven
#define WXL_GFX_LIGHT_FAMILY_COUNT       15u

#define WXL_GFX_LIGHT_SOURCE_CARRIED 0x01u  ///< rides a unit (a torch in a hand)
#define WXL_GFX_LIGHT_SOURCE_TUBE    0x02u  ///< lit by the closest point of a segment (extent)
#define WXL_GFX_LIGHT_SOURCE_COOKIE  0x04u  ///< a baked cookie is known for it
#define WXL_GFX_LIGHT_SOURCE_ROOM    0x08u  ///< it stands in an interior room
#define WXL_GFX_LIGHT_SOURCE_ENGINE  0x10u  ///< an engine M2 light (the engine may light models with it)

/**
 * @brief One light in HDR, world space. Plain C, no 64-bit member.
 */
typedef struct WXL_GfxLightSource
{
    uint32_t id;                ///< WXL_GfxLight::id (0 for a given light)
    int32_t  index;             ///< its index in this frame's list
    uint8_t  family;            ///< WXL_GFX_LIGHT_FAMILY_*
    uint8_t  flicker;           ///< WXL_GFX_LIGHT_FLICKER_* (GraphicsLightsApi.h)
    uint8_t  profile;           ///< WXL_GFX_LIGHT_PROFILE_* (GraphicsLightsApi.h)
    uint8_t  flags;             ///< WXL_GFX_LIGHT_SOURCE_*
    float    position[3];       ///< where it lights from this frame, the flicker's jitter included
    float    rest[3];           ///< where it rests (what shadows render from)
    float    intensity[3];      ///< linear rgb, scene units at one yard (see the header comment)
    float    kelvin;            ///< colour temperature; 0 for a tint
    float    reach;             ///< yards where its window reaches zero
    float    softRadius;        ///< yards: its soft core (the source's size, or a carried light's core)
    float    direction[3];      ///< spot axis (unit); ignored for a point light
    float    cosOuter;          ///< cosine of the cone's edge; <= -1 for a point light
    float    cosInner;          ///< cosine where the cone reaches full strength
    float    extent[3];         ///< half a tube's length, world axes; zero for a point
    float    emissive;          ///< luminance of the lamp head itself, scene units (for bloom)
    float    emissiveRadius;    ///< yards around the source that glow
    float    flickerGain;       ///< this frame's flicker factor (already in intensity)
    float    flickerOffset[3];  ///< position - rest: the flame's jitter this frame
    float    fade;              ///< 0..1 its importance fade (already in intensity)
    int32_t  room;              ///< index in WXL_GraphicsLightsApi::Rooms' order, -1 outside every room
    float    roomGate;          ///< 0..1 how much the room gate applies to it (eases as it changes room)
    int32_t  shadowSlot;        ///< wxl-graphics-shadow's slot this frame, -1 none
} WXL_GfxLightSource;

/**
 * @brief The sun, the moon and the sky as the engine lights with them this frame, linear, scene units.
 */
typedef struct WXL_GfxSkyLight
{
    uint32_t structSize;
    uint32_t frameIndex;
    int32_t  valid;             ///< 0 before the first world frame
    float    toSun[3];          ///< unit, towards the drawn sun
    float    toMoon[3];         ///< unit, towards the drawn moon
    float    toLight[3];        ///< unit, towards the body the engine lights with this frame
    float    sunColor[3];       ///< linear, luminance 1
    float    sunIlluminance;    ///< scene units on a surface facing it
    float    moonColor[3];
    float    moonIlluminance;
    float    sunWeight;         ///< 0..1: how much the sun lights (by its elevation)
    float    moonWeight;        ///< 0..1: how much the moon lights (0 by day)
    float    diffuse[3];        ///< the engine's directional light, linear (the body it lights with)
    float    ambient[3];        ///< the engine's ambient, linear
    float    skyZenith[3];      ///< linear
    float    skyHorizon[3];     ///< linear
    float    fogColor[3];       ///< the zone's distance fog colour, linear
    float    dayFactor;         ///< 0 night .. 1 day
    float    weather;           ///< 0 clear .. 1 storm; 0 until the core has a weather binding
    float    kelvinSun;         ///< the sun colour's correlated temperature, for reference
} WXL_GfxSkyLight;

typedef struct WXL_GraphicsLightsSourcesApi
{
    uint32_t structSize;
    uint32_t apiVersion;

    /// This frame's sources and the frame they were published for; null and *count 0 before the first.
    const WXL_GfxLightSource*(__cdecl* Sources)(int* count, uint32_t* frameIndex);

    /**
     * @brief IDirect3DTexture9*, WXL_GFX_LIGHTS_MAX x WXL_GFX_LIGHT_SOURCE_ROWS, A32B32G32R32F, D3DPOOL_DEFAULT.
     *
     * Filled with UpdateTexture, so a Vulkan pass may import it. Released before a device reset, back
     * with the next publish: re-read the pointer every frame. The rows are those of
     * shaders/wxl/lights/sources.hlsli (camera-relative positions).
     */
    void*(__cdecl* SourceTexture)(void);

    /// The sun, the moon and the sky this frame; 0 before the first world frame. Set structSize first.
    int(__cdecl* Sky)(WXL_GfxSkyLight* out);

    /// A family's name ("torch", ...), "" when unknown. Static.
    const char*(__cdecl* FamilyName)(uint32_t family);
} WXL_GraphicsLightsSourcesApi;

#ifdef __cplusplus
}
#endif

#endif // WXL_GRAPHICS_LIGHTS_SOURCES_API_H
