// wxl-graphics-fog: the ray march. pc.a.x selects the pass:
//   0  near: half resolution, from the camera to march.x yards or the scene, march.z steps spaced
//      quadratically (dense by the lens), with the erosion detail and the engine's cascades
//   1  far: quarter resolution, from march.x to the scene or march.y for the sky, march.w steps spaced
//      exponentially, and the haze and the atmosphere beyond it in closed form
// Step boundaries are fixed along the ray (a stable pattern); each pixel's sample within its step is
// jittered by the blue-noise tile, rotated every frame. A far step that meets the lying fog is split
// in up to three, so a long step across a thin layer seen from above does not turn to grain. Every
// medium is summed in the sample: one transmittance. A pixel whose footprint holds a silhouette
// carries two layers: the ray is marched to the farthest scene distance of its footprint, and its
// state is kept on the way where it passes the nearest (a branch against the sky gives the branch's
// fog and the sky's). Out: in-scattered light and transmittance of both layers, their representative
// distances (what the temporal pass reprojects by) and the two scene distances.
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

#include "medium.hlsli"

FOG_TEX(0) Texture2D<float>  depthTex;     // INTZ, full resolution
FOG_TEX(8) Texture2D<float4> blueNoise;    // 128^2, four channels
FOG_OUT(0) [[vk::image_format("rgba16f")]] RWTexture2D<float4> outFog;     // back layer: light rgb, transmittance
FOG_OUT(1) [[vk::image_format("rgba32f")]] RWTexture2D<float4> outAux;     // rep. distance back, front; scene distance near, far
FOG_OUT(2) [[vk::image_format("rgba16f")]] RWTexture2D<float4> outDebug;   // per-pixel debug views (near only)
FOG_OUT(3) [[vk::image_format("rgba16f")]] RWTexture2D<float4> outFront;   // front layer: light rgb, transmittance

// The nearest and the farthest scene distance over a pixel's footprint, every full-resolution texel
// of it. On a continuous surface they are one layer (their mean).
float2 FootprintRange(uint2 pix, uint scale, float far)
{
    float lo = 1e9, hi = 0.0;
    [loop] for (uint i = 0u; i < scale * scale; ++i)
    {
        uint2 full = min(pix * scale + uint2(i % scale, i / scale), uint2(screenFull.xy) - 1u);
        float d = depthTex.Load(int3(full, 0));
        float2 uv = (float2(full) + 0.5) * screenFull.zw;
        float dist = SceneDistance(uv, d, far);
        lo = min(lo, dist);
        hi = max(hi, dist);
    }
    if (hi - lo < 0.25 * lo + 1.0) return 0.5 * (lo + hi);
    return float2(lo, hi);
}

// What a ray has gathered so far.
struct Gathered
{
    float3 S;
    float  T;
    float  wSum, tSum, indoorSum, lampSum, sunSum;
};

// Adds one sample standing for a step of length dt.
void Accumulate(inout Gathered g, Sample s, float3 p, float3 view, float t, float dt, bool nearPass)
{
    float ext = Extinction(s);
    if (ext <= 1e-6) return;
    float3 airL;
    float3 Lin = InScatter(s, p, view, t, nearPass, airL);
    float stepT = exp(-ext * dt);
    float absorbed = g.T * (1.0 - stepT);
    g.S += Scattered(s, Lin, airL) * absorbed;
    g.wSum += absorbed;
    g.tSum += absorbed * t;
    g.indoorSum += absorbed * s.indoor;
    g.lampSum += absorbed * Luma(s.lamps);
    g.sunSum += absorbed * Luma(Lin - s.lamps);
    g.T *= stepT;
}

// Step boundary u in 0..1 -> distance.
float Boundary(bool nearPass, float u)
{
    if (nearPass) return march2.z + (march.x - march2.z) * u * u;
    return march.x * pow(max(march.y / march.x, 1.0), u);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    bool nearPass = pc.a.x == 0u;
    float4 res = nearPass ? screenHalf : screenQuarter;
    if (any(float2(id.xy) >= res.xy)) return;
    uint2 pix = id.xy;
    float2 uv = (float2(pix) + 0.5) * res.zw;
    uint scale = nearPass ? 2u : 4u;

    float far = march.y;
    float2 range = FootprintRange(pix, scale, far);
    float dist = range.y;
    float3 view = RayDir(uv);
    float tStart = nearPass ? march2.z : march.x;
    float tEnd = nearPass ? min(dist, march.x) : min(dist, far);
    float tFront = nearPass ? min(range.x, march.x) : min(range.x, far);
    int steps = int(nearPass ? march.z : march.w);

    float2 noisePix = float2(pix) + frame.zw;
    float jitter = blueNoise.Load(int3(int2(noisePix) & 127, 0))[nearPass ? 0 : 1];

    Gathered g;
    g.S = 0.0;
    g.T = 1.0;
    g.wSum = 0.0; g.tSum = 0.0; g.indoorSum = 0.0; g.lampSum = 0.0; g.sunSum = 0.0;
    float skipped = 0.0, taken = 0.0;
    // The front layer: the state where the ray passes the footprint's nearest scene distance.
    Gathered front = g;
    bool single = tFront >= tEnd - 1e-3;
    bool frontKept = single || tFront <= tStart;
    bool off = nearPass ? Isolated(FOG_ISO_NO_NEAR) : Isolated(FOG_ISO_NO_FAR);
    if (!off && tEnd > tStart)
    {
        [loop] for (int i = 0; i < steps; ++i)
        {
            float b0 = Boundary(nearPass, float(i) / float(steps));
            if (b0 >= tEnd) break;
            float b1 = min(Boundary(nearPass, float(i + 1) / float(steps)), tEnd);
            if (!frontKept && b1 >= tFront)
            {
                // The front layer ends inside this step: its own share of the step, then kept.
                front = g;
                float dtf = tFront - b0;
                if (dtf > 1e-3)
                {
                    float tf = b0 + dtf * jitter;
                    float3 pf = eye.xyz + view * tf;
                    Accumulate(front, SampleMedium(pf, tf, uv, nearPass), pf, view, tf, dtf, nearPass);
                }
                frontKept = true;
            }
            float dt = b1 - b0;
            float t = b0 + dt * jitter;
            float3 p = eye.xyz + view * t;
            Sample s = SampleMedium(p, t, uv, nearPass);
            taken += 1.0;
            if (s.empty) skipped += 1.0;

            // A far step through the lying fog: split so it is crossed a cell or two at a time, not in
            // one guess. The sample taken already is the split one's at the same jitter, so it is kept.
            int split = 1;
            if (!nearPass && !s.empty && s.fog + s.smoke > 1e-4 && s.level < 4u)
                split = clamp(int(ceil(dt * (abs(view.z) + 0.1) / (2.5 * CellZ(s.level)))), 1, 3);
            if (split == 1)
            {
                Accumulate(g, s, p, view, t, dt, nearPass);
            }
            else
            {
                float sdt = dt / float(split);
                float sub = frac(jitter * float(split));
                int kept = min(int(jitter * float(split)), split - 1);
                [loop] for (int k = 0; k < split; ++k)
                {
                    float tk = b0 + sdt * (float(k) + sub);
                    float3 pk = eye.xyz + view * tk;
                    if (k == kept) Accumulate(g, s, pk, view, tk, sdt, nearPass);
                    else Accumulate(g, SampleMedium(pk, tk, uv, nearPass), pk, view, tk, sdt, nearPass);
                    if (g.T < 0.01) break;
                }
            }
            if (g.T < 0.01) break;
        }
        if (!frontKept) front = g;

        // Past the far march the haze and the atmosphere go on to the horizon, in closed form.
        if (!nearPass && dist >= far && g.T > 0.01)
        {
            float3 pf = eye.xyz + view * far;
            float len = view.z > 0.0 ? 20000.0 : 6000.0;
            float tauHaze = 0.0, tauAir = 0.0;
            if (!Isolated(FOG_ISO_NO_HAZE) && outA.w > 0.0)
            {
                float k = outB.y;
                float dz = view.z * k;
                float base = outA.w * exp(-max(pf.z - outB.x, 0.0) * k);
                tauHaze = max(abs(dz) > 1e-5 ? base * (1.0 - exp(-dz * len)) / dz : base * len, 0.0);
            }
            float airHere = 0.0;
            if (airA.x > 0.0)
            {
                float k = airA.y;
                float dz = view.z * k;
                airHere = airA.x * exp(-max(pf.z - airA.z - airB.w, 0.0) * k);
                tauAir = max(abs(dz) > 1e-5 ? airHere * (1.0 - exp(-dz * len)) / dz : airHere * len, 0.0);
            }
            float tau = tauHaze + tauAir;
            if (tau > 1e-5)
            {
                Sample s;
                s.fog = 0.0; s.smoke = 0.0; s.dust = 0.0; s.haze = tauHaze; s.air = tauAir;
                s.indoor = 0.0; s.lamps = 0.0; s.lampsRaw = 0.0;
                s.level = 4u; s.h = 1000.0; s.empty = false;
                // The light at the far point, the atmosphere's slant set by its density there.
                Sample lit = s;
                lit.air = airHere;
                float3 airL;
                float3 Lin = InScatter(lit, pf, view, far, false, airL);
                float stepT = exp(-tau);
                float3 L = Scattered(s, Lin, airL);
                g.S += L * g.T * (1.0 - stepT);
                g.T *= stepT;
            }
        }
    }
    if (single) front = g;

    float3 S = g.S;
    float T = g.T;
    float wSum = g.wSum, tSum = g.tSum, indoorSum = g.indoorSum, lampSum = g.lampSum, sunSum = g.sunSum;
    float rep = wSum > 1e-4 ? tSum / wSum : tEnd;
    float repFront = front.wSum > 1e-4 ? front.tSum / front.wSum : tFront;
    outFog[pix] = float4(S * outE.z, T);
    outFront[pix] = float4(front.S * outE.z, front.T);
    outAux[pix] = float4(rep, repFront, range.x, range.y);
    if (nearPass && debug.x > 0.5)
    {
        uint mode = uint(debug.x + 0.5);
        float4 d = 0.0;
        float norm = max(wSum, 1e-4);
        if (mode == FOG_VIEW_INDOOR) d = float4(indoorSum / norm, 0.0, 1.0 - indoorSum / norm, wSum);
        else if (mode == FOG_VIEW_LAMPS) d = float4(lampSum / norm, lampSum / norm * 0.8, lampSum / norm * 0.5, 1.0);
        else if (mode == FOG_VIEW_SUN) d = float4(sunSum / norm, sunSum / norm, sunSum / norm, 1.0);
        else if (mode == FOG_VIEW_OCCUPANCY) d = float4(skipped / max(taken, 1.0), taken / max(march.z, 1.0), 0.0, 1.0);
        outDebug[pix] = d;
    }
}
