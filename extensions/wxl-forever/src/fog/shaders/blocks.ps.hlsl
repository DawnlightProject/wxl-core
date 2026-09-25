// wxl-forever fog, froxel pass: blocks.
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

// Integration, first half: each column's slices in blocks of eight, integrated on their own
// (in-scatter, transmittance), so the second half composes at most a few blocks.
sampler2D injected : register(s1);

float4 main(float2 vpos : VPOS) : COLOR0
{
    float b = floor(vpos.x / grid.x);
    if (b >= frame2.y) return float4(0, 0, 0, 1);
    float2 cell = float2(vpos.x - b * grid.x, vpos.y) + 0.5;

    // Energy-conserving step (Hillaire): scattering integrated analytically across each slice.
    float3 scatter = 0.0;
    float transmittance = 1.0;
    float first = b * 8.0;
    float nearD = SliceStart(first);
    for (int i = 0; i < 8; ++i)
    {
        float k = first + i;
        if (k >= grid.z) break;
        float farD = SliceToDist(k + 1.0);
        float4 v = tex2Dlod(injected, float4(AtlasUV(cell, k), 0, 0));
        float sigma = max(v.a, 0.000001);
        float stepT = exp(-sigma * (farD - nearD));
        scatter += transmittance * (v.rgb - v.rgb * stepT) / sigma;
        transmittance *= stepT;
        nearD = farD;
    }
    return float4(scatter, transmittance);
}
