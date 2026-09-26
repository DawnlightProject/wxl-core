// wxl-graphics-shadow: shadow lookups at any point, for Vulkan compute (DXC, SPIR-V). The same functions
// build the service's own surface masks, so a lamp's shadow on the ground and its wedge in the fog agree.
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

#ifndef WXL_SHADOW_HLSLI
#define WXL_SHADOW_HLSLI

// Use. Compile with -I <wxl-core>/extensions/wxl-graphics-shadow/shaders, then:
//
//     #define WXL_SHADOW_BINDING 40          // first set-0 binding of the shadow set (yours to choose)
//     #include "wxl/shadow/shadow.hlsli"
//
// On the host: append DescribeBindings(40, ...) to your pipeline's bindings and call
// WriteBindings(set, 40) on every set you allocate for it (GraphicsShadowApi.h).
//
// Every position is camera-relative (world - eye); ShadowRel converts a world position. Normals are
// world space and unit; pass float3(0, 0, 0) for a point in the air (no normal offset). Every function
// returns 1 (lit) while the service is off, for a free slot, and outside what a map or cascade covers.
//
//   ShadowLight(slot, pos)          a light's visibility at a point: its map, or its body capsules
//   ShadowLight(slot, pos, n)       the same on a surface (normal offset, wider filter)
//   ShadowSlotOf(listIndex)         the slot of wxl-graphics-lights' light index this frame, -1 none
//   ShadowSun(pos) / ShadowMoon(pos)          the engine's cascades and the terrain horizon
//   ShadowSun(pos, n) / ShadowMoon(pos, n)    the same on a surface
//   ShadowTerrain(pos, towards)     the terrain horizon alone, towards any direction
//   ShadowBodies(pos, lightPos, sourceSize)   every body capsule between a point and any light
//
// Cost. ShadowLight in the air: one trilinear fetch of the filtered map, or the capsules in reach of
// the light (analytic); on a surface: the same fetch at a wider footprint. ShadowSun in the air: one
// gather; on a surface: nine gathers (a tent of bilinear comparisons). Cheap enough per froxel.

#include "wxl/shadow/layout.h"

#ifndef WXL_SHADOW_BINDING
#error "define WXL_SHADOW_BINDING (the first set-0 binding of the shadow set) before including wxl/shadow/shadow.hlsli"
#endif
#ifndef WXL_SHADOW_SET
#define WXL_SHADOW_SET 0
#endif

[[vk::binding(WXL_SHADOW_BINDING + WXL_SHADOW_B_CONSTANTS, WXL_SHADOW_SET)]] cbuffer WxlShadowBlock
{
    float4 wxlShadow[WXL_SHADOW_ROWS];
};
[[vk::binding(WXL_SHADOW_BINDING + WXL_SHADOW_B_POINT, WXL_SHADOW_SET)]]  SamplerState wxlShadowPoint;
[[vk::binding(WXL_SHADOW_BINDING + WXL_SHADOW_B_LINEAR, WXL_SHADOW_SET)]] SamplerState wxlShadowLinear;
[[vk::binding(WXL_SHADOW_BINDING + WXL_SHADOW_B_MAPS, WXL_SHADOW_SET)]]   Texture2D<float4> wxlShadowMaps;
[[vk::binding(WXL_SHADOW_BINDING + WXL_SHADOW_B_CASCADE0 + 0, WXL_SHADOW_SET)]] Texture2D<float> wxlShadowCascade0;
[[vk::binding(WXL_SHADOW_BINDING + WXL_SHADOW_B_CASCADE0 + 1, WXL_SHADOW_SET)]] Texture2D<float> wxlShadowCascade1;
[[vk::binding(WXL_SHADOW_BINDING + WXL_SHADOW_B_CASCADE0 + 2, WXL_SHADOW_SET)]] Texture2D<float> wxlShadowCascade2;
[[vk::binding(WXL_SHADOW_BINDING + WXL_SHADOW_B_CASCADE0 + 3, WXL_SHADOW_SET)]] Texture2D<float> wxlShadowCascade3;
[[vk::binding(WXL_SHADOW_BINDING + WXL_SHADOW_B_HORIZON, WXL_SHADOW_SET)]] Texture3D<float> wxlShadowHorizon;
[[vk::binding(WXL_SHADOW_BINDING + WXL_SHADOW_B_HEIGHTS, WXL_SHADOW_SET)]] Texture2D<float> wxlShadowHeights;

static const float kWxlShadowTile = 533.0 + 1.0 / 3.0;

// --- the block ---------------------------------------------------------------------------------------------

bool ShadowEnabled() { return wxlShadow[WXL_SHADOW_ROW_FRAME].w > 0.5; }

float3 ShadowRel(float3 world) { return world - wxlShadow[WXL_SHADOW_ROW_FRAME].xyz; }

float3 ShadowToSun() { return wxlShadow[WXL_SHADOW_ROW_SUN].xyz; }
float3 ShadowToMoon() { return wxlShadow[WXL_SHADOW_ROW_MOON].xyz; }
float  ShadowSunWeight() { return wxlShadow[WXL_SHADOW_ROW_SUN].w; }
float  ShadowMoonWeight() { return wxlShadow[WXL_SHADOW_ROW_MOON].w; }

int ShadowSlotOf(int listIndex)
{
    if (listIndex < 0 || listIndex >= WXL_SHADOW_MAX_INDEX) return -1;
    uint4 v = asuint(wxlShadow[WXL_SHADOW_ROW_INDEX + (listIndex >> 2)]);
    uint c = (listIndex & 3) == 0 ? v.x : (listIndex & 3) == 1 ? v.y : (listIndex & 3) == 2 ? v.z : v.w;
    return int(c) - 1;
}

// --- body capsules -----------------------------------------------------------------------------------------

// Closest points of the segments p0 + t (p1 - p0) and q0 + s (q1 - q0), t and s in 0..1.
void WxlShadowClosest(float3 p0, float3 p1, float3 q0, float3 q1, out float t, out float s)
{
    float3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
    float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    float c = dot(d1, r), b = dot(d1, d2);
    float denom = a * e - b * b;
    t = denom > 1e-6 ? saturate((b * f - c * e) / denom) : 0.0;
    s = e > 1e-6 ? (b * t + f) / e : 0.0;
    if (s < 0.0) { s = 0.0; t = a > 1e-6 ? saturate(-c / a) : 0.0; }
    else if (s > 1.0) { s = 1.0; t = a > 1e-6 ? saturate((b - c) / a) : 0.0; }
}

// How much of a light of source radius `size` at L reaches p past one capsule (a, b, radius): the
// capsule's cross-section against the cone from p to the source, softened by the penumbra that cone
// has where it meets the capsule.
float WxlShadowCapsule(float3 p, float3 L, float3 a, float3 b, float radius, float size)
{
    float t, s;
    WxlShadowClosest(p, L, a, b, t, s);
    float3 onRay = p + (L - p) * t;
    float3 onAxis = a + (b - a) * s;
    float d = length(onRay - onAxis);
    // The source's footprint at the occluder, as the cone from the receiver widens towards it.
    float w = max(size * t * wxlShadow[WXL_SHADOW_ROW_CAPSULE].y, 0.03);
    float occ = 1.0 - smoothstep(radius - w, radius + w, d);
    // Nothing behind the receiver or past the light.
    occ *= smoothstep(0.0, 0.03, t) * (1.0 - smoothstep(0.97, 1.0, t));
    return 1.0 - occ;
}

// Every capsule in `mask` between p and a light at L (camera-relative), except `skip`. A receiver inside
// a capsule (plus the self margin) is never shadowed by it: a body does not darken itself. Each capsule's
// occlusion is scaled by its weight, which fades in and out as its unit joins or leaves the list.
float ShadowBodies(float3 p, float3 L, float size, uint mask, int skip)
{
    if (!ShadowEnabled()) return 1.0;
    float4 info = wxlShadow[WXL_SHADOW_ROW_CAPSULE];
    uint count = uint(info.x);
    mask &= count >= 32u ? 0xFFFFFFFFu : ((1u << count) - 1u);
    if (skip >= 0) mask &= ~(1u << uint(skip));
    float vis = 1.0;
    [loop] while (mask != 0u)
    {
        uint k = firstbitlow(mask);
        mask &= mask - 1u;
        float4 A = wxlShadow[WXL_SHADOW_ROW_CAPSULES + k * 2u];
        float4 B = wxlShadow[WXL_SHADOW_ROW_CAPSULES + k * 2u + 1u];
        if (B.w <= 0.0) continue;
        float3 ab = B.xyz - A.xyz;
        float s = saturate(dot(p - A.xyz, ab) / max(dot(ab, ab), 1e-6));
        if (length(p - (A.xyz + ab * s)) < A.w + info.z) continue;
        vis *= lerp(1.0, WxlShadowCapsule(p, L, A.xyz, B.xyz, A.w, size), saturate(B.w));
        if (vis < 0.004) break;
    }
    return vis;
}

float ShadowBodies(float3 p, float3 L, float size)
{
    return ShadowBodies(p, L, size, 0xFFFFFFFFu, -1);
}

// Whether p lies inside capsule k widened by margin.
bool WxlShadowInsideCapsule(float3 p, int k, float margin)
{
    if (k < 0) return false;
    float4 A = wxlShadow[WXL_SHADOW_ROW_CAPSULES + k * 2];
    float4 B = wxlShadow[WXL_SHADOW_ROW_CAPSULES + k * 2 + 1];
    float3 ab = B.xyz - A.xyz;
    float s = saturate(dot(p - A.xyz, ab) / max(dot(ab, ab), 1e-6));
    return length(p - (A.xyz + ab * s)) < A.w + margin;
}

// --- the filtered cube maps (EVSM) ---------------------------------------------------------------------------

float WxlShadowCheb(float2 m, float t, float minVariance, float bleed)
{
    float variance = max(m.y - m.x * m.x, minVariance);
    float d = t - m.x;
    float p = variance / (variance + d * d);
    p = saturate((p - bleed) / (1.0 - bleed));
    return t <= m.x ? 1.0 : p;
}

// Visibility of depth d (0..1 over the radius, along the face's axis) from a texel of moments.
float WxlShadowEvsm(float4 moments, float d)
{
    float2 e = wxlShadow[WXL_SHADOW_ROW_EVSM].xy;
    float4 bias = wxlShadow[WXL_SHADOW_ROW_BIAS];
    float x = 2.0 * saturate(d) - 1.0;
    float2 w = float2(exp(e.x * x), -exp(-e.y * x));
    float2 scale = e * float2(w.x, -w.y) * bias.z;
    float pos = WxlShadowCheb(moments.xy, w.x, scale.x * scale.x, bias.w);
    float neg = WxlShadowCheb(moments.zw, w.y, scale.y * scale.y, bias.w);
    return min(pos, neg);
}

// Face and in-cell uv of a direction v from the light (the service's own cube convention).
void WxlShadowFace(float3 v, out int face, out float2 uv, out float along)
{
    float3 a = abs(v);
    int axis = a.x >= a.y && a.x >= a.z ? 0 : (a.y >= a.z ? 1 : 2);
    along = axis == 0 ? a.x : (axis == 1 ? a.y : a.z);
    float lead = axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
    float2 side = axis == 0 ? v.yz : (axis == 1 ? v.zx : v.xy);
    face = axis * 2 + (lead >= 0.0 ? 0 : 1);
    uv = side / max(along, 1e-6) * 0.5 + 0.5;
}

// Map m's visibility at p: normal-offset receiver (n may be zero), a footprint that grows with the
// source size and the chosen lod bias (0 on surfaces, higher for the air).
float ShadowMap(int m, float3 p, float3 n, float size, float lodBias)
{
    if (m < 0 || m >= WXL_SHADOW_MAX_MAPS) return 1.0;
    float4 L = wxlShadow[WXL_SHADOW_ROW_MAPS + m * WXL_SHADOW_MAP_ROWS_EACH];
    float4 R = wxlShadow[WXL_SHADOW_ROW_MAPS + m * WXL_SHADOW_MAP_ROWS_EACH + 1];
    if (L.w <= 0.0 || R.z < 0.5) return 1.0;
    float4 atlas = wxlShadow[WXL_SHADOW_ROW_ATLAS];
    float4 bias = wxlShadow[WXL_SHADOW_ROW_BIAS];
    float faceTexels = atlas.z;
    float3 v = p - L.xyz;
    float dist = length(v);
    if (dist >= L.w || dist < 1e-3) return 1.0;
    int face;
    float2 uv;
    float along;
    WxlShadowFace(v, face, uv, along);
    // A 90-degree face spans 2 x `along` yards across its texels.
    float texel = 2.0 * along / faceTexels;
    if (dot(n, n) > 0.5)
    {
        float NdotL = saturate(dot(n, -v / dist));
        v += n * texel * bias.x * (1.0 + 2.0 * (1.0 - NdotL));
        WxlShadowFace(v, face, uv, along);
    }
    // The footprint: a texel, widened by the source's size as the receiver moves off the light.
    float4 evsm = wxlShadow[WXL_SHADOW_ROW_EVSM];
    float width = texel + size * evsm.w * saturate(dist / max(L.w * 0.35, 0.5));
    float lod = clamp(log2(max(width / texel, 1.0)) + lodBias + evsm.z, 0.0, atlas.w - 1.0);
    // Kept half a texel of the coarser level inside the cell, so no neighbour face bleeds in.
    float border = 0.5 * exp2(ceil(lod)) / faceTexels;
    uv = clamp(uv, border, 1.0 - border);
    float2 cellSize = float2(1.0 / WXL_SHADOW_ATLAS_FACES_X, 1.0 / WXL_SHADOW_ATLAS_FACES_Y);
    float2 cell = R.xy + float2(float(face % 3), float(face / 3)) * cellSize;
    float4 moments = wxlShadowMaps.SampleLevel(wxlShadowLinear, cell + uv * cellSize, lod);
    return WxlShadowEvsm(moments, along / L.w - bias.y * texel / L.w);
}

// --- a light slot --------------------------------------------------------------------------------------------

float WxlShadowLightAt(int slot, float3 p, float3 n, float lodBias)
{
    if (!ShadowEnabled() || slot < 0 || slot >= WXL_SHADOW_MAX_SLOTS) return 1.0;
    int base = WXL_SHADOW_ROW_SLOTS + slot * WXL_SHADOW_SLOT_ROWS;
    float4 S0 = wxlShadow[base], S1 = wxlShadow[base + 1], S2 = wxlShadow[base + 2], S3 = wxlShadow[base + 3];
    if (S0.w <= 0.0 || S2.x <= 0.0) return 1.0;
    float3 v = p - S0.xyz;
    if (dot(v, v) >= S0.w * S0.w) return 1.0;
    // A carrier is never darkened by its own light: the map and the capsules skip its body.
    int carrier = int(S3.y);
    if (WxlShadowInsideCapsule(p, carrier, S3.z)) return 1.0;
    float mapped = 1.0, bodies = 1.0;
    if (S2.w >= 0.0 && S2.y > 0.0) mapped = ShadowMap(int(S2.w), p, n, S2.z, lodBias);
    if (S2.y < 1.0) bodies = ShadowBodies(p, S0.xyz, S2.z, asuint(S3.x), carrier);
    return lerp(1.0, lerp(bodies, mapped, S2.y), S2.x);
}

float ShadowLight(int slot, float3 p) { return WxlShadowLightAt(slot, p, float3(0.0, 0.0, 0.0), 1.0); }
float ShadowLight(int slot, float3 p, float3 n) { return WxlShadowLightAt(slot, p, n, 0.0); }

// --- the sun and the moon ------------------------------------------------------------------------------------

float WxlShadowCascadeGatherLit(int m, float2 uv, float z, float2 size)
{
    float2 st = uv * size - 0.5;
    float2 f = frac(st);
    float2 at = (floor(st) + 1.0) / size;
    float4 g = m == 0 ? wxlShadowCascade0.GatherRed(wxlShadowPoint, at)
             : m == 1 ? wxlShadowCascade1.GatherRed(wxlShadowPoint, at)
             : m == 2 ? wxlShadowCascade2.GatherRed(wxlShadowPoint, at)
                      : wxlShadowCascade3.GatherRed(wxlShadowPoint, at);
    // Gather order: x (0, 1), y (1, 1), z (1, 0), w (0, 0). Lit where the map's caster lies at or past z.
    float4 lit = float4(g.x >= z, g.y >= z, g.z >= z, g.w >= z);
    return lerp(lerp(lit.w, lit.z, f.x), lerp(lit.x, lit.y, f.x), f.y);
}

// One cascade: a single bilinear comparison (taps 1) or a 3 x 3 tent of them (taps 9) spaced by the
// filter width, whose world size stays the same from cascade to cascade.
float WxlShadowCascadeLit(int m, float2 uv, float z, float size, float spacing, int taps)
{
    float2 s2 = float2(size, size);
    if (taps <= 1) return WxlShadowCascadeGatherLit(m, uv, z, s2);
    float lit = 0.0;
    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i)
        {
            float w = (2.0 - abs(float(i))) * (2.0 - abs(float(j)));
            lit += w * WxlShadowCascadeGatherLit(m, uv + float2(i, j) * spacing / size, z, s2);
        }
    return lit / 16.0;
}

// The engine's maps at p, finest first, with a soft hand-over to the next near each map's edge.
float ShadowCascades(float3 p, float3 n, int taps)
{
    float4 info = wxlShadow[WXL_SHADOW_ROW_CASCADE];
    int maps = int(info.x);
    if (maps <= 0) return 1.0;
    float size = 1.0 / max(info.z, 1e-6);
    float4 dir = wxlShadow[WXL_SHADOW_ROW_CASCADEDIR];
    float4 bias = wxlShadow[WXL_SHADOW_ROW_BIAS];
    bool surface = dot(n, n) > 0.5;
    float NdotL = surface ? saturate(dot(n, dir.xyz)) : 1.0;
    float slope = surface ? min(sqrt(1.0 - NdotL * NdotL) / max(NdotL, 0.2), 5.0) : 0.0;
    float result = 1.0, carried = 0.0;
    [loop] for (int m = 0; m < WXL_SHADOW_MAX_CASCADES; ++m)
    {
        if (m >= maps) break;
        float4 par = wxlShadow[WXL_SHADOW_ROW_CASCADEPAR + m];
        float yardsPerTexel = par.y;
        float3 q = surface ? p + n * yardsPerTexel * bias.x * (1.0 + 2.0 * (1.0 - NdotL)) : p;
        float4 h = float4(q, 1.0);
        int row = WXL_SHADOW_ROW_CASCADEROWS + m * 3;
        float3 s = float3(dot(h, wxlShadow[row]), dot(h, wxlShadow[row + 1]), dot(h, wxlShadow[row + 2]));
        float edge = max(abs(s.x), abs(s.y));
        if (edge >= 0.97) continue;
        float2 uv = s.xy * 0.5 + 0.5;
        // Depth bias: the engine's own, plus a slope term of a texel or two.
        float z = s.z - (par.z + yardsPerTexel * bias.y * (1.0 + slope)) * dir.w;
        // The filter spacing in texels, scaled so the world width matches the finest map's.
        float finest = wxlShadow[WXL_SHADOW_ROW_CASCADEPAR].y;
        float spacing = clamp(info.w * finest / yardsPerTexel, 0.5, 2.0);
        float lit = WxlShadowCascadeLit(m, uv, z, size, spacing, taps);
        // Hand over to the next map over the last 15 % of this one.
        float blend = saturate((edge - 0.82) / 0.15);
        if (blend <= 0.0 || m + 1 >= maps)
            return lerp(lit, 1.0, m + 1 >= maps ? blend : 0.0);
        result = lit;
        carried = blend;
        // The next map finishes the blend.
        float4 par2 = wxlShadow[WXL_SHADOW_ROW_CASCADEPAR + m + 1];
        float3 q2 = surface ? p + n * par2.y * bias.x * (1.0 + 2.0 * (1.0 - NdotL)) : p;
        float4 h2 = float4(q2, 1.0);
        int row2 = row + 3;
        float3 s2 = float3(dot(h2, wxlShadow[row2]), dot(h2, wxlShadow[row2 + 1]), dot(h2, wxlShadow[row2 + 2]));
        float z2 = s2.z - (par2.z + par2.y * bias.y * (1.0 + slope)) * dir.w;
        float spacing2 = clamp(info.w * finest / par2.y, 0.5, 2.0);
        float lit2 = WxlShadowCascadeLit(m + 1, s2.xy * 0.5 + 0.5, z2, size, spacing2, taps);
        return lerp(result, lit2, carried);
    }
    return 1.0;
}

// The terrain horizon towards a direction (world, unit), from the baked maps of the block around the camera.
float ShadowTerrain(float3 p, float3 towards)
{
    float4 H = wxlShadow[WXL_SHADOW_ROW_HORIZON];
    float4 H2 = wxlShadow[WXL_SHADOW_ROW_HORIZON2];
    if (!ShadowEnabled() || H.z <= 0.0) return 1.0;
    float3 world = p + wxlShadow[WXL_SHADOW_ROW_FRAME].xyz;
    float2 t = 32.0 - world.yx / kWxlShadowTile;
    float2 d = t - H.xy;
    if (any(d < 0.0) || any(d >= 4.0)) return 1.0;
    float2 uv = (t + 0.5 / 128.0) / 4.0;
    float azimuth = atan2(towards.y, towards.x);
    float w = azimuth / 6.28318530718 + 0.5 / H2.z;
    float horizon = wxlShadowHorizon.SampleLevel(wxlShadowLinear, float3(uv, w), 0) * 1.57079632679;
    if (H2.y > 0.5)
    {
        float ground = wxlShadowHeights.SampleLevel(wxlShadowLinear, uv, 0);
        float above = ground > -5000.0 ? max(world.z - ground, 0.0) : 0.0;
        horizon = atan(max(tan(horizon) - above / max(H2.x, 1.0), 0.0));
    }
    float elevation = asin(clamp(towards.z, -1.0, 1.0));
    float vis = smoothstep(-H.w, H.w, elevation - horizon);
    return lerp(1.0, vis, saturate(H.z));
}

// A body (0 the sun, 1 the moon): the terrain towards it, times the engine's maps when they were
// rendered along it.
float WxlShadowBody(int body, float3 p, float3 n, int taps)
{
    if (!ShadowEnabled()) return 1.0;
    float3 towards = body == 0 ? ShadowToSun() : ShadowToMoon();
    float vis = ShadowTerrain(p, towards);
    if (int(wxlShadow[WXL_SHADOW_ROW_CASCADE].y) == body) vis *= ShadowCascades(p, n, taps);
    return vis;
}

float ShadowSun(float3 p) { return WxlShadowBody(0, p, float3(0.0, 0.0, 0.0), 1); }
float ShadowSun(float3 p, float3 n) { return WxlShadowBody(0, p, n, 9); }
float ShadowMoon(float3 p) { return WxlShadowBody(1, p, float3(0.0, 0.0, 0.0), 1); }
float ShadowMoon(float3 p, float3 n) { return WxlShadowBody(1, p, n, 9); }

#endif
