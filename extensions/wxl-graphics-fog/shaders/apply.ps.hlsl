// wxl-graphics-fog: the composite (ps_3_0, D3D9 pass at WXL_GFX_ORDER_ATMOSPHERE). Every pixel takes
// the half-resolution fog from its four nearest texels, each offering two layers (the nearest and the
// farthest scene distance of its footprint): the layer whose distance matches this pixel's, weighted
// bilinearly and by how well it matches. So the sky between thin branches gets the sky's fog, and the
// branch its own. It lays the fog over the scene in linear light: scene * T + S, the in-scattered
// light given a soft shoulder. Inside dense fog the scene
// softens a little and the fog's own colour closes in from the edges. Debug views overlay the result.
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

sampler2D depthTex : register(s0);   // INTZ, point
sampler2D fogTex   : register(s1);   // half resolution, back layer: light rgb, transmittance
sampler2D auxTex   : register(s2);   // half resolution: rep. distance back, front; scene distance near, far
sampler2D sceneTex : register(s3);   // the scene as the world pass left it
sampler2D debugTex : register(s4);   // half resolution debug image
sampler2D blueTex  : register(s5);   // 128 x 128 blue noise, wrapping
sampler2D frontTex : register(s6);   // half resolution, front layer

float4 full        : register(c0);   // w, h, 1 / w, 1 / h
float4 halfRes     : register(c1);
float4 depthRange  : register(c2);   // min, 1 / (max - min), max, far distance
float4 invVP[4]    : register(c3);   // columns of clip -> (r, 1)
float4 look        : register(c7);   // debug view, immersion blur, immersion veil, soft white
float4 noise       : register(c8);   // blue-noise offset x, y, dither on, immersion (0..1)

float3 Decode(float3 c) { return pow(max(c, 0.0), 2.2); }
float3 Encode(float3 c) { return pow(max(c, 0.0), 1.0 / 2.2); }

float SceneDistance(float2 uv, float d)
{
    if (d > depthRange.z + 0.00001) return depthRange.w;
    float ndc = saturate((d - depthRange.x) * depthRange.y);
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, ndc, 1.0);
    float4 h = float4(dot(clip, invVP[0]), dot(clip, invVP[1]), dot(clip, invVP[2]), dot(clip, invVP[3]));
    return min(length(h.xyz / h.w), depthRange.w);
}

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 uv = (vpos + 0.5) * full.zw;
    float dist = SceneDistance(uv, tex2Dlod(depthTex, float4(uv, 0, 0)).r);

    // Joint-bilateral upsample from the half-resolution fog.
    float2 hp = uv * halfRes.xy - 0.5;
    float2 base = floor(hp);
    float2 f = hp - base;
    float4 fog = 0.0;
    float wsum = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        float2 o = float2(i == 1 || i == 3 ? 1.0 : 0.0, i >= 2 ? 1.0 : 0.0);
        float2 tuv = (base + o + 0.5) * halfRes.zw;
        float4 a = tex2Dlod(auxTex, float4(tuv, 0, 0));
        float bil = (o.x > 0.5 ? f.x : 1.0 - f.x) * (o.y > 0.5 ? f.y : 1.0 - f.y);
        float missFront = abs(a.z - dist) / max(dist, 1.0);
        float missBack = abs(a.w - dist) / max(dist, 1.0);
        float useFront = missFront < missBack ? 1.0 : 0.0;
        float w = max(bil, 0.001) / (0.01 + min(missFront, missBack));
        float4 layer = lerp(tex2Dlod(fogTex, float4(tuv, 0, 0)), tex2Dlod(frontTex, float4(tuv, 0, 0)), useFront);
        fog += layer * w;
        wsum += w;
    }
    fog /= max(wsum, 1e-6);
    float3 S = fog.rgb / (1.0 + fog.rgb / max(look.w, 0.5));
    float T = saturate(fog.a);

    float3 scene = Decode(tex2Dlod(sceneTex, float4(uv, 0, 0)).rgb);
    float immersion = noise.w;
    if (immersion > 0.001 && look.y > 0.0)
    {
        float r = immersion * look.y * 4.0;
        float3 blur = 0.0;
        blur += Decode(tex2Dlod(sceneTex, float4(uv + float2(r, r * 0.5) * full.zw, 0, 0)).rgb);
        blur += Decode(tex2Dlod(sceneTex, float4(uv + float2(-r * 0.5, r) * full.zw, 0, 0)).rgb);
        blur += Decode(tex2Dlod(sceneTex, float4(uv + float2(-r, -r * 0.5) * full.zw, 0, 0)).rgb);
        blur += Decode(tex2Dlod(sceneTex, float4(uv + float2(r * 0.5, -r) * full.zw, 0, 0)).rgb);
        scene = lerp(scene, blur * 0.25, saturate(immersion * look.y));
    }
    float3 result = scene * T + S;
    if (immersion > 0.001 && look.z > 0.0)
    {
        float3 own = fog.rgb / max(1.0 - fog.a, 0.1);
        float2 c = uv * 2.0 - 1.0;
        float edge = saturate(dot(c, c) * 0.6);
        result = lerp(result, own, saturate(immersion * look.z * (0.25 + 0.75 * edge)));
    }

    int view = (int)(look.x + 0.5);
    if (view == 1) result = S;
    else if (view == 2) result = T.xxx;
    else if (view >= 3)
    {
        float4 d = tex2Dlod(debugTex, float4(uv, 0, 0));
        result = lerp(result, d.rgb, saturate(d.a));
    }

    float3 encoded = Encode(result);
    if (noise.z > 0.5)
    {
        float u = tex2Dlod(blueTex, float4((vpos + noise.xy + 0.5) / 128.0, 0, 0)).r;
        float t = u * 2.0 - 1.0;
        encoded += sign(t) * (1.0 - sqrt(max(1.0 - abs(t), 0.0))) / 255.0;
    }
    return float4(encoded, 1.0);
}
