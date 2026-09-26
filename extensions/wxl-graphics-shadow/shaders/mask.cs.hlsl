// wxl-graphics-shadow: the surface masks. Per traced pixel, from its position and normal: the sun and
// the moon (terrain horizon x cascades x contact), then every slot (its filtered map or its capsules,
// times a contact shadow for the most important), with shadow.hlsli's own functions.
//   out0  masks (layer 0 sun, moon, terrain, contact; layers 1..4 the slots), full or half resolution
//   out1  the traced pixel's view depth (half resolution only, for the upsample)
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

#include "common.hlsli"

[[vk::binding(SH_B_OUT0, 0)]] [[vk::image_format("rgba8")]] RWTexture3D<float4> outMask;
[[vk::binding(SH_B_OUT1, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> outDepth;

// A short march from p towards `to` (camera-relative) through the depth buffer. No jitter: the samples
// are spaced evenly along the ray's image on screen, about one every 1.5 pixels (at least 4, at most
// maxSteps), so a thin occluder -- a leg, a post, a blade of grass -- is never stepped over by some
// receivers and hit by their neighbours, which is what drew combs of copies of it. A sample occludes
// when the scene lies in front of the ray point by more than a small bias and less than the thickness,
// and not on the receiver's own plane (which is what removes self-intersection stripes on grazing
// surfaces). Returns 1 lit.
float Contact(float3 p, float3 n, float3 to, int maxSteps)
{
    float4 C = P[SH_ROW_CONTACT];
    if (C.w <= 0.0 || maxSteps <= 0) return 1.0;
    float3 ray = to - p;
    float len = length(ray);
    if (len < 0.01) return 1.0;
    float3 dir = ray / len;
    // Surfaces facing away are dark from the light anyway; the march would only add noise there.
    if (dot(n, dir) <= 0.02) return 1.0;
    float dist = length(p);
    float3 start = p + n * (0.015 + 0.0015 * dist);
    float4 c0 = ShClip(start);
    float4 c1 = ShClip(start + ray);
    if (c0.w <= 0.05) return 1.0;
    // A ray running towards the camera stops at the near plane.
    float reach = 1.0;
    if (c1.w < 0.05)
    {
        reach = (c0.w - 0.05) / max(c0.w - c1.w, 1e-6);
        c1 = lerp(c0, c1, reach);
    }
    float2 size = P[SH_ROW_SCREEN].xy;
    float2 uv0 = ShClipToUv(c0), uv1 = ShClipToUv(c1);
    float pixels = length((uv1 - uv0) * size);
    int steps = clamp(int(ceil(pixels / 1.5)), 4, maxSteps);
    float iw0 = 1.0 / c0.w, iw1 = 1.0 / c1.w;
    float thickness = C.z;
    float occ = 0.0;
    [loop] for (int i = 1; i <= steps; ++i)
    {
        // Even on screen; the ray point's view depth follows perspective (1 / w is linear on screen).
        float s = float(i) / float(steps);
        float2 uv = lerp(uv0, uv1, s);
        if (any(uv <= 0.0) || any(uv >= 1.0)) break;
        float w = 1.0 / lerp(iw0, iw1, s);
        float t = s * iw1 * w * reach;   // the share of the ray walked, in yards over its length
        int2 px = int2(uv * size);
        float d = depthTex.Load(int3(px, 0));
        if (ShIsSky(d)) continue;
        float ndc = ShNdc(d);
        float zs = ShLinear(ndc);
        float delta = w - zs;
        // The receiver's own surface, seen again further along: never an occluder.
        float3 ps = ShRel((float2(px) + 0.5) / size, ndc);
        float tolerance = 0.03 + 0.004 * zs;
        if (abs(dot(ps - p, n)) < tolerance) continue;
        float bias = 0.01 + 0.0015 * w;
        float o = saturate((delta - bias) / (0.04 + 0.006 * w)) * saturate((thickness - delta) / (0.3 * thickness));
        occ = max(occ, o * (1.0 - 0.5 * t));
    }
    // Fades out in the distance and near the screen's edges.
    float2 uvp = ShClipToUv(ShClip(p));
    float edge = saturate(min(min(uvp.x, 1.0 - uvp.x), min(uvp.y, 1.0 - uvp.y)) * 20.0);
    float far = 1.0 - smoothstep(P[SH_ROW_CONTACT2].w * 0.6, P[SH_ROW_CONTACT2].w, dist);
    return 1.0 - occ * C.w * edge * far;
}

bool IsContactSlot(int s, out int k)
{
    float4 c = P[SH_ROW_CSLOTS];
    k = int(c.x) == s ? 0 : (int(c.y) == s ? 1 : (int(c.z) == s ? 2 : (int(c.w) == s ? 3 : -1)));
    return k >= 0;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float4 T = P[SH_ROW_TRACE];
    if (any(float2(id.xy) >= T.xy)) return;
    float2 size = P[SH_ROW_SCREEN].xy;
    int2 px = int2(id.xy) * int(T.z);
    float d = depthTex.Load(int3(px, 0));
    if (ShIsSky(d) || !ShadowEnabled())
    {
        [unroll] for (uint l = 0u; l < WXL_SHADOW_MASK_LAYERS; ++l) outMask[uint3(id.xy, l)] = 1.0;
        if (T.z > 1.5) outDepth[id.xy] = 1e6;
        return;
    }
    float ndc = ShNdc(d);
    float3 p = ShRel((float2(px) + 0.5) / size, ndc);
    float3 n = ShNormal(px, p);
    if (T.z > 1.5) outDepth[id.xy] = ShLinear(ndc);

    // The sun and the moon; contact towards the one that shines.
    float4 C2 = P[SH_ROW_CONTACT2];
    int body = int(C2.z);
    float3 towards = body == 1 ? ShadowToMoon() : ShadowToSun();
    float terrain = body >= 0 ? ShadowTerrain(p, towards) : 1.0;
    float contact = body >= 0 ? Contact(p, n, p + towards * P[SH_ROW_CONTACT].x, int(C2.x)) : 1.0;
    float sun = ShadowSunWeight() > 0.001 ? ShadowSun(p, n) : 1.0;
    float moon = ShadowMoonWeight() > 0.001 ? ShadowMoon(p, n) : 1.0;
    if (body == 0) sun *= contact;
    if (body == 1) moon *= contact;
    outMask[uint3(id.xy, 0)] = float4(sun, moon, terrain, contact);

    // The slots, four to a layer.
    [loop] for (uint layer = 0u; layer < 4u; ++layer)
    {
        float4 v = 1.0;
        [unroll] for (uint c = 0u; c < 4u; ++c)
        {
            int s = int(layer * 4u + c);
            float vis = ShadowLight(s, p, n);
            int k;
            if (vis > 0.01 && IsContactSlot(s, k))
            {
                int base = WXL_SHADOW_ROW_SLOTS + s * WXL_SHADOW_SLOT_ROWS;
                float4 S0 = wxlShadow[base];
                float3 toLight = S0.xyz - p;
                float dl = length(toLight);
                if (dl < S0.w && !WxlShadowInsideCapsule(p, int(wxlShadow[base + 3].y), wxlShadow[base + 3].z))
                {
                    float reach = min(P[SH_ROW_CONTACT].y, 0.5 * dl);
                    float cs = Contact(p, n, p + toLight / max(dl, 1e-4) * reach, int(C2.y));
                    vis *= lerp(1.0, cs, saturate(wxlShadow[base + 2].x));
                }
            }
            v[c] = vis;
        }
        outMask[uint3(id.xy, layer + 1u)] = v;
    }
}
