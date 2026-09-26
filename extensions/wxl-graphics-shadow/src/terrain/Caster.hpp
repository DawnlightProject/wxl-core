// wxl-graphics-shadow: the terrain as a caster in the engine's sun shadow maps (ported from wxl-forever).
// The engine draws WMOs, doodads and units into its cascades but never the terrain, so hills cast nothing.
// After each of its cascade renders (core shadows::ChainAfter) this draws a heightfield mesh of the
// resident horizon tiles (4.17 yd per vertex, 129 x 129 per tile with the neighbour's seam row) into
// the same map with the pass's own light view, projection and viewport (shadows::DescribeRender),
// writing what the engine's caster shader writes. Patches of 32 x 32 cells are culled against the
// pass's rectangle and drawn at a step that grows with the cascade. Every D3D state touched is read
// back and put back.
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

namespace wxl::gfx::shadow::caster
{
    /// Once per frame on the render thread, before the world pass: chains to the engine's render
    /// callback when it exists and rebuilds tile meshes whose heights changed (a couple per frame).
    void Frame(IDirect3DDevice9* dev);

    struct Stats
    {
        int      passes;           // render callbacks that drew terrain this frame
        int      patches[5];       // by shadows::Slot
        int      triangles;
        int      meshes;           // tile meshes resident
        uint32_t failures;         // draws the device refused
    };
    Stats GetStats();
    const char* Status();

    /// Frees the meshes and shaders; they come back on the next Frame.
    void Release();
}
