// wxl-forever surface lighting: every light of the light service on every surface of the world.
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

struct IDirect3DTexture9;

// The engine lights a model with its four nearest lights and terrain and buildings with none. After
// the world pass (before the fog, which then veils lit surfaces) this feature lights every pixel of
// the world with every light of its cluster, as deferred lighting:
//   1. the light buffer, at full or half resolution: every light of the pixel's cluster summed into
//      an FP16 target (normals from the G-buffer or the depth, Lambert with a wrap, GGX specular in
//      alpha, the light service's falloff, omni shadow maps and screen-space contact shadows, the
//      fog's extinction between light and surface), accumulated over time and denoised;
//   2. resolve: the buffer brought to full resolution by a depth-aware upsample, capped, and
//      published twice: for apply, and for the engine's own materials with units cleared from it;
//   3. apply: scene + albedo x light, the albedo estimated from the scene colour and the light that
//      made it, so a lamp tints what it lights.
// Works with the fog off.
namespace wxl::forever::surface
{
    struct Settings
    {
        int   enabled     = 1;
        int   resolution  = 2;       // full pixels per lighting pixel: 1 full, 2 half
        float strength    = 0.6f;    // scale on every light
        float softness    = 0.7f;    // 0 raw .. 1 softest: falloff tail, penumbra size
        float wrap        = 0.6f;    // how far light wraps past the terminator
        float specular    = 0.35f;   // scale on the GGX highlight; 0 off
        float wetness     = 0.0f;    // 0 dry .. 1 soaked: smoother, shinier surfaces (for rain)
        float highlightCap = 2.5f;   // the applied light's luminance rolls off towards this; 0 off
        float highlightKnee = 0.6f;  // luminance where apply starts compressing the lit result towards 1
        float indirect    = 0.35f;   // one bounce of light off nearby surfaces; 0 off
        float indirectRadius = 3.0f; // how far that bounce reaches, yards
        float emissive    = 0.15f;   // how much the scene's own glowing pixels light their surroundings
        int   historyLength = 32;    // most frames a still pixel averages
        int   denoise     = 2;       // a-trous iterations; 0 off
        float engineOnUnits = 0.0f;  // share of the engine's own model lights added again on units
        float carriedOnCarrier = 0.25f; // share of a carried light (a held torch) on its own carrier
        int   carriedShadowMaps = 0; // carried lights may take an omni shadow map (their carrier and item then shadow)
        float indirectOnUnits = 0.3f; // share of the bounced light added on characters
        float changeCut   = 1.0f;    // sigmas of local noise a history may drift from the mean before it shortens; 0 off
        int   gbuffer     = 1;       // take the engine's G-buffer normals where it writes them
        int   engineMaterials = 1;   // the engine's own materials read last frame's light buffer
        float unitMargin  = 0.5f;    // yards around a unit cleared from the buffer the engine reads
        int   omniShadows = 1;       // shadow maps of the four most important lights
        int   omniUnitsFirst = 1;    // the maps go first to lights with a unit near them; a still lamp with a cookie waits
        int   omniTaps    = 12;      // omni filter: 0 plain 4-tap, 5..12 blocker search and a disc that wide
        int   omniDenoise = 2;       // passes of the spatial filter on the omni shadow share (0 off)
        float omniSelf    = 0.5f;    // yards around a light whose casters (its own housing) do not shadow it
        int   omniFaceSize = 512;    // texels per cube face of the omni maps
        int   omniRefresh = 3;       // faces of still lights redrawn a frame, in turn (moved ones go at once)
        float shadowBias  = 0.03f;   // yards the omni receiver depth is pulled back
        float shadowSlope = 1.0f;    // omni map texels of extra bias per unit of slope
        float contactLength = 2.0f;  // yards the contact shadow steps cover for a light with a map
        int   shadowSteps = 3;       // screen-space steps per light; 0 off
        int   fogDimming  = 1;       // the fog between a light and a surface dims it
        float normalSmoothing = 0.6f;  // 0 raw depth normals .. 1 widest smoothing (creases stay sharp)
        int   view        = 0;       // 0 lit, 1 surface lights only, 2 normals, 3 indirect only, 4 specular only,
                                     // 5 omni shadow atlas of the first light, 6 its face per pixel, 7 the buffer
                                     // the engine reads, 8 the albedo estimate, 9 what the engine's materials added
        // Isolate: each switch removes one ingredient.
        int   noShadows = 0, noIndirect = 0, noSpecular = 0, noProfiles = 0, noDenoise = 0, noTemporal = 0;
        int   noOmni = 0, noGBuffer = 0, noCarried = 0, noNearCap = 0;
        int   plainOmniFilter = 0, noSlopeBias = 0, noChangeCut = 0, noHotCore = 0;
        int   washLog = 0;           // with the self-check: its stage line every 2 s instead of every 10 s
        int   selfCheck = 1;         // an asynchronous probe of every lighting stage; logs its range and warns on a wash
        float albedoFloor = 0.25f;   // least scene lighting the albedo estimate divides by (night is darker than surfaces are)
    };

    Settings& Config();

    /// The light buffer as the last frame wrote it, shareable with anything that draws after it: rgb
    /// the linear light reaching the surface (what a white surface facing it reflects), a the soft
    /// specular luminance. Full resolution, point-sampled at VPOS. Null before the first frame.
    struct LightBuffer
    {
        IDirect3DTexture9* texture;
        uint32_t           format;   // D3DFORMAT, A16B16G16R16F
        unsigned           width, height;
        uint32_t           frame;    // passes::FrameIndex of the frame it is valid for
    };
    LightBuffer CurrentLightBuffer();

    /// Reads the config, registers the pass and the panel. Called once at load.
    void Install();

    /// The feature's parts of the Forever window (core/ForeverUi.cpp): its line in Overview, its own
    /// tab, and its group in Debug.
    void UiOverview();
    void UiSettings();
    void UiDebug();
}
