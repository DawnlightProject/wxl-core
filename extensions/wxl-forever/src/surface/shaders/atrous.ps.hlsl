// wxl-forever surface lighting: one edge-aware a-trous iteration over an accumulated light.
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

// A 3 x 3 kernel with holes (step atrousC.x pixels, doubling each iteration), each tap weighted by
// how close its distance is (atrousC.y of the distance), how alike its normal is (to the power
// atrousC.z) and how close its luminance is, measured in the local noise (atrousC.w sigmas of the
// temporal pass's variance). A pixel with a long, settled history is filtered less, and one whose
// history is long and whose neighbourhood no longer varies is left alone: once a still view has
// converged the pass costs its two reads and nothing more.
sampler2D normalsTex : register(s1);
sampler2D colorTex : register(s3);
sampler2D metaTex : register(s6);

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 uv = (vpos + 0.5) * resC.xy;
    float4 centre = tex2Dlod(colorTex, float4(uv, 0, 0));
    float4 meta = tex2Dlod(metaTex, float4(uv, 0, 0));
    if (meta.y < 0.0) return centre;
    if (meta.x > 12.0 && meta.z < 0.0005) return centre;
    float3 n = tex2Dlod(normalsTex, float4(uv, 0, 0)).xyz;
    // A short history (a light that just changed) is filtered wider, since it has averaged little.
    float lumaSigma = (atrousC.w * sqrt(max(meta.z, 0.000001)) + 0.0005) * (1.0 + 2.0 / max(meta.x, 1.0));
    float centreLuma = Luma(centre.rgb);

    float4 sum = 0.0;
    float total = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            float2 at = uv + float2(x, y) * atrousC.x * resC.xy;
            float4 c = tex2Dlod(colorTex, float4(at, 0, 0));
            float4 m = tex2Dlod(metaTex, float4(at, 0, 0));
            if (m.y < 0.0) continue;
            float k = (x == 0 ? 0.5 : 0.25) * (y == 0 ? 0.5 : 0.25);
            float wDepth = exp(-abs(m.y - meta.y) / (atrousC.y * meta.y + 0.02));
            float wNormal = pow(saturate(dot(n, tex2Dlod(normalsTex, float4(at, 0, 0)).xyz)), atrousC.z);
            float wLuma = exp(-abs(Luma(c.rgb) - centreLuma) / lumaSigma);
            float w = k * wDepth * wNormal * wLuma;
            sum += c * w;
            total += w;
        }
    float4 filtered = total > 0.0001 ? sum / total : centre;
    // A settled history needs less filtering: keep more of the centre as it grows.
    return lerp(filtered, centre, saturate((meta.x - 4.0) / 28.0) * 0.5);
}
