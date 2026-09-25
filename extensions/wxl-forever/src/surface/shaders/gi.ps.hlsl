// wxl-forever surface lighting: one bounce of indirect light, at quarter resolution.
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
#include "core/shaders/bluenoise.hlsli"

// Light that surfaces around reflect onto this one: six jittered directions over the hemisphere of
// its normal, each followed to the surface the screen shows there (within the radius, facing this
// point), whose light (the light buffer times its albedo, plus its own glow if it is a light source)
// arrives weakened by the distance. A lamp's warm pool spills onto the walls beside it. The
// directions turn every frame; the temporal and a-trous passes then average them. The bounce's
// colour is held near its luminance and its strength capped, so a warm lamp does not dye what is
// around it. Reads the resolved, full-resolution light buffer.
sampler2D normalsTex : register(s1);
sampler2D sceneTex : register(s2);
sampler2D lightBuffer : register(s3);
sampler2D blueTex : register(s10);
sampler2D gbufferNormals : register(s6);

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 uv = (vpos + 0.5) * quarterC.xy;
    float4 here = SurfaceAt(PassUv(vpos));
    if (here.w < 0.0) return float4(0.0, 0.0, 0.0, 1.0);
    // The half-resolution normal of this texel's own surface point.
    float3 n = normalize(tex2Dlod(normalsTex, float4(TexelOf(PassUv(vpos), 2.0), 0, 0)).xyz);
    float4 j = frac(BlueNoiseAt(blueTex, vpos) + blueRot);
    float3 T = normalize(cross(n, abs(n.z) < 0.9 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0)));
    float3 B = cross(n, T);

    float3 sum = 0.0;
    for (int k = 0; k < 6; ++k)
    {
        // Cosine-weighted directions, spread by the rotated noise.
        float phi = 6.2831853 * (k + j.x) / 6.0;
        float r2 = frac(j.y + k * 0.618034);
        float rr = sqrt(r2);
        float3 dir = T * (cos(phi) * rr) + B * (sin(phi) * rr) + n * sqrt(1.0 - r2);
        float3 q = here.xyz + dir * (giC.y * (0.15 + 0.85 * frac(j.z + k * 0.381966)));
        float4 h = float4(q, 1.0);
        float4 c = float4(dot(h, vp0), dot(h, vp1), dot(h, vp2), dot(h, vp3));
        if (c.w <= 0.1) continue;
        float2 suv = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
        if (any(suv < 0.0) || any(suv > 1.0)) continue;
        float4 s = SurfaceAt(suv);
        if (s.w < 0.0) continue;
        float3 v = s.xyz - here.xyz;
        float len = length(v);
        if (len < 0.05 || len > giC.y * 1.5) continue;
        float facing = dot(n, v / len);
        if (facing <= 0.0) continue;
        float3 sceneS = Linear(tex2Dlod(sceneTex, float4(suv, 0, 0)).rgb);
        float3 light = tex2Dlod(lightBuffer, float4(suv, 0, 0)).rgb;
        // Where the engine's material already added the light, the scene shows it: the albedo
        // estimate divides it out again.
        float added = engineC.x > 0.5 && tex2Dlod(gbufferNormals, float4(suv, 0, 0)).a > 0.75 ? Luma(light) * engineC.y : 0.0;
        float3 lit = light * Albedo(sceneS, added, Luma(sceneS));   // the bounce is smooth: no texture needed
        float3 glow = sceneS * saturate((Luma(sceneS) - 0.7) / 0.3) * giC.z;
        sum += (lit + glow) * facing / (1.0 + len * len / (giC.y * giC.y * 0.25));
    }
    float3 gi = sum / 6.0;

    float giLuma = Luma(gi);
    gi = giLuma + (gi - giLuma) * 0.6;
    gi *= min(1.0, 0.6 / max(giLuma, 0.0001));
    return float4(gi, 1.0);
}
