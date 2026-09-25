// wxl-forever fog: the ground under a point, from the terrain heightfield.
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

#ifndef WXL_FOREVER_FOG_GROUND_HLSLI
#define WXL_FOREVER_FOG_GROUND_HLSLI

#include "fog/shaders/common.hlsli"

// Ground under a camera-relative point from the toroidal heightfield (64 x 64 cells, wrapping):
// (ground z, mean ground z around it, how far inside the measured window it is, 0..1).
sampler2D groundTex : register(s7);

float3 Ground(float3 r)
{
    if (terrainC.w < 0.5) return float3(terrainC.y, terrainC.y, 0.0);
    float inside = saturate((terrainC.z - max(abs(r.x), abs(r.y))) / 16.0);
    if (terrainC.w > 1.5)
        return float3(tex2Dlod(groundTex, float4((cam.xy + r.xy) * terrainC.x / 64.0, 0, 0)).rg, inside);
    float2 c = (cam.xy + r.xy) * terrainC.x - 0.5;
    float2 base = floor(c);
    float2 f = c - base;
    float2 uv = (base + 0.5) / 64.0;
    float2 a = tex2Dlod(groundTex, float4(uv, 0, 0)).rg;
    float2 b = tex2Dlod(groundTex, float4(uv + float2(1.0 / 64.0, 0), 0, 0)).rg;
    float2 d = tex2Dlod(groundTex, float4(uv + float2(0, 1.0 / 64.0), 0, 0)).rg;
    float2 e = tex2Dlod(groundTex, float4(uv + float2(1.0 / 64.0, 1.0 / 64.0), 0, 0)).rg;
    float2 g = lerp(lerp(a, b, f.x), lerp(d, e, f.x), f.y);
    return float3(g, inside);
}

#endif
