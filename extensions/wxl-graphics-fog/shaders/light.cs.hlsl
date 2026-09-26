// wxl-graphics-fog: a level's light volume, at half the level's resolution and toroidal like it (so it
// shares the level's texture coordinates). Per texel:
//   r  optical depth towards the active body (sun or moon): a march growing by half its step each
//      time, through the finest level holding each point
//   g  optical depth straight up, for the sky light under deep fog
//   b  the terrain's shadow: a march over the floor's maxima towards the body, soft by elevation
// pc.a.x = the level.
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

FOG_TEX(0) Texture3D<float2> stateTex;
FOG_TEX(1) Texture3D<float>  groundTex;
FOG_TEX(2) Texture2D<float2> floorTex;
FOG_OUT(0) [[vk::image_format("rgba16f")]] RWTexture3D<float4> outLight;   // 64^2 x (16 * levels)

// Fog and smoke at a world point from the finest level at or above `from` that holds it.
float DensityFrom(uint from, float3 p)
{
    [loop] for (uint L = from; L < uint(FOG_LEVELS); ++L)
    {
        if (LevelInside(L, p.xy) < 2.0) continue;
        float h = p.z - LevelGround(groundTex, L, p.xy);
        if (h < HMin(L) || h > HMax(L) - CellZ(L)) continue;
        float2 v = SampleLevel(stateTex, L, p.xy, h);
        return v.x + v.y;
    }
    return 0.0;
}

float TerrainShadow(float3 p, float3 toLight)
{
    if (toLight.z <= 0.0) return 0.0;
    float horiz = max(length(toLight.xy), 1e-4);
    float2 dirXY = toLight.xy / horiz;
    float tanElev = toLight.z / horiz;
    float vis = 1.0;
    float t = 3.0;
    [loop] for (int i = 0; i < 18; ++i)
    {
        float2 q = p.xy + dirXY * t;
        float mip = clamp(log2(t / 24.0), 0.0, 6.0);
        float hq = FloorAt(floorTex, q, mip).g;
        vis = min(vis, saturate((p.z + t * tanElev - hq) / (t * shadow.z) + 0.5));
        if (vis <= 0.0) break;
        t *= 1.4;
    }
    return vis;
}

[numthreads(8, 8, 2)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FOG_VEL_N) || id.z >= FOG_VEL_NZ) return;
    uint L = pc.a.x;
    float s = CellXY(L);
    int2 cell = CellOfTexel(L, int2(id.xy) * 2);
    float2 xy = (float2(cell) + 1.0) * s;
    float h = HMin(L) + (float(id.z) * 2.0 + 1.0) * CellZ(L);
    float3 p = float3(xy, LevelGround(groundTex, L, xy) + h);
    float3 toLight = lightDir.xyz;

    float tauSun = 0.0;
    float vis = 0.0;
    if (lightDir.w > 0.0 && toLight.z > -0.05)
    {
        float step = 0.75 * s;
        float t = 0.0;
        [loop] for (int i = 0; i < 10; ++i)
        {
            float3 q = p + toLight * (t + 0.5 * step);
            tauSun += DensityFrom(L, q) * step;
            t += step;
            step *= 1.5;
        }
        vis = Isolated(FOG_ISO_NO_TERRAIN_SH) ? 1.0 : TerrainShadow(p, toLight);
    }

    float tauUp = 0.0;
    {
        float step = 2.0 * CellZ(L);
        float t = 0.0;
        [loop] for (int i = 0; i < 8; ++i)
        {
            float3 q = p + float3(0.0, 0.0, t + 0.5 * step);
            tauUp += DensityFrom(L, q) * step;
            t += step;
            step *= 1.6;
        }
    }
    outLight[int3(id.xy, L * FOG_VEL_NZ + id.z)] = float4(tauSun, tauUp, vis, 0.0);
}
