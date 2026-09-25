// wxl-forever surface lighting: the registers and helpers every surface pass shares.
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

#ifndef WXL_FOREVER_SURFACE_HLSLI
#define WXL_FOREVER_SURFACE_HLSLI

#include "lights/shaders/cookies.hlsli"

// Registers, set by surface/Surface.cpp:
//   c0..c3   columns of inverse(view * projection), clip -> camera-relative
//   c4       1 / width, 1 / height, view (see Settings::view; 9 = engine materials added light), 0
//   c5       world depth range minZ, 1 / (maxZ - minZ), maxZ, 0
//   c6       light count, contact shadow steps, strength, wrap
//   c7       clusterC (lights.hlsli)
//   c8       clusterD (lights.hlsli)
//   c9       specular scale, wetness, scene lighting estimate (linear), extinction per yard
//   c10..c13 columns of view * projection, camera-relative
//   c14      albedo assumed where the scene is too dark to tell, softness, knee of apply's final
//            compression (luminance), penumbra scale on each light's source size
//   c15      1 / half width, 1 / half height, half width, half height (the normals' resolution)
//   c16..c19 columns of the previous frame's view * projection, camera-relative
//   c20      camera movement since the previous frame xyz, history cut on change (sigmas, 0 off)
//   c21      blur direction (half-resolution texels), taps each side, cosine of the crease angle
//   c22      this frame's rotation of the blue-noise channels
//   c23      1 = external normals bound (s4: xyz * 0.5 + 0.5, a = 1 where valid), 1 = light profiles
//            off, hot core share (a flame's light whitens near its source), 0
//   c24      1 / quarter width, 1 / quarter height, quarter width, quarter height (indirect light)
//   c25      indirect: strength, radius (yards), emissive share, 0
//   c26..c41 units near the camera: feet (camera-relative xyz), height
//   c42      unit count, unit radius (yards), share of engine lights on units, 0
//   c43      temporal: longest history (frames), clamp width (sigmas), depth tolerance (share of
//            the distance), normal tolerance (cosine)
//   c44      a-trous: step (pixels), depth tolerance (share of the distance), normal power,
//            luminance tolerance (sigmas)
//   c45      1 / width, 1 / height of the pass being drawn, its scale in full-resolution pixels, 0
//   c46..c48 rows of the camera-relative view's rotation: world normal = (row . view normal)
//   c49..c52 omni shadow lights: camera-relative position, radius (radius 0 = none)
//   c53..c124 their face rows: light k, face f, row j at 53 + k * 18 + f * 3 + j, taking a
//            camera-relative (x, y, z, 1) to (u * w, v * w, w) in its atlas
//   c125     shadows: omni bias (yards), slope bias (texels), contact shadow length (yards),
//            omni filter taps (0 = the plain 4-tap filter)
//   c126     1 / lighting width, 1 / lighting height, lighting width, lighting height
//   c127     tone: highlight cap (applied luminance, 0 = off), albedo cap, unit mask margin (yards)
//            for the buffer the engine reads, 0
//   c128     omni face size (texels), lighting scale (full pixels per lighting pixel), 1 = the
//            normals texture is at the lighting resolution, casters nearer the light than this many
//            yards do not shadow it (its own housing; the cookie shapes that)
//   c129     source size of each omni slot's light (yards; shaders/shading.hlsli, omniSizes)
//   c130     cookies (lights::cookies::Constants): face width, face height, strength (0 off), floor
//   c131     cookies: half texel u, half texel v, 1 = the Cookie factor view, flame spared share; the atlas on s15
//   c132     cookies: share of a texel's glass colour a light takes, 0, 0, 0
//   c149     1 / atlas width, 1 / atlas height, 1 = omni shadows on, 0
//   c150     1 = engine materials added last frame's light buffer, the share of it they added (it
//            fades out with fast camera motion; apply adds the rest), 0, 0
//   c151     omni atlas cell width, cell height (texture coordinates), cells across, near plane (yards)
//   c152     carried lights: core radius (yards, 0 off), share on the carrier, cap on any light's
//            strength near its source (times its colour), 1 = carried handling on
//   c153     moving lights: count, 0, clamp width near one (sigmas),
//            1 = the omni shadow term goes to its own target and joins after the history
//   c154..c157 moving lights: camera-relative position, radius
//   c158     share of the indirect light on units, room count, share of a room's light that reaches
//            another floor (1 = rooms not gated), 0
//   c159..c194 the light service's rooms (lights::rooms), 3 rows each, smallest first
//   c195     each omni slot's shadow share, 0..1: it fades in when a light takes the slot and out
//            before the slot changes hands
float4 inv0 : register(c0);
float4 inv1 : register(c1);
float4 inv2 : register(c2);
float4 inv3 : register(c3);
float4 screen : register(c4);
float4 range : register(c5);
float4 lightsC : register(c6);
float4 clusterC : register(c7);
float4 clusterD : register(c8);
float4 shading : register(c9);
float4 vp0 : register(c10);
float4 vp1 : register(c11);
float4 vp2 : register(c12);
float4 vp3 : register(c13);
float4 albedoC : register(c14);
float4 halfC : register(c15);
float4 prev0 : register(c16);
float4 prev1 : register(c17);
float4 prev2 : register(c18);
float4 prev3 : register(c19);
float4 moved : register(c20);
float4 blurC : register(c21);
float4 blueRot : register(c22);
float4 sourcesC : register(c23);
float4 quarterC : register(c24);
float4 giC : register(c25);
float4 units[16] : register(c26);
float4 unitsC : register(c42);
float4 temporalC : register(c43);
float4 atrousC : register(c44);
float4 resC : register(c45);
float4 viewRows[3] : register(c46);
float4 omniLights[4] : register(c49);
float4 omniRows[72] : register(c53);
float4 shadowC : register(c125);
float4 cookieC : register(c130);
float4 cookieD : register(c131);
float4 cookieE : register(c132);
float4 lowC : register(c126);
float4 toneC : register(c127);
float4 faceC : register(c128);
float4 omniC : register(c149);
float4 engineC : register(c150);
float4 omniCell : register(c151);
float4 carriedC : register(c152);
float4 movingC : register(c153);
float4 movingLights[4] : register(c154);
float4 unitsGiC : register(c158);
float4 roomRows[36] : register(c159);
float4 omniFade : register(c195);

// 1 where a camera-relative point lies within margin yards of a unit (a player or creature near the
// camera): those pixels move on their own, so their history is short, and the engine lights them.
float OnUnitWide(float3 p, float margin)
{
    float r = unitsC.y + margin;
    for (int i = 0; i < 16; ++i)
    {
        if ((float)i >= unitsC.x) break;
        float3 d = p - units[i].xyz;
        if (dot(d.xy, d.xy) < r * r && d.z > -0.3 - margin && d.z < units[i].w + 0.3 + margin) return 1.0;
    }
    return 0.0;
}

float OnUnit(float3 p) { return OnUnitWide(p, 0.0); }

// The smallest room holding a camera-relative point (its index), -1 outside every room.
float SurfaceRoom(float3 p)
{
    [loop] for (int b = 0; b < 12; ++b)
    {
        if ((float)b >= unitsGiC.y) break;
        if (InRoom(p, roomRows[b * 3], roomRows[b * 3 + 1], roomRows[b * 3 + 2]) > 0.5) return (float)b;
    }
    return -1.0;
}

// The share of the light buffer the engine's materials take at a point of the screen: all of it
// inside, easing to none over the outer 8% towards each edge, so where their reprojection of last
// frame's buffer runs out (the edge a moving view uncovers) apply takes over along a gradient, not
// a seam. Resolve scales the buffer the engine reads by it; apply adds the rest.
float EngineEdge(float2 uv)
{
    float2 e = min(uv, 1.0 - uv);
    return saturate(min(e.x, e.y) / 0.08);
}

float3 ToYCoCg(float3 c)
{
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 FromYCoCg(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

sampler2D depthTex : register(s0);

// The camera-relative surface point at uv and its distance; w < 0 for the sky.
float4 SurfaceAt(float2 uv)
{
    float depth = tex2Dlod(depthTex, float4(uv, 0, 0)).r;
    if (depth > range.z + 0.00001) return float4(0, 0, 0, -1);
    float depth01 = saturate((depth - range.x) * range.y);
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth01 * 2.0 - 1.0, 1.0);
    float4 h = float4(dot(clip, inv0), dot(clip, inv1), dot(clip, inv2), dot(clip, inv3));
    float3 p = h.xyz / h.w;
    return float4(p, length(p));
}

// The full-resolution texel a pixel of a pass drawn at resC.z full pixels per pixel stands for: the
// first texel of its block, the same one every pass at that resolution reads, so the depth, the
// normal and the history all agree on which surface point a texel is.
float2 PassUv(float2 vpos) { return (vpos * resC.z + 0.5) * screen.xy; }

// The texture coordinate, in a target drawn at scale full pixels per texel, of the texel whose
// surface point is the full-resolution position uv. PassUv gives a texel its block's first full
// pixel, which lies (scale - 1) / 2 full pixels before the texel's centre; a pass that reads another
// grid by position (the history, an upsample) adds that back, or its reads slide by that much.
float2 TexelOf(float2 uv, float scale) { return uv + (scale - 1.0) * 0.5 * screen.xy; }

float3 Encode(float3 c) { return pow(max(c, 0.0), 1.0 / 2.2); }

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float3 Linear(float3 c) { return pow(max(c, 0.0), 2.2); }

// The surface's albedo from the scene colour over the light that made it: the scene's own lighting
// plus, where the engine's material already added the light buffer (added, its luminance), that
// light too, so a lit surface is not taken for a white one. The luminance is soft-capped (toneC.y)
// instead of clipped, so a texture keeps its contrast under a lamp. Where the scene is far darker
// than a lit surface would be (night, shade) the albedo is assumed: albedoC.x times the pixel's
// brightness against the scene's around it (localLuma), carrying a third of its hue, no channel
// past 0.6. That ratio is the texture: a dark crevice stays darker than the planks around it.
// No channel ever passes toneC.y, so a light never adds more than that times itself.
float3 Albedo(float3 scene, float added, float localLuma)
{
    float sceneLuma = Luma(scene);
    float3 fromLighting = scene / max(shading.z + added * lightsC.z, 0.02);
    float l = Luma(fromLighting);
    float knee = toneC.y * 0.6, room = toneC.y - knee;
    float mapped = l <= knee ? l : knee + (l - knee) * room / (room + (l - knee));
    fromLighting *= mapped / max(l, 0.0001);
    float3 chroma = scene / max(sceneLuma, 0.0001);
    float contrast = clamp(sceneLuma / max(localLuma, 0.0001), 0.0, 2.0);
    float3 fromHue = albedoC.x * contrast * lerp(1.0, chroma, 0.35);
    fromHue *= min(1.0, 0.6 / max(max(fromHue.r, fromHue.g), max(fromHue.b, 0.0001)));
    float dark = saturate(1.0 - sceneLuma / max(shading.z * albedoC.x * 0.5, 0.0001));
    return min(lerp(fromLighting, fromHue, dark), toneC.y);
}

// The light buffer's highlight cap: the applied luminance (times strength) rolls off towards toneC.x,
// gently, so a lamp's pool keeps its gradient and a surface touching the lamp does not burn out.
// Apply and the engine's own materials both read a capped buffer, so both get the same look.
float4 CapLight(float4 light)
{
    if (toneC.x <= 0.0) return light;
    return light / (1.0 + Luma(light.rgb) * lightsC.z / toneC.x);
}

// The scene's mean linear luminance around uv: the pixel and four taps a few pixels out, the
// reference Albedo holds a dark pixel's brightness against.
float LocalLuma(sampler2D scene, float2 uv, float centre)
{
    float2 o = screen.xy * 6.0;
    float sum = centre;
    sum += Luma(Linear(tex2Dlod(scene, float4(uv + float2(o.x, 0.0), 0, 0)).rgb));
    sum += Luma(Linear(tex2Dlod(scene, float4(uv - float2(o.x, 0.0), 0, 0)).rgb));
    sum += Luma(Linear(tex2Dlod(scene, float4(uv + float2(0.0, o.y), 0, 0)).rgb));
    sum += Luma(Linear(tex2Dlod(scene, float4(uv - float2(0.0, o.y), 0, 0)).rgb));
    return sum * 0.2;
}

#endif
