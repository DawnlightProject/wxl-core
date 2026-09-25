// wxl-forever fog, froxel pass: integrate.
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

#include "fog/shaders/common.hlsli"

// Integration, second half: front to back up to each froxel, whole blocks first, then the
// slices of its own block.
sampler2D injected : register(s1);
sampler2D blocks : register(s2);

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 tile = floor(vpos / grid.xy);
    float k = tile.y * grid.w + tile.x;
    if (k >= grid.z) return float4(0, 0, 0, 1);
    float2 cell = vpos - tile * grid.xy + 0.5;

    float3 scatter = 0.0;
    float transmittance = 1.0;
    float block = floor(k / 8.0);
    for (int j = 0; j < 16; ++j)
    {
        if ((float)j >= block) break;
        float4 v = tex2Dlod(blocks, float4((float2(j * grid.x, 0.0) + cell) * frame2.zw, 0, 0));
        scatter += transmittance * v.rgb;
        transmittance *= v.a;
    }

    float first = block * 8.0;
    float nearD = SliceStart(first);
    for (int i = 0; i < 8; ++i)
    {
        float s = first + i;
        if (s > k) break;
        float farD = SliceToDist(s + 1.0);
        float4 v = tex2Dlod(injected, float4(AtlasUV(cell, s), 0, 0));
        float sigma = max(v.a, 0.000001);
        float stepT = exp(-sigma * (farD - nearD));
        scatter += transmittance * (v.rgb - v.rgb * stepT) / sigma;
        transmittance *= stepT;
        nearD = farD;
    }
    return float4(scatter, transmittance);
}
