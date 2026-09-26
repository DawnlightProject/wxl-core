// wxl-graphics-fog: temporal accumulation at half resolution, per layer (the nearest and the farthest
// scene distance of a pixel's footprint). Each layer's history is reprojected through last frame's
// camera at its own representative distance, taken from whichever of last frame's two layers saw the
// same surface, then clipped to this frame's 3 x 3 neighbourhood of the matching layers (mean plus or
// minus temporal.y standard deviations) so nothing ghosts behind moving bodies. It is dropped off
// screen, when the camera jumped, and where no layer of last frame saw this surface (disocclusion).
// At a silhouette the history is read at the nearest texel, not blended across it. temporal.x is the
// share of the new frame; fog past the near range keeps more of its history.
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

#include "common.hlsli"

FOG_TEX(0) Texture2D<float4> curFog;       // back layer
FOG_TEX(1) Texture2D<float4> curAux;       // rep back, rep front, distance near, far
FOG_TEX(2) Texture2D<float4> histFog;
FOG_TEX(3) Texture2D<float4> histAux;
FOG_TEX(4) Texture2D<float4> curFront;
FOG_TEX(5) Texture2D<float4> histFront;
FOG_OUT(0) [[vk::image_format("rgba16f")]] RWTexture2D<float4> outFog;
FOG_OUT(1) [[vk::image_format("rgba32f")]] RWTexture2D<float4> outAux;
FOG_OUT(2) [[vk::image_format("rgba16f")]] RWTexture2D<float4> outDebug;
FOG_OUT(3) [[vk::image_format("rgba16f")]] RWTexture2D<float4> outFront;

float Mismatch(float a, float d) { return abs(a - d) / max(d, 1.0); }

bool IsSky(float d) { return d >= march.y * 0.99; }

// One layer: c this frame's value, (m1, sigma) its neighbourhood, d its scene distance, rep its
// representative distance. rejected is set when the history is not used.
float4 Resolve(float4 c, float4 m1, float4 sigma, float d, float rep, float3 view, bool edge, out float rejected)
{
    rejected = 1.0;
    float3 prev = ReprojectUv(view * rep);
    if (temporal.z < 0.5 || Isolated(FOG_ISO_NO_HISTORY) || prev.z <= 0.0 || any(prev.xy <= 0.0) || any(prev.xy >= 1.0))
        return c;
    float4 ha = histAux.SampleLevel(sPointClamp, prev.xy, 0);
    // The surface this layer sees, as last frame's camera saw its distance: whichever layer matches.
    float expected = length(view * d + eye.xyz - prevEye.xyz);
    float mFront = Mismatch(ha.z, expected), mBack = Mismatch(ha.w, expected);
    bool useFront = mFront < mBack;
    bool sky = IsSky(d) && IsSky(useFront ? ha.z : ha.w);
    if (!sky && min(mFront, mBack) > temporal.w) return c;
    float4 h;
    if (edge)
    {
        int2 at = int2(prev.xy * screenHalf.xy);
        h = useFront ? histFront.Load(int3(at, 0)) : histFog.Load(int3(at, 0));
    }
    else
    {
        h = useFront ? histFront.SampleLevel(sLinearClamp, prev.xy, 0) : histFog.SampleLevel(sLinearClamp, prev.xy, 0);
    }
    if (!Isolated(FOG_ISO_NO_CLIP))
    {
        float4 lo = m1 - temporal.y * sigma, hi = m1 + temporal.y * sigma;
        h = clamp(h, min(lo, c), max(hi, c));
    }
    rejected = 0.0;
    // Fog past the near range comes from the coarser far march: it keeps more of its history.
    float fresh = temporal.x * lerp(1.0, 0.55, saturate((rep - march.x) / (2.0 * march.x)));
    return lerp(h, c, fresh);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(float2(id.xy) >= screenHalf.xy)) return;
    int2 pix = int2(id.xy);
    int2 maxP = int2(screenHalf.xy) - 1;
    float4 ca = curAux.Load(int3(pix, 0));
    float4 cb = curFog.Load(int3(pix, 0));
    float4 cf = curFront.Load(int3(pix, 0));
    bool edge = ca.z < ca.w;

    // Each layer's neighbourhood from the neighbours' layers that see the same distance.
    float4 m1b = 0.0, m2b = 0.0, m1f = 0.0, m2f = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            int3 at = int3(clamp(pix + int2(x, y), 0, maxP), 0);
            float4 a = curAux.Load(at);
            float4 vb = curFog.Load(at);
            float4 vf = curFront.Load(at);
            float4 forBack = Mismatch(a.z, ca.w) < Mismatch(a.w, ca.w) ? vf : vb;
            float4 forFront = Mismatch(a.z, ca.z) < Mismatch(a.w, ca.z) ? vf : vb;
            m1b += forBack;
            m2b += forBack * forBack;
            m1f += forFront;
            m2f += forFront * forFront;
        }
    m1b /= 9.0; m2b /= 9.0; m1f /= 9.0; m2f /= 9.0;
    float4 sb = sqrt(max(m2b - m1b * m1b, 0.0));
    float4 sf = sqrt(max(m2f - m1f * m1f, 0.0));

    float2 uv = (float2(pix) + 0.5) * screenHalf.zw;
    float3 view = RayDir(uv);
    float rejBack, rejFront;
    float4 back = Resolve(cb, m1b, sb, ca.w, ca.x, view, edge, rejBack);
    float4 front = back;
    rejFront = rejBack;
    if (edge) front = Resolve(cf, m1f, sf, ca.z, ca.y, view, edge, rejFront);

    outFog[pix] = back;
    outFront[pix] = front;
    outAux[pix] = ca;
    if (uint(debug.x + 0.5) == FOG_VIEW_DEPTH)
        outDebug[pix] = float4(saturate(ca.x / max(debug.w, 1.0)), max(rejBack, rejFront), saturate(ca.w / max(debug.w, 1.0)) * 0.5, 1.0);
}
