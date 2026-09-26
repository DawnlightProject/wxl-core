// wxl-graphics-fog: the near and far marches as one image at half resolution, per layer: S = Sn + Tn Sf,
// T = Tn Tf. Each half-resolution pixel carries two layers (the nearest and the farthest scene
// distance of its footprint); each takes its far part from the four nearest quarter-resolution texels,
// every texel offering its own two layers: the one whose distance matches is used, weighted bilinearly
// and by how well it matches. A quarter texel's footprint holds its four half texels', so a layer's
// distance is always among its parent's. A layer whose scene lies within the near range has no far part.
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

FOG_TEX(0) Texture2D<float4> nearFog;      // back layer
FOG_TEX(1) Texture2D<float4> nearAux;      // rep back, rep front, distance near, far
FOG_TEX(2) Texture2D<float4> farFog;
FOG_TEX(3) Texture2D<float4> farAux;
FOG_TEX(4) Texture2D<float4> nearFront;
FOG_TEX(5) Texture2D<float4> farFront;
FOG_OUT(0) [[vk::image_format("rgba16f")]] RWTexture2D<float4> outFog;
FOG_OUT(1) [[vk::image_format("rgba32f")]] RWTexture2D<float4> outAux;
FOG_OUT(2) [[vk::image_format("rgba16f")]] RWTexture2D<float4> outFront;

// How well a layer at distance a stands for a pixel at distance d.
float Match(float a, float d) { return 1.0 / (0.02 + abs(a - d) / max(d, 1.0)); }

// The far part of a layer whose scene lies at distance d, and its representative distance.
float4 FarPart(int2 pix, float d, out float rep)
{
    rep = d;
    if (d <= march.x + 0.01 || Isolated(FOG_ISO_NO_FAR)) return float4(0.0, 0.0, 0.0, 1.0);
    float2 q = (float2(pix) + 0.5) * 0.5 - 0.5;
    int2 base = int2(floor(q));
    float2 fr = q - float2(base);
    int2 maxQ = int2(screenQuarter.xy) - 1;
    float4 acc = 0.0;
    float racc = 0.0, wacc = 0.0;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        int2 o = int2(i & 1, i >> 1);
        int2 at = clamp(base + o, 0, maxQ);
        float4 a = farAux.Load(int3(at, 0));
        float mFront = Match(a.z, d), mBack = Match(a.w, d);
        bool useFront = mFront > mBack;
        float depth = useFront ? a.z : a.w;
        float bil = (o.x ? fr.x : 1.0 - fr.x) * (o.y ? fr.y : 1.0 - fr.y);
        float w = max(bil, 0.001) * max(mFront, mBack) * (depth > march.x ? 1.0 : 0.02);
        acc += (useFront ? farFront.Load(int3(at, 0)) : farFog.Load(int3(at, 0))) * w;
        racc += (useFront ? a.y : a.x) * w;
        wacc += w;
    }
    rep = racc / max(wacc, 1e-6);
    return acc / max(wacc, 1e-6);
}

// A layer: its near part, then its far part behind it.
float4 Layer(float4 n, float repNear, float4 f, float repFar, float d, out float rep)
{
    float wn = 1.0 - n.a, wf = n.a * (1.0 - f.a);
    rep = wn + wf > 1e-4 ? (wn * repNear + wf * repFar) / (wn + wf) : min(d, march.y);
    return float4(n.rgb + n.a * f.rgb, n.a * f.a);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(float2(id.xy) >= screenHalf.xy)) return;
    int2 pix = int2(id.xy);
    float4 na = nearAux.Load(int3(pix, 0));

    float repFarBack, repFarFront, repBack, repFront;
    float4 fBack = FarPart(pix, na.w, repFarBack);
    float4 back = Layer(nearFog.Load(int3(pix, 0)), na.x, fBack, repFarBack, na.w, repBack);
    float4 front = back;
    repFront = repBack;
    if (na.z < na.w)
    {
        float4 fFront = FarPart(pix, na.z, repFarFront);
        front = Layer(nearFront.Load(int3(pix, 0)), na.y, fFront, repFarFront, na.z, repFront);
    }
    outFog[pix] = back;
    outFront[pix] = front;
    outAux[pix] = float4(repBack, repFront, na.z, na.w);
}
