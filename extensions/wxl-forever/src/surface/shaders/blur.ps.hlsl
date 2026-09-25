// wxl-forever surface lighting: one direction of the normals' bilateral blur, at half resolution.
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

#include "surface/shaders/surface.hlsli"

// A Gaussian along blurC.xy (one half-resolution texel per tap), each tap weighted by how close its
// distance is to this texel's (a different object does not bleed in) and by how close its normal is
// (below the crease angle, cos blurC.w, it counts not at all). Facets of a character, a few degrees
// apart, melt into a smooth surface; the corner of a wall stays sharp.
sampler2D normalsTex : register(s1);

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 uv = (vpos + 0.5) * halfC.xy;
    float4 centre = tex2Dlod(normalsTex, float4(uv, 0, 0));
    if (centre.w < 0.0) return centre;

    float3 sum = centre.xyz;
    float total = 1.0;
    float sigma = max(blurC.z * 0.5, 0.5);
    for (int i = 1; i <= 6; ++i)
    {
        if ((float)i > blurC.z) break;
        float g = exp(-(i * i) / (2.0 * sigma * sigma));
        for (int side = 0; side < 2; ++side)
        {
            float2 at = uv + blurC.xy * halfC.xy * (side == 0 ? i : -i);
            float4 s = tex2Dlod(normalsTex, float4(at, 0, 0));
            if (s.w < 0.0) continue;
            float near = exp(-abs(s.w - centre.w) / (0.02 * centre.w + 0.05));
            float alike = saturate((dot(s.xyz, centre.xyz) - blurC.w) / max(1.0 - blurC.w, 0.001));
            float w = g * near * alike;
            sum += s.xyz * w;
            total += w;
        }
    }
    return float4(normalize(sum / total), centre.w);
}
