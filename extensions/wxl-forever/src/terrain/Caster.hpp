// wxl-forever terrain: the terrain as a caster in the engine's sun shadow maps, and a lower sun.
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

struct IDirect3DDevice9;

// The engine renders WMOs and doodads into its sun shadow maps (extShadowQuality 1..5) but never
// the terrain, so hills cast nothing. After each of its render callbacks (core shadows::ChainAfter)
// this draws a heightfield mesh of the resident horizon tiles (Horizon.cpp, 4.17 yd per vertex,
// 129 x 129 per tile with the neighbour's seam row) depth-only into the same map, with the pass's
// own light view, projection and viewport (core shadows::DescribeRender), writing what the engine's
// caster shader writes. Patches of 32 x 32 cells are culled against the pass's rectangle and drawn
// at a step that grows with the cascade. Every D3D state touched is read back and put back.
//
// The engine lifts its shadow light to at least 50 degrees (sun height times five): with "low sun"
// on, the core's wxl.shadowlight adjuster receives the true sun and hands back a direction with a
// gentler lift and a lower floor, so dusk shadows stretch.
namespace wxl::forever::terrain::caster
{
    struct Settings
    {
        int   enabled      = 1;      // terrain into the maps
        int   cascades     = 4;      // maps that receive terrain: 1 main only, 2 + band 0, 3 + band 1, 4 all
        float bias         = 0.3f;   // yards the terrain is pushed along the light (against self-shadow acne)
        int   detail       = 0;      // -1 finer, 0 the cascade's step (4 / 4 / 8 / 17 yd), 1 coarser
        int   lowSun       = 1;      // adjust the engine's shadow light direction
        float heightScale  = 1.5f;   // the sun's height multiplied by this (the engine uses 5)
        float minElevation = 15.0f;  // degrees the shadow light never falls below (the engine's floor is 50)
    };

    Settings& Get();

    /// Reads WXL_FOREVER_TERRAIN_SHADOW_*. Called once at load.
    void Install();

    /// Once per frame on the render thread: chains to the engine's render callback when it exists,
    /// applies the light adjuster, rebuilds tile meshes whose heights changed (a couple per frame).
    void Frame(IDirect3DDevice9* dev);

    /// Per-cascade patches and triangles drawn last frame, for the panel.
    struct Stats
    {
        int      passes;           // render callbacks that drew terrain this frame
        int      patches[5];       // by shadows::Slot
        int      triangles;
        int      meshes;           // tile meshes resident
        uint32_t failures;         // draws the device refused (logged once)
        float    lightDir[3];      // the direction the maps were rendered along
    };
    Stats GetStats();
    const char* Status();

    /// Frees the meshes (MANAGED); they rebuild on the next Frame.
    void ReleaseBuffers();
}
