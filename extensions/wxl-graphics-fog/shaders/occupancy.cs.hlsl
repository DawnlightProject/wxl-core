// wxl-graphics-fog: a level's occupancy, the largest density of every 8 x 8 x 8 texels of its slab
// (toroidal texel blocks, like the state). The march skips a sample's full cost where it is empty.
// One workgroup per block. pc.a.x = the level.
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

FOG_TEX(0) Texture3D<float2> stateTex;
FOG_OUT(0) [[vk::image_format("r16f")]] RWTexture3D<float> outOcc;   // FOG_OCC_N^2 x (FOG_OCC_NZ * levels)

groupshared float gMax[16];

[numthreads(8, 8, 8)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    uint L = pc.a.x;
    uint3 t = gid * 8u + gtid;
    float2 v = stateTex.Load(int4(t.xy, L * FOG_LEVEL_NZ + t.z, 0));
    float m = WaveActiveMax(v.x + v.y);
    // 512 threads in waves of 32 (or more): one value per wave, then the first wave folds them.
    uint waves = (512u + WaveGetLaneCount() - 1u) / WaveGetLaneCount();
    if (WaveIsFirstLane()) gMax[gi / WaveGetLaneCount()] = m;
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0u)
    {
        float r = 0.0;
        for (uint i = 0u; i < waves && i < 16u; ++i) r = max(r, gMax[i]);
        outOcc[int3(gid.xy, L * FOG_OCC_NZ + gid.z)] = r;
    }
}
