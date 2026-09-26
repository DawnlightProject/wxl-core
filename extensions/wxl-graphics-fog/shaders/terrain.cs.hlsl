// wxl-graphics-fog: the terrain block's derived maps. pc.a.x selects:
//   0  the fog's floor at mip 0: the terrain, or a liquid's surface above it (mean and max alike);
//      the fallback height where no tile is resident.
//   1  one mip of the floor from the level above: r the mean of the four texels, g their maximum.
//      pc.a.y = the mip written.
//   2  the ground's sun visibility (FOG_SUNVIS_N^2 over the block): a march over the floor's maxima
//      towards the sun, soft by elevation. Read by the transport layer's burn-off.
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

#include "terrain.hlsli"

FOG_TEX(0) Texture2D<float>  rawHeight;   // R16F, the bake's heights
FOG_TEX(1) Texture2D<float2> rawWater;    // RG16F, liquid height and kind
FOG_TEX(2) Texture2D<float2> floorTex;    // RG32F, every mip (modes 1 and 2)

FOG_OUT(0) [[vk::image_format("rg32f")]] RWTexture2D<float2> outFloor;   // one mip's view
FOG_OUT(1) [[vk::image_format("r8")]]    RWTexture2D<float>  outSunVis;

float2 BuildFloor(int2 texel)
{
    float2 g = TexelToGrid(texel);
    if (!TileResident(g)) return block.w;
    float h = rawHeight.Load(int3(texel, 0));
    if (h < -5000.0) return block.w;
    float2 w = rawWater.Load(int3(texel, 0));
    if (w.y > 0.5 && w.x > -5000.0) h = max(h, w.x);
    return h;
}

float2 Reduce(int2 texel, uint mip)
{
    int2 s = texel * 2;
    float2 a = floorTex.Load(int3(s, mip - 1u));
    float2 b = floorTex.Load(int3(s + int2(1, 0), mip - 1u));
    float2 c = floorTex.Load(int3(s + int2(0, 1), mip - 1u));
    float2 d = floorTex.Load(int3(s + int2(1, 1), mip - 1u));
    return float2((a.x + b.x + c.x + d.x) * 0.25, max(max(a.y, b.y), max(c.y, d.y)));
}

// Visibility of the sun from the ground at a block texel of the FOG_SUNVIS_N grid.
float SunVisibility(int2 texel)
{
    if (sunDir.z <= 0.01) return 0.0;
    float scale = kBlockN / float(FOG_SUNVIS_N);
    float2 g = TexelToGrid(int2((float2(texel) + 0.5) * scale));
    float2 xy = GridToWorld(g);
    float z = FloorAt(floorTex, xy, 2.0).r + 1.0;
    float2 dirXY = sunDir.xy / max(length(sunDir.xy), 1e-4);
    float tanElev = sunDir.z / max(length(sunDir.xy), 1e-4);
    float vis = 1.0;
    float t = 8.0;
    for (int i = 0; i < 20; ++i)
    {
        float2 q = xy + dirXY * t;
        float mip = clamp(log2(t / 32.0), 0.0, 6.0);
        float hq = FloorAt(floorTex, q, mip).g;
        float rise = z + t * tanElev;
        vis = min(vis, saturate((rise - hq) / (t * shadow.z) + 0.5));
        t *= 1.35;
    }
    return vis;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (pc.a.x == 0u)
    {
        if (any(id.xy >= FOG_BLOCK_N)) return;
        outFloor[id.xy] = BuildFloor(int2(id.xy));
    }
    else if (pc.a.x == 1u)
    {
        uint size = uint(FOG_BLOCK_N) >> pc.a.y;
        if (any(id.xy >= size)) return;
        outFloor[id.xy] = Reduce(int2(id.xy), pc.a.y);
    }
    else
    {
        if (any(id.xy >= FOG_SUNVIS_N)) return;
        outSunVis[id.xy] = SunVisibility(int2(id.xy));
    }
}
