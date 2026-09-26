// wxl-graphics-fog: wxl-graphics-lights' texture layouts read from compute (its shaders/wxl/lights
// lights.hlsli and cookies.hlsli, the same maths, with Load instead of tex2Dlod): the light list, the
// clusters, the cookies and the omni shadow maps.
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

#ifndef FOG_LAMPS_HLSLI
#define FOG_LAMPS_HLSLI

#include "common.hlsli"

static const float kLightCapacity = 128.0;
static const float kCookiesAcross = 4.0;

FOG_TEX(4) Texture2D<float4> lightTex;     // WXL_GFX_LIGHTS_MAX x 4, RGBA32F
FOG_TEX(5) Texture2D<float4> clusterTex;   // RGBA32F, every value exact
FOG_TEX(6) Texture2D<float4> omniTable;    // WXL_GFX_LIGHTS_MAX x 2
FOG_TEX(7) Texture2D<float4> cookieAtlas;  // L8 (read .r) or X8R8G8B8
FOG_TEX(8) Texture2D<float>  omni0;        // the core's R32F omni atlases, 4 x 2 faces
FOG_TEX(9) Texture2D<float>  omni1;
FOG_TEX(10) Texture2D<float> omni2;
FOG_TEX(11) Texture2D<float> omni3;

float ClusterGridRows() { return ceil(clusterC.x * clusterC.y * clusterC.z / clusterC.w); }
float ClusterPoolStart() { return ClusterGridRows() + 2.0 * ceil(kLightCapacity / clusterC.w); }

float4 ClusterTexel(float n, float firstRow)
{
    float row = floor((n + 0.5) / clusterC.w);
    float col = n - row * clusterC.w;
    return clusterTex.Load(int3(int(col), int(firstRow + row), 0));
}

// (offset, length) of the list of the cluster holding a point at uv01 and a distance.
float2 LightList(float2 uv01, float dist)
{
    float depth01 = log(max(dist, clusterD.z) / clusterD.z) * clusterD.w;
    float3 c = clamp(floor(float3(uv01, depth01) * clusterC.xyz), 0.0, clusterC.xyz - 1.0);
    float id = c.x + c.y * clusterC.x + c.z * clusterC.x * clusterC.y;
    return floor(ClusterTexel(id, 0.0).rg + 0.5);
}

float LightAt(float entry)
{
    float texel = floor(entry * 0.25);
    float4 q = ClusterTexel(texel, ClusterPoolStart());
    float m = entry - texel * 4.0;
    return floor((m < 0.5 ? q.r : (m < 1.5 ? q.g : (m < 2.5 ? q.b : q.a))) + 0.5);
}

float4 LightRow(float index, int row) { return lightTex.Load(int3(int(index), row, 0)); }

float2 OmniSlotOf(float index) { return omniTable.Load(int3(int(index), 0, 0)).rg; }

float3 LightPoint(float3 p, float4 a, float4 c, float4 shape)
{
    if (shape.z < 0.5) return a.xyz;
    float t = clamp(dot(p - a.xyz, c.xyz) / max(dot(c.xyz, c.xyz), 0.0001), -1.0, 1.0);
    return a.xyz + c.xyz * t;
}

float LightProfile(float profile, float3 fromLight)
{
    if (profile < 0.5) return 1.0;
    if (profile > 1.5 && profile < 2.5) return 0.3 + 0.7 * saturate(0.3 - fromLight.z * 1.2);
    if (profile > 2.5 && profile < 3.5) return 0.8 + 0.2 * saturate(fromLight.z + 0.5);
    float rho = length(fromLight.xy);
    float c = fromLight.x / max(rho, 0.0001);
    if (profile < 1.5)
    {
        float c3 = c * (4.0 * c * c - 3.0);
        float p2 = c3 * c3, p8 = p2 * p2;
        p8 *= p8;
        float bars = p8 * p8 * p8;
        return lerp(1.0, 0.3, bars) * (fromLight.z > 0.75 ? 0.6 : 1.0);
    }
    float c2 = 2.0 * c * c - 1.0;
    float c4 = 2.0 * c2 * c2 - 1.0;
    float e2 = 2.0 * rho * rho - 1.0;
    float e4 = 2.0 * e2 * e2 - 1.0;
    float e8 = 2.0 * e4 * e4 - 1.0;
    float2 g = float2(c4, e8);
    g *= g;
    float2 g4 = g * g;
    float2 g16 = g4 * g4;
    g16 *= g16;
    float2 g20 = g16 * g4;
    return 1.0 - 0.5 * max(g20.x, g20.y);
}

float LightFalloffCore(float d2, float radius, float reference, float core)
{
    float r = max(reference, 0.25 * radius);
    float r0 = max(0.5 * r, core);
    float x = d2 / (radius * radius);
    float window = saturate(1.0 - x * x);
    return (r * r * 1.25) / (d2 + r0 * r0) * window * window;
}

float LightFalloffWide(float d2, float radius, float reference)
{
    float r = max(reference, 0.25 * radius);
    float r2 = r * r;
    float x = d2 / (radius * radius);
    float window = saturate(1.0 - x * x);
    return (r2 * 2.0) / (d2 + r2) * window * window;
}

float3 LightHot(float3 rgb, float d, float reference, float hot)
{
    if (hot <= 0.0) return rgb;
    float core = saturate(1.0 - d / max(reference, 0.05));
    float l = dot(rgb, float3(0.299, 0.587, 0.114));
    float3 white = lerp(rgb, l.xxx, 0.45) * 1.15;
    return lerp(rgb, white, core * core * hot);
}

float LightCone(float3 fromLight, float3 axis, float cosCone)
{
    return cosCone <= -1.0 ? 1.0 : saturate((dot(fromLight, axis) - cosCone) / max(1.0 - cosCone, 0.001) * 4.0);
}

float RoomGate(float flags, float room, float leak)
{
    float rooms = floor(flags * 0.25);
    if (leak >= 1.0 || room < -0.5 || rooms < 0.5) return 1.0;
    return lerp(leak, 1.0, fmod(floor(rooms / exp2(room)), 2.0));
}

// --- cookies ---------------------------------------------------------------------------------------

float4 CookieRow(float index, float row)
{
    float perBlock = ceil(kLightCapacity / clusterC.w);
    return ClusterTexel(index, ClusterGridRows() + row * perBlock);
}

float3 QuatRotate(float4 q, float3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }

float2 CookieUv(float3 d, float2 cell)
{
    float3 a = abs(d);
    float3 m = a.x >= a.y && a.x >= a.z ? float3(1.0, 0.0, 0.0) : (a.y >= a.z ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0));
    float sgn = dot(d, m) > 0.0 ? 1.0 : -1.0;
    float f = dot(m, float3(0.0, 2.0, 4.0)) + (sgn > 0.0 ? 0.0 : 1.0);
    float sc = m.x * (-sgn * d.z) + m.y * d.x + m.z * (sgn * d.x);
    float tc = m.y * (sgn * d.z) - (1.0 - m.y) * d.y;
    float2 uv = float2(sc, tc) / max(dot(a, m), 1e-5) * 0.5 + 0.5;
    float2 inset = cookieD.xy / cookieC.xy;
    uv = clamp(uv, inset, 1.0 - inset);
    float2 faceOrigin = float2(fmod(f, 4.0), floor(f * 0.25));
    return cell + (faceOrigin + uv) * cookieC.xy;
}

// The cookie's transmittance rgb towards fromLight (unit, from the light), floored and scaled by its
// strength; `has` says whether the light carries one.
float3 LightCookie(float index, float3 fromLight, out float has)
{
    has = 0.0;
    if (cookieC.z <= 0.0) return 1.0;
    float4 b = CookieRow(index, 1.0);
    if (b.y + b.z + b.w <= 0.0) return 1.0;
    has = 1.0;
    float3 t = b.yzw;
    float code = floor(b.x + 0.5);
    float faded = floor(code / 128.0);
    float cell = code - faded * 128.0 - 1.0;
    [branch] if (cell > -0.5)
    {
        float4 q = CookieRow(index, 0.0);
        float3 local = QuatRotate(normalize(q), fromLight);
        float2 origin = float2(fmod(cell, kCookiesAcross) * 4.0 * cookieC.x, floor(cell / kCookiesAcross) * 2.0 * cookieC.y);
        float3 s = cookieAtlas.SampleLevel(sLinearClamp, CookieUv(local, origin), 0).rgb;
        if (lights.y > 0.5) s = s.rrr;   // a single-channel atlas reads (L, 0, 0) through Vulkan
        t = lerp(t, lerp(dot(s, float3(0.299, 0.587, 0.114)).xxx, s, cookieE.x), faded / 255.0);
    }
    return lerp(1.0, max(t, cookieC.w), cookieC.z);
}

// --- omni shadow maps -------------------------------------------------------------------------------------

// How much of omni slot `slot`'s light reaches camera-relative r past the casters its map holds.
float OmniVisibility(float slot, float3 r)
{
    int base = int(slot) * 20;
    float4 L = omniTable.Load(int3(base, 1, 0));
    float3 v = r - L.xyz;
    if (dot(v, v) >= L.w * L.w) return 1.0;
    float4 T = omniTable.Load(int3(base + 1, 1, 0));
    float3 ax = abs(v);
    float f0 = ax.x >= ax.y && ax.x >= ax.z ? (v.x > 0.0 ? 0.0 : 1.0) : (ax.y >= ax.z ? (v.y > 0.0 ? 2.0 : 3.0) : (v.z > 0.0 ? 4.0 : 5.0));
    int row = base + 2 + int(f0) * 3;
    float4 h = float4(r, 1.0);
    float3 s = float3(dot(h, omniTable.Load(int3(row, 1, 0))), dot(h, omniTable.Load(int3(row + 1, 1, 0))),
                      dot(h, omniTable.Load(int3(row + 2, 1, 0))));
    float2 uv = s.xy / max(s.z, 0.05);
    float2 cell = float2(fmod(f0, 4.0), floor(f0 / 4.0)) * float2(0.25, 0.5);
    if (!(s.z > 0.05 && all(uv >= cell) && all(uv <= cell + float2(0.25, 0.5)))) return 1.0;
    float depth = (s.z - lampGrid2.w) / L.w;
    float2 lo = cell + 0.5 * T.xy, hi = cell + float2(0.25, 0.5) - 0.5 * T.xy;
    float blocked = 0.0;
    [unroll] for (int t = 0; t < 4; ++t)
    {
        float2 o = (float2(float(t & 1), float(t >> 1)) - 0.5) * T.xy;
        float2 at = clamp(uv + o, lo, hi);
        float z = slot < 0.5 ? omni0.SampleLevel(sPointClamp, at, 0)
                : slot < 1.5 ? omni1.SampleLevel(sPointClamp, at, 0)
                : slot < 2.5 ? omni2.SampleLevel(sPointClamp, at, 0)
                             : omni3.SampleLevel(sPointClamp, at, 0);
        blocked += z < depth && z * L.w >= T.w ? 0.25 : 0.0;
    }
    return 1.0 - blocked;
}

#endif
