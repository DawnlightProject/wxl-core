// wxl-graphics-shadow: the settings (WXL_GFX_SHADOW_* in wxl-graphics-shadow.cfg, the environment
// first), their defaults and the debug views. The panel edits them live.
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

namespace wxl::gfx::shadow
{
    // Isolates: each removes one ingredient, for A/B comparisons.
    constexpr int kIsoNoMaps     = 1 << 0;
    constexpr int kIsoNoCapsules = 1 << 1;
    constexpr int kIsoNoContact  = 1 << 2;
    constexpr int kIsoNoCascades = 1 << 3;
    constexpr int kIsoNoTerrain  = 1 << 4;
    constexpr int kIsoNoCaster   = 1 << 5;
    constexpr int kIsoNoFade     = 1 << 6;

    // Debug views (the mask pass's debug image, drawn over the finished frame).
    enum View : int
    {
        kViewNone = 0,
        kViewSun,        // the sun's mask
        kViewMoon,       // the moon's mask
        kViewSlot,       // one slot's mask
        kViewSlots,      // every slot at once, each its colour
        kViewContact,    // the contact term towards the body that shines
        kViewTerrain,    // the terrain horizon towards it
        kViewCascades,   // which cascade each pixel reads
        kViewCapsules,   // the body capsules, as a heat view
        kViewAtlas,      // the filtered map atlas (mean depth)
        kViewNormals,    // the receiver normals the pass uses (G-buffer or depth)
        kViewCount
    };

    struct Settings
    {
        int   enabled = 1;

        // Light maps.
        int   maps = 1;              // cube maps for the most important lights
        int   stillMaps = 4;         // still lamps with maps (static cached + units redrawn)
        int   movingMaps = 2;        // moving and carried lights with maps (all casters, every frame)
        int   staticFace = 512;      // core face size of a still lamp's static map
        int   unitFace = 256;        // core face size of its units map
        int   movingFace = 512;      // core face size of a moving light's map
        int   refresh = 2;           // static faces refreshed per frame when the engine's static set changes
        int   atlasFace = 256;       // face size of the filtered atlas (128 or 256)
        float housing = 0.45f;       // yards around a lamp whose casters (its own fixture) cast nothing
        float carriedHousing = 0.3f; // the same for a carried light: its item and the hand
        float carrierMargin = 0.12f; // yards around the carrier's capsule that its own light skips
        float evsmPositive = 40.0f;  // EVSM exponents
        float evsmNegative = 8.0f;
        float bleed = 0.3f;          // light bleeding reduction, 0..0.9
        float minVariance = 0.0004f; // EVSM minimum variance, in depth units
        float softness = 1.0f;       // penumbra per yard of source size
        float lodBias = 0.0f;        // extra blur on every map lookup (mip levels)
        float normalOffset = 1.5f;   // receiver moved off its surface, texels of the map
        float slopeBias = 1.0f;      // depth bias, texels of the map, grows on slopes

        // Slots.
        float slotHold = 1.0f;       // seconds a light keeps its slot at least
        float slotFade = 0.5f;       // seconds a slot or map fades in or out
        float slotMargin = 1.25f;    // how much more important a newcomer must be to take a slot

        // Bodies.
        int   capsules = 1;
        float capsuleRange = 60.0f;  // yards around the camera
        float capsuleRadius = 0.2f;  // radius per yard of height
        float capsulePenumbra = 1.0f;

        // Sun and moon.
        int   sun = 1;
        int   cascades = 1;          // read the engine's cascades
        int   lowSun = 1;            // render them along a lower, truer sun (and the moon at night)
        int   moon = 1;              // moon shadows at night
        float sunLift = 1.6f;        // the body's height multiplied by this (the engine uses 5)
        float sunMinElevation = 12.0f; // degrees the shadow light never falls below (the engine's floor is 50)
        float cascadeFilter = 1.0f;  // tent spacing in texels of the finest map
        float duskSoftness = 1.0f;   // extra spacing as the body nears the horizon
        float cascadeBias = 0.0f;    // extra depth bias, yards

        // The terrain.
        int   terrainCaster = 1;     // the terrain drawn into the engine's cascades
        int   casterCascades = 4;    // cascades that receive it: 1 main only .. 4 all
        float casterBias = 0.3f;     // yards the terrain is pushed along the light
        float casterDrop = 0.75f;    // yards the terrain is lowered per 4.17 yd of mesh cell (its own surface never shadows itself)
        int   casterDetail = 0;      // -1 finer, 0 the cascade's step, 1 coarser
        int   horizon = 1;           // the baked horizon maps
        float horizonStrength = 1.0f;
        float horizonPenumbra = 2.5f;  // degrees
        float horizonOccluder = 120.0f; // yards the horizon's terrain is assumed to be

        // Contact shadows.
        int   contact = 1;
        float contactSun = 1.5f;     // yards marched towards the sun or the moon
        int   contactSlots = 4;      // the most important slots that get one
        float contactLamp = 1.0f;    // yards marched towards a lamp at most
        float contactThickness = 0.35f; // yards behind the depth buffer a sample may occlude
        float contactStrength = 1.0f;

        // Masks.
        int   halfRes = 0;           // trace at half resolution and upsample

        // Debug.
        int   view = kViewNone;
        int   viewSlot = 0;
        int   isolate = 0;
        int   timers = 1;
    };

    Settings& Config();

    /// Reads every key once, from WXL_Load.
    void LoadSettings();

    /// True when an isolate bit is set.
    inline bool Isolated(int bit) { return (Config().isolate & bit) != 0; }
}
