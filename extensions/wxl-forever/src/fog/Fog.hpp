// wxl-forever fog: screen-space volumetric fog drawn over the world from its sampled depth.
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

// The world pass draws its depth into an INTZ texture (OnWorldSceneBegin); the froxel volume samples
// it to rebuild each pixel's world position and composite the fog over the scene (OnWorldSceneEnd).
// Every cell takes the indoor or the outdoor profile depending on whether it lies inside an interior
// WMO group. Inert, and logged, when INTZ or float targets are unavailable or the depth is multisampled.
namespace wxl::forever::fog
{
    /// Switches and debug views that are not part of any profile.
    struct Settings
    {
        int   enabled         = 1;       // master switch; off also stops the depth redirect
        int   indoorDetect    = 1;       // classifies samples by interior WMO groups; off = all outdoor
        float debugFar        = 200.0f;  // distance shown as white in the linear debug view
        float transitionTime  = 3.0f;    // seconds a profile change takes to blend in
        float indoorTransition = 5.0f;   // seconds the fog takes to follow the camera through a doorway

        // The froxel volume.
        int   froxelX         = 160;     // froxels across
        int   froxelY         = 90;      // froxels down
        int   froxelZ         = 64;      // depth slices, spaced exponentially
        float froxelNear      = 1.0f;    // first slice's far edge, yards (slice 0 starts at the camera)
        float sliceBend       = 0.5f;    // 0 plain exponential slices; towards 1 the near slices thin, the far ones thicken
        int   froxelView      = 0;       // 0 fog, 1 slices, 2 density, 3 scatter, 4 transmittance,
                                         // 5 history clamp, 6 lights only, 7 terrain, 8 linear depth,
                                         // 9 raw depth, 10 indoor/outdoor, 11 shadow visibility,
                                         // 12 sky visibility, 13 fog banks, 14 noise pattern, 15 trails
        float temporal        = 0.9f;    // share of last frame's volume kept; 0 disables
        int   omniShadows     = 1;       // the surface lighting's omni shadow maps shadow the lamps' glow in the fog
        float omniBias        = 0.15f;   // yards a froxel is pulled towards the light before the map is compared
        float lightMargin     = 3.0f;    // yards over which interior lights fade across a box boundary
        int   debugLantern    = 0;       // a warm point light at the player, to test light injection
        int   diagnostics     = 0;       // logs the celestial inputs' spread once a second

        int   projectiles     = 1;       // missiles in flight carve tunnels
        int   plumes          = 1;       // smoke and steam emitters become plumes in the volume
        int   immersion       = 1;       // standing in dense fog or smoke shows on screen
        int   wakes           = 1;       // units push the fog away and leave a fading trail
        float wakeRange       = 40.0f;   // units this far from the player carve the fog
        float wakeTrail       = 1.2f;    // seconds a trail takes to fade
        int   quality         = 0;       // 0 custom, 1 low, 2 medium, 3 high, 4 ultra (sets the fields below)
        int   nearField       = 1;       // near wisps: the finer noise folded into the density over the first yards
        int   terrain         = 1;       // fog height measured from a heightfield around the camera
        int   terrainBudget   = 48;      // ground traces per tick for it
        int   dither          = 1;       // +-0.5/255 of noise on the final colour, against banding
        int   phaseModel      = 0;       // 0 dual Henyey-Greenstein, 1 Cornette-Shanks
        int   worldShadows    = 1;       // the world shades the fog from the sun and moon
        int   shadowSteps     = 10;      // screen-space steps per froxel for it
        int   skyOcclusion    = 1;       // less sky light where the world or the screen hides the sky
        int   nativeFog       = 1;       // 1 pushes the client's own distance fog to the far clip
        float nativeFogStart  = 0.85f;   // where it then starts, as a fraction of its end

        // Isolate: each switch removes one ingredient, so the one that stops an artefact names it.
        struct Isolate
        {
            int freezeJitter  = 0;   // one fixed sample offset instead of the per-frame sequence
            int freezeTime    = 0;   // the noise stops drifting
            int noHistory     = 0;   // the resolve passes this frame through
            int noClamp       = 0;   // history is blended without the neighbourhood clamp
            int noChangeCut   = 0;   // the history is only clamped, never weighed down where the fog changed
            int noCrossfade   = 0;   // a doorway switches each cell's fog whole instead of cross-fading the two
            int noOmniShadows = 0;   // the lamps' glow in the fog ignores the shadow maps
            int noHeat        = 0;   // flames no longer thin the fog around them
            int noHotCore     = 0;   // a flame's glow in the fog keeps one colour near its source
            int noCloudLight  = 0;   // the sun and moon light the fog evenly, with no cloud patches
            int flatDensity   = 0;   // no noise at all: height fog only
            int noShadow      = 0;
            int noCelestial   = 0;   // no sun or moon term
            int noLights      = 0;
            int noBoxes       = 0;   // indoor detection off for the volume
            int noWakes       = 0;
            int noProjectiles = 0;   // missiles no longer carve
            int noPlumes      = 0;   // emitters make no smoke in the volume
            int noImmersion   = 0;   // no veil or softened lights
            int noHaze        = 0;   // the volume stops at its far distance instead of going on to the horizon
            int noNear        = 0;   // no near wisps in the density
            int noTerrain     = 0;   // height from the absolute base only
            int noContact     = 0;
            int noFlow        = 0;   // one noise layer, a fixed warp
            int noWorldShadow = 0;   // the world no longer shades the fog
            int noBanks       = 0;   // no macro map: fog everywhere
            int noHighHaze    = 0;   // the ground mist alone
            int noSkyOcclusion = 0;  // the sky light reaches everywhere
            int noBankShading = 0;   // no long bank shadow, no darkening under fog above
        } isolate;
    };

    Settings& Config();

    /// Reads the config, attaches the hooks and registers the panel. Called once from WXL_Load.
    void Install();

    /// The feature's parts of the Forever window (core/ForeverUi.cpp): its line in Overview, its own
    /// tab, and its group in Debug.
    void UiOverview();
    void UiSettings();
    void UiDebug();
}
