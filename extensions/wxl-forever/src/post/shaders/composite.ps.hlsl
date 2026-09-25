// wxl-forever post: bloom, exposure and the filmic curve laid over the image.
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

// image + bloom x tint x intensity, in linear light; then the eye's exposure, clamped, and the
// filmic curve.
sampler2D sceneTex : register(s0);
sampler2D bloomTex : register(s1);
sampler2D adaptedTex : register(s3);

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 uv = (vpos + 0.5) * texel.zw;
    float3 bloom = tex2Dlod(bloomTex, float4(uv, 0, 0)).rgb * bloomC.rgb * bloomC.w;
    if (viewC.z > 0.5 && viewC.z < 1.5) return float4(Encode(bloom), 1.0);

    float3 scene = Linear(tex2Dlod(sceneTex, float4(uv, 0, 0)).rgb);
    if (exposureC.w <= 0.0) return float4(Encode(scene + bloom), 1.0);

    // The eye: exposure to bring the adapted luminance towards the key, by the adaptation strength
    // (in the log), within its bounds.
    float exposure = 1.0;
    if (toneC.x > 0.5)
    {
        float adapted = tex2Dlod(adaptedTex, float4(0.5, 0.5, 0, 0)).r;
        exposure = clamp(pow(max(exposureC.z / max(adapted, 0.0001), 0.0001), 1.0 - curveC.w), exposureC.x, exposureC.y);
    }
    float3 light = (Unclip(scene) + bloom) * exposure;
    if (viewC.z > 1.5) return float4(Encode(saturate(Luma(light).xxx * 0.5)), 1.0);
    float3 toned = Filmic(light * toneC.y);
    return float4(Encode(lerp(saturate(scene + bloom), toned, exposureC.w)), 1.0);
}
