// wxl-graphics-fog: a level step's second half. The MacCormack correction (half the error a trace
// forward then back makes, clamped to the departure cells' range: second order, no overshoot), then
// the sources: renewal towards the target over outI.x, gameplay's primitives (add, carve, hold, smoke),
// and for a coarse level the finer level's average where it covers the cell. Writes the level's slab
// of the state in place (a cell reads and writes only itself there). pc.a.x = the level.
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

FOG_TEX(0) Texture3D<float4> hatTex;       // this level's forward estimate
FOG_TEX(1) Texture3D<float>  groundTex;
FOG_TEX(2) Texture3D<float4> velTex;
FOG_TEX(3) Texture2D<float4> layerTex;
FOG_TEX(4) Texture3D<float4> shapeNoise;
FOG_TEX(5) Texture3D<float2> stateTex;     // every level, sampled: the finer level's average
FOG_TEX(6) Texture2D<float2> floorTex;
FOG_TEX(7) Texture2D<float>  tracerTex;
FOG_OUT(0) [[vk::image_format("rg16f")]] RWTexture3D<float2> state;   // every level; this one's slab written

// The finer level's fog over this cell's footprint, at the same world height.
float2 FinerAverage(uint L, float2 xy, float z)
{
    uint F = L - 1u;
    float q = CellXY(L) * 0.25;
    float2 sum = 0.0;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        float2 o = float2((i & 1) ? q : -q, (i & 2) ? q : -q);
        float2 at = xy + o;
        sum += SampleLevel(stateTex, F, at, z - LevelGround(groundTex, F, at));
    }
    return sum * 0.25;
}

[numthreads(8, 8, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FOG_LEVEL_N) || id.z >= FOG_LEVEL_NZ) return;
    uint L = pc.a.x;
    float s = CellXY(L);
    float dt = levelC[L].x;
    int2 texel = int2(id.xy);
    int k = int(id.z);
    int2 cell = CellOfTexel(L, texel);
    float2 xy = (float2(cell) + 0.5) * s;
    float h = CellCentreH(L, k);
    float G = groundTex.Load(int4(texel, L, 0));
    float3 p = float3(xy, G + h);
    int3 st = StateTexel(L, cell, k);

    TargetInputs ti = GatherTarget(layerTex, tracerTex, floorTex, shapeNoise, p, G);
    float boil = BoilNoise(shapeNoise, L, StreakPoint(p, ti));
    float target = TargetFog(p.z, boil, ti);
    float4 hat = hatTex.Load(int4(texel, k, 0));

    float fog, smoke;
    if (hat.z < 0.0)
    {
        fog = target;
        smoke = 0.0;
    }
    else
    {
        fog = hat.x;
        smoke = hat.y;
        float3 v = VelAt(velTex, L, xy, h);
        float3 b = p + v * dt;
        if (LevelInsidePrev(L, xy) >= 0.5 && LevelInside(L, b.xy) >= 1.0)
        {
            float hb = b.z - LevelGround(groundTex, L, b.xy);
            float2 back = hatTex.SampleLevel(sLinearWrap, float3(LevelUv(L, b.xy), ScratchW(L, hb)), 0).xy;
            float2 corrected = hat.xy + 0.5 * (state[st] - back);
            fog = clamp(corrected.x, hat.z, hat.w);
            smoke = max(corrected.y, 0.0);
        }

        // Renewal towards the target; smoke thins on its own.
        fog += (target - fog) * (1.0 - exp(-dt * outI.x));
        smoke *= exp(-dt * 0.12);

        // The finer level where it covers this cell: what it carries (wakes, holes) reaches here.
        if (L > 0u && levelC[L].w > 0.0 && LevelInside(L - 1u, xy) > 6.0
            && h > HMin(L - 1u) + CellZ(L - 1u) && h < HMax(L - 1u) - 2.0 * CellZ(L - 1u))
        {
            float2 fine = FinerAverage(L, xy, p.z);
            fog = lerp(fog, fine.x, levelC[L].w);
            smoke = lerp(smoke, fine.y, levelC[L].w);
        }

        // Primitives.
        uint4 mask = BinMask(L, cell - int2(levelB[L].xy));
        [loop] for (uint word = 0u; word < 4u; ++word)
        {
            uint bits = mask[word];
            [loop] while (bits != 0u)
            {
                uint bit = firstbitlow(bits);
                bits &= bits - 1u;
                Prim q = LoadPrim(word * 32u + bit);
                float w = PrimWeight(q, p, h, boil);
                if (w <= 0.0) continue;
                if (q.r2.x > 0.0)
                {
                    float add = q.r2.x * w * dt * (0.4 + 1.2 * boil);
                    fog = q.r2.z > 0.0 ? max(fog, min(fog + add, q.r2.z)) : fog + add;
                }
                else if (q.r2.x < 0.0)
                {
                    fog *= exp(q.r2.x * w * dt);
                    smoke *= exp(q.r2.x * w * dt);
                }
                else if (q.r2.z > 0.0)
                {
                    fog = max(fog, q.r2.z * w * lerp(1.0, 2.0 * boil, q.r4.w));
                }
                smoke += q.r2.y * w * dt;
            }
        }
    }
    state[st] = float2(max(fog, 0.0), max(smoke, 0.0));
}
