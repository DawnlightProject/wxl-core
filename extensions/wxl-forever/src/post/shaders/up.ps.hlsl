// wxl-forever post: one level up the bloom pyramid (dual filter), adding that level's own glow.
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

// Twice the size: a tent of eight bilinear reads of the smaller level, plus the same-size level of
// the way down by its weight (upC.x; the softness fades the fine levels), so every radius of glow
// adds up: a tight halo at the source and a wide soft one, in the balance the softness sets.
sampler2D smaller : register(s0);
sampler2D sameSize : register(s1);

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 uv = (vpos + 0.5) * texel.zw;
    float2 h = texel.xy;
    float3 sum = tex2Dlod(smaller, float4(uv + float2(-2.0 * h.x, 0.0), 0, 0)).rgb;
    sum += tex2Dlod(smaller, float4(uv + float2( 2.0 * h.x, 0.0), 0, 0)).rgb;
    sum += tex2Dlod(smaller, float4(uv + float2(0.0, -2.0 * h.y), 0, 0)).rgb;
    sum += tex2Dlod(smaller, float4(uv + float2(0.0,  2.0 * h.y), 0, 0)).rgb;
    sum += tex2Dlod(smaller, float4(uv + float2(-h.x, -h.y), 0, 0)).rgb * 2.0;
    sum += tex2Dlod(smaller, float4(uv + float2( h.x, -h.y), 0, 0)).rgb * 2.0;
    sum += tex2Dlod(smaller, float4(uv + float2(-h.x,  h.y), 0, 0)).rgb * 2.0;
    sum += tex2Dlod(smaller, float4(uv + float2( h.x,  h.y), 0, 0)).rgb * 2.0;
    return float4(sum / 12.0 + tex2Dlod(sameSize, float4(uv, 0, 0)).rgb * upC.x, 1.0);
}
