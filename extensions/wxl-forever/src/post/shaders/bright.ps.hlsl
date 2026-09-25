// wxl-forever post: the bright pass, at half resolution.
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

#include "post/shaders/post.hlsli"

// What glows: the scene's own bright pixels (lantern glass, fire, lit windows) above a threshold
// with a soft knee, so light fades in rather than switching on, plus a share of the light buffer's
// peaks where surface lighting ran, so a lamp's brightest pool glows too. Linear, FP16. With
// stableC.x the four source pixels behind a sample are averaged with a weight of 1 / (1 + luma), so
// one hot pixel (a specular dot on a rail) cannot make the glow flicker as it moves.
sampler2D sceneTex : register(s0);
sampler2D lightBuffer : register(s1);

float3 Threshold(float3 c)
{
    float luma = Luma(c);
    float knee = max(brightC.y, 0.0001);
    float soft = clamp(luma - brightC.x + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    return c * max(soft, luma - brightC.x) / max(luma, 0.0001);
}

// How much a colour looks like lit skin (warm, moderately saturated): such pixels glow less, so a
// face under a lamp stays a face.
float Skin(float3 c)
{
    float warm = step(c.b, c.g) * step(c.g, c.r);
    float saturation = (c.r - c.b) / max(c.r, 0.0001);
    return warm * smoothstep(0.1, 0.25, saturation) * (1.0 - smoothstep(0.55, 0.7, saturation));
}

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 uv = (vpos + 0.5) * texel.zw;
    float3 scene;
    if (stableC.x > 0.5)
    {
        float3 sum = 0.0;
        float total = 0.0;
        for (int i = 0; i < 4; ++i)
        {
            float2 o = (float2(fmod(i, 2.0), floor(i * 0.5)) - 0.5) * texel.xy;
            float3 c = Linear(tex2Dlod(sceneTex, float4(uv + o, 0, 0)).rgb);
            float w = 1.0 / (1.0 + Luma(c));
            sum += c * w;
            total += w;
        }
        scene = sum / total;
    }
    // Otherwise the four source pixels at once through the bilinear filter.
    else scene = Linear(tex2Dlod(sceneTex, float4(uv, 0, 0)).rgb);
    float3 bright = Threshold(scene) * (1.0 - 0.7 * Skin(scene));
    if (brightC.w > 0.5)
    {
        float3 light = tex2Dlod(lightBuffer, float4(uv, 0, 0)).rgb;
        bright += Threshold(light * scene / max(Luma(scene), 0.05) * 0.5) * brightC.z;
    }
    return float4(bright, 1.0);
}
