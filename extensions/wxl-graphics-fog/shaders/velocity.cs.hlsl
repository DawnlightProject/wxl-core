// wxl-graphics-fog: a level's velocity field for one step, at half the level's resolution and aligned
// to its window (texel i covers the window's cells 2i and 2i + 1). Kinematic: the transport layer's
// flow inside the layer, the wind (a height profile, its up-slope part removed near steep ground, so
// relief deflects it), a vertical part that keeps the fog on the terrain, curl-noise turbulence
// strongest at the layer's top, the wake fluid's flow near the ground, and the primitives' pushes.
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
#include "wake.hlsli"

FOG_TEX(0) Texture2D<float2> floorTex;
FOG_TEX(1) Texture2D<float4> layerTex;
FOG_TEX(2) Texture3D<float>  groundTex;
FOG_TEX(3) Texture3D<float4> curlNoise;
FOG_TEX(4) Texture3D<float4> wakeTex;
FOG_OUT(0) [[vk::image_format("rgba16f")]] RWTexture3D<float4> outVel;   // FOG_VEL_N^2 x FOG_VEL_NZ

[numthreads(8, 8, 2)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FOG_VEL_N) || id.z >= FOG_VEL_NZ) return;
    uint L = pc.a.x;
    float s = CellXY(L);
    int2 rel = int2(id.xy) * 2 + 1;
    float2 xy = (levelB[L].xy + float2(rel)) * s;
    float h = HMin(L) + (float(id.z) * 2.0 + 1.0) * CellZ(L);
    float G = LevelGround(groundTex, L, xy);
    float3 p = float3(xy, G + h);

    // The layer's own flow, inside it.
    float2 g = WorldToGrid(xy);
    float4 lay = blockMask.w > 0.5 && TileResident(g) ? layerTex.SampleLevel(sLinearWrap, GridUv(g), 0) : 0.0;
    float d = Isolated(FOG_ISO_NO_LAYER) ? 0.0 : lay.x;
    // Inside the layer by the target's own ground.
    float hLayer = p.z - min(FloorAt(floorTex, xy, 0.0).r, G);
    float inLayer = d > 0.05 ? 1.0 - smoothstep(0.8 * d, 1.2 * d + 1.5, hLayer) : 0.0;
    float2 v = lay.yz * inLayer * outJ.w;

    // The wind: calm at the ground, full by 40 yd, damped inside a deep layer.
    float windH = saturate(log(1.0 + max(h, 0.0) / 0.4) / log(1.0 + 40.0 / 0.4));
    float2 wind = outG.xy * outG.z * lerp(0.35, 1.0, windH) * (1.0 - 0.6 * inLayer);
    float mip = max(log2(2.0 * s / kTexelYards), 0.0);
    float2 grad = FloorGradient(floorTex, xy, mip);
    float gs = length(grad);
    if (gs > 0.02)
    {
        float2 gn = grad / gs;
        float into = dot(wind, gn);
        if (into > 0.0) wind -= into * gn * saturate(gs * 2.5) * exp(-max(h, 0.0) / 15.0);
    }
    v += wind;

    // The air the bodies stir (the wake fluid), up to a little over their heads.
    if (L < 3u)
    {
        float4 wk = WakeAt(wakeTex, xy);
        v += wk.xy * outK.y * WakeReach(h, max(WakeTop(wk), 2.5));
    }

    // Following the terrain: rising with it where the flow climbs, sinking where it descends.
    float vz = dot(v, grad) * exp(-max(h, 0.0) / 20.0);

    // Turbulence, strongest at the layer's top where the flow shears.
    float turbScale = max(4.0 * outJ.x, 6.0 * s);
    float3 curl = curlNoise.SampleLevel(sLinearWrap, p / turbScale + drift2.yzw, 0).xyz * 2.0 - 1.0;
    float top = d > 0.3 ? exp(-pow((hLayer - d) / max(0.4 * d, 1.5), 2.0)) : 0.0;
    float3 vel = float3(v, vz) + curl * outG.w * (0.35 + 0.65 * max(top, 0.5 * inLayer));

    // Primitives: pushes, radial pushes and swirls.
    uint4 mask = BinMask(L, rel);
    [loop] for (uint word = 0u; word < 4u; ++word)
    {
        uint bits = mask[word];
        [loop] while (bits != 0u)
        {
            uint bit = firstbitlow(bits);
            bits &= bits - 1u;
            Prim q = LoadPrim(word * 32u + bit);
            vel += PrimVelocity(q, p, PrimWeight(q, p, h, 0.5));
        }
    }
    outVel[id] = float4(vel, 0.0);
}
