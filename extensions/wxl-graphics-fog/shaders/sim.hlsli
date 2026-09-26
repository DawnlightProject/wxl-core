// wxl-graphics-fog: what the clipmap steps share -- the target density each cell relaxes towards (the
// transport layer, the ground mist, the drifting banks, shaped by boiling Perlin-Worley noise) and the
// primitives gameplay, bodies, trails and plumes act through.
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

#ifndef FOG_SIM_HLSLI
#define FOG_SIM_HLSLI

#include "clip.hlsli"
#include "terrain.hlsli"

// --- noise ------------------------------------------------------------------------------------------

// The shape noise's channels are octaves of feature size P/4 (R), P/8, P/16, P/32 for a tile of P yards.
// Two tiles are used: fine (boil[0].w, 16 billow sizes) and coarse (boil[1].w, 8 x fine). A level keeps
// the octaves at least three of its cells wide, so levels agree at large scale and the fine ones add
// detail. The second copy of each tile is turned about the vertical, so the tiling never lines up.
float OctaveWeight(float size) { return pow(size, 0.35); }

float4 ShapeTap(Texture3D<float4> shapeNoise, float3 uvw) { return shapeNoise.SampleLevel(sLinearWrap, uvw, 0); }

// Boiling fBm at a world point, 0..1: each tile sampled twice, scrolling in opposite directions and
// summed, so the pattern evolves without moving; the simulation carries what persists.
float BoilNoise(Texture3D<float4> shapeNoise, uint L, float3 p)
{
    float minSize = CellXY(L) * levelC[L].z;
    float sum = 0.0, norm = 0.0, square = 0.0;
    [unroll] for (int t = 1; t >= 0; --t)
    {
        float P = boil[t].w;
        if (P * 0.25 < minSize) continue;
        float3 uvw = p / P;
        float3 turned = float3(uvw.x * 0.8 - uvw.y * 0.6, uvw.x * 0.6 + uvw.y * 0.8, uvw.z);
        float4 a = ShapeTap(shapeNoise, uvw + boil[t].xyz);
        float4 b = ShapeTap(shapeNoise, turned - boil[t].xyz + float3(0.37, 0.61, 0.13));
        float4 n = 0.5 * (a + b);
        float4 sizes = P * float4(0.25, 0.125, 0.0625, 0.03125);
        float4 w = float4(OctaveWeight(sizes.x), OctaveWeight(sizes.y), OctaveWeight(sizes.z), OctaveWeight(sizes.w));
        w *= step(minSize, sizes);
        // The average of two copies is less contrasty than one: stretched back around the mean.
        n = saturate((n - 0.5) * 1.4 + 0.5);
        sum += dot(n, w);
        norm += w.x + w.y + w.z + w.w;
        square += dot(w, w);
    }
    if (norm <= 0.0) return 0.5;
    // A weighted sum of independent octaves spreads less than one: stretched back to one's spread.
    float spread = sqrt(square) / norm;
    return saturate((sum / norm - 0.5) / max(spread, 0.3) + 0.5);
}

// Coverage c (1 = full fog) reshaped by noise n, Schneider style: below 1 only the noise peaks survive
// (wisps, holes, tendrils); above 1 the fog is full and lumpy. Its mean over uniform n is c.
float ShapeCoverage(float c, float n, float contrast, float denseContrast)
{
    float g;
    if (c <= 1.0)
    {
        float cc = max(c, 0.02);
        g = 2.0 * saturate((n - (1.0 - cc)) / cc) * (c / cc);
    }
    else
    {
        g = c * lerp(1.0, 2.0 * n, denseContrast);
    }
    return lerp(c, g, contrast);
}

// --- the target ---------------------------------------------------------------------------------------

struct TargetInputs
{
    float layerDepth;   // transport layer depth at the point
    float2 layerFlow;   // its velocity, world xy
    float cascade;      // the share of it that came from cascades
    float floorZ;       // the ground heights are measured from, world z
    float bankNoise;    // 0..1 macro noise
    float topNoise;     // 0..1 low-frequency noise for the layer's billowing top
};

// Extinction per yard of outdoor fog the simulation renews towards at world height z. Heights are
// measured from the lower of the fine floor and the level's smoothed ground: a coarse cell over a
// narrow valley keeps the valley's fog at its bottom, and one over a hill holds no ground fog inside
// the hill for interpolation to spread into the air beside it.
float TargetFog(float z, float boil, TargetInputs t)
{
    float h = z - t.floorZ;
    if (Isolated(FOG_ISO_FLAT)) return outA.y * exp(-max(h, 0.0) / max(outA.z, 0.1));

    float d = Isolated(FOG_ISO_NO_LAYER) ? 0.0 : t.layerDepth;
    // Cascade fog pouring down a slope is a density current yards thick, deeper than the thin sheet
    // its flow alone would leave.
    d += t.cascade * min(3.0 * sqrt(max(d, 0.0)), 6.0);
    // Open ground keeps a least depth at night (a share of it by day), between the rivers.
    d = max(d, pourB.w * lerp(transB.y, 1.0, 1.0 - sunDir.w));
    float layer = 0.0;
    if (d > 0.02)
    {
        float soft = outF.z * (0.4 + 0.2 * d);
        float hh = h - (t.topNoise - 0.5) * outF.w * min(d, 10.0);
        float core = 1.0 - smoothstep(d - soft, d + soft, hh);
        // Above the top a thin veil climbs, the higher the deeper the layer; the shaping noise turns it
        // into wisps and billows rising off the top rather than a slab.
        float tall = outK.x * saturate(d / 4.0) + 0.3 * soft;
        float veil = 0.3 * exp(-max(hh - d, 0.0) / max(tall, 0.1));
        layer = outA.x * max(core, veil) * saturate(d / 0.6);
    }
    float mist = outA.y * exp(-max(h, 0.0) / max(outA.z, 0.1));

    float cover = saturate((t.bankNoise - (1.0 - outB.z)) / 0.18);
    float bank = lerp(1.0, cover, outB.w);
    float macro = layer * lerp(1.0, bank, 0.4) + mist * bank;
    // Under the ground (caves, points below the terrain) the outdoor fog thins out.
    macro *= saturate(1.0 + (h + 0.5) / 1.5);

    float ref = max(max(outA.x, outA.y), 1e-5);
    return ref * ShapeCoverage(macro / ref, boil, outF.x, outF.y);
}

// Where the layer runs fast (and the more for cascade fog), the shaping noise is stretched along its
// fall: the fog pours in long strands, not round billows. p's coordinates, stretched.
float3 StreakPoint(float3 p, TargetInputs t)
{
    float speed = length(t.layerFlow);
    if (pourB.y <= 0.0 || speed < 0.2 || t.layerDepth < 0.05) return p;
    // The flow runs at drainage times the slope: the fall's steepness follows from its speed.
    float3 fall = normalize(float3(t.layerFlow, -speed * speed / max(transA.x, 0.5)));
    float stretch = 1.0 + pourB.y * saturate(speed / 3.0) * (0.4 + 0.6 * t.cascade);
    return p - fall * dot(p, fall) * (1.0 - 1.0 / stretch);
}

// Samples what the target needs at a world point over a level's ground G.
TargetInputs GatherTarget(Texture2D<float4> layerTex, Texture2D<float> tracerTex, Texture2D<float2> floorTex, Texture3D<float4> shapeNoise,
                          float3 p, float G)
{
    TargetInputs t;
    float2 g = WorldToGrid(p.xy);
    bool resident = blockMask.w > 0.5 && TileResident(g);
    float4 lay = resident ? layerTex.SampleLevel(sLinearWrap, GridUv(g), 0) : 0.0;
    t.layerDepth = lay.x;
    t.layerFlow = lay.yz;
    t.cascade = resident ? tracerTex.SampleLevel(sLinearWrap, GridUv(g), 0) : 0.0;
    t.floorZ = min(FloorAt(floorTex, p.xy, 0.0).r, G);
    float bankScale = max(outJ.z, 10.0);
    t.bankNoise = shapeNoise.SampleLevel(sLinearWrap, float3(p.xy / bankScale + drift.xy, 0.37), 0).r;
    t.topNoise = shapeNoise.SampleLevel(sLinearWrap, float3(p.xy / max(outJ.x * 6.0, 1.0) + drift.xy * 0.5, 0.71), 0).g;
    return t;
}

// --- the step's velocity field ----------------------------------------------------------------------------

// The floor mip a level's ground is read from: about one texel per cell.
float GroundMip(uint L) { return max(log2(CellXY(L) / kTexelYards), 0.0); }

// The velocity image of the level being stepped: half resolution, aligned to its new window.
float3 VelAt(Texture3D<float4> velTex, uint L, float2 xy, float h)
{
    float2 rel = xy * levelA[L].w - levelB[L].xy;
    float3 uvw = float3(rel / kN, (h - HMin(L)) / (CellZ(L) * kNZ));
    return velTex.SampleLevel(sLinearClamp, uvw, 0).xyz;
}

// The w coordinate of a height in the single-level scratch images (the forward estimate).
float ScratchW(uint L, float h) { return clamp((h - HMin(L)) / CellZ(L), 0.5, kNZ - 0.5) / kNZ; }

// The world cell a level texel holds in the current window.
int2 CellOfTexel(uint L, int2 texel)
{
    int2 o = int2(levelB[L].xy);
    return o + ((texel - (o & (FOG_LEVEL_N - 1))) & (FOG_LEVEL_N - 1));
}

// --- primitives -------------------------------------------------------------------------------------------
// Five rows each (FOG_B_PRIMS):
//   0  a.xyz (centre, start, a point of a front), kind
//   1  b.xyz (end of a capsule, half extents of a box, direction of a front; height of a cylinder in z),
//      radius (sphere, capsule, cylinder, a ring's current radius, a front's half width, 0 = none)
//   2  fog rate (+ extinction per yd per s added, - share per s removed), smoke rate, hold (a density
//      the fog is kept at least at; for an add, its cap), soft edge (share of the size)
//   3  push xyz (yd/s), radial push (yd/s, + outwards)
//   4  yaw (box) or height over ground (front, ring), body depth (front, ring), swirl (yd/s), billow 0..1

struct Prim
{
    float4 r0, r1, r2, r3, r4;
};

[[vk::binding(FOG_B_PRIMS, 0)]] StructuredBuffer<float4> primRows;
[[vk::binding(FOG_B_BINS, 0)]]  StructuredBuffer<uint4>  primBins;

Prim LoadPrim(uint i)
{
    Prim p;
    p.r0 = primRows[i * 5u + 0u];
    p.r1 = primRows[i * 5u + 1u];
    p.r2 = primRows[i * 5u + 2u];
    p.r3 = primRows[i * 5u + 3u];
    p.r4 = primRows[i * 5u + 4u];
    return p;
}

// The bin mask of a level cell (x, y relative to the window origin, 0..N-1).
uint4 BinMask(uint L, int2 rel)
{
    int2 bin = clamp(rel / (FOG_LEVEL_N / FOG_BINS), 0, FOG_BINS - 1);
    return primBins[L * uint(FOG_BINS * FOG_BINS) + uint(bin.y * FOG_BINS + bin.x)];
}

// Signed distance to the primitive's shape (negative inside) and its size for the soft edge.
float PrimDistance(Prim q, float3 p, float h, out float size)
{
    uint kind = uint(q.r0.w + 0.5);
    float3 a = q.r0.xyz;
    size = max(q.r1.w, 0.1);
    if (kind == FOG_PRIM_SPHERE) return length(p - a) - q.r1.w;
    if (kind == FOG_PRIM_BOX)
    {
        float2 cs = float2(cos(q.r4.x), sin(q.r4.x));
        float3 l = p - a;
        l.xy = float2(l.x * cs.x + l.y * cs.y, -l.x * cs.y + l.y * cs.x);
        float3 e = abs(l) - q.r1.xyz;
        size = max(min(q.r1.x, min(q.r1.y, q.r1.z)), 0.1);
        return length(max(e, 0.0)) + min(max(e.x, max(e.y, e.z)), 0.0);
    }
    if (kind == FOG_PRIM_CAPSULE)
    {
        float3 ab = q.r1.xyz - a;
        float t = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-5));
        return length(p - (a + ab * t)) - q.r1.w;
    }
    if (kind == FOG_PRIM_CYLINDER)
    {
        float radial = length(p.xy - a.xy) - q.r1.w;
        float vertical = max(a.z - p.z, p.z - (a.z + q.r1.z));
        return max(radial, vertical);
    }
    // Fronts: the body lies behind the front (against its direction), body depth r4.y deep, r4.x high.
    float depth = max(q.r4.y, 0.5);
    float height = h - q.r4.x;
    size = depth;
    if (kind == FOG_PRIM_FRONT)
    {
        float3 n = q.r1.xyz;
        float s = dot(p.xy - a.xy, n.xy);                       // + ahead of the front
        float body = max(s, -depth - s);                        // inside when -depth <= s <= 0
        float lateral = q.r1.w > 0.0 ? abs(dot(p.xy - a.xy, float2(-n.y, n.x))) - q.r1.w : -1e6;
        return max(max(body, lateral), height);
    }
    // FOG_PRIM_RING: the fog stands outside the shrinking circle, depth thick.
    float r = length(p.xy - a.xy);
    float ring = max(q.r1.w - r, r - (q.r1.w + depth));
    return max(ring, height);
}

// The primitive's weight at a point: 1 inside, easing to 0 over its soft edge; billowing noise n
// (0..1) moves the edge by up to the billow share of the soft edge.
float PrimWeight(Prim q, float3 p, float h, float n)
{
    float size;
    float d = PrimDistance(q, p, h, size);
    float soft = max(q.r2.w * size, 0.05);
    d += (n - 0.5) * q.r4.w * soft * 2.0;
    return 1.0 - smoothstep(-soft, 0.0, d);
}

// The flow a primitive adds at a point.
float3 PrimVelocity(Prim q, float3 p, float w)
{
    if (w <= 0.0) return 0.0;
    float3 v = q.r3.xyz;
    float3 away = p - q.r0.xyz;
    uint kind = uint(q.r0.w + 0.5);
    if (kind == FOG_PRIM_CAPSULE)
    {
        float3 ab = q.r1.xyz - q.r0.xyz;
        float t = saturate(dot(away, ab) / max(dot(ab, ab), 1e-5));
        away = p - (q.r0.xyz + ab * t);
    }
    if (kind == FOG_PRIM_FRONT) away = float3(q.r1.xy, 0.0);
    float len = length(away);
    if (len > 1e-3) v += away / len * q.r3.w;
    // Swirl turns round the shape's axis: the vertical, or a capsule's own (a tunnel rolls in).
    float3 axis = float3(0.0, 0.0, 1.0);
    if (kind == FOG_PRIM_CAPSULE)
    {
        float3 ab = q.r1.xyz - q.r0.xyz;
        float abl = length(ab);
        if (abl > 1e-3) axis = ab / abl;
    }
    float3 sw = cross(axis, away);
    float swl = length(sw);
    if (swl > 1e-3) v += sw / swl * q.r4.z;
    return v * w;
}

#endif
