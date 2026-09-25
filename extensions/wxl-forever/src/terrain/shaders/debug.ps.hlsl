// wxl-forever terrain: debug views of the horizon maps and the sun cascades over the scene.
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

#define HORIZON_VOLUME_SLOT s1
#define HORIZON_SKY_SLOT s2
#define HORIZON_HEIGHT_SLOT s3
#define HORIZON_REG_C c12
#define HORIZON_REG_D c13
#include "terrain/shaders/horizon.hlsli"

// Constants (terrain/Terrain.cpp):
//   c0..c3   columns of inverse(view * projection), clip -> camera-relative
//   c4       camera world xyz, view (1 sun visibility, 2 sky occlusion, 3 horizon at the sun's
//            azimuth, 4 cascade coverage, 5 terrain height, 6 resident tiles)
//   c5       world depth range min, 1 / (max - min), max, 1 / screen width
//   c6       direction towards the sun xyz, 1 / screen height
//   c7..c10  one cascade each: centre x, centre y, half-extent, 1 when present
//   c11      block height range min, 1 / (max - min), 0, 0
sampler2D depthTex : register(s0);
float4 inv0 : register(c0);
float4 inv1 : register(c1);
float4 inv2 : register(c2);
float4 inv3 : register(c3);
float4 cam : register(c4);
float4 range : register(c5);
float4 sun : register(c6);
float4 cascades[4] : register(c7);
float4 heights : register(c11);

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 uv = (vpos + 0.5) * float2(range.w, sun.w);
    float depth = tex2Dlod(depthTex, float4(uv, 0, 0)).r;
    if (depth > range.z + 0.00001) return float4(0.1, 0.1, 0.15, 1.0);   // the sky
    float depth01 = saturate((depth - range.x) * range.y);
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth01 * 2.0 - 1.0, 1.0);
    float4 h = float4(dot(clip, inv0), dot(clip, inv1), dot(clip, inv2), dot(clip, inv3));
    float3 world = cam.xyz + h.xyz / h.w;

    int view = (int)(cam.w + 0.5);
    bool valid;
    float2 t = HorizonTileCoord(world, valid);
    if (view == 1)
    {
        float v = TerrainSunVisibility(world, sun.xyz);
        return valid ? float4(v, v, v, 1.0) : float4(0.5, 0.2, 0.2, 1.0);
    }
    if (view == 2)
    {
        float v = TerrainSkyOcclusion(world);
        return valid ? float4(v, v, v, 1.0) : float4(0.5, 0.2, 0.2, 1.0);
    }
    if (view == 3)
    {
        if (!valid) return float4(0.5, 0.2, 0.2, 1.0);
        float e = TerrainHorizon(t, atan2(sun.y, sun.x)) / 1.57079632679;
        // Heat ramp: black flat, blue low, green mid, yellow and red steep.
        float3 c = e < 0.25 ? lerp(float3(0, 0, 0), float3(0, 0, 1), e * 4.0)
                 : e < 0.5 ? lerp(float3(0, 0, 1), float3(0, 1, 0), (e - 0.25) * 4.0)
                 : e < 0.75 ? lerp(float3(0, 1, 0), float3(1, 1, 0), (e - 0.5) * 4.0)
                 : lerp(float3(1, 1, 0), float3(1, 0, 0), (e - 0.75) * 4.0);
        return float4(c, 1.0);
    }
    if (view == 4)
    {
        // Innermost cascade holding the point wins: main red, bands green, blue, yellow; none grey.
        float3 c = float3(0.25, 0.25, 0.25);
        static const float3 tints[4] = { float3(1, 0.2, 0.2), float3(0.2, 1, 0.2), float3(0.2, 0.4, 1), float3(1, 1, 0.2) };
        [unroll] for (int i = 3; i >= 0; --i)
        {
            float4 k = cascades[i];
            if (k.w > 0.5 && all(abs(world.xy - k.xy) <= k.z)) c = tints[i];
        }
        float sh = TerrainSunVisibility(world, sun.xyz) * 0.5 + 0.5;
        return float4(c * sh, 1.0);
    }
    if (view == 5)
    {
        bool ok;
        float z = TerrainHeight(world, ok);
        if (!ok) return float4(0.5, 0.2, 0.2, 1.0);
        float v = saturate((z - heights.x) * heights.y);
        return float4(v, v, v, 1.0);
    }
    // Resident tiles: a checkerboard of tile cells, the block's edge in red.
    if (!valid) return float4(0.5, 0.2, 0.2, 1.0);
    float2 cell = floor(t);
    float parity = fmod(cell.x + cell.y, 2.0);
    float2 f = frac(t);
    float edge = min(min(f.x, 1.0 - f.x), min(f.y, 1.0 - f.y)) < 0.01 ? 1.0 : 0.0;
    bool ok2;
    float z2 = TerrainHeight(world, ok2);
    float3 c = lerp(float3(0.3, 0.3, 0.35), float3(0.6, 0.6, 0.65), parity) * (ok2 ? 1.0 : 0.4);
    return float4(lerp(c, float3(1, 0, 0), edge), 1.0);
}
