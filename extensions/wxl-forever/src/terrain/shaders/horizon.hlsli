// wxl-forever terrain: reading the baked horizon maps (terrain/Horizon.cpp) from a shader.
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

#ifndef WXL_FOREVER_HORIZON_HLSLI
#define WXL_FOREVER_HORIZON_HLSLI

// The horizon service keeps a 4 x 4 block of ADT tiles around the camera resident in three atlases,
// each tile at the cell (cx mod 4, cy mod 4), so the atlases wrap: a tile's texel c, r sits at atlas
// u = (cx + (c + 0.5) / 128) / 4, v likewise with cy, r. World to tile coordinates: tx = 32 - Y / T,
// ty = 32 - X / T, T = 533.3333 (X north, Y west). Only points inside the block are valid; anything
// else, and a cell whose tile is not resident (held neutral: horizon 0, sky 1, height -10000), reads
// as unshadowed. Every function below returns 1 in that case.
//
// A consumer sets the sampler slots before including this file (defaults below), binds the atlases
// with horizon::Bind at those stages (linear, wrap), and uploads horizon::Constants into two float4
// registers named here (defaults c220, c221):
//   horizonC  block origin tile ox, oy; block size (4); strength (0 = off)
//   horizonD  penumbra half-width (radians); azimuth slices (16); occluder distance (yards, how
//             far the horizon's terrain is assumed to be, for points above the ground); 1 when
//             the height atlas is bound
// Volume: slice k = azimuth 2 pi k / K from north (+X) towards west (+Y), value = horizon elevation
// over 90 degrees. Sky: cosine-weighted share of the sky left visible. Height: terrain height, R16F.

#ifndef HORIZON_VOLUME_SLOT
#define HORIZON_VOLUME_SLOT s6
#endif
#ifndef HORIZON_SKY_SLOT
#define HORIZON_SKY_SLOT s7
#endif
#ifndef HORIZON_HEIGHT_SLOT
#define HORIZON_HEIGHT_SLOT s8
#endif
#ifndef HORIZON_REG_C
#define HORIZON_REG_C c220
#endif
#ifndef HORIZON_REG_D
#define HORIZON_REG_D c221
#endif

sampler3D horizonVolume : register(HORIZON_VOLUME_SLOT);
sampler2D horizonSky    : register(HORIZON_SKY_SLOT);
sampler2D horizonHeight : register(HORIZON_HEIGHT_SLOT);
float4 horizonC : register(HORIZON_REG_C);
float4 horizonD : register(HORIZON_REG_D);

static const float kHorizonTile = 533.0 + 1.0 / 3.0;
static const float kHorizonTexels = 128.0;

// Tile coordinates of a world position, and whether it lies inside the resident block.
float2 HorizonTileCoord(float3 worldPos, out bool valid)
{
    float2 t = 32.0 - worldPos.yx / kHorizonTile;
    float2 d = t - horizonC.xy;
    valid = horizonC.w > 0.0 && all(d >= 0.0) && all(d < horizonC.z);
    return t;
}

// Atlas uv of a tile coordinate (the atlases wrap, so no modulo is needed).
float2 HorizonUv(float2 t)
{
    return (t + 0.5 / kHorizonTexels) / horizonC.z;
}

// Terrain height under a world position; valid is false outside the block or where no tile is
// resident (the neutral -10000 is then returned).
float TerrainHeight(float3 worldPos, out bool valid)
{
    float2 t = HorizonTileCoord(worldPos, valid);
    if (!valid || horizonD.w < 0.5) { valid = false; return -10000.0; }
    float h = tex2Dlod(horizonHeight, float4(HorizonUv(t), 0, 0)).r;
    valid = h > -5000.0;
    return h;
}

// Horizon elevation (radians, >= 0) seen from the terrain under worldPos towards a world azimuth
// (radians from north +X towards west +Y), interpolated between the two nearest baked azimuths.
float TerrainHorizon(float2 t, float azimuth)
{
    float w = azimuth / 6.28318530718 + 0.5 / horizonD.y;
    return tex3Dlod(horizonVolume, float4(HorizonUv(t), w, 0)).r * 1.57079632679;
}

// Sun visibility of a world position from the terrain around it: 1 lit, 0 in the terrain's shadow,
// a soft ramp of the penumbra width around the horizon. sunDir points towards the sun (unit). A
// point above the ground lowers the horizon it sees, assuming the occluding terrain sits at the
// occluder distance (horizonD.z); the height atlas supplies the ground when bound.
float TerrainSunVisibility(float3 worldPos, float3 sunDir)
{
    bool valid;
    float2 t = HorizonTileCoord(worldPos, valid);
    if (!valid) return 1.0;
    float azimuth = atan2(sunDir.y, sunDir.x);
    float horizon = TerrainHorizon(t, azimuth);
    if (horizonD.w > 0.5)
    {
        float ground = tex2Dlod(horizonHeight, float4(HorizonUv(t), 0, 0)).r;
        float above = ground > -5000.0 ? max(worldPos.z - ground, 0.0) : 0.0;
        horizon = atan(max(tan(horizon) - above / max(horizonD.z, 1.0), 0.0));
    }
    float elevation = asin(clamp(sunDir.z, -1.0, 1.0));
    float penumbra = max(horizonD.x, 0.001);
    float vis = smoothstep(-penumbra, penumbra, elevation - horizon);
    return lerp(1.0, vis, saturate(horizonC.w));
}

// Share of the sky a world position sees past the terrain: 1 open, towards 0 deep in a gorge. A
// point above the ground sees more of the sky, easing to 1 over the occluder distance.
float TerrainSkyOcclusion(float3 worldPos)
{
    bool valid;
    float2 t = HorizonTileCoord(worldPos, valid);
    if (!valid) return 1.0;
    float sky = tex2Dlod(horizonSky, float4(HorizonUv(t), 0, 0)).r;
    if (horizonD.w > 0.5)
    {
        float ground = tex2Dlod(horizonHeight, float4(HorizonUv(t), 0, 0)).r;
        float above = ground > -5000.0 ? max(worldPos.z - ground, 0.0) : 0.0;
        sky = lerp(sky, 1.0, saturate(above / max(horizonD.z, 1.0)));
    }
    return lerp(1.0, sky, saturate(horizonC.w));
}

#endif
