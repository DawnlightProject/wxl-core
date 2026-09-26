// wxl-graphics-fog: the map debug views, drawn into the debug image as an inset in the lower right
// (alpha 1 inside it, 0 elsewhere), north up, the camera ringed:
//   slice (top)   a level's fog seen from above at debug.z yards over its ground
//   slice (side)  a vertical slice along the view, height over the ground upwards
//   layer         the transport layer's depth; flow its velocity (hue the heading, brightness the
//                 speed); sources its net rate (green forming, red fading)
//   terrain       the floor (shaded), hollows (blue) and the sky share (brightness)
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

#include "clip.hlsli"
#include "terrain.hlsli"
#include "wake.hlsli"

FOG_TEX(0) Texture3D<float2> stateTex;
FOG_TEX(1) Texture3D<float>  groundTex;
FOG_TEX(2) Texture2D<float4> layerTex;
FOG_TEX(3) Texture2D<float2> floorTex;
FOG_TEX(4) Texture2D<float>  skyTex;
FOG_TEX(5) Texture3D<float4> wakeTex;
FOG_TEX(6) Texture2D<float4> cascadeTex;
FOG_TEX(7) Texture2D<float>  tracerTex;
FOG_OUT(0) [[vk::image_format("rgba16f")]] RWTexture2D<float4> outDebug;

float3 Ramp(float x)
{
    x = saturate(x) * 4.0;
    if (x < 1.0) return lerp(float3(0.02, 0.03, 0.12), float3(0.1, 0.3, 0.95), x);
    if (x < 2.0) return lerp(float3(0.1, 0.3, 0.95), float3(0.1, 0.9, 0.85), x - 1.0);
    if (x < 3.0) return lerp(float3(0.1, 0.9, 0.85), float3(1.0, 0.85, 0.2), x - 2.0);
    return lerp(float3(1.0, 0.85, 0.2), float3(1.0, 1.0, 1.0), x - 3.0);
}

float3 Hue(float h) { return saturate(abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0); }

float DensityRef() { return max(max(outA.x, outA.y), 1e-4); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(float2(id.xy) >= screenHalf.xy)) return;
    uint mode = uint(debug.x + 0.5);
    float size = floor(screenHalf.y * 0.46);
    float2 corner = screenHalf.xy - size - 8.0;
    float2 local = (float2(id.xy) - corner) / size;
    if (any(local < 0.0) || any(local >= 1.0))
    {
        outDebug[id.xy] = 0.0;
        return;
    }
    uint L = min(uint(debug.y + 0.5), 3u);
    float3 col = 0.0;
    float extent;
    if (mode == FOG_VIEW_SLICE_SIDE)
    {
        float3 fwd = RayDir(float2(0.5, 0.5));
        float2 dir = normalize(fwd.xy + float2(1e-4, 0.0));
        extent = CellXY(L) * kN * 0.9;
        float2 xy = eye.xy + dir * (local.x - 0.1) * extent;
        float h = lerp(HMax(L), HMin(L), local.y);
        float v = stateTex.SampleLevel(sLinearWrap, float3(LevelUv(L, xy), LevelW(L, h)), 0).x;
        col = Ramp(v / DensityRef());
        if (abs(local.x - 0.1) < 0.004) col = float3(1.0, 0.2, 0.9);
    }
    else
    {
        bool block = mode == FOG_VIEW_LAYER || mode == FOG_VIEW_FLOW || mode == FOG_VIEW_SOURCES || mode == FOG_VIEW_TERRAIN
                  || mode == FOG_VIEW_CASCADE;
        // A map of the block spans twice the distance scale (debug.w), at most the whole block; the
        // wake map the fine wake level (or the coarse one past 64 yards of distance scale).
        extent = block ? clamp(debug.w * 2.0, 100.0, kBlockN * kTexelYards) : CellXY(L) * kN * 0.95;
        if (mode == FOG_VIEW_WAKE) extent = (debug.w > 64.0 ? WakeCell(1u) : WakeCell(0u)) * kWakeN * 0.95;
        float2 xy = eye.xy + float2(0.5 - local.y, 0.5 - local.x) * extent;
        if (mode == FOG_VIEW_WAKE)
        {
            // Red where the fog is cleared, blue where thickened; the air's flow as a hue, by speed.
            float4 wk = WakeAt(wakeTex, xy);
            float speed = length(wk.xy);
            float2 d = speed > 0.001 ? wk.xy / speed : float2(1.0, 0.0);
            col = Hue(atan2(d.y, d.x) / 6.2831853 + 0.5) * saturate(speed / 4.0) * 0.6;
            col += float3(saturate(-wk.z), 0.0, saturate(wk.z)) * 0.8;
        }
        else if (mode == FOG_VIEW_SLICE_TOP)
        {
            float h = debug.z;
            float v = stateTex.SampleLevel(sLinearWrap, float3(LevelUv(L, xy), LevelW(L, h)), 0).x;
            col = Ramp(v / DensityRef());
        }
        else
        {
            float2 g = WorldToGrid(xy);
            bool resident = blockMask.w > 0.5 && TileResident(g);
            float4 lay = layerTex.SampleLevel(sLinearWrap, GridUv(g), 0);
            if (!resident) col = float3(0.03, 0.03, 0.03);
            else if (mode == FOG_VIEW_LAYER) col = Ramp(log2(1.0 + max(lay.x, 0.0)) / log2(1.0 + max(transC.w, 1.0)));
            else if (mode == FOG_VIEW_FLOW)
            {
                float speed = length(lay.yz);
                float2 d = speed > 0.001 ? lay.yz / speed : float2(1.0, 0.0);
                col = Hue(atan2(d.y, d.x) / 6.2831853 + 0.5) * (0.2 + 0.8 * saturate(speed / max(transD.y * 0.4, 0.1)));
                float dash = frac(dot(xy, d) / 12.0 - eye.w * speed / 12.0);
                col *= dash < 0.5 ? 1.0 : 0.5;
                col = lerp(float3(0.05, 0.05, 0.05), col, saturate(lay.x));
            }
            else if (mode == FOG_VIEW_CASCADE)
            {
                // Red spill points, blue reservoirs over the terrain; green where the layer is cascade fog.
                float4 c = cascadeTex.SampleLevel(sPointWrap, GridUv(g), 0);
                float2 grad = FloorGradient(floorTex, xy, 0.0);
                float shade = saturate(0.6 + dot(normalize(float3(-grad, 1.0)), normalize(float3(-0.5, 0.5, 0.7))) * 0.5) * 0.35;
                float share = tracerTex.SampleLevel(sLinearWrap, GridUv(g), 0) * saturate(lay.x / 2.0);
                col = float3(shade, shade, shade) + float3(c.r, share * 0.8, c.g * 0.8);
            }
            else if (mode == FOG_VIEW_SOURCES)
            {
                float ref = max(transB.x * 4.0, 1e-5);
                col = float3(saturate(-lay.w / ref), saturate(lay.w / ref), saturate(lay.x / max(transC.w, 1.0)) * 0.5);
            }
            else
            {
                float2 f = FloorAt(floorTex, xy, 0.0);
                float relief = FloorAt(floorTex, xy, 4.0).r - f.r;
                float2 grad = FloorGradient(floorTex, xy, 0.0);
                float shade = saturate(0.6 + dot(normalize(float3(-grad, 1.0)), normalize(float3(-0.5, 0.5, 0.7))) * 0.5);
                float sky = skyTex.SampleLevel(sLinearWrap, GridUv(g), 0);
                col = float3(shade, shade, shade) * (0.4 + 0.6 * sky);
                col = lerp(col, float3(0.1, 0.3, 1.0), saturate(relief / 10.0) * 0.7);
            }
        }
        float2 cam = float2(0.5, 0.5);
        float ring = abs(length((local - cam) * size) - 5.0);
        if (ring < 1.2) col = float3(1.0, 0.2, 0.9);
    }
    // The inset's frame.
    float2 edge = min(local, 1.0 - local) * size;
    if (min(edge.x, edge.y) < 1.0) col = float3(0.6, 0.6, 0.6);
    outDebug[id.xy] = float4(col, 1.0);
}
