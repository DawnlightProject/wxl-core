// wxl-graphics-fog: the medium at a point, as the march and the camera probe see it -- the finest
// clipmap level holding it (blended with the next near its edges), the erosion that frays its edges,
// the atmosphere, the indoor air, the analytic haze -- and the light it scatters: sun or moon through
// the light volume (self-shadow, terrain shadow, the engine's cascades), dual-lobe phase, powder and
// multiple-scattering octaves, the sky light darkened in hollows and under deep fog, and the lamps.
// The atmosphere has its own phase, colour and strengths but shares the sample, so one transmittance
// covers every medium.
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

#ifndef FOG_MEDIUM_HLSLI
#define FOG_MEDIUM_HLSLI

#include "clip.hlsli"
#include "terrain.hlsli"
#include "indoor.hlsli"
#include "wake.hlsli"

FOG_TEX(1)  Texture3D<float2> stateTex;
FOG_TEX(2)  Texture3D<float>  groundTex;
FOG_TEX(3)  Texture3D<float4> lightVol;
FOG_TEX(4)  Texture3D<float>  occTex;
FOG_TEX(5)  Texture3D<float4> detailNoise;
FOG_TEX(6)  Texture3D<float4> curlNoise;
FOG_TEX(7)  Texture3D<float4> lampGridTex;
FOG_TEX(9)  Texture2D<float>  skyTex;
FOG_TEX(10) Texture2D<float>  cascade0;
FOG_TEX(11) Texture2D<float>  cascade1;
FOG_TEX(12) Texture2D<float>  cascade2;
FOG_TEX(13) Texture3D<float4> shapeNoise;
FOG_TEX(14) Texture3D<float4> wakeTex;

struct Sample
{
    float  fog;        // outdoor fog, extinction per yard
    float  smoke;
    float  dust;       // indoor air
    float  haze;
    float  air;        // the atmosphere
    float  indoor;     // 0..1
    float3 lamps;      // lamp light per unit of scattering, at the fog's (or the indoor air's) strength
    float3 lampsRaw;   // the same before any medium's strength
    uint   level;      // the finest level holding the point (4: none)
    float  h;          // height over that level's ground
    bool   empty;      // skipped by the occupancy
};

// The fog and smoke of the clipmaps at a world point, blended across level edges.
float2 ClipFog(float3 p, out uint used, out float usedH, out bool empty)
{
    float2 acc = 0.0;
    float remaining = 1.0;
    used = 4u;
    usedH = 0.0;
    empty = true;
    [loop] for (uint L = 0u; L < uint(FOG_LEVELS); ++L)
    {
        if (LevelInside(L, p.xy) < 2.0) continue;
        float h = p.z - LevelGround(groundTex, L, p.xy);
        float w = LevelWeight(L, p.xy, h, 6.0);
        if (w <= 0.0) continue;
        if (used == 4u)
        {
            used = L;
            usedH = h;
        }
        float occ = Isolated(FOG_ISO_NO_SKIP) ? 1.0
                  : occTex.SampleLevel(sPointWrap, float3(LevelUv(L, p.xy),
                        (float(L) * FOG_OCC_NZ + clamp((h - HMin(L)) / (8.0 * CellZ(L)), 0.0, FOG_OCC_NZ - 0.01)) / (kLevels * FOG_OCC_NZ)), 0);
        if (occ > march2.y)
        {
            acc += SampleLevel(stateTex, L, p.xy, h) * (w * remaining);
            empty = false;
        }
        remaining *= 1.0 - w;
        if (remaining < 0.01) break;
    }
    return acc;
}

// Fraying: Worley noise, its coordinates bent by curl noise, eats the fog's thin edges and leaves its
// full cores. Two octaves: the base, and a fine one (detailA.x of a period) near the camera, bent by
// the same curl magnified. The fine fades out by half the detail distance, the base by the detail
// distance; each also fades once a pixel would cover a sixth of it. Near march only: the far march's
// long steps would turn fraying into grain.
float Erode(float fog, float3 p, float t, bool nearPass)
{
    if (!nearPass || fog <= 0.0 || outI.z <= 0.0 || Isolated(FOG_ISO_NO_DETAIL)) return fog;
    float ref = max(max(outA.x, outA.y), 1e-5);
    float c = fog / ref;
    if (c >= 1.0) return fog;
    float D = max(march2.x, 1.0);
    float P = max(outJ.y, 0.05) * 4.0;
    float Pf = P * detailA.x;
    float footprint = max(t * detailA.y, 1e-4);
    float wb = (1.0 - smoothstep(0.6 * D, D, t)) * saturate(P / (6.0 * footprint) - 1.0);
    float wf = 0.7 * (1.0 - smoothstep(0.25 * D, 0.5 * D, t)) * saturate(Pf / (6.0 * footprint) - 1.0);
    float norm = wb + wf;
    if (norm <= 1e-3) return fog;

    float3 drift3 = float3(drift.zw, drift2.x);
    float3 bend = (curlNoise.SampleLevel(sLinearWrap, p / (P * 2.0) + drift2.yzw, 0).xyz * 2.0 - 1.0) * outI.w;
    float hf = 0.0;
    if (wb > 0.0)
    {
        float4 n = detailNoise.SampleLevel(sLinearWrap, p / P + drift3 + bend, 0);
        hf += wb * (n.r * 0.5 + n.g * 0.3 + n.a * 0.2);
    }
    if (wf > 0.0)
    {
        // The curl magnified for the fine octave: its wisps twist around the base's.
        float4 n = detailNoise.SampleLevel(sLinearWrap, p / Pf + (drift3 + bend * 1.6) * (P / Pf), 0);
        hf += wf * (n.r * 0.4 + n.g * 0.4 + n.b * 0.2);
    }
    // A blend of independent octaves spreads less than one: stretched back to one's spread.
    float spread = sqrt(wb * wb + wf * wf) / norm;
    hf = saturate((hf / norm - 0.5) / max(spread, 0.5) + 0.5);
    float e = outI.z * saturate(norm / max(wb, 0.7)) * hf;
    return ref * max(c - e, 0.0) / max(1.0 - e, 0.05);
}

float HazeAt(float z)
{
    if (Isolated(FOG_ISO_NO_HAZE)) return 0.0;
    return outA.w * exp(-max(z - outB.x, 0.0) * outB.y);
}

// The atmosphere: full up to its base over the ground, thinning above with its scale height, varying
// in large slow patches flattened in height (the small detail tile's Perlin channel, cheap to sample
// along long far rays). Its ground blends the smoothed ground under the camera and the terrain under
// the point (the ground the clipmap sample already read; none above every level) by the follow share.
float AirAt(float3 p, float terrain, bool haveTerrain)
{
    if (airA.x <= 0.0) return 0.0;
    // Above every level's layers, the coarsest level's ground (the terrain over 32 yards) stands in:
    // the reference is always the terrain under the point, never the camera's height.
    if (!haveTerrain && LevelInside(3u, p.xy) >= 1.0)
    {
        terrain = LevelGround(groundTex, 3u, p.xy);
        haveTerrain = true;
    }
    float ground = haveTerrain ? lerp(airA.z, terrain, airA.w) : airA.z;
    float d = airA.x * exp(-max(p.z - ground - airB.w, 0.0) * airA.y);
    if (airB.x > 0.0)
    {
        float n = detailNoise.SampleLevel(sLinearWrap, float3(p.xy, p.z * 3.0) * (airB.y * 0.25) + airF.xyz, 0).b;
        d *= max(1.0 + airB.x * (n - 0.5) * 2.0, 0.0);
    }
    return d;
}

// The indoor air: dust drifting slowly, thicker in a haze near the floor.
float DustAt(float3 p, float hGround)
{
    float n = shapeNoise.SampleLevel(sLinearWrap, p / max(inC.z, 0.5) + boil[2].xyz, 0).g;
    float floorHaze = 1.0 + inA.y * exp(-max(hGround, 0.0) / max(inA.z, 0.1));
    return inA.x * max(1.0 + inA.w * (n - 0.5) * 2.0, 0.0) * floorHaze;
}

Sample SampleMedium(float3 p, float t, float2 rayUv, bool nearPass)
{
    Sample s;
    uint used;
    float h;
    bool empty;
    float2 clip = ClipFog(p, used, h, empty);
    s.level = used;
    s.h = h;
    s.empty = empty;
    s.fog = Erode(clip.x, p, t, nearPass);
    s.smoke = clip.y;
    // The wake fluid near the ground: cleared where bodies passed, piled by blasts, thickened by frost.
    // A torch's pocket keeps only a thin mist on the ground, and thins the atmosphere a little too.
    float airThin = 1.0;
    if (used < 4u && h < 30.0 && s.fog + s.smoke > 0.0)
    {
        float4 wk = WakeAt(wakeTex, p.xy);
        if (wk.z != 0.0)
        {
            float reach = WakeReach(h, WakeTop(wk));
            float m = max(1.0 + wk.z * reach, 0.0);
            float before = s.fog;
            s.fog *= m;
            s.smoke *= m;
            if (WakeIsTorch(wk))
            {
                s.fog = max(s.fog, min(before, torchA.x * exp(-max(h, 0.0) / 0.6)));
                airThin = max(1.0 + wk.z * reach * torchA.y, 0.0);
            }
        }
    }
    if (Isolated(FOG_ISO_ONLY_AIR))
    {
        s.fog = 0.0;
        s.smoke = 0.0;
    }
    s.haze = HazeAt(p.z);
    s.air = AirAt(p, p.z - h, used < 4u);
    s.lampsRaw = 0.0;
    s.indoor = 0.0;
    if (lights.w > 0.5 && t < lampGrid2.z)
    {
        float4 g = lampGridTex.SampleLevel(sLinearClamp, float3(rayUv, GridW(t)), 0);
        s.lampsRaw = g.rgb;
        s.indoor = Isolated(FOG_ISO_NO_INDOOR) ? 0.0 : g.a;
    }
    // Around the camera the medium follows the camera's eased indoor state while it lags behind.
    float lag = indoorState.w * (1.0 - smoothstep(0.5 * indoorState.z, indoorState.z, t));
    s.indoor = lerp(s.indoor, indoorState.x, lag);
    s.lamps = s.lampsRaw * lerp(outD.w, inC.y, s.indoor);
    s.dust = s.indoor > 0.001 ? DustAt(p, h) : 0.0;
    float I = march2.w;
    s.fog *= I * (1.0 - s.indoor);
    s.smoke *= I * (1.0 - s.indoor);
    s.haze *= 1.0 - s.indoor;
    s.air *= I * (1.0 - s.indoor) * airThin;
    s.dust *= s.indoor;
    return s;
}

float Extinction(Sample s) { return s.fog + s.smoke * outH.w + s.dust + s.haze + s.air; }

// --- lighting --------------------------------------------------------------------------------------------

float4 LightVolumeAt(uint L, float3 p, float h)
{
    float k = clamp((h - HMin(L)) / (2.0 * CellZ(L)), 0.5, FOG_VEL_NZ - 0.5);
    return lightVol.SampleLevel(sLinearWrap, float3(LevelUv(L, p.xy), (float(L) * FOG_VEL_NZ + k) / (kLevels * FOG_VEL_NZ)), 0);
}

float CascadeTap(uint band, float2 uv)
{
    return band == 0u ? cascade0.SampleLevel(sPointClamp, uv, 0)
         : band == 1u ? cascade1.SampleLevel(sPointClamp, uv, 0)
                      : cascade2.SampleLevel(sPointClamp, uv, 0);
}

// The engine's sun cascades at a camera-relative point: 0 shadowed, 1 lit; 1 outside every band.
float WorldShadow(float3 r)
{
    uint bands = uint(cascadeInfo.x + 0.5);
    float4 h = float4(r, 1.0);
    [loop] for (uint b = 0u; b < bands && b < 3u; ++b)
    {
        float3 s = float3(dot(h, cascade[b * 3u + 0u]), dot(h, cascade[b * 3u + 1u]), dot(h, cascade[b * 3u + 2u]));
        float2 uv = s.xy * 0.5 + 0.5;
        if (any(uv <= 0.002) || any(uv >= 0.998)) continue;
        return CascadeTap(b, uv) < s.z ? 0.0 : 1.0;
    }
    return 1.0;
}

// Light scattered towards the camera per unit of scattering at the sample (albedo applied by the caller):
// the fog's (returned; the haze, smoke and indoor air share it) and the atmosphere's (airL).
float3 InScatter(Sample s, float3 p, float3 view, float t, bool worldShadow, out float3 airL)
{
    float3 L = s.lamps;
    airL = s.lampsRaw * airD.w;
    float outdoor = 1.0 - s.indoor;
    if (outdoor > 0.0)
    {
        float4 lv = s.level < 4u ? LightVolumeAt(s.level, p, s.h) : float4(0.0, 0.0, 1.0, 0.0);
        float cosTheta = dot(lightDir.xyz, view);
        float tau = Isolated(FOG_ISO_NO_SHADOW) ? 0.0 : lv.r * shadow.x;
        float vis = Isolated(FOG_ISO_NO_TERRAIN_SH) ? 1.0 : lerp(1.0, lv.b, shadow.y);
        if (worldShadow && !Isolated(FOG_ISO_NO_WORLD_SH) && shadow.w > 0.0)
            vis *= lerp(1.0, WorldShadow(p - eye.xyz), shadow.w);
        // The atmosphere above thins the body's light (its density times its scale height, slanted);
        // what it scatters forward still arrives, so it fades as a diffusion, not an exponential.
        float tauAir = s.air / (max(airA.y, 1e-4) * max(lightDir.z, 0.08));
        float throughAir = 1.0 / (1.0 + 0.6 * min(tauAir, 50.0));

        // Multiple scattering: octaves of less extinction and a flatter phase (Wrenninge). Where only
        // the thin haze and the atmosphere are, the first bounce is all there is to light.
        bool fogHere = s.fog + s.smoke + s.dust > 1e-5;
        float sun = 0.0;
        float a = 1.0, b = 1.0, c = 1.0;
        int octaves = Isolated(FOG_ISO_NO_MS) || !fogHere ? 1 : 1 + int(scatterMs.y + 0.5);
        [loop] for (int o = 0; o < octaves; ++o)
        {
            float4 ph = float4(phase.xy * c, phase.z, 0.0);
            sun += a * exp(-b * tau) * PhaseDual(cosTheta, ph);
            a *= 0.5 * scatterMs.x;
            b *= 0.5;
            c *= 0.5;
        }
        // Diffusion inside thick fog: light that has bounced many times, fading slowly with depth.
        if (fogHere && !Isolated(FOG_ISO_NO_MS))
        {
            float thick = 1.0 - exp(-0.5 * (lv.r + lv.g));
            sun += scatterMs.x * 0.5 * thick / (1.0 + 0.75 * tau);
        }
        // Powder: thin fog facing away from the light scatters less of it forward.
        float powder = 1.0 - exp(-(s.fog + s.smoke) * 10.0);
        sun *= lerp(1.0, powder, phase.w * (0.5 - 0.5 * cosTheta));
        L += lightColor.rgb * lightDir.w * sun * vis * throughAir * outdoor;

        // The atmosphere: single scattering, in the lying fog's shadow but lit by what diffuses through.
        if (s.air > 0.0)
        {
            float underFog = max(exp(-tau), 0.3 * scatterMs.x / (1.0 + 0.75 * tau));
            airL += airD.rgb * lightDir.w * HG(cosTheta, airB.z) * underFog * vis * throughAir;
        }

        if (!Isolated(FOG_ISO_NO_AMBIENT))
        {
            float skyShare = max(SkyShareAt(skyTex, p.xy), 0.001);
            // Diffuse transmittance through the fog overhead (two-stream), not an exponential.
            float overhead = 1.0 / (1.0 + lv.g * scatterMs.z);
            float sky = max(pow(skyShare, outD.z) * overhead, outD.y);
            L += skyZenith.rgb * sky * outdoor;
            airL += airE.rgb * max(pow(skyShare, airC.w) * overhead, outD.y) * outdoor;
        }
    }
    if (s.indoor > 0.0) L += skyZenith.rgb * inC.x * 0.3 * s.indoor;
    return L;
}

// The light the sample scatters towards the camera per unit of extinction: each medium's colour and
// light, weighted by its share of the extinction.
float3 Scattered(Sample s, float3 fogL, float3 airL)
{
    float ext = max(Extinction(s), 1e-6);
    float3 a = outC.rgb * (s.fog + s.haze) + outH.rgb * (s.smoke * outH.w) + inB.rgb * s.dust;
    return (fogL * a + airL * airC.rgb * s.air) / ext;
}

#endif
