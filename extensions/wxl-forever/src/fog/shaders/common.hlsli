// wxl-forever fog: the registers every froxel pass shares, and the helpers built on them.
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

#ifndef WXL_FOREVER_FOG_COMMON_HLSLI
#define WXL_FOREVER_FOG_COMMON_HLSLI

#include "core/shaders/bluenoise.hlsli"

// The volume is W x H x Z froxels stored as a 2D atlas: slice k is the tile at column k % tiles,
// row k / tiles. Slices are spaced exponentially in distance from the camera between near and far,
// bent towards the camera (c192): the first slice starts at the camera itself.
// Everything is camera-relative: r is a position minus the camera, so large world coordinates never
// reach the shader except where noise and terrain need the absolute place.
//
// Registers shared by the passes, set by Froxel.cpp:
//   c0..c3     columns of inverse(view * projection), clip -> camera-relative
//   c4         1/screen width, 1/screen height, seconds, view (0 fog, 5 slices, 6 density,
//              7 scatter, 8 transmittance, 9 history clamp, 11 lights only,
//              13 terrain, 14 linear depth, 15 raw depth, 16 indoor/outdoor)
//   c5         froxels W, H, Z, tiles per atlas row
//   c6         1/atlas width, 1/atlas height, near distance, log(far / near)
//   c7         world depth range minZ, 1 / (maxZ - minZ), maxZ, history weight (0 resets)
//   c8         this frame's jitter rotation (x, y, slice; 0..1, added to each froxel's own
//              offset), tonemap white (<= 1 off)
//   c9..c12    columns of the previous frame's view * projection, camera-relative
//   c13        camera movement since the previous frame, exposure
//   c14        camera world xyz, interior box count
//   c15        direction towards the drawn sun
//   c16        direction towards the drawn moon
//   c17        celestial light rgb (smoothed, glare dimming undone)
//   c18        direction self-shadow marches along, 0
//   c19        density volume count, cell width per yard of distance, 1 = no history clamp, share of a
//              room's light that reaches another floor (1 = rooms not gated)
//   c20..c31   outdoor profile, 12 rows:
//                P0 (density, falloff, baseZ, noise tiles per yard)
//                P1 (first layer drift xyz, noise strength)
//                P2 (albedo rgb, ambient)
//                P3 (coverage, erosion, billow, warp)
//                P4 (sun scatter, moon scatter, light scatter, max scatter)
//                P5 (forward g, back g, back share, detail tiles per yard)
//                P6 (shadow strength, shadow reach, powder, wake strength)
//                P7 (ambient low, ambient high, ground follow, valley pooling)
//                P8 (second layer drift xyz with roll, second layer share)
//                P9 (contact, contact thickness, wake swirl, near strength)
//                P10 (multiple scattering: energy per octave, extinction per octave,
//                     phase contrast per octave, extra octaves)
//                P11 (sky-driven ambient share, heat, cloud light share, cloud tiles per yard)
//   c32..c43   indoor profile, same layout
//   c44..c79   interior boxes, 3 rows each: the light service's rooms (lights::rooms), smallest first; c80..c91 free
//   c92        light count, 0, soft indoor margin (yards), how far the
//              camera's eased indoorness lags its actual state
//   c93        clusters across, down, deep, cluster texture width
//   c94        1 / cluster texture width, 1 / cluster texture height, near distance, 1 / log(far / near)
//   Lights live in a texture (s8, lights/shaders/lights.hlsli: 128 columns of 4 rows, one column per
//   light). The cluster texture (s9) holds each cluster's list (LightList, LightAt: every light that
//   reaches it, no cap), and the cookie rows (lights/shaders/cookies.hlsli).
//   c116..c175 density volumes, 3 rows each: (r xyz, shape) (extent xyz, density or carved
//              fraction) (falloff, carve, swirl, radius). Shape 0 sphere, 1 box, 2 vertical capsule,
//              3 segment: r is its head, extent the vector to its tail, radius its thickness.
//              4 plume: r is its base, extent the vector to its top, radius the base radius, falloff
//              the top radius, carve 1 for a smoke tint.
//   c176       haze: strength of the volume's continuation past its far distance (0 off), the
//              longest stretch it may continue over (yards), 0, 0
//   c177       cookies (lights::cookies::Constants, cookieE.x): share of a texel's glass colour a
//              lamp takes; share of the glow a light no cookie shapes keeps, 0, 0
//   c178..c179 the baked horizon maps (terrain/shaders/horizon.hlsli: horizonC, horizonD), read by
//              the visibility pass with the atlases on s1..s3
//   c180..c181 free
//   c182       terrain: 1/cell size, fallback ground z, half window (yards), 1 = on
//   c183       near wisps: reach (yards), strength, 1 = on, 0
//   c184       near wisps: turbulence drift xyz, noise tiles per yard
//   c185       contact: strength, thickness (yards), 1 = on, 1 = doorway cross-fade of the shape densities
//   c186       extinction ratio per channel (chromatic extinction), debug far distance
//   c187       1 = Cornette-Shanks phase, dither amount, dither rotation this frame, aerial
//              desaturation
//   c188       sky top rgb
//   c189       sky horizon rgb
//   c190       ground bounce rgb
//   c191       far distance, 8-slice block count, 1 / block atlas width, 1 / block atlas height
//   c192       slice bend b (0 the plain exponential), 1 - b, (1 - b)^2, 1 / (2 b) or 0
//   c193..c196 columns of this frame's view * projection, camera-relative
//   c197       first tangent of the shadow direction xyz, world shadow steps (0 off)
//   c198       second tangent of the shadow direction xyz, 1 = sky occlusion on
//   c199..c203 outdoor profile, second part, 5 rows:
//                Q0 (world shadow strength, world shadow softness, sky occlusion, bank shading)
//                Q1 (macro tiles per yard, macro contrast, macro threshold, macro strength)
//                Q2 (macro drift xy, bank edge erosion, bank shadow reach)
//                Q3 (haze density, haze falloff, haze height above the ground, haze tiles per yard)
//                Q4 (haze drift xyz, haze noise strength)
//   c204..c208 indoor profile, second part, same layout
//   c209       this frame's rotation of the four blue-noise channels (R4 sequence)
//   c210       free
//   c211       immersion: share eased in this frame, density of a full immersion, strength, softening
//              of lights
//   c212       free
//   c213       smoke tint rgb (linear), yards from the camera past which no density volume reaches
//   c214       projectile trails: live segments, yards past which none reaches, tiles across, down
//   c215       1 / trail tile texture width, 1 / its height, 1 / segment texture width, 0
//   c221       omni shadow map slots bound for the lamps (0..4; s4 the table, s11..s14 the atlases), their
//              bias (yards), hot core share, 1 / omni table width
//   c222       cookies (lights::cookies::Constants): face width, face height, strength (0 off), floor
//   c223       cookies: half texel u, half texel v, 1 = the Cookie factor view, flame spared share; the atlas on s15
//   Views 17 shadow visibility, 18 sky visibility, 19 fog banks (macro map), 20 noise pattern,
//   21 projectile trails.
float4 inv0 : register(c0);
float4 inv1 : register(c1);
float4 inv2 : register(c2);
float4 inv3 : register(c3);
float4 screen : register(c4);
float4 grid : register(c5);
float4 atlas : register(c6);
float4 range : register(c7);
float4 jitter : register(c8);
float4 prev0 : register(c9);
float4 prev1 : register(c10);
float4 prev2 : register(c11);
float4 prev3 : register(c12);
float4 moved : register(c13);
float4 cam : register(c14);
float4 toSun : register(c15);     // w: a carried light's core, yards
float4 toMoon : register(c16);    // w: the cap on any light near its source, times its colour
float4 celestial : register(c17);
float4 shadowDir : register(c18);
float4 counts : register(c19);
float4 outP[12] : register(c20);
float4 inP[12] : register(c32);
float4 boxes[36] : register(c44);
float4 lightsC : register(c92);
float4 clusterC : register(c93);
float4 clusterD : register(c94);
float4 vols[60] : register(c116);
float4 hazeC : register(c176);
float4 cookieTintC : register(c177);
float4 terrainC : register(c182);
float4 nearC : register(c183);
float4 nearD : register(c184);
float4 contactC : register(c185);
float4 realismA : register(c186);
float4 realismB : register(c187);
float4 skyTop : register(c188);
float4 skyHorizon : register(c189);
float4 skyGround : register(c190);
float4 frame2 : register(c191);
float4 sliceC : register(c192);
float4 vp0 : register(c193);
float4 vp1 : register(c194);
float4 vp2 : register(c195);
float4 vp3 : register(c196);
float4 shadowT1 : register(c197);
float4 shadowT2 : register(c198);
float4 outQ[5] : register(c199);
float4 inQ[5] : register(c204);
float4 blueRot : register(c209);
float4 immersionC : register(c211);
float4 smokeC : register(c213);
float4 trailC : register(c214);
float4 trailD : register(c215);
sampler2D blueTex : register(s10);
sampler2D depthTex : register(s0);

// Blue noise at an integer pixel: four independent channels of the suite's tile.
float4 BlueRaw(float2 pixel)
{
    return BlueNoiseAt(blueTex, pixel);
}

// The same, each channel rotated this frame, so every pixel walks a low-discrepancy sequence over
// time while its neighbours stay decorrelated: undersampling leaves fine grain that the history
// averages, never a fixed lattice.
float4 Blue(float2 pixel)
{
    return frac(BlueRaw(pixel) + blueRot);
}

// Slice s (0..Z) lies at near * exp(L * h(s / Z)) with h(u) = (1 - b) u + b u^2: at b = 0 the plain
// exponential, at b > 0 the near slices are thinner and the far ones thicker by the same share, so
// the wisps' scale near the camera is held in more slices. L is log(far / near); the resolve passes
// last frame's. Slice 0 is integrated from the camera itself, not from the near distance.
float SliceToDistL(float s, float L)
{
    float u = s / grid.z;
    return atlas.z * exp(L * u * (sliceC.y + sliceC.x * u));
}

float DistToSliceL(float d, float L)
{
    float x = log(max(d, 0.0001) / atlas.z) / L;
    if (sliceC.x <= 0.0) return x * grid.z;
    return (sqrt(max(sliceC.z + 4.0 * sliceC.x * x, 0.0)) - sliceC.y) * sliceC.w * grid.z;
}

float SliceToDist(float s) { return SliceToDistL(s, atlas.w); }
float DistToSlice(float d) { return DistToSliceL(d, atlas.w); }

// The distance a slice's integration starts at: the camera for slice 0.
float SliceStart(float s) { return s < 0.5 ? 0.0 : SliceToDist(s); }

// Texel-space position of a cell (continuous, 0..W, 0..H) inside slice k's tile, as atlas uv.
float2 AtlasUV(float2 cell, float k)
{
    float tx = fmod(k, grid.w);
    float ty = floor(k / grid.w);
    return (float2(tx * grid.x, ty * grid.y) + cell) * atlas.xy;
}

// Bilinear inside one slice, clamped so the filter never reaches the next tile.
float4 SampleSlice(sampler2D s, float2 uv01, float k)
{
    float2 cell = clamp(uv01 * grid.xy, 0.5, grid.xy - 0.5);
    return tex2Dlod(s, float4(AtlasUV(cell, k), 0, 0));
}

// Trilinear at a continuous slice coordinate measured at slice centres (centre k sits at k + 0.5).
float4 SampleVolume(sampler2D s, float2 uv01, float sliceCoord)
{
    float z = clamp(sliceCoord - 0.5, 0.0, grid.z - 1.0);
    float k0 = floor(z);
    float k1 = min(k0 + 1.0, grid.z - 1.0);
    return lerp(SampleSlice(s, uv01, k0), SampleSlice(s, uv01, k1), z - k0);
}

float3 RayDir(float2 uv01)
{
    float4 clip = float4(uv01.x * 2.0 - 1.0, 1.0 - uv01.y * 2.0, 0.5, 1.0);
    float4 h = float4(dot(clip, inv0), dot(clip, inv1), dot(clip, inv2), dot(clip, inv3));
    return normalize(h.xyz / h.w);
}

// Distance from the camera of the surface at uv, or the last slice's distance for the sky.
float SurfaceDist(float depth, float2 uv, out bool sky)
{
    sky = depth > range.z + 0.00001;
    if (sky) return frame2.x;
    float depth01 = saturate((depth - range.x) * range.y);
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth01 * 2.0 - 1.0, 1.0);
    float4 h = float4(dot(clip, inv0), dot(clip, inv1), dot(clip, inv2), dot(clip, inv3));
    return length(h.xyz / h.w);
}

float4 Project(float3 q)
{
    float4 h = float4(q, 1.0);
    return float4(dot(h, vp0), dot(h, vp1), dot(h, vp2), dot(h, vp3));
}

// How much the scene hides a point at clip position c, d yards from the camera: behind the surface
// drawn there by more than a bias, but by less than the thickness an occluder is assumed to have, so
// a trunk shades a strip of fog rather than everything behind it. Off screen hides nothing.
float Hidden(float4 c, float d, float ramp)
{
    if (c.w <= 0.1) return 0.0;
    float2 uv = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
    float edge = saturate(min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)) * 20.0);
    if (edge <= 0.0) return 0.0;
    bool sky;
    float surf = SurfaceDist(tex2Dlod(depthTex, float4(uv, 0, 0)).r, uv, sky);
    if (sky) return 0.0;
    float behind = d - surf;
    float bias = 0.3 + 0.01 * d;
    float thick = 2.5 + 0.06 * surf;
    return edge * saturate((behind - bias) / ramp) * saturate((thick - behind) / ramp + 1.0);
}

// Each froxel has its own sample offset (three independent blue-noise channels over cell and
// slice), rotated every frame, so neighbours carry different errors that the history averages out.
// Depth jitter tapers in the far slices, where one slice spans tens of yards.
float3 FroxelOffset(float2 cell, float k)
{
    float3 base = BlueRaw(cell + k * float2(37.0, 91.0)).rgb;
    float3 o = frac(base + jitter.xyz) - 0.5;
    o.z *= lerp(1.0, 0.4, k / grid.z);
    return o;
}

// This frame's jittered sample position of the froxel at cell (integer) in slice k.
float3 FroxelPoint(float2 cell, float k)
{
    float3 o = FroxelOffset(cell, k);
    float2 uv01 = (cell + 0.5 + o.xy) / grid.xy;
    return RayDir(uv01) * SliceToDist(k + 0.5 + o.z);
}

// The froxel's centre, where its stored value belongs and where history is looked up.
float3 FroxelCentre(float2 cell, float k)
{
    return RayDir((cell + 0.5) / grid.xy) * SliceToDist(k + 0.5);
}

// One phase lobe, scaled so an isotropic medium gives 1: Henyey-Greenstein, or Cornette-Shanks,
// which adds the (1 + cos^2) Mie-like shoulder. Both integrate to the same energy, so the dual lobe
// below, a convex blend of two of them, conserves it too.
float Lobe(float cosTheta, float g)
{
    float den = pow(max(1.0 + g * g - 2.0 * g * cosTheta, 0.0001), 1.5);
    float hg = (1.0 - g * g) / den;
    float cs = 1.5 * (1.0 - g * g) * (1.0 + cosTheta * cosTheta) / ((2.0 + g * g) * den);
    return realismB.x > 0.5 ? cs : hg;
}

float Phase(float cosTheta, float4 P5)
{
    return lerp(Lobe(cosTheta, P5.x), Lobe(cosTheta, P5.y), P5.z);
}

// 1 when the camera-relative point lies inside any interior box.
// Signed distance in yards from r to the nearest interior box's boundary: positive inside. Each
// box row maps yards to box units, so its length is the box's units per yard on that axis.
float IndoorDistance(float3 r)
{
    float4 p = float4(r, 1.0);
    float best = -1000.0;
    for (int b = 0; b < 12; ++b)
    {
        if ((float)b >= cam.w) break;
        float4 x = boxes[b * 3], y = boxes[b * 3 + 1], z = boxes[b * 3 + 2];
        float3 q = float3(dot(p, x), dot(p, y), dot(p, z));
        float3 gap = (1.0 - abs(q)) / float3(length(x.xyz), length(y.xyz), length(z.xyz));
        best = max(best, min(gap.x, min(gap.y, gap.z)));
    }
    return best;
}

float IndoorAt(float3 r)
{
    float4 p = float4(r, 1.0);
    for (int b = 0; b < 12; ++b)
    {
        if ((float)b >= cam.w) break;
        float3 q = float3(dot(p, boxes[b * 3]), dot(p, boxes[b * 3 + 1]), dot(p, boxes[b * 3 + 2]));
        if (max(abs(q.x), max(abs(q.y), abs(q.z))) <= 1.0) return 1.0;
    }
    return 0.0;
}

#endif
