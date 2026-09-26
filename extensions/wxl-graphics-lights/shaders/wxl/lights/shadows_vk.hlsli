// wxl-graphics-lights: the omni shadow maps of GraphicsLightsApi.h's omni table, read from Vulkan compute
// (DXC, SPIR-V) with one filter shared by every consumer: a blocker search over the light's source, then
// a Poisson disc as wide as the penumbra those blockers cast (PCSS). The surface lighting and the fog can
// both include it, so a halo and the ground under it darken alike.
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

#ifndef WXL_LIGHTS_SHADOWS_VK_HLSLI
#define WXL_LIGHTS_SHADOWS_VK_HLSLI

// The omni table (OmniTexture, A32B32G32R32F, WXL_GFX_LIGHTS_MAX x 2) row 0 holds, per light index,
// (slot + 1, the slot's share, receiver skip yards, 0); row 1 holds, 20 texels a slot:
// (camera-relative position, radius) (1 / atlas width, 1 / atlas height, face size, own-housing yards)
// then 3 rows a face (+X -X +Y -Y +Z -Z): (r, 1) . row = (u w, v w, w), w the depth along the face's
// axis. The atlas (R32F, 4 x 2 faces) holds that depth over the radius, 1 where nothing was drawn.

static const float2 kWxlOmniDisc[12] = {
    float2(-0.326, -0.406), float2(-0.840, -0.074), float2(-0.696,  0.457), float2(-0.203,  0.621),
    float2( 0.962, -0.195), float2( 0.473, -0.480), float2( 0.519,  0.767), float2( 0.185, -0.893),
    float2( 0.507,  0.064), float2( 0.896,  0.412), float2(-0.322, -0.933), float2(-0.792, -0.598) };

// 1 where the texel lets the light through: at or past the receiver, or a caster within the light's
// own housing (its post, bracket and glass, which its cookie shapes instead).
float WxlOmniLit(float z, float depth, float radius, float housing)
{
    return z >= depth || z * radius < housing ? 1.0 : 0.0;
}

// How much of omni slot `slot`'s light reaches the camera-relative point r (surface normal n; zero for
// a point in the air), 0..1.
//   size      the light's source radius, yards: how wide its penumbrae grow
//   taps      filter taps, 1..12 (1: one compare, 4: a plain 2 x 2)
//   bias      yards the receiver is pulled towards the light, plus a slope term of the map's texel
//   rotation  radians the disc turns by, per pixel
float WxlOmniVisibility(Texture2D<float4> table, Texture2D<float> atlas, SamplerState pointClamp, float slot, float3 r,
                        float3 n, float size, float taps, float bias, float rotation)
{
    int base = int(slot) * 20;
    float4 L = table.Load(int3(base, 1, 0));
    float4 T = table.Load(int3(base + 1, 1, 0));
    // Normal offset: the receiver moves off its surface by a texel and a half of the map at its
    // distance (more at grazing light), so a sloped surface never reads its own depth back (acne).
    if (dot(n, n) > 0.5)
    {
        float3 u = r - L.xyz;
        float du = length(u);
        float grazing = 1.0 - saturate(dot(n, -u / max(du, 1e-4)));
        r += n * (2.0 * du / max(T.z, 1.0)) * (1.5 + 3.0 * grazing);
    }
    float3 v = r - L.xyz;
    float R = L.w;
    float d2 = dot(v, v);
    if (R <= 0.0 || d2 >= R * R) return 1.0;
    float3 ax = abs(v);
    float face = ax.x >= ax.y && ax.x >= ax.z ? (v.x > 0.0 ? 0.0 : 1.0) : (ax.y >= ax.z ? (v.y > 0.0 ? 2.0 : 3.0) : (v.z > 0.0 ? 4.0 : 5.0));
    int row = base + 2 + int(face) * 3;
    float4 h = float4(r, 1.0);
    float3 s = float3(dot(h, table.Load(int3(row, 1, 0))), dot(h, table.Load(int3(row + 1, 1, 0))), dot(h, table.Load(int3(row + 2, 1, 0))));
    if (s.z <= 0.02) return 1.0;
    float2 uv = s.xy / s.z;
    float2 cellSize = float2(0.25, 0.5);
    float2 cell = float2(fmod(face, 4.0), floor(face * 0.25)) * cellSize;
    if (any(uv < cell - T.xy) || any(uv > cell + cellSize + T.xy)) return 1.0;
    float2 lo = cell + 0.5 * T.xy, hi = cell + cellSize - 0.5 * T.xy;

    // A 90-degree face spans 2 z yards across its cell: the texel at this depth, in yards.
    float texelYards = 2.0 * s.z / max(T.z, 1.0);
    float NdotL = dot(n, n) > 0.5 ? saturate(dot(n, -v * rsqrt(d2))) : 1.0;
    float slope = min(sqrt(1.0 - NdotL * NdotL) / max(NdotL, 0.15), 6.0);
    float depth = (s.z - bias - 1.5 * texelYards * slope) / R;

    if (taps < 1.5)
        return WxlOmniLit(atlas.SampleLevel(pointClamp, clamp(uv, lo, hi), 0), depth, R, T.w);
    float2 rc = float2(cos(rotation), sin(rotation));
    if (taps < 4.5)
    {
        float lit = 0.0;
        [unroll] for (int t = 0; t < 4; ++t)
        {
            float2 o = (float2(float(t & 1), float(t >> 1)) - 0.5) * T.xy;
            lit += 0.25 * WxlOmniLit(atlas.SampleLevel(pointClamp, clamp(uv + o, lo, hi), 0), depth, R, T.w);
        }
        return lit;
    }

    // Texture coordinates per yard on this face at depth z.
    float2 uvPerYard = cellSize / (2.0 * s.z);
    // Blockers: over the source's footprint, a few texels at least and at most.
    float2 search = clamp(size * uvPerYard, T.xy, 6.0 * T.xy);
    float blocker = 0.0, blockers = 0.0;
    [unroll] for (int b = 0; b < 5; ++b)
    {
        float2 o = b == 0 ? 0.0 : kWxlOmniDisc[b * 2] * search;
        o = float2(o.x * rc.x - o.y * rc.y, o.x * rc.y + o.y * rc.x);
        float z = atlas.SampleLevel(pointClamp, clamp(uv + o, lo, hi), 0);
        if (WxlOmniLit(z, depth, R, T.w) < 0.5) { blocker += z; blockers += 1.0; }
    }
    if (blockers < 0.5) return 1.0;
    float zb = max(blocker / blockers * R, 0.05);
    // The penumbra those blockers cast on the receiver, in yards, then in the map.
    float penumbra = size * max(s.z - zb, 0.0) / zb;
    float2 radius = clamp(penumbra * uvPerYard, 0.75 * T.xy, 8.0 * T.xy);
    float lit = 0.0;
    [loop] for (int t = 0; t < 12; ++t)
    {
        if (float(t) >= taps) break;
        float2 o = kWxlOmniDisc[t] * radius;
        o = float2(o.x * rc.x - o.y * rc.y, o.x * rc.y + o.y * rc.x);
        lit += WxlOmniLit(atlas.SampleLevel(pointClamp, clamp(uv + o, lo, hi), 0), depth, R, T.w);
    }
    return lit / min(taps, 12.0);
}

// Whether camera-relative r lies within the receiver skip of the light at list index `index` (the omni
// table's row 0 z, yards): a carried light's own carrier, which its map must not darken.
bool WxlOmniSkipsReceiver(Texture2D<float4> table, float index, float slot, float3 r)
{
    float skip = table.Load(int3(int(index), 0, 0)).z;
    if (skip <= 0.0) return false;
    float3 v = r - table.Load(int3(int(slot) * 20, 1, 0)).xyz;
    return dot(v, v) < skip * skip;
}

#endif
