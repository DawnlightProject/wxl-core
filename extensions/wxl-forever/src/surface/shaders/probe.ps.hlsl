// wxl-forever surface lighting: the self-check probe.
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

// A 192 x 37 read-back of the lighting, 8 bits a channel. Light values are log-encoded,
// Enc(x) = log2(1 + 64 x) / 10, so 0 to 16 fit without clipping (Surface.cpp decodes them).
// Rows 0..35 are three blocks of the same 64 x 36 grid over the screen, alpha 0 on the sky:
//   block 0: r the resolved buffer's luminance, g what apply adds (albedo times direct plus
//            indirect light, times the strength), b the surface distance / 256, a (1 + lights of
//            the cluster that reach the point) / 32;
//   block 1: r the raw light pass, g its temporal accumulation, b the denoised light (luminances);
//   block 2: r the indirect light's luminance, g the omni shadow share, b the distance the lighting
//            passes reconstructed at that texel (their meta) / 256, a the least share any omni slot
//            lets through there (the omni pass itself, before any light takes it; 1 without maps).
// Row 36 decodes three points (screen centre, lower centre, left), three pixels each: their
// camera-relative position / 64 + 0.5 and distance / 256; the index + 1 / 255 of the light reaching
// them most (unshadowed), its encoded share, the count / 32, the encoded buffer luminance there; the
// lighting passes' distance / 256, the raw depth as two bytes, the depth within the world range.
sampler2D ratioTex : register(s1);
sampler2D sceneTex : register(s2);
sampler2D lightBuffer : register(s3);
sampler2D giTex : register(s4);
sampler2D rawTex : register(s5);
sampler2D accTex : register(s6);
sampler2D denoisedTex : register(s7);
sampler2D lightTex : register(s8);
sampler2D clusterTex : register(s9);
sampler2D metaTex : register(s12);
sampler2D omniVisTex : register(s13);

float Enc(float x) { return saturate(log2(1.0 + 64.0 * max(x, 0.0)) / 10.0); }

// The lights of the point's cluster that reach it: how many, and the strongest (unshadowed).
void Reach(float2 uv, float4 here, out float count, out float best, out float bestShare)
{
    count = 0.0;
    best = -1.0;
    bestShare = 0.0;
    float2 list = LightList(clusterTex, uv, here.w, clusterC, clusterD);
    [loop] for (int i = 0; i < 128; ++i)
    {
        if ((float)i >= list.y) break;
        float index = LightAt(clusterTex, list.x + i, clusterC, clusterD);
        float4 a, b, c;
        ReadLight(lightTex, index, a, b, c);
        float4 shape = ReadLightShape(lightTex, index);
        float3 v = here.xyz - LightPoint(here.xyz, a, c, shape);
        float d2 = dot(v, v);
        float radius = abs(a.w);
        if (d2 >= radius * radius) continue;
        count += 1.0;
        float share = Luma(b.rgb) * LightFalloffSoft(d2, radius, c.w, albedoC.y, 0.0);
        if (share > bestShare) { bestShare = share; best = index; }
    }
}

float4 Point(float2 vpos)
{
    float k = floor(vpos.x / 3.0);
    if (k > 2.5) return 0.0;
    float2 uv = k < 0.5 ? float2(0.5, 0.5) : (k < 1.5 ? float2(0.5, 0.8) : float2(0.2, 0.6));
    float4 here = SurfaceAt(uv);
    if (here.w < 0.0) return 0.0;
    float part = vpos.x - k * 3.0;
    if (part < 0.5) return float4(saturate(here.xyz / 64.0 + 0.5), saturate(here.w / 256.0));
    if (part < 1.5)
    {
        float count, best, share;
        Reach(uv, here, count, best, share);
        return float4((best + 1.0) / 255.0, Enc(share), count / 32.0, Enc(Luma(tex2Dlod(lightBuffer, float4(uv, 0, 0)).rgb)));
    }
    float depth = tex2Dlod(depthTex, float4(uv, 0, 0)).r;
    float d16 = floor(saturate(depth) * 65535.0 + 0.5);
    float hi = floor(d16 / 256.0);
    return float4(saturate(tex2Dlod(metaTex, float4(uv, 0, 0)).y / 256.0), hi / 255.0, (d16 - hi * 256.0) / 255.0,
                  saturate((depth - range.x) * range.y));
}

float4 main(float2 vpos : VPOS) : COLOR0
{
    if (vpos.y > 35.5) return Point(vpos);
    float block = floor(vpos.x / 64.0);
    float2 uv = (float2(vpos.x - block * 64.0, vpos.y) + 0.5) * resC.xy;
    float4 here = SurfaceAt(uv);
    if (here.w < 0.0) return 0.0;
    if (block > 1.5)
    {
        float gi = giC.x > 0.0 ? Luma(tex2Dlod(giTex, float4(uv, 0, 0)).rgb) : 0.0;
        float ratio = movingC.w > 0.5 ? tex2Dlod(ratioTex, float4(uv, 0, 0)).r : 1.0;
        float4 slots = movingC.w > 0.5 ? tex2Dlod(omniVisTex, float4(uv, 0, 0)) : 1.0;
        float least = min(min(slots.x, slots.y), min(slots.z, slots.w));
        return float4(Enc(gi), saturate(ratio), saturate(tex2Dlod(metaTex, float4(uv, 0, 0)).y / 256.0), saturate(least));
    }
    if (block > 0.5)
        return float4(Enc(Luma(tex2Dlod(rawTex, float4(uv, 0, 0)).rgb)), Enc(Luma(tex2Dlod(accTex, float4(uv, 0, 0)).rgb)),
                      Enc(Luma(tex2Dlod(denoisedTex, float4(uv, 0, 0)).rgb)), 1.0);
    float3 light = tex2Dlod(lightBuffer, float4(uv, 0, 0)).rgb;
    float3 gi = giC.x > 0.0 ? tex2Dlod(giTex, float4(uv, 0, 0)).rgb * giC.x : 0.0;
    float3 scene = Linear(tex2Dlod(sceneTex, float4(uv, 0, 0)).rgb);
    float3 added = Albedo(scene, 0.0, LocalLuma(sceneTex, uv, Luma(scene))) * (light + gi) * lightsC.z;
    float count, best, share;
    Reach(uv, here, count, best, share);
    return float4(Enc(Luma(light)), Enc(Luma(added)), saturate(here.w / 256.0), (1.0 + count) / 32.0);
}
