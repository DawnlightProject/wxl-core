// wxl-graphics-shadow: the half-resolution masks brought to full resolution. Four taps around the pixel,
// bilinear weights times how close each traced depth lies to the pixel's own, so a silhouette never
// takes the other side's shadow.
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

[[vk::binding(SH_B_IN0, 0)]] Texture3D<float4> halfMask;
[[vk::binding(SH_B_IN1, 0)]] Texture2D<float>  halfDepth;
[[vk::binding(SH_B_OUT0, 0)]] [[vk::image_format("rgba8")]] RWTexture3D<float4> outMask;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float2 size = P[SH_ROW_SCREEN].xy;
    if (any(float2(id.xy) >= size)) return;
    float d = depthTex.Load(int3(id.xy, 0));
    if (ShIsSky(d))
    {
        [unroll] for (uint l = 0u; l < WXL_SHADOW_MASK_LAYERS; ++l) outMask[uint3(id.xy, l)] = 1.0;
        return;
    }
    float z = ShLinear(ShNdc(d));
    int2 trace = int2(P[SH_ROW_TRACE].xy);
    // Traced texel t stands for full pixel 2 t: its centre sits at 2 t + 0.5 in full pixels.
    float2 hp = (float2(id.xy) - 0.0) * 0.5;
    int2 base = int2(floor(hp));
    float2 f = hp - float2(base);
    float wsum = 0.0;
    float4 acc[WXL_SHADOW_MASK_LAYERS];
    [unroll] for (uint l = 0u; l < WXL_SHADOW_MASK_LAYERS; ++l) acc[l] = 0.0;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        int2 o = int2(k & 1, k >> 1);
        int2 t = clamp(base + o, int2(0, 0), trace - 1);
        float bil = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
        float zt = halfDepth.Load(int3(t, 0));
        float w = max(bil, 0.001) / (0.02 + abs(zt - z) / max(z * 0.02 + 0.05, 1e-3));
        [unroll] for (uint l2 = 0u; l2 < WXL_SHADOW_MASK_LAYERS; ++l2) acc[l2] += halfMask.Load(int4(t, l2, 0)) * w;
        wsum += w;
    }
    [unroll] for (uint l3 = 0u; l3 < WXL_SHADOW_MASK_LAYERS; ++l3) outMask[uint3(id.xy, l3)] = acc[l3] / max(wsum, 1e-6);
}
