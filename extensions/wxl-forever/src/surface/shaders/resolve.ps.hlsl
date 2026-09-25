// wxl-forever surface lighting: the light buffer brought to full resolution and published.
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

#include "surface/shaders/surface.hlsli"

// Full resolution. The accumulated, denoised light (at the lighting resolution) times this frame's
// share of it the omni shadow maps let through (movingC.w; applied after the history so a moving
// light's shadow never lingers), brought up by a joint bilateral upsample: of the four lighting
// texels around a pixel, each weighted by its bilinear share and by how close its distance is to
// this pixel's, so no light crosses a silhouette; when none agrees the nearest is taken. Then the
// highlight cap. Out: COLOR0 the light buffer apply and the indirect pass read; COLOR1 the buffer the
// engine's own materials read next frame (rgb the light, a the 0.5 marker the rewritten shaders test),
// reprojected as if the world stood still, so units (which do not) and a margin around them are
// cleared from it: apply lights those pixels itself.
sampler2D lightTex : register(s3);
sampler2D metaTex : register(s6);
sampler2D ratioTex : register(s7);

struct Out
{
    float4 light : COLOR0;
    float4 engine : COLOR1;
};

float4 Sample(float2 at)
{
    float4 l = tex2Dlod(lightTex, float4(at, 0, 0));
    if (movingC.w > 0.5) l *= tex2Dlod(ratioTex, float4(at, 0, 0));
    return l;
}

Out main(float2 vpos : VPOS)
{
    Out o;
    float2 uv = (vpos + 0.5) * screen.xy;
    float4 here = SurfaceAt(uv);
    if (here.w < 0.0) { o.light = 0.0; o.engine = 0.0; return o; }
    float4 light;
    if (faceC.y < 1.5) light = Sample(uv);
    else
    {
        // Lighting texel i stands for the surface point of full pixel i * scale (PassUv).
        float2 q = (uv - (faceC.y - 1.0) * 0.5 * screen.xy) * lowC.zw - 0.5;
        float2 base = floor(q);
        float2 f = q - base;
        float4 sum = 0.0;
        float total = 0.0;
        float nearest = 1e9;
        float4 fallback = 0.0;
        for (int y = 0; y <= 1; ++y)
            for (int x = 0; x <= 1; ++x)
            {
                float2 at = (base + float2(x, y) + 0.5) * lowC.xy;
                float4 m = tex2Dlod(metaTex, float4(at, 0, 0));
                if (m.y < 0.0) continue;
                float gap = abs(m.y - here.w);
                float w = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y)
                        * exp(-gap / (0.03 * here.w + 0.02));
                float4 s = Sample(at);
                sum += s * w;
                total += w;
                if (gap < nearest) { nearest = gap; fallback = s; }
            }
        light = total > 0.001 ? sum / total : fallback;
    }
    // The Normals and Omni faces views carry colours, not light: no cap on them.
    bool view = (screen.z > 1.5 && screen.z < 2.5) || (screen.z > 5.5 && screen.z < 6.5) || cookieD.z > 0.5;
    if (!view) light = CapLight(light);
    o.light = light;
    // Alpha 0.5 marks a texel the engine's materials may add (an unbound sampler reads 0 or 1).
    o.engine = here.w > 0.0 && OnUnitWide(here.xyz, toneC.z) > 0.5 ? 0.0 : float4(light.rgb * EngineEdge(uv), 0.5);
    return o;
}
