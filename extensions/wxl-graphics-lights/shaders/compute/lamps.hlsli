// wxl-graphics-lights: one lamp at one point -- the cluster lists, the HDR sources, the angular profile,
// the cookie (prefiltered by footprint), the room gate and the fog's thinning -- shared by the surfaces
// and the field. docs/design.md, sections 3, 5, 6 and 8.3.
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

#ifndef LIGHTS_LAMPS_HLSLI
#define LIGHTS_LAMPS_HLSLI

#include "common.hlsli"
#include "wxl/lights/sources.hlsli"

static const float kLightCapacity = 128.0;
static const float kCookiesAcross = 4.0;

LIGHTS_TEX(LIGHTS_T_SOURCES)  Texture2D<float4> sourceTex;
LIGHTS_TEX(LIGHTS_T_CLUSTERS) Texture2D<float4> clusterTex;   // RGBA32F, every value exact
LIGHTS_TEX(LIGHTS_T_COOKIES)  Texture2D<float4> cookieAtlas;  // L8 (read .r) or X8R8G8B8

// --- the cluster lists (GraphicsLightsApi.h ClusterTexture) ---------------------------------------------

float ClusterGridRows() { return ceil(clusterC.x * clusterC.y * clusterC.z / clusterC.w); }
float ClusterPoolStart() { return ClusterGridRows() + 2.0 * ceil(kLightCapacity / clusterC.w); }

float4 ClusterTexel(float n, float firstRow)
{
    float row = floor((n + 0.5) / clusterC.w);
    float col = n - row * clusterC.w;
    return clusterTex.Load(int3(int(col), int(firstRow + row), 0));
}

// (offset, length) of the list of the cluster holding a point at screen uv01 and a distance.
float2 LightList(float2 uv01, float dist)
{
    float depth01 = log(max(dist, clusterD.z) / clusterD.z) * clusterD.w;
    float3 c = clamp(floor(float3(uv01, depth01) * clusterC.xyz), 0.0, clusterC.xyz - 1.0);
    float id = c.x + c.y * clusterC.x + c.z * clusterC.x * clusterC.y;
    return floor(ClusterTexel(id, 0.0).rg + 0.5);
}

// The same for a cluster given by its coordinates.
float2 LightListAt(float3 cell)
{
    float3 c = clamp(cell, 0.0, clusterC.xyz - 1.0);
    float id = c.x + c.y * clusterC.x + c.z * clusterC.x * clusterC.y;
    return floor(ClusterTexel(id, 0.0).rg + 0.5);
}

uint LightAt(float entry)
{
    float texel = floor(entry * 0.25);
    float4 q = ClusterTexel(texel, ClusterPoolStart());
    float m = entry - texel * 4.0;
    return uint(floor((m < 0.5 ? q.r : (m < 1.5 ? q.g : (m < 2.5 ? q.b : q.a))) + 0.5));
}

// --- shape ------------------------------------------------------------------------------------------------

// The closest point of a tube to p (the representative-point approximation of an area light).
float3 LightPoint(float3 p, WxlLightSource s)
{
    if (!WxlSourceHas(s.flags, 0x02u)) return s.position;
    float t = clamp(dot(p - s.position, s.extent) / max(dot(s.extent, s.extent), 1e-4), -1.0, 1.0);
    return s.position + s.extent * t;
}

// The analytic profiles of a family (lights.hlsli, LightProfile), fromLight the unit direction from the
// light to the point: 1 a lantern's cage, 2 a street lamp's hood (down and out, 30 % up), 3 a flame,
// 4 a window's grille.
float LightProfile(float profile, float3 fromLight)
{
    if (profile < 0.5 || Isolated(LIGHTS_ISO_NO_PROFILE)) return 1.0;
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

float LightCone(WxlLightSource s, float3 fromLight)
{
    if (s.cosOuter <= -1.0) return 1.0;
    return smoothstep(s.cosOuter, max(s.cosInner, s.cosOuter + 1e-3), dot(fromLight, s.axis));
}

// A flame's light runs whiter near its source (within about six soft radii).
float3 LightHot(float3 rgb, float d, float softRadius, float hot)
{
    if (hot <= 0.0) return rgb;
    float core = saturate(1.0 - d / max(6.0 * softRadius, 0.3));
    float l = Luma(rgb);
    return lerp(rgb, l.xxx, core * core * hot);
}

// --- cookies (cookies.hlsli, read with Load) ---------------------------------------------------------------

float4 CookieRow(float index, float row)
{
    float perBlock = ceil(kLightCapacity / clusterC.w);
    return ClusterTexel(index, ClusterGridRows() + row * perBlock);
}

float3 QuatRotate(float4 q, float3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }

// The atlas coordinates of direction d in a cookie whose cells start at cell, moved by `nudge` texels
// of the face (the taps of the magnification filter) and kept inside the face.
float2 CookieUv(float3 d, float2 cell, float2 nudge)
{
    float3 a = abs(d);
    float3 m = a.x >= a.y && a.x >= a.z ? float3(1.0, 0.0, 0.0) : (a.y >= a.z ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0));
    float sgn = dot(d, m) > 0.0 ? 1.0 : -1.0;
    float f = dot(m, float3(0.0, 2.0, 4.0)) + (sgn > 0.0 ? 0.0 : 1.0);
    float sc = m.x * (-sgn * d.z) + m.y * d.x + m.z * (sgn * d.x);
    float tc = m.y * (sgn * d.z) - (1.0 - m.y) * d.y;
    float2 uv = float2(sc, tc) / max(dot(a, m), 1e-5) * 0.5 + 0.5 + nudge / max(cookieE.y, 1.0);
    float2 inset = cookieD.xy / cookieC.xy;
    uv = clamp(uv, inset, 1.0 - inset);
    float2 faceOrigin = float2(fmod(f, 4.0), floor(f * 0.25));
    return cell + (faceOrigin + uv) * cookieC.xy;
}

// The cookie's transmittance rgb for light `index` towards fromLight, floored and scaled by its
// strength. footprint is the angle (radians) the lit point covers as seen from the lamp: where it
// spans more than a few of the cookie's texels, the pattern gives way to its mean, so distant or
// grazing surfaces never alias its bars. has says whether the light carries a cookie.
float3 LightCookie(uint index, float3 fromLight, float footprint, out float has)
{
    has = 0.0;
    if (cookieC.z <= 0.0 || Isolated(LIGHTS_ISO_NO_COOKIES)) return 1.0;
    float4 b = CookieRow(float(index), 1.0);
    if (b.y + b.z + b.w <= 0.0) return 1.0;
    has = 1.0;
    float3 t = b.yzw;
    float code = floor(b.x + 0.5);
    float faded = floor(code / 128.0);
    float cell = code - faded * 128.0 - 1.0;
    // One texel spans (pi / 2) / face texels radians; the pattern is kept while the footprint stays
    // within a few of them.
    float texelAngle = 1.5708 / max(cookieE.y, 1.0);
    float sharp = Isolated(LIGHTS_ISO_NO_PREFILTER) ? 1.0 : saturate(cookieE.z * texelAngle / max(footprint, 1e-5));
    [branch] if (cell > -0.5 && sharp > 0.01)
    {
        float4 q = CookieRow(float(index), 0.0);
        float3 local = QuatRotate(normalize(q), fromLight);
        float2 origin = float2(fmod(cell, kCookiesAcross) * 4.0 * cookieC.x, floor(cell / kCookiesAcross) * 2.0 * cookieC.y);
        // Four bilinear taps three quarters of a texel apart, a tent about two texels wide: a baked cage is
        // coarse, and a wall right beside the lamp magnifies it until one bilinear fetch shows its texels
        // as a staircase. The source's own size would soften it this much and more.
        float3 s = 0.0;
        [unroll] for (int k = 0; k < 4; ++k)
        {
            float2 nudge = float2((k & 1) != 0 ? 0.75 : -0.75, (k & 2) != 0 ? 0.75 : -0.75);
            s += cookieAtlas.SampleLevel(sLinearClamp, CookieUv(local, origin, nudge), 0).rgb;
        }
        s *= 0.25;
        if (lights.y > 0.5) s = s.rrr;   // a single-channel atlas reads (L, 0, 0) through Vulkan
        float3 pattern = lerp(dot(s, float3(0.299, 0.587, 0.114)).xxx, s, cookieE.x);
        t = lerp(t, pattern, (faded / 255.0) * sharp);
    }
    return lerp(1.0, max(t, cookieC.w), cookieC.z);
}

// A light's shape towards the point it lights: the family's profile and its cookie (a baked cage
// replaces the analytic one); a flame is spared part of its cookie.
float3 LightShape(uint index, WxlLightSource s, float3 fromLight, float footprint)
{
    float has = 0.0;
    float3 cookie = LightCookie(index, fromLight, footprint, has);
    if (s.profile > 2.5 && s.profile < 3.5) cookie = lerp(cookie, 1.0, cookieD.w);
    float profile = LightProfile(s.profile, fromLight);
    if (has > 0.5 && (abs(s.profile - 1.0) < 0.5 || abs(s.profile - 4.0) < 0.5)) profile = 1.0;
    return profile * cookie;
}

// --- rooms ------------------------------------------------------------------------------------------------

// How much of a light reaches a point of these rooms. Two models, blended by the light's own gate (which
// already carries its room's fade), so nothing ever flips:
//   outdoor light: full outdoors, the outdoor-indoors leak inside a room (by how indoor the point is);
//   room light:    full in any room of its mask, other rooms by the floor leak, outdoors the
//                  indoor-outdoors leak.
// Every term is continuous in the point's memberships, so a box face never shows as a line.
float LightRoomGate(WxlLightSource s, RoomSet rs)
{
    if (Isolated(LIGHTS_ISO_NO_ROOMS)) return 1.0;
    float outdoor = lerp(1.0, roomInfo.z, rs.indoor);
    if (s.room < -0.5 || s.roomGate <= 0.0) return outdoor;
    float own = RoomsIn(rs, uint(s.roomMask + 0.5));
    float indoor = lerp(lerp(roomInfo.w, roomInfo.y, rs.indoor), 1.0, own);
    return lerp(outdoor, indoor, saturate(s.roomGate));
}

// The medium between a lamp and the point thins its light (the fog's extinction around the camera).
float LightMedium(float d)
{
    return Isolated(LIGHTS_ISO_NO_FOG) ? 1.0 : exp(-medium.x * d);
}

#endif
