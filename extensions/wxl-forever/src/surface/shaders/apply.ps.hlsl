// wxl-forever surface lighting: the light buffer applied to the scene.
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

// scene + albedo x (direct light + indirect light) + specular, the albedo estimated from the scene
// (surface.hlsli, Albedo), so a lamp tints what it lights. The direct light is the resolved buffer
// (full resolution, capped). The indirect light, at quarter resolution, is brought up by a joint
// bilateral upsample of the four quarter texels around, each kept only while its distance is close
// to this pixel's, so no halo crosses a silhouette. The lit colour is then compressed towards white
// by its luminance, keeping its hue and never passing 1, so a warm light brightens skin without
// pushing it to orange and a bright pool keeps its gradient instead of clipping. Pixels no light
// reached are discarded.
sampler2D sceneTex : register(s2);
sampler2D lightBuffer : register(s3);
sampler2D giTex : register(s4);
sampler2D giMeta : register(s5);
sampler2D gbufferNormals : register(s6);
sampler2D engineBuffer : register(s7);
sampler2D omniAtlas : register(s11);

float3 Indirect(float2 uv, float dist)
{
    // Quarter texel i stands for the surface point of full pixel 4 i (PassUv).
    float2 q = (uv - 1.5 * screen.xy) * quarterC.zw - 0.5;
    float2 base = floor(q);
    float2 f = q - base;
    float3 sum = 0.0;
    float total = 0.0;
    for (int y = 0; y <= 1; ++y)
        for (int x = 0; x <= 1; ++x)
        {
            float2 at = (base + float2(x, y) + 0.5) * quarterC.xy;
            float d = tex2Dlod(giMeta, float4(at, 0, 0)).y;
            if (d < 0.0) continue;
            float gap = abs(d - dist) / dist;
            if (gap > 0.05) continue;
            float w = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y) * (1.0 - gap * 15.0);
            sum += tex2Dlod(giTex, float4(at, 0, 0)).rgb * w;
            total += w;
        }
    return total > 0.0001 ? sum / total : 0.0;
}

// Luminance above the knee is compressed towards 1 along a soft shoulder, the channels scaled
// together. The knee is never below the scene's own luminance: only what the lights add is
// compressed.
float3 Compress(float3 c, float knee)
{
    float l = Luma(c);
    if (l <= knee) return c;
    float over = l - knee, room = 1.0 - knee;
    float mapped = knee + over * room / (room + over);
    return c * (mapped / l);
}

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 uv = (vpos + 0.5) * screen.xy;
    float4 light = tex2Dlod(lightBuffer, float4(uv, 0, 0));
    float4 here = SurfaceAt(uv);
    float3 gi = 0.0;
    float unit = here.w > 0.0 ? OnUnitWide(here.xyz, toneC.z) : 0.0;
    if (giC.x > 0.0 && here.w > 0.0)
        gi = Indirect(uv, here.w) * giC.x * (unit > 0.5 ? unitsGiC.x : 1.0);
    if (screen.z < 4.5) clip(light.r + light.g + light.b + light.a + gi.r + gi.g + gi.b - 0.00001);

    float3 scene = Linear(tex2Dlod(sceneTex, float4(uv, 0, 0)).rgb);
    // What the engine's own material already lit with last frame's buffer keeps its diffuse; units
    // are cleared from that buffer (resolve.ps), so they are lit here.
    // They added their share of it: engineC.y (lower only in very fast motion) times the buffer's
    // edge fade where this point sat in last frame's view (EngineEdge; resolve wrote it so). Apply
    // adds the rest, so no boundary shows where their reprojection runs out.
    bool engineLit = engineC.x > 0.5 && unit < 0.5 && tex2Dlod(gbufferNormals, float4(uv, 0, 0)).a > 0.75;
    float share = 0.0;
    if (engineLit)
    {
        float4 h = float4(here.xyz + moved.xyz, 1.0);
        float4 pc = float4(dot(h, prev0), dot(h, prev1), dot(h, prev2), dot(h, prev3));
        float2 puv = pc.w > 0.0001 ? float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5) : -1.0;
        share = engineC.y * EngineEdge(puv);
    }
    float3 direct = light.rgb * (1.0 - share);
    float3 albedo = Albedo(scene, Luma(light.rgb) * share, LocalLuma(sceneTex, uv, Luma(scene)));
    float3 specular = light.a * light.rgb / max(Luma(light.rgb), 0.0001);

    if (cookieD.z > 0.5) return float4(light.rgb, 1.0);   // the light service's Cookie factor view
    // What the engine's materials added: they drew only that (their params' z = 0), black where they did not.
    if (screen.z > 8.5) return float4(engineLit ? tex2Dlod(sceneTex, float4(uv, 0, 0)).rgb : 0.0, 1.0);
    if (screen.z > 7.5) return float4(Encode(albedo), 1.0);
    if (screen.z > 6.5) return float4(Encode(tex2Dlod(engineBuffer, float4(uv, 0, 0)).rgb * lightsC.z), 1.0);
    if (screen.z > 5.5) return float4(tex2Dlod(lightBuffer, float4(uv, 0, 0)).rgb, 1.0);
    if (screen.z > 4.5) return float4(tex2Dlod(omniAtlas, float4(uv, 0, 0)).rrr, 1.0);
    if (screen.z > 3.5) return float4(Encode(specular * lightsC.z), 1.0);
    if (screen.z > 2.5) return float4(Encode(gi * lightsC.z), 1.0);
    if (screen.z > 1.5) return float4(light.rgb, 1.0);
    if (screen.z > 0.5) return float4(Encode((light.rgb + gi) * lightsC.z), 1.0);

    float3 lit = scene + (albedo * (direct + gi) + specular) * lightsC.z;
    return float4(Encode(Compress(lit, max(albedoC.z, Luma(scene)))), 1.0);
}
