// wxl-forever lights: reading a light's baked cookie (lights/Cookies.cpp) from a shader.
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

#ifndef WXL_FOREVER_COOKIES_HLSLI
#define WXL_FOREVER_COOKIES_HLSLI

#include "lights/shaders/lights.hlsli"

// Per light, two texels of the cluster texture past the per-cluster texels (Clusters.cpp): row A the
// rotation from a world direction into the model's frame as a quaternion (xyzw), row B the cookie's
// cell code (atlas cell + 1, 0 = not resident, plus 128 times the share its pattern has faded in over
// its mean, on 8 bits) and its mean transmittance rgb, the glass
// tint taken by cookieE.x (0 when the light has no cookie known). The atlas (lights::cookies::Bind,
// L8 or X8R8G8B8 when any cookie carries its glass's tint, bilinear, clamped) holds each cookie as a
// 4 x 2 grid of cube faces, +X -X +Y -Y +Z -Z in D3D order, the omni shadow layout, four cookies to an
// atlas row (kCookiesAcross, lights::cookies kCellsAcross). Constants (lights::cookies::Constants):
//   cookieC  face width, face height (atlas uv), strength (0 = cookies off), floor
//   cookieD  half a texel in u, half a texel in v, debug view (1 = show the factor), share of the
//            cookie a flame light is spared (1 - the share it takes; applied by the callers)
//   cookieE  share of a texel's glass colour a light takes (0 its luminance only), 0, 0, 0
static const float kCookiesAcross = 4.0;

float4 CookieRow(sampler2D clusters, float index, float row, float4 clusterC, float4 clusterD)
{
    float perBlock = ceil(kLightCapacity / clusterC.w);        // texel rows one attribute takes
    return ClusterTexel(clusters, index, ClusterGridRows(clusterC) + row * perBlock, clusterC, clusterD);
}

float3 QuatRotate(float4 q, float3 v)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

// Atlas uv of a direction in the model's frame, inside the cookie cell whose origin is cell.
float2 CookieUv(float3 d, float2 cell, float4 cookieC, float4 cookieD)
{
    // The major axis as a mask, its sign, then D3D's cube layout: the face (+X -X +Y -Y +Z -Z), s
    // (-+z on x, x on y, +-x on z) and t (+-z on y, -y otherwise). Branch-free: few registers.
    float3 a = abs(d);
    float3 m = a.x >= a.y && a.x >= a.z ? float3(1.0, 0.0, 0.0) : (a.y >= a.z ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0));
    float sgn = dot(d, m) > 0.0 ? 1.0 : -1.0;
    float f = dot(m, float3(0.0, 2.0, 4.0)) + (sgn > 0.0 ? 0.0 : 1.0);
    float sc = m.x * (-sgn * d.z) + m.y * d.x + m.z * (sgn * d.x);
    float tc = m.y * (sgn * d.z) - (1.0 - m.y) * d.y;
    float2 uv = float2(sc, tc) / max(dot(a, m), 1e-5) * 0.5 + 0.5;
    // Half a texel inside the face, so bilinear filtering never reads the neighbouring face.
    float2 inset = cookieD.xy / cookieC.xy;
    uv = clamp(uv, inset, 1.0 - inset);
    float2 faceOrigin = float2(fmod(f, 4.0), floor(f * 0.25));
    return cell + (faceOrigin + uv) * cookieC.xy;
}

// The cookie's transmittance rgb for the light at index towards fromLight, the unit direction from
// the light to the point it lights (world axes; camera-relative vectors are the same directions),
// floored and scaled by strength per channel; the light's published colour carries no glass tint, so
// this is its only colour. A light without a cookie returns 1 after one fetch; one whose cookie is not
// resident (streaming in, or evicted) takes its mean, the same colour and energy without the bars,
// and a pattern that just became resident fades in over that mean.
// has reports whether the light carries a cookie (so a caller can drop an analytic cage profile).
float3 LightCookie(sampler2D clusters, sampler2D atlas, float index, float3 fromLight,
                   float4 clusterC, float4 clusterD, float4 cookieC, float4 cookieD, float4 cookieE, out float has)
{
    has = 0.0;
    if (cookieC.z <= 0.0) return 1.0;
    float4 b = CookieRow(clusters, index, 1.0, clusterC, clusterD);
    if (b.y + b.z + b.w <= 0.0) return 1.0;
    has = 1.0;
    float3 t = b.yzw;
    float code = floor(b.x + 0.5);
    float faded = floor(code / 128.0);
    float cell = code - faded * 128.0 - 1.0;
    [branch] if (cell > -0.5)
    {
        float4 q = CookieRow(clusters, index, 0.0, clusterC, clusterD);
        float3 local = QuatRotate(normalize(q), fromLight);
        float2 origin = float2(fmod(cell, kCookiesAcross) * 4.0 * cookieC.x, floor(cell / kCookiesAcross) * 2.0 * cookieC.y);
        float3 s = tex2Dlod(atlas, float4(CookieUv(local, origin, cookieC, cookieD), 0, 0)).rgb;
        t = lerp(t, lerp(dot(s, float3(0.299, 0.587, 0.114)).xxx, s, cookieE.x), faded / 255.0);
    }
    return lerp(1.0, max(t, cookieC.w), cookieC.z);
}

#endif
