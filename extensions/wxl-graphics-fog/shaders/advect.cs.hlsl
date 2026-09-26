// wxl-graphics-fog: a level step's forward estimate (MacCormack, first half). Each cell of the level's
// new window traces back along the velocity and reads last step's state there (trilinear from the
// eight cells around the departure point, whose smallest and largest fog values the correction is
// later clamped to). A departure point outside the level's last window reads the next coarser level;
// with neither, the cell is marked for its target (min = -1). pc.a.x = the level.
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

#include "sim.hlsli"

FOG_TEX(0) Texture3D<float2> stateTex;    // every level, sampled
FOG_TEX(1) Texture3D<float>  groundTex;
FOG_TEX(2) Texture3D<float4> velTex;
FOG_TEX(3) Texture2D<float2> floorTex;
FOG_OUT(0) [[vk::image_format("rgba16f")]] RWTexture3D<float4> outHat;   // this level: fog, smoke, min, max

[numthreads(8, 8, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FOG_LEVEL_N) || id.z >= FOG_LEVEL_NZ) return;
    uint L = pc.a.x;
    float s = CellXY(L);
    int2 texel = int2(id.xy);
    int k = int(id.z);
    if (levelC[L].y > 0.5)
    {
        outHat[id] = float4(0.0, 0.0, -1.0, -1.0);
        return;
    }
    float2 xy = (float2(CellOfTexel(L, texel)) + 0.5) * s;
    float h = CellCentreH(L, k);
    float G = groundTex.Load(int4(texel, L, 0));
    float3 p = float3(xy, G + h);
    float3 q = p - VelAt(velTex, L, xy, h) * levelC[L].x;

    float4 hat = float4(0.0, 0.0, -1.0, -1.0);
    if (LevelInsidePrev(L, q.xy) >= 1.0)
    {
        float Gq = LevelInside(L, q.xy) >= 1.0 ? LevelGround(groundTex, L, q.xy)
                                               : FloorAt(floorTex, q.xy, GroundMip(L)).r;
        float hq = q.z - Gq;
        float2 c = q.xy / s - 0.5;
        int2 i0 = int2(floor(c));
        float2 f = c - float2(i0);
        float kz = clamp((hq - HMin(L)) / CellZ(L) - 0.5, 0.0, kNZ - 1.0);
        int k0 = min(int(kz), FOG_LEVEL_NZ - 2);
        float fz = kz - float(k0);
        float2 acc = 0.0;
        float lo = 1e9, hi = -1e9;
        [unroll] for (int dz = 0; dz < 2; ++dz)
            [unroll] for (int dy = 0; dy < 2; ++dy)
                [unroll] for (int dx = 0; dx < 2; ++dx)
                {
                    float2 v = stateTex.Load(int4(StateTexel(L, i0 + int2(dx, dy), k0 + dz), 0));
                    float w = (dx ? f.x : 1.0 - f.x) * (dy ? f.y : 1.0 - f.y) * (dz ? fz : 1.0 - fz);
                    acc += v * w;
                    lo = min(lo, v.x);
                    hi = max(hi, v.x);
                }
        hat = float4(acc, lo, hi);
    }
    else if (L + 1u < uint(FOG_LEVELS) && LevelInside(L + 1u, q.xy) >= 1.0)
    {
        float hc = q.z - LevelGround(groundTex, L + 1u, q.xy);
        float2 v = SampleLevel(stateTex, L + 1u, q.xy, hc);
        hat = float4(v, v.x, v.x);
    }
    outHat[id] = hat;
}
