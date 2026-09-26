// wxl-graphics-fog: the built-in inputs. Bodies, missiles, blasts, fires and frost are splatted into
// the wake fluid (a 2D flow at the ground around the player); missiles' tunnels, plumes and fires'
// heat are simulation primitives in the 3D volume.
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

#include "wxl/GraphicsLightsApi.h"

namespace wxl::gfx::fog::inputs
{
    /// Samples the bodies within range of the player and moves the phantoms (the game tick).
    void UpdateBodies(float dt, const float player[3], bool havePlayer, float range);

    /// Samples the missiles in flight and ages their tunnels, and the spell and fire emitters (from the
    /// compute record: the engine advanced the missiles and animated the models by then).
    void UpdateMissiles(const float eye[3], double clock);

    /// The primitives of every input switched on in the settings.
    void Emit(const float eye[3], double clock, const WXL_GfxLight* lights, int lightCount);

    /// This frame's wake splats, two rows of four floats each (shaders/wake.cs.hlsl), nearest to the
    /// centre first; at most cap. Returns how many were written.
    int WakeSplats(const float centre[3], double clock, const WXL_GfxLight* lights, int lightCount, float* out, int cap);

    /// Test bodies for the panel: count walkers spread across a line, walking from `from` to `to` at
    /// speed yards per second, then gone.
    /// The bodies nearest a point that shadow lamps in the fog: per body world feet xyz and height,
    /// at most cap. Returns how many were written.
    int Occluders(const float centre[3], float* rows, int cap);

    void SpawnPhantoms(const float from[3], const float to[3], int count, float speed, float spread);
    void ClearPhantoms();

    int  Bodies();
    int  Phantoms();
    int  Missiles();
    int  Segments();
    int  Shocks();
    int  Plumes();
    int  Fires();
}
