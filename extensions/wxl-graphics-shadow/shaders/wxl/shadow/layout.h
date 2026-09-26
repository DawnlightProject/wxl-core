// wxl-graphics-shadow: the binding set and uniform rows shadow.hlsli reads, shared by the service (C++)
// and every shader that includes it (HLSL through DXC). Macros only, so both languages read it.
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

#ifndef WXL_SHADOW_LAYOUT_H
#define WXL_SHADOW_LAYOUT_H

// --- the binding set, offsets from the consumer's first binding (WXL_SHADOW_BINDING) ------------------
#define WXL_SHADOW_B_CONSTANTS   0   // uniform buffer: the rows below
#define WXL_SHADOW_B_POINT       1   // sampler: point, clamp (gathers)
#define WXL_SHADOW_B_LINEAR      2   // sampler: linear, clamp, trilinear (the filtered maps, the horizon)
#define WXL_SHADOW_B_MAPS        3   // Texture2D<float4>: every filtered cube map in one atlas (EVSM moments)
#define WXL_SHADOW_B_CASCADE0    4   // Texture2D<float>: the engine's sun maps, main then bands 0..2
#define WXL_SHADOW_B_HORIZON     8   // Texture3D<float>: terrain horizon, 16 azimuths
#define WXL_SHADOW_B_HEIGHTS     9   // Texture2D<float>: terrain height under the horizon block
#define WXL_SHADOW_B_COUNT       10

// --- the filtered map atlas ------------------------------------------------------------------------------
// Map m occupies the rectangle (m % 2, m / 2) of a 2 x 4 grid; inside it face f (+X -X +Y -Y +Z -Z) is the
// cell (f % 3, f / 3) of a 3 x 2 grid of face x face texels. So the atlas is 6 faces wide, 8 faces high.
#define WXL_SHADOW_GRID_COLS     2
#define WXL_SHADOW_GRID_ROWS     4
#define WXL_SHADOW_FACE_COLS     3
#define WXL_SHADOW_FACE_ROWS     2
#define WXL_SHADOW_ATLAS_FACES_X 6
#define WXL_SHADOW_ATLAS_FACES_Y 8
#define WXL_SHADOW_MIPS          5

// --- the uniform block: float4 rows -------------------------------------------------------------------------
#define WXL_SHADOW_ROW_FRAME       0    // eye xyz (world), enabled (0: every lookup returns 1)
#define WXL_SHADOW_ROW_SUN         1    // towards the sun xyz (world), its weight
#define WXL_SHADOW_ROW_MOON        2    // towards the moon xyz, its weight
#define WXL_SHADOW_ROW_CASCADE     3    // maps bound, body the maps were rendered for (0 sun, 1 moon, -1 none), 1 / size, filter texels
#define WXL_SHADOW_ROW_CASCADEDIR  4    // towards the light the maps were rendered along xyz, depth per yard
#define WXL_SHADOW_ROW_CASCADEROWS 5    // 4 maps x 3 rows: (rel, 1) -> (u', v', depth), uv = u'v' * 0.5 + 0.5
#define WXL_SHADOW_ROW_CASCADEPAR  17   // 4 maps: half extent yards, yards per texel, depth bias yards, 0
#define WXL_SHADOW_ROW_HORIZON     21   // block origin tile x, y; strength (0 off); penumbra radians
#define WXL_SHADOW_ROW_HORIZON2    22   // occluder yards; heights bound (1); azimuths; 0
#define WXL_SHADOW_ROW_BIAS        23   // normal offset texels, slope scale, EVSM minimum variance, light bleeding reduction
#define WXL_SHADOW_ROW_CAPSULE     24   // capsules, penumbra scale, self margin yards, 0
#define WXL_SHADOW_ROW_EVSM        25   // positive exponent, negative exponent, lod bias, softness scale
#define WXL_SHADOW_ROW_ATLAS       26   // 1 / atlas width, 1 / atlas height, face texels, mips
#define WXL_SHADOW_ROW_CAPSULES    27   // 32 capsules x 2 rows: (a rel xyz, radius) (b rel xyz, 0)
#define WXL_SHADOW_ROW_SLOTS       91   // 16 slots x 4 rows, see below
#define WXL_SHADOW_ROW_MAPS        155  // 8 maps x 2 rows, see below
#define WXL_SHADOW_ROW_INDEX       171  // 32 rows of uint4: list index i -> slot + 1 (0 none) at row i / 4, component i % 4
#define WXL_SHADOW_ROWS            203

#define WXL_SHADOW_MAX_SLOTS     16
#define WXL_SHADOW_MAX_MAPS      8
#define WXL_SHADOW_MAX_CAPSULES  32
#define WXL_SHADOW_MAX_CASCADES  4
#define WXL_SHADOW_MAX_INDEX     128
#define WXL_SHADOW_MASK_LAYERS   5    // layer 0 the sun and the moon, 1..4 the slots

// A slot's rows:
//   +0 light position rel xyz, radius (0: the slot is free)
//   +1 spot axis xyz, cos of the half-angle (<= -1 a point light)
//   +2 weight, map share, source size yards, map index (-1 none)
//   +3 capsule mask (asuint), carrier capsule (-1 none), receiver margin yards, contact length yards
#define WXL_SHADOW_SLOT_ROWS 4

// A map's rows:
//   +0 light position rel xyz, radius (0: the map holds nothing)
//   +1 atlas u, v of its rectangle's corner; 1 when valid, 0 while it fills; 0
// Inside the rectangle, face f's texel for a direction v from the light (axis a = f / 2, positive
// when f is even) is ((v[(a + 1) % 3], v[(a + 2) % 3]) / |v[a]|) * 0.5 + 0.5 of its cell. A texel holds
// the EVSM moments (exp(p x), exp(p x)^2, -exp(-n x), exp(-n x)^2) of x = 2 d - 1, d the nearest
// caster's depth along the face's axis over the radius (1 where nothing casts).
#define WXL_SHADOW_MAP_ROWS_EACH 2

#endif
