// wxl-graphics-fog: the terrain block. 8 x 8 ADT tiles around the camera in toroidal atlases of
// 1024 x 1024 texels: the baked grid of 5.tools/forever-bake, where texel column c runs along -Y and
// row r along -X, and texel i of tile t is the vertex at grid coordinate 128 t + i.
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

#ifndef FOG_TERRAIN_HLSLI
#define FOG_TERRAIN_HLSLI

#include "common.hlsli"

static const float kBlockN = float(FOG_BLOCK_N);
static const float kTexelYards = FOG_TEXEL_YARDS;

// World xy -> grid coordinates (texels of the whole map, vertices on integers).
float2 WorldToGrid(float2 xy)
{
    return (32.0 * FOG_TILE_YARDS - xy.yx) / kTexelYards;
}

float2 GridToWorld(float2 g)
{
    return (32.0 * FOG_TILE_YARDS - g.yx * kTexelYards);
}

float2 GridUv(float2 g) { return (g + 0.5) / kBlockN; }

// Whether the tile holding a grid coordinate lies in the block and is resident.
bool TileResident(float2 g)
{
    int2 t = int2(floor(g / float(FOG_TILE_TEXELS)));
    int2 rel = t - int2(block.xy);
    if (any(rel < 0) || any(rel >= FOG_BLOCK_TILES)) return false;
    uint bit = uint((t.y & 7) * 8 + (t.x & 7));
    uint word = bit < 32u ? asuint(blockMask.x) : asuint(blockMask.y);
    return ((word >> (bit & 31u)) & 1u) != 0u;
}

// Whether the tile holding a grid coordinate starts over this step (blockInit's bits, or all of them).
bool TileInit(float2 g)
{
    if (blockInit.z > 0.5) return true;
    int2 t = int2(floor(g / float(FOG_TILE_TEXELS)));
    uint bit = uint((t.y & 7) * 8 + (t.x & 7));
    uint word = bit < 32u ? asuint(blockInit.x) : asuint(blockInit.y);
    return ((word >> (bit & 31u)) & 1u) != 0u;
}

// The world grid coordinate a block texel holds (the tile congruent to it inside the block).
float2 TexelToGrid(int2 texel)
{
    int2 tile = texel >> 7;
    int2 o = int2(block.xy);
    int2 t = o + ((tile - (o & 7)) & 7);
    return float2(t * FOG_TILE_TEXELS + (texel & (FOG_TILE_TEXELS - 1)));
}

// The fog's floor (terrain or the water above it): mean in r, max in g, at a mip; the fallback
// height outside the resident block.
float2 FloorAt(Texture2D<float2> floorTex, float2 xy, float mip)
{
    float2 g = WorldToGrid(xy);
    if (blockMask.w < 0.5 || !TileResident(g)) return block.w;
    return floorTex.SampleLevel(sLinearWrap, GridUv(g), mip);
}

// The floor's gradient (dz per yard along world x, y) at a mip, by central differences.
float2 FloorGradient(Texture2D<float2> floorTex, float2 xy, float mip)
{
    float e = kTexelYards * exp2(mip);
    float dx = FloorAt(floorTex, xy + float2(e, 0.0), mip).r - FloorAt(floorTex, xy - float2(e, 0.0), mip).r;
    float dy = FloorAt(floorTex, xy + float2(0.0, e), mip).r - FloorAt(floorTex, xy - float2(0.0, e), mip).r;
    return float2(dx, dy) / (2.0 * e);
}

// Share of the sky the terrain leaves visible (the baked cosine-weighted share); 1 off the block.
float SkyShareAt(Texture2D<float> skyTex, float2 xy)
{
    float2 g = WorldToGrid(xy);
    if (blockMask.w < 0.5 || !TileResident(g)) return 1.0;
    return skyTex.SampleLevel(sLinearWrap, GridUv(g), 0);
}

#endif
