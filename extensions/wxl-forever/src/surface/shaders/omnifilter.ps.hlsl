// wxl-forever surface lighting: the omni shadow share, filtered in space.
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

// The share of the light the omni shadow maps let through (light.ps.hlsl, COLOR1) joins after the
// history, so nothing averages the per-pixel blue-noise turn of its PCSS disc: this pass does, before
// the resolve. A 3 x 3 kernel with holes (step atrousC.x lighting pixels), each tap weighted by how
// close its distance (atrousC.y of it) and its normal (to the power atrousC.z) are, so a shadow never
// crosses to another surface. The share stays this frame's: nothing lingers behind a moving light.
sampler2D normalsTex : register(s1);
sampler2D metaTex : register(s6);
sampler2D ratioTex : register(s7);

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 uv = (vpos + 0.5) * resC.xy;
    float4 centre = tex2Dlod(ratioTex, float4(uv, 0, 0));
    float4 meta = tex2Dlod(metaTex, float4(uv, 0, 0));
    if (meta.y < 0.0) return centre;
    float3 n = tex2Dlod(normalsTex, float4(uv, 0, 0)).xyz;

    float sum = 0.0, total = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            float2 at = uv + float2(x, y) * atrousC.x * resC.xy;
            float m = tex2Dlod(metaTex, float4(at, 0, 0)).y;
            if (m < 0.0) continue;
            float k = (x == 0 ? 0.5 : 0.25) * (y == 0 ? 0.5 : 0.25);
            float wDepth = exp(-abs(m - meta.y) / (atrousC.y * meta.y + 0.02));
            float wNormal = pow(saturate(dot(n, tex2Dlod(normalsTex, float4(at, 0, 0)).xyz)), atrousC.z);
            float w = k * wDepth * wNormal;
            sum += tex2Dlod(ratioTex, float4(at, 0, 0)).r * w;
            total += w;
        }
    return (total > 0.0001 ? sum / total : centre.r).xxxx;
}
