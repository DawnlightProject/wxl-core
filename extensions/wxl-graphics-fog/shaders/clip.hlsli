// wxl-graphics-fog: clipmap addressing. Four camera-centred levels, each FOG_LEVEL_N x FOG_LEVEL_N x
// FOG_LEVEL_NZ cells, stacked along z in one image (level L owns layers L * NZ .. L * NZ + NZ - 1).
// x and y are toroidal (world cell i lives at texel i mod N); z is the height above the level's ground.
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

#ifndef FOG_CLIP_HLSLI
#define FOG_CLIP_HLSLI

#include "common.hlsli"

static const float kN = float(FOG_LEVEL_N);
static const float kNZ = float(FOG_LEVEL_NZ);
static const float kLevels = float(FOG_LEVELS);

float CellXY(uint L) { return levelA[L].x; }
float CellZ(uint L) { return levelA[L].y; }
float HMin(uint L) { return levelA[L].z; }
float HMax(uint L) { return levelA[L].z + levelA[L].y * kNZ; }

// The world centre of a level's cell (x, y from the texel through the window origin; z unused).
float2 CellCentreXY(uint L, int2 texel, float2 origin)
{
    // The window holds cells origin .. origin + N - 1; texel = cell mod N.
    int2 o = int2(origin);
    int2 cell = o + ((texel - (o & (FOG_LEVEL_N - 1))) & (FOG_LEVEL_N - 1));
    return (float2(cell) + 0.5) * CellXY(L);
}

float CellCentreH(uint L, int k) { return HMin(L) + (float(k) + 0.5) * CellZ(L); }

// Texture coordinates of a world xy for a level (wrapping in x and y).
float2 LevelUv(uint L, float2 xy) { return xy * levelA[L].w / kN; }

// The stacked w coordinate of a height above ground, clamped inside the level's own layers.
float LevelW(uint L, float h)
{
    float k = clamp((h - HMin(L)) / CellZ(L), 0.5, kNZ - 0.5);
    return (float(L) * kNZ + k) / (kLevels * kNZ);
}

// The level's ground height under a world xy (bilinear over its cell centres).
float LevelGround(Texture3D<float> groundTex, uint L, float2 xy)
{
    return groundTex.SampleLevel(sLinearWrap, float3(LevelUv(L, xy), (float(L) + 0.5) / kLevels), 0);
}

// How far inside the level's current window a world xy lies, in cells (negative outside).
float LevelInside(uint L, float2 xy)
{
    float2 c = xy * levelA[L].w;
    float2 lo = c - levelB[L].xy;
    float2 hi = levelB[L].xy + kN - c;
    return min(min(lo.x, lo.y), min(hi.x, hi.y));
}

// The same against the window the level had before its last step.
float LevelInsidePrev(uint L, float2 xy)
{
    float2 c = xy * levelA[L].w;
    float2 lo = c - levelB[L].zw;
    float2 hi = levelB[L].zw + kN - c;
    return min(min(lo.x, lo.y), min(hi.x, hi.y));
}

// How far inside the level's height range h lies, in layers.
float LevelInsideH(uint L, float h)
{
    float k = (h - HMin(L)) / CellZ(L);
    return min(k, kNZ - k);
}

// The level's share of a sample at (xy, h): 1 well inside, 0 outside, a band of `band` cells at
// the window's edge and 2 layers at its top where the next level takes over.
float LevelWeight(uint L, float2 xy, float h, float band)
{
    float edge = saturate((LevelInside(L, xy) - 2.0) / band);
    float top = saturate((kNZ - (h - HMin(L)) / CellZ(L) - 1.5) / 2.0);
    float bottom = h >= HMin(L) ? 1.0 : 0.0;
    return edge * top * bottom;
}

// One level's fog and smoke at a world point, h its height above that level's ground.
float2 SampleLevel(Texture3D<float2> state, uint L, float2 xy, float h)
{
    return state.SampleLevel(sLinearWrap, float3(LevelUv(L, xy), LevelW(L, h)), 0);
}

// Toroidal texel of a world cell index for a level: (x, y) wrapped, z the stacked layer.
int3 StateTexel(uint L, int2 cell, int k)
{
    return int3(cell & (FOG_LEVEL_N - 1), int(L) * FOG_LEVEL_NZ + clamp(k, 0, FOG_LEVEL_NZ - 1));
}

#endif
