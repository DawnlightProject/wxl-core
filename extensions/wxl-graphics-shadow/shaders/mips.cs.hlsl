// wxl-graphics-shadow: one mip level of the filtered atlas, face by face: each texel the mean of the 2 x 2
// moments below it, inside its own face's cell (cells are aligned powers of two, so no face ever mixes
// with a neighbour).
//   push a.x  jobs 0..31 (bit map * 6 + face), a.y jobs 32..47; b.x the level's face texels
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

[[vk::binding(SH_B_OUT0, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> dstLevel;
[[vk::binding(SH_B_OUT1, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> srcLevel;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint job = id.z;
    if (job >= 48u) return;
    uint bits = job < 32u ? push.a.x : push.a.y;
    if (((bits >> (job & 31u)) & 1u) == 0u) return;
    uint F = uint(push.b.x);
    if (F == 0u || any(id.xy >= F)) return;
    uint map = job / 6u, face = job % 6u;
    uint2 cell = uint2((map % 2u) * 3u + face % 3u, (map / 2u) * 2u + face / 3u);
    uint2 dst = cell * F + id.xy;
    uint2 src = cell * (F * 2u) + id.xy * 2u;
    float4 m = srcLevel[src] + srcLevel[src + uint2(1, 0)] + srcLevel[src + uint2(0, 1)] + srcLevel[src + uint2(1, 1)];
    dstLevel[dst] = m * 0.25;
}
