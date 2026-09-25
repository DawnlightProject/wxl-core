// wxl-forever post: one level down the bloom pyramid (dual filter).
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

// Half the size: the centre four times and the four diagonal corners once, each a bilinear read of
// four source texels, so nothing flickers as a bright pixel crosses texel boundaries.
sampler2D source : register(s0);

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 uv = (vpos + 0.5) * texel.zw;
    float2 h = texel.xy;
    float3 sum = tex2Dlod(source, float4(uv, 0, 0)).rgb * 4.0;
    sum += tex2Dlod(source, float4(uv + float2(-h.x, -h.y), 0, 0)).rgb;
    sum += tex2Dlod(source, float4(uv + float2( h.x, -h.y), 0, 0)).rgb;
    sum += tex2Dlod(source, float4(uv + float2(-h.x,  h.y), 0, 0)).rgb;
    sum += tex2Dlod(source, float4(uv + float2( h.x,  h.y), 0, 0)).rgb;
    return float4(sum / 8.0, 1.0);
}
