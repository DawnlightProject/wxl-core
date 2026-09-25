// wxl-forever fog, froxel pass: inject.
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

#include "fog/shaders/common.hlsli"
#include "fog/shaders/ground.hlsli"

sampler3D noiseTex : register(s3);

// A profile row for a cell: the outdoor row at sel 0, the indoor one at sel 1, blended between. The
// shape functions below read their rows here, at the use, so no row lives across the whole shader.
float4 PRow(int i, float sel) { return lerp(outP[i], inP[i], sel); }
float4 QRow(int i, float sel) { return lerp(outQ[i], inQ[i], sel); }

// A census variant (compiled with WXL_CENSUS set to a path number) keeps only the froxels that
// leave through that path, so an occlusion query counts them: 1 nothing possible, 2 between banks,
// 3 empty after the noise, 4 thin (no shadow steps), 5 full.
#ifdef WXL_CENSUS
#define CENSUS(path) { clip((WXL_CENSUS == (path)) ? 1.0 : -1.0); return 1.0; }
#else
#define CENSUS(path)
#endif
sampler2D visTex : register(s5);
sampler2D trailTiles : register(s2);
sampler2D trailTex : register(s6);

// Projectile trails (fog/Projectiles.cpp): each segment of this froxel's screen tile carves a hole
// that shrinks from its edge inwards as it refills, the edge perturbed by noise, and adds a shell of
// thicker fog at that edge whose noise rolls around the segment's axis and drifts, so the fog reads
// as smoke curling back in. Out: carve (a factor on the fog), rim (added share of the fog), and how
// much of the rim is a fire spell's darker, warmer smoke.
void Trails(float3 r, float2 uv01, float lod, inout float carve, inout float rim, inout float fire)
{
    float2 tile = min(floor(uv01 * trailC.zw), trailC.zw - 1.0);
    [loop] for (int i = 0; i < 8; ++i)
    {
        float4 q = tex2Dlod(trailTiles, float4((tile.x * 2.0 + floor(i * 0.25) + 0.5) * trailD.x, (tile.y + 0.5) * trailD.y, 0, 0));
        float m = fmod(i, 4.0);
        float code = m < 0.5 ? q.r : (m < 1.5 ? q.g : (m < 2.5 ? q.b : q.a));
        float index = floor(code * 65535.0 + 0.5);
        if (index > 65534.5) break;

        float u = (index + 0.5) * trailD.z;
        float4 a = tex2Dlod(trailTex, float4(u, 0.125, 0, 0));
        float4 b = tex2Dlod(trailTex, float4(u, 0.375, 0, 0));
        float3 axis = b.xyz - a.xyz;
        float along = saturate(dot(r - a.xyz, axis) / max(dot(axis, axis), 0.0001));
        float3 centre = a.xyz + axis * along;
        float3 off = r - centre;
        float dist = length(off);
        if (dist < a.w * 2.5 + 0.5)
        {
            float4 c = tex2Dlod(trailTex, float4(u, 0.625, 0, 0));
            float4 e = tex2Dlod(trailTex, float4(u, 0.875, 0, 0));
            // The billows roll: the noise is read at this point turned about the axis.
            float3 ax = dot(axis, axis) > 0.0001 ? normalize(axis) : float3(0.0, 0.0, 1.0);
            float s, co;
            sincos(c.y, s, co);
            float3 turned = off * co + cross(ax, off) * s + ax * dot(ax, off) * (1.0 - co);
            float4 n = tex3Dlod(noiseTex, float4((cam.xyz + centre + turned) * (c.w * 0.25) + e.xyz, lod));
            // The hole: open while fresh, shrinking from an irregular edge as it refills.
            float edge = a.w * (1.0 - b.w) * (0.7 + 0.6 * n.a);
            float inside = saturate((edge - dist) / max(0.3 * a.w, 0.1));
            carve *= 1.0 - e.w * inside;
            // The billows: a shell of thicker fog at the edge.
            float gap = dist - edge;
            float shell = exp(-gap * gap / max(0.25 * a.w * a.w + 0.15, 0.01));
            float amount = c.x * shell * saturate(n.b * 2.2 - 0.4);
            rim += amount;
            fire = max(fire, c.z * saturate(amount * 4.0));
        }
    }
}

// The lamps' light, from their own pass (lamps.ps.hlsl): their single scattering and its mean
// distance, then their multiple-scattering halo and the heat, without the extinction on the way.
sampler2D lampsTex : register(s1);
sampler2D lampsMultiTex : register(s8);

// Mip level that averages the noise over a footprint of so many yards: 64 texels per tile.
float NoiseLod(float footprint, float tilesPerYard)
{
    return log2(max(footprint * tilesPerYard * 64.0, 1.0));
}

// Height above a blend of the absolute base and the ground, as a density factor, with the extra fog
// valleys hold. Cheap: the early-out tests it before any noise is fetched.
float HeightTerm(float3 p, float3 ground, float sel)
{
    float4 P0 = PRow(0, sel), P7 = PRow(7, sel);
    float follow = P7.z * ground.z;
    float h = lerp(p.z - P0.z, p.z - ground.x, follow);
    float valley = 1.0 + P7.w * follow * saturate((ground.y - ground.x) / 6.0);
    return min(exp(-P0.y * h), 3.0) * valley;
}

// The warp field's value near p; it varies slowly, so the shadow steps reuse the froxel's own.
float WarpAt(float3 p, float lod, float sel)
{
    float4 P0 = PRow(0, sel), P8 = PRow(8, sel);
    return tex3Dlod(noiseTex, float4(p * P0.w * 0.43 + P8.xyz * 0.6 + 0.17, lod)).a - 0.5;
}

// The weather map: a 2D field of very low frequency drifting with the wind, 1 inside a fog bank and
// 0 in the clear gaps between banks. Coverage sets how much land the banks hold, contrast how sharp
// their boundary is.
float Bank(float3 p, float footprint, float sel)
{
    float4 Q1 = QRow(1, sel), Q2 = QRow(2, sel);
    if (Q1.w <= 0.0) return 1.0;
    float4 n = tex3Dlod(noiseTex, float4(p.xy * Q1.x + Q2.xy, 0.37 + screen.z * 0.0005, NoiseLod(footprint, Q1.x)));
    float u = saturate((lerp(n.r, n.a, 0.5) - 0.5) * 3.5 + 0.5);
    return lerp(1.0, saturate((u - Q1.z) * Q1.y + 0.5), Q1.w);
}

// The high haze: a thin band at a height above the ground, summed with the ground mist.
float HazeTerm(float3 p, float3 ground, float sel)
{
    float4 P0 = PRow(0, sel), Q3 = QRow(3, sel);
    if (Q3.x <= 0.0) return 0.0;
    float h = p.z - lerp(P0.z, ground.y, ground.z) - Q3.z;
    return Q3.x * exp(-Q3.y * abs(h));
}

// Density at world point p, the noise averaged over footprint yards. Two noise layers drift at
// different speeds and the warp field drifts on its own, so the fog rolls rather than slides; swirl
// (a wake) churns it locally. Inside a bank (cover 1) the coverage cut is the profile's own; towards
// the bank's boundary the cut rises and erosion strengthens, so banks end in torn wisps instead of
// fading evenly.
float Density(float3 p, float heightTerm, float warp, float footprint, float swirl, float cover,
              float sel, bool detail, bool flow)
{
    float4 P0 = PRow(0, sel), P1 = PRow(1, sel), P3 = PRow(3, sel), P5 = PRow(5, sel), P8 = PRow(8, sel), Q2 = QRow(2, sel);
    if (!flow) P8.w = 0.0;   // the shadow steps read the first layer only
    float edge = 1.0 - abs(cover * 2.0 - 1.0);
    float lod = NoiseLod(footprint, P0.w);
    float3 q = p * P0.w;
    float3 churn = float3(sin(screen.z * 3.1 + p.y * 0.7), cos(screen.z * 2.7 + p.x * 0.7), 0.0);
    q += warp * P3.w * (1.0 + swirl * 3.0) + churn * (swirl * 0.15);

    float4 n = tex3Dlod(noiseTex, float4(q + P1.xyz, lod));
    float shape = lerp(n.r, n.b, P3.z);
    if (P8.w > 0.001)
    {
        float4 m = tex3Dlod(noiseTex, float4(q * 1.37 + P8.xyz, lod));
        shape = lerp(shape, lerp(m.r, m.b, P3.z), P8.w);
    }
    if (detail)
    {
        float d = tex3Dlod(noiseTex, float4(p * P5.w + P1.xyz * 1.9, NoiseLod(footprint, P5.w))).g;
        shape -= (1.0 - d) * saturate(P3.y + edge * Q2.z) * (1.0 - shape);
    }
    float cut = saturate(lerp(1.0, P3.x, cover) + edge * Q2.z * 0.15);
    float shaped = saturate((shape - cut) / max(1.0 - cut, 0.05));
    float density = P0.x * heightTerm * lerp(cover, shaped * 2.0, P1.w);

    // Near wisps: within nearC.x yards of the camera the fog is redistributed by a finer noise read
    // in the same warped, wind-driven frame (so it rides the billows) with its own churn (nearD), at
    // the froxel's footprint, so it is part of this density and fades out by itself where a froxel
    // outgrows it. The modulation keeps the mean: fog moves into curls, none is added or lost, and
    // it never empties (a lamp's halo in a gap stays).
    if (detail && nearC.z > 0.5)
    {
        float fade = nearC.y * (1.0 - smoothstep(nearC.x * 0.6, nearC.x, length(p - cam.xyz)));
        [branch] if (fade > 0.001)
        {
            float ratio = nearD.w / P0.w;
            float4 w = tex3Dlod(noiseTex, float4((q + P1.xyz) * ratio + nearD.xyz, NoiseLod(footprint, nearD.w)));
            float wisp = saturate((lerp(w.r, w.b, 0.6) - 0.3) * 1.8) * (0.6 + 0.4 * w.g);
            density *= max(1.0 + (wisp * 3.4 - 1.0) * 0.8 * fade, 0.35);
        }
    }
    return density;
}

// The haze's own density: slower, larger noise of its own.
float HazeDensity(float3 p, float hazeTerm, float footprint, float sel)
{
    float4 Q3 = QRow(3, sel), Q4 = QRow(4, sel);
    if (hazeTerm <= Q3.x * 0.05) return hazeTerm;
    float n = tex3Dlod(noiseTex, float4(p * Q3.w + Q4.xyz, NoiseLod(footprint, Q3.w))).r;
    return hazeTerm * lerp(1.0, saturate(n * 2.0 - 0.3), Q4.w);
}

// One profile's fog at this froxel, its rows chosen at compile time (sel 0 the outdoor rows, 1 the
// indoor ones, so no blended row stays live across the shader): the extinction and the light it
// scatters, before the lamps. Out: rgb the light, a the extinction; path names the injection path
// taken for the census (1 nothing possible, 2 between banks, 3 empty after the noise, 4 thin, 5 full).
float4 Evaluate(float sel, float3 p, float3 dir, float3 ground, float footprint, float swirl, float keep, float added,
                float smoke, float plume, float trailScale, float indoor, float2 seen, out float path)
{
    float4 P0 = PRow(0, sel), P2 = PRow(2, sel), P4 = PRow(4, sel), P5 = PRow(5, sel), P6 = PRow(6, sel);
    float4 P7 = PRow(7, sel), P10 = PRow(10, sel), P11 = PRow(11, sel), Q0 = QRow(0, sel);
    float heightTerm = HeightTerm(p, ground, sel);
    float hazeTerm = HazeTerm(p, ground, sel);

    // Nothing to do where even the densest noise could not make fog (above the layers, carved away,
    // between banks): no noise is fetched and no light is gathered.
    float layerMax = P0.x * heightTerm * (1.0 + PRow(1, sel).w);
    if ((layerMax + hazeTerm + added + plume) * keep < 0.0000001) { path = 1.0; return 0.0; }
    float cover = Bank(p, footprint, sel);
    if ((layerMax * step(0.001, cover) + hazeTerm + added + plume) * keep < 0.0000001) { path = 2.0; return 0.0; }

    float warp = WarpAt(p, NoiseLod(footprint, P0.w), sel);
    float sigma = Density(p, heightTerm, warp, footprint, swirl, cover, sel, true, true) + HazeDensity(p, hazeTerm, footprint, sel);
    sigma = max(sigma + added + smoke, 0.0) * keep * trailScale;
    if (sigma < 0.0000001) { path = 3.0; return 0.0; }

    // Thin fog (the high haze, a bank's fringe) shades itself too little to see: it skips the
    // shadow and bank steps, the most costly part of injection.
    bool thin = sigma < max(0.05 * P0.x, 0.0005);
    path = thin ? 4.0 : 5.0;

    // Self-shadow: three growing steps towards the light, Beer's law, optional powder darkening.
    // Each step reads the noise once, coarser, without the flow layer or the detail. A froxel wider
    // than half the reach holds the whole march inside its own footprint, so one step at the middle
    // reads the same value the three would.
    float tau = 0.0;
    if (P6.x > 0.0 && !thin)
    {
        float reach = P6.y;
        float fp = max(footprint, reach * 0.3) * 2.0;
        [branch] if (footprint > reach * 0.5)
        {
            float3 p2 = p + shadowDir.xyz * (reach * 0.5);
            tau += (Density(p2, HeightTerm(p2, ground, sel), warp, fp, 0.0, cover, sel, false, false)
                    + HazeTerm(p2, ground, sel)) * reach;
        }
        else
        {
            float3 p1 = p + shadowDir.xyz * (reach * 0.08);
            float3 p2 = p + shadowDir.xyz * (reach * 0.3);
            float3 p3 = p + shadowDir.xyz * (reach * 0.75);
            tau += (Density(p1, HeightTerm(p1, ground, sel), warp, fp, 0.0, cover, sel, false, false)
                    + HazeTerm(p1, ground, sel)) * (reach * 0.16);
            tau += (Density(p2, HeightTerm(p2, ground, sel), warp, fp, 0.0, cover, sel, false, false)
                    + HazeTerm(p2, ground, sel)) * (reach * 0.3);
            tau += (Density(p3, HeightTerm(p3, ground, sel), warp, fp, 0.0, cover, sel, false, false)
                    + HazeTerm(p3, ground, sel)) * (reach * 0.54);
        }
    }
    // Whole banks shade themselves: one long, coarse step towards the light adds the bank beyond the
    // short steps, and one straight up darkens the ambient under fog piled above, so the tops of
    // banks stay bright and their undersides go dark. At this scale the banks and the height layers
    // are the shape, so these steps read the macro map only, no noise.
    float bankAmbient = 1.0;
    float bankReach = QRow(2, sel).w;
    if (bankReach > 0.0 && !thin)
    {
        float far = bankReach;
        float fpFar = max(footprint, far * 0.4);
        if (P6.x > 0.0 && far > P6.y)
        {
            float3 q = p + shadowDir.xyz * far;
            float3 gq = Ground(q - cam.xyz);
            float dq = P0.x * HeightTerm(q, gq, sel) * Bank(q, fpFar, sel) + HazeTerm(q, gq, sel);
            tau += dq * (far - P6.y) * 0.7;
        }
        if (Q0.w > 0.0)
        {
            float3 q = p + float3(0.0, 0.0, far * 0.5);
            float du = P0.x * HeightTerm(q, ground, sel) * cover + HazeTerm(q, ground, sel);
            bankAmbient = lerp(1.0, exp(-du * far * 0.5), Q0.w);
        }
    }

    // Multiple scattering, octave method (Wrenninge): each octave carries less energy, sees less
    // extinction and a flatter phase, which is what makes dense fog soft and luminous. Octave 0 is
    // the single scattering, with its powder darkening.
    float cosSun = dot(dir, toSun.xyz), cosMoon = dot(dir, toMoon.xyz);
    float energy = 1.0, reach = 1.0, contrast = 1.0, celestialSum = 0.0;
    for (int o = 0; o < 4; ++o)
    {
        if ((float)o > P10.w) break;
        float trans = exp(-tau * P6.x * reach);
        float lit = o == 0 ? lerp(trans, trans * (1.0 - exp(-2.0 * tau * P6.x - 0.05)), P6.z) : trans;
        float4 lobes = float4(P5.xy * contrast, P5.zw);
        celestialSum += energy * lit * (P4.x * Phase(cosSun, lobes) + P4.y * Phase(cosMoon, lobes));
        energy *= P10.x;
        reach *= P10.y;
        contrast *= P10.z;
    }
    float3 direct = celestial.rgb * celestialSum * (1.0 - indoor) * lerp(1.0, seen.x, Q0.x);
    // Clouds between the fog and the sun or moon (P11.z): the noise, coarse, where the ray towards the
    // light crosses a layer 120 yards above the camera, drifting two and a half times as fast as the fog.
    if (P11.z > 0.0)
    {
        float3 L = shadowDir.xyz;
        float up = max(cam.z + 120.0 - p.z, 0.0) / max(L.z, 0.2);
        float4 P1 = PRow(1, sel);
        float2 drift = P0.w > 0.0 ? P1.xy / P0.w : 0.0;
        float2 c = (p.xy + L.xy * up + 2.5 * drift) * P11.w;
        float n = tex3Dlod(noiseTex, float4(c, 0.61 + P1.z * 0.5, NoiseLod(footprint, P11.w) + 1.0)).r;
        direct *= lerp(1.0, smoothstep(0.3, 0.7, n), P11.z);
    }

    // Ambient from the sky: the gradient seen along the view (top above, horizon at the side, ground
    // bounce below), focused by the forward lobe, blended with the profile colour. Brighter towards
    // the top of the layer, darker near its floor.
    float3 gradient = dir.z >= 0.0 ? lerp(skyHorizon.rgb, skyTop.rgb, dir.z) : lerp(skyHorizon.rgb, skyGround.rgb, -dir.z);
    float3 dome = (skyTop.rgb + 2.0 * skyHorizon.rgb + skyGround.rgb) * 0.25;
    float3 sky = lerp(dome, gradient, saturate(abs(P5.x)));
    float layer = P0.y > 0.001 ? clamp(1.0 / P0.y, 5.0, 60.0) : 20.0;
    float floorZ = lerp(P0.z, ground.x, P7.z * ground.z);
    float ambient = P2.w * lerp(P7.x, P7.y, saturate((p.z - floorZ) / layer)) * bankAmbient * lerp(1.0, seen.y, Q0.z);

    float3 light = ambient * lerp(P2.rgb, sky, P11.x) + P2.rgb * direct;
    return float4(min(light, P4.w), sigma);
}

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 tile = floor(vpos / grid.xy);
    float k = tile.y * grid.w + tile.x;
    if (k >= grid.z) return 0.0;
    float2 cell = vpos - tile * grid.xy;

    float3 r = FroxelPoint(cell, k);
    float dist = length(r);
    float3 dir = r / max(dist, 0.0001);
    float3 p = cam.xyz + r;

    // What one froxel covers: its slice thickness or its width, whichever is larger.
    float footprint = dist * max(atlas.w / grid.z, counts.y);

    // Indoor and world shadowing come from the visibility pass, at this same point.
    float4 seen = tex2Dlod(visTex, float4((vpos + 0.5) * atlas.xy, 0, 0));
    // b: indoor share, + 2 when unseen, + 4 * (the froxel's room + 1).
    float roomCode = floor(seen.b * 0.25);
    float state = seen.b - roomCode * 4.0;
    bool visible = state < 1.5;
    float indoor = visible ? state : state - 2.0;
    // Only two profile values are needed by the volumes; the rows follow them, which keeps the
    // loop's registers free.
    float wakeStrength = lerp(outP[6].w, inP[6].w, indoor);
    float wakeSwirl = lerp(outP[9].z, inP[9].z, indoor);

    // Density volumes add fog; carving ones (bodies and their wakes) remove a share of it and churn
    // what is left.
    float keep = 1.0, added = 0.0, swirl = 0.0, plume = 0.0, plumeTint = 0.0;
    // No volume reaches past smokeC.w yards; the far froxels skip the loop.
    float volumeCount = length(r) < smokeC.w ? counts.x : 0.0;
    [loop] for (int v = 0; v < 20; ++v)
    {
        if ((float)v >= volumeCount) break;
        float4 a = vols[v * 3];
        float4 b = vols[v * 3 + 1];
        float4 c = vols[v * 3 + 2];
        float3 d = r - a.xyz;
        float t = 1000.0;
        if (a.w < 0.5)      t = length(d) / b.x;
        else if (a.w < 1.5) t = max(abs(d.x) / b.x, max(abs(d.y) / b.y, abs(d.z) / b.z));
        else if (a.w < 2.5) t = length(float3(d.xy, d.z - clamp(d.z, 0.0, b.z))) / b.x;
        // A segment (a projectile's tunnel): from its head at a.xyz along b.xyz, c.w thick.
        // A plume: a cone from its base at a.xyz along b.xyz, widening from c.w to c.x.
        float along = 0.0;
        if (a.w > 2.5)
        {
            along = saturate(dot(d, b.xyz) / max(dot(b.xyz, b.xyz), 0.0001));
            float thickness = a.w > 3.5 ? lerp(c.w, c.x, along) : c.w;
            t = length(d - b.xyz * along) / max(thickness, 0.05);
        }
        float w = c.x > 0.001 ? saturate((1.0 - t) / c.x) : (t < 1.0 ? 1.0 : 0.0);
        if (a.w > 3.5)
        {
            // Soft all round, thinning as it rises, and growing in over its first yards.
            float amount = b.w * saturate(1.0 - t) * (1.0 - along) * saturate(along * 6.0 + 0.2);
            plume += amount;
            plumeTint += amount * c.y;
            swirl = max(swirl, saturate(c.z * saturate(1.5 - t)));
            continue;
        }
        // A tunnel carves fully at its head and closes towards its tail, churning as it does.
        float carved = a.w > 2.5 ? b.w * (1.0 - 0.8 * along) : b.w * wakeStrength;
        if (c.y > 0.5) keep *= 1.0 - saturate(carved) * w;
        else           added += b.w * w;
        float churn = a.w > 2.5 ? c.z * (0.3 + 0.7 * along) : c.z * wakeSwirl;
        swirl = max(swirl, saturate(churn * saturate(1.5 - t)));
    }

    float3 ground = Ground(r);
    bool showBanks = screen.w > 18.5 && screen.w < 19.5;
    if (showBanks) return float4(0.004 * (indoor > 0.5 ? Bank(p, footprint, 1.0) : Bank(p, footprint, 0.0)).xxx, 0.004);

    // Smoke has its own noise, rising and churning faster than the fog around it.
    float smoke = 0.0;
    if (plume > 0.0)
    {
        float n = tex3Dlod(noiseTex, float4(p * 0.2 + float3(0.0, 0.0, -screen.z * 0.3), NoiseLod(footprint, 0.2))).r;
        smoke = plume * saturate(n * 2.2 - 0.3) * 1.6;
    }
    // Projectile trails carve the fog and thicken it at their edges.
    float trailCarve = 1.0, trailRim = 0.0, trailFire = 0.0;
    if (trailC.x > 0.5 && dist < trailC.y)
        Trails(r, (cell + 0.5) / grid.xy, NoiseLod(footprint, 0.2), trailCarve, trailRim, trailFire);
    if (screen.w > 20.5) return float4(0.004 * float3(1.0 - trailCarve, saturate(trailRim), trailFire), 0.004);

    // The cell's kind, eased across a doorway (indoor 0..1), cross-fades the two profiles' fog: each
    // is evaluated with its own rows and the results blend (contactC.w), so the pattern never
    // re-shapes on screen: the outdoor fog fades out where the indoor fog fades in. Without the
    // cross-fade the cell takes the nearer profile whole.
    bool crossfade = contactC.w > 0.5;
    float wIn = crossfade ? saturate(indoor) : step(0.5, indoor);
    float wOut = 1.0 - wIn;
    float4 fogO = 0.0, fogI = 0.0;
    float pathO = 0.0, pathI = 0.0;
    [branch] if (wOut > 0.0)
        fogO = Evaluate(0.0, p, dir, ground, footprint, swirl, keep, added, smoke, plume, trailCarve + trailRim, indoor, seen.rg, pathO);
    [branch] if (wIn > 0.0)
        fogI = Evaluate(1.0, p, dir, ground, footprint, swirl, keep, added, smoke, plume, trailCarve + trailRim, indoor, seen.rg, pathI);
    float sigma = wOut * fogO.a + wIn * fogI.a;
    float path = wOut >= wIn ? pathO : pathI;
    if (sigma < 0.0000001) { CENSUS(max(path, 1.0)) return 0.0; }
    float3 light = (wOut * fogO.a * fogO.rgb + wIn * fogI.a * fogI.rgb) / sigma;

    // A trail's billows are darker than the fog, and a fire spell's warmer too, by their share.
    if (trailRim > 0.0)
    {
        float3 tint = lerp(smokeC.rgb, smokeC.rgb * float3(1.2, 0.9, 0.65), saturate(trailFire));
        light *= lerp(1.0, tint, saturate(trailRim / (trailCarve + trailRim)) * saturate(0.4 + trailFire));
    }
    // Smoke is darker than the fog it mixes with, by its share of the froxel's density.
    if (smoke > 0.0)
        light *= lerp(1.0, smokeC.rgb, saturate(smoke * keep * (plumeTint / plume) / max(sigma, 0.000001)));

    // The visibility views show the sun and sky terms as the light; the lamps-only view drops the rest.
    if (screen.w > 16.5 && screen.w < 18.5) return float4(sigma * (screen.w < 17.5 ? seen.r : seen.g).xxx, sigma);
    if (screen.w > 10.5 && screen.w < 11.5) light = 0.0;

    // The lamps (their own pass), dimmed by this froxel's extinction over their mean distance (the
    // halo by the multiple-scattering share of it), then saturating softly, their luma levelling off
    // at twice the max scatter instead of clipping into a flat disk; the two profiles' rows blended
    // by the cell's indoor share.
    float4 lampA = tex2Dlod(lampsTex, float4((vpos + 0.5) * atlas.xy, 0, 0));
    float4 lampB = tex2Dlod(lampsMultiTex, float4((vpos + 0.5) * atlas.xy, 0, 0));
    float3 lampLight = (lampA.rgb * exp(-sigma * lampA.a) + lampB.rgb * exp(-sigma * lampA.a * PRow(10, indoor).y))
                     * PRow(4, indoor).z;
    lampLight /= 1.0 + dot(lampLight, float3(0.299, 0.587, 0.114)) / (2.0 * PRow(4, indoor).w);
    float heat = lampB.a;
    // Heat lifts the fog around flames: less of it, so less veil right at the fire, never all of it
    // (the halo stays).
    sigma *= 1.0 - 0.6 * saturate(heat);
    light += lampLight;

    CENSUS(path)
    return float4(sigma * light, sigma);
}
