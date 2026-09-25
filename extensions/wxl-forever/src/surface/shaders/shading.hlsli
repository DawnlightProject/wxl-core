// wxl-forever surface lighting: the surface normal and the omni shadow maps, shared by the light and omni passes.
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

#ifndef WXL_FOREVER_SURFACE_SHADING_HLSLI
#define WXL_FOREVER_SURFACE_SHADING_HLSLI

#include "surface/shaders/surface.hlsli"
#include "core/shaders/bluenoise.hlsli"

// Normals from the depth (s1, half resolution) or the engine's G-buffer (s4), and the omni shadow
// maps of the four most important lights (s11..s14): the light pass reads each light's share of the
// maps from the omni pass (omni.ps.hlsl), which evaluates them here.
sampler2D normalsTex : register(s1);
sampler2D blueTex : register(s10);
sampler2D omni0 : register(s11);
sampler2D omni1 : register(s12);
sampler2D omni2 : register(s13);
sampler2D omni3 : register(s14);
sampler2D externalNormals : register(s4);

// Source sizes of the four omni lights, yards: how wide their penumbras grow.
float4 omniSizes : register(c129);

// A G-buffer normal, in the engine's view space, turned into camera-relative world space.
float3 GNormal(float4 g)
{
    float3 v = g.xyz * 2.0 - 1.0;
    return normalize(float3(dot(viewRows[0].xyz, v), dot(viewRows[1].xyz, v), dot(viewRows[2].xyz, v)));
}

float3 DepthNormal(float2 vpos, float2 uv, float4 here);

// The surface normal. External normals (a G-buffer the engine's own shaders write) win where they
// are valid; at the edge of what the engine wrote (a model's outline over terrain) they blend with
// the depth's normal by how many neighbours carry one, so the seam does not show.
float3 SurfaceNormal(float2 vpos, float2 uv, float4 here)
{
    if (sourcesC.x > 0.5)
    {
        float4 g = tex2Dlod(externalNormals, float4(uv, 0, 0));
        if (g.a > 0.25)
        {
            float2 sx = float2(screen.x * resC.z, 0.0), sy = float2(0.0, screen.y * resC.z);
            float cover = 0.0;
            cover += tex2Dlod(externalNormals, float4(uv + sx, 0, 0)).a > 0.25 ? 0.25 : 0.0;
            cover += tex2Dlod(externalNormals, float4(uv - sx, 0, 0)).a > 0.25 ? 0.25 : 0.0;
            cover += tex2Dlod(externalNormals, float4(uv + sy, 0, 0)).a > 0.25 ? 0.25 : 0.0;
            cover += tex2Dlod(externalNormals, float4(uv - sy, 0, 0)).a > 0.25 ? 0.25 : 0.0;
            float3 gn = GNormal(g);
            return cover > 0.99 ? gn : normalize(lerp(DepthNormal(vpos, uv, here), gn, 0.5 + 0.5 * cover));
        }
    }
    return DepthNormal(vpos, uv, here);
}

// The depth's own normal, reconstructed and smoothed at half resolution. Drawn at that resolution
// this pixel simply reads its own texel. Drawn at full resolution the 3 x 3 half-resolution texels
// around are brought here, each kept only if its distance is within 2% of this pixel's (a
// silhouette's other side is rejected, not blended) and weighted by closeness; when none qualifies
// (a thin edge the half-resolution grid missed) the normal is rebuilt here from the depth.
float3 DepthNormal(float2 vpos, float2 uv, float4 here)
{
    if (faceC.z > 0.5) return normalize(tex2Dlod(normalsTex, float4((vpos + 0.5) * halfC.xy, 0, 0)).xyz);

    float2 q = uv * halfC.zw - 0.5;
    float2 base = floor(q);
    float2 f = q - base;
    float3 sum = 0.0;
    float total = 0.0;
    for (int y = -1; y <= 2; ++y)
        for (int x = -1; x <= 2; ++x)
        {
            if ((x == -1 || x == 2) && (y == -1 || y == 2)) continue;
            float4 s = tex2Dlod(normalsTex, float4((base + float2(x, y) + 0.5) * halfC.xy, 0, 0));
            if (s.w < 0.0) continue;
            float gap = abs(s.w - here.w) / here.w;
            if (gap > 0.02) continue;
            float2 o = float2(x, y) - f;
            float w = exp(-dot(o, o)) * (1.0 - gap * 40.0);
            sum += s.xyz * w;
            total += w;
        }
    if (total > 0.0001) return normalize(sum);

    float2 step = screen.xy;
    float4 right = SurfaceAt(uv + float2(step.x, 0.0));
    float4 left  = SurfaceAt(uv - float2(step.x, 0.0));
    float4 down  = SurfaceAt(uv + float2(0.0, step.y));
    float4 up    = SurfaceAt(uv - float2(0.0, step.y));
    float3 dx = (right.w > 0.0 && (left.w < 0.0 || abs(right.w - here.w) < abs(left.w - here.w)))
              ? right.xyz - here.xyz : here.xyz - left.xyz;
    float3 dy = (down.w > 0.0 && (up.w < 0.0 || abs(down.w - here.w) < abs(up.w - here.w)))
              ? down.xyz - here.xyz : here.xyz - up.xyz;
    float3 n = normalize(cross(dx, dy));
    return dot(n, here.xyz) > 0.0 ? -n : n;
}

// Poisson disc for the omni filter, unit radius.
static const float2 kDisc[12] = {
    float2(-0.326, -0.406), float2(-0.840, -0.074), float2(-0.696,  0.457), float2(-0.203,  0.621),
    float2( 0.962, -0.195), float2( 0.473, -0.480), float2( 0.519,  0.767), float2( 0.185, -0.893),
    float2( 0.507,  0.064), float2( 0.896,  0.412), float2(-0.322, -0.933), float2(-0.792, -0.598) };

// One omni light's shadow at p (surface normal n): the face whose axis the point lies along (+X -X
// +Y -Y +Z -Z, the core's order), its rows, then the atlas read within that face's cell. Lit
// wherever the map cannot answer: past the light's radius, before the near plane, on a face never
// rendered (its rows are zero) or off its cell. The receiver's depth is pulled back by a bias in
// yards plus a slope term of the map's texel at that depth, so a floor at a grazing angle shows no
// acne and a shadow still meets its caster. The filter is a blocker search over the source's
// footprint, then a disc as wide as the penumbra those blockers cast from a source omniSizes wide
// (a lantern's cage bar an inch from the flame stays crisp on the wall, a post yards away softens),
// rotated per pixel by blue noise; shadowC.w at 0 is the plain 4-tap filter.
// A map texel lets the light through when it lies at or past the receiver, or when its caster sits
// within faceC.w yards of the light: the lamp's own head, glass and bracket, which its cookie
// shapes instead of darkening everything.
float MapLit(float z, float depth, float R)
{
    return z >= depth || z * R < faceC.w ? 1.0 : 0.0;
}

// face returns the face used, -1 when none, 6 when the point fell off its cell.
float OmniSlot(sampler2D atlas, int slot, float3 p, float3 n, float size, float rot, out float face)
{
    face = -1.0;
    float3 v = p - omniLights[slot].xyz;
    float R = omniLights[slot].w;
    float d2 = dot(v, v);
    if (d2 >= R * R) return 1.0;
    float3 ax = abs(v);
    int f0 = ax.x >= ax.y && ax.x >= ax.z ? (v.x > 0.0 ? 0 : 1) : (ax.y >= ax.z ? (v.y > 0.0 ? 2 : 3) : (v.z > 0.0 ? 4 : 5));
    float3 s = 0.0;
    float4 h = float4(p, 1.0);
    for (int f = 0; f < 6; ++f)
        if (f == f0)
            s = float3(dot(h, omniRows[slot * 18 + f * 3]), dot(h, omniRows[slot * 18 + f * 3 + 1]),
                       dot(h, omniRows[slot * 18 + f * 3 + 2]));
    if (s.z <= omniCell.w) return 1.0;
    face = f0;
    float2 uv = s.xy / s.z;
    float2 cell = float2(fmod(face, omniCell.z), floor(face / omniCell.z)) * omniCell.xy;
    if (any(uv < cell - omniC.xy) || any(uv > cell + omniCell.xy + omniC.xy)) { face = 6.0; return 1.0; }
    float2 lo = cell + 0.5 * omniC.xy, hi = cell + omniCell.xy - 0.5 * omniC.xy;

    // A 90-degree face spans 2 z yards across its cell: the texel at this depth, in yards.
    float texelYards = 2.0 * s.z / faceC.x;
    float NdotL = saturate(dot(n, -v * rsqrt(d2)));
    float slope = min(sqrt(1.0 - NdotL * NdotL) / max(NdotL, 0.15), 6.0);
    float depth = (s.z - shadowC.x - shadowC.y * texelYards * slope) / R;

    if (shadowC.w < 4.5)
    {
        float lit = 0.0;
        for (int t = 0; t < 4; ++t)
        {
            float2 o = (float2(fmod(t, 2.0), floor(t * 0.5)) - 0.5) * omniC.xy;
            lit += 0.25 * MapLit(tex2Dlod(atlas, float4(clamp(uv + o, lo, hi), 0, 0)).r, depth, R);
        }
        return lerp(1.0, lit, omniFade[slot]);
    }

    // Texture coordinates per yard on this face at depth z: the cell width over 2 z.
    float uvPerYard = omniCell.x / (2.0 * s.z);
    float2 rc = float2(cos(rot), sin(rot));
    // Blockers: where a caster anywhere between the near plane and the surface could hide the
    // source from this point, clamped to a few texels.
    float search = clamp(size * (s.z - omniCell.w) / max(omniCell.w, 0.01) * uvPerYard, omniC.x, 6.0 * omniC.x);
    float blocker = 0.0, blockers = 0.0;
    float centre = tex2Dlod(atlas, float4(clamp(uv, lo, hi), 0, 0)).r;
    if (MapLit(centre, depth, R) < 0.5) { blocker += centre; blockers += 1.0; }
    for (int b = 0; b < 4; ++b)
    {
        float2 o = kDisc[b * 3] * search;
        o = float2(o.x * rc.x - o.y * rc.y, o.x * rc.y + o.y * rc.x);
        float z = tex2Dlod(atlas, float4(clamp(uv + o, lo, hi), 0, 0)).r;
        if (MapLit(z, depth, R) < 0.5) { blocker += z; blockers += 1.0; }
    }
    if (blockers < 0.5) return 1.0;
    float zb = blocker / blockers * R;
    // The penumbra those blockers cast on the surface, in yards, then in the map.
    float penumbra = size * (s.z - zb) / max(zb, omniCell.w);
    float radius = clamp(penumbra * uvPerYard, 0.75 * omniC.x, 8.0 * omniC.x);
    float lit = 0.0;
    [loop] for (int t = 0; t < 12; ++t)
    {
        if ((float)t >= shadowC.w) break;
        float2 o = kDisc[t] * radius;
        o = float2(o.x * rc.x - o.y * rc.y, o.x * rc.y + o.y * rc.x);
        lit += MapLit(tex2Dlod(atlas, float4(clamp(uv + o, lo, hi), 0, 0)).r, depth, R);
    }
    // The slot's shadow fades in and out as its light takes or leaves it.
    return lerp(1.0, lit / min(shadowC.w, 12.0), omniFade[slot]);
}

// The Omni faces view: each face its colour (+X red, -X dark red, +Y green, -Y dark green, +Z blue,
// -Z dark blue), white off its cell, black out of reach, darkened where the map shadows.
float3 FaceColour(float face, float lit)
{
    if (face < -0.5) return 0.0;
    if (face > 5.5) return 1.0;
    float3 c = face < 1.5 ? float3(1, 0, 0) : (face < 3.5 ? float3(0, 1, 0) : float3(0, 0, 1));
    return c * (fmod(face, 2.0) > 0.5 ? 0.35 : 1.0) * (0.3 + 0.7 * lit);
}

#endif
