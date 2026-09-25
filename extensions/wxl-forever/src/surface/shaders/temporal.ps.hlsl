// wxl-forever surface lighting: temporal accumulation of a noisy light (direct or indirect).
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

#include "surface/shaders/surface.hlsli"

// Each pixel keeps its own history length: one more frame each time last frame's value at this
// surface point is trusted, up to temporalC.x frames, so a still view averages up to that many
// jittered samples and the noise goes. The history is trusted when the reprojected point's distance
// and normal agree with last frame's (a disocclusion or a moving object breaks one of them) and is
// always clamped to this frame's neighbourhood, mean plus or minus temporalC.y sigmas in YCoCg, so a
// change of light shows at once. On units the history stays short. Where the history's luminance
// sits further from this frame's local mean than the local noise explains (moved.w sigmas), the
// lighting changed there (a torch passed, a flame flared) and the history shortens to a couple of
// frames; where it sits within the noise (a jittered penumbra) it keeps averaging. So a carried
// torch leaves no trail and the ground it lights shows no grain. Within the radius of a light that
// moved lately the clamp is tighter (movingC.z sigmas). Drawn at the resolution of the light it
// accumulates (resC), each pixel standing for the full-resolution texel PassUv names.
// Out: COLOR0 the accumulated light, COLOR1 (history length, distance, luminance variance, 0).
sampler2D normalsTex : register(s1);
sampler2D rawTex : register(s3);
sampler2D previousNormals : register(s4);
sampler2D historyTex : register(s5);
sampler2D historyMeta : register(s6);

struct Out
{
    float4 light : COLOR0;
    float4 meta : COLOR1;
};

Out main(float2 vpos : VPOS)
{
    Out o;
    float2 uv = (vpos + 0.5) * resC.xy;
    float4 here = SurfaceAt(PassUv(vpos));
    float4 raw = tex2Dlod(rawTex, float4(uv, 0, 0));
    if (here.w < 0.0)
    {
        o.light = 0.0;
        o.meta = float4(0.0, -1.0, 0.0, 0.0);
        return o;
    }

    // This frame's neighbourhood, in YCoCg, for the clamp and the variance.
    float3 m1 = 0.0, m2 = 0.0;
    float a1 = 0.0, aMin = 1e9, aMax = -1e9;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            float4 s = tex2Dlod(rawTex, float4(uv + float2(x, y) * resC.xy, 0, 0));
            float3 c = ToYCoCg(s.rgb);
            m1 += c;
            m2 += c * c;
            aMin = min(aMin, s.a);
            aMax = max(aMax, s.a);
        }
    m1 /= 9.0;
    m2 /= 9.0;
    float3 sigma = sqrt(max(m2 - m1 * m1, 0.0));

    // Near a light that moved lately the clamp is tight.
    float cap = temporalC.x, sigmas = temporalC.y;
    for (int i = 0; i < 4; ++i)
    {
        if ((float)i >= movingC.x) break;
        float3 v = here.xyz - movingLights[i].xyz;
        if (dot(v, v) < movingLights[i].w * movingLights[i].w) sigmas = min(sigmas, movingC.z);
    }

    float length_ = 1.0;
    float4 light = raw;
    if (temporalC.x > 1.5)
    {
        float4 h = float4(here.xyz + moved.xyz, 1.0);
        float4 pc = float4(dot(h, prev0), dot(h, prev1), dot(h, prev2), dot(h, prev3));
        if (pc.w > 0.0001)
        {
            float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
            if (all(puv > 0.0) && all(puv < 1.0))
            {
                // The history texel whose surface point was here; the normals are at half resolution.
                float2 at = TexelOf(puv, resC.z);
                float4 before = tex2Dlod(historyMeta, float4(at, 0, 0));
                float expect = length(h.xyz);
                bool sameDepth = before.y > 0.0 && abs(before.y - expect) < temporalC.z * expect + 0.05;
                float3 nNow = tex2Dlod(normalsTex, float4(TexelOf(PassUv(vpos), 2.0), 0, 0)).xyz;
                float3 nBefore = tex2Dlod(previousNormals, float4(TexelOf(puv, 2.0), 0, 0)).xyz;
                bool sameNormal = dot(nNow, nBefore) > temporalC.w;
                if (sameDepth && sameNormal)
                {
                    // The further this point moved on screen, the less its resampled (so blurred and
                    // offset) history is trusted: 8 full pixels keep 7 frames, 24 keep 3.
                    float motion = length((puv - PassUv(vpos)) / screen.xy);
                    cap = min(cap, 1.0 + 48.0 / max(motion, 1.0));
                    sigmas = lerp(sigmas, min(sigmas, 0.75), saturate(motion / 16.0));
                    // Around a character the shadow it casts moves with it: a short history there.
                    if (OnUnitWide(here.xyz, 2.5) > 0.5) cap = min(cap, 4.0);
                    length_ = min(before.x + 1.0, cap);
                    if (OnUnit(here.xyz) > 0.5) length_ = min(length_, 2.0);
                    float4 past = tex2Dlod(historyTex, float4(at, 0, 0));
                    float3 pastY = ToYCoCg(past.rgb);
                    if (moved.w > 0.0)
                    {
                        // The shift of the history from the local mean, in units of the local noise
                        // (with a floor of a few percent of the mean, so a smooth pool still counts).
                        float shift = abs(pastY.x - m1.x) / (sigma.x + 0.002 + 0.03 * m1.x);
                        float cut = saturate((shift - moved.w) / (2.0 * moved.w));
                        length_ = lerp(length_, min(length_, 2.0), cut);
                    }
                    float3 clamped = clamp(pastY, m1 - sigma * sigmas, m1 + sigma * sigmas);
                    past.rgb = max(FromYCoCg(clamped), 0.0);
                    past.a = clamp(past.a, aMin, aMax);
                    light = lerp(past, raw, 1.0 / length_);
                }
            }
        }
    }
    o.light = light;
    o.meta = float4(length_, here.w, sigma.x * sigma.x, 0.0);
    return o;
}
