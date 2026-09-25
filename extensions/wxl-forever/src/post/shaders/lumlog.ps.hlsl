// wxl-forever post: the image's log luminance at 64 x 36.
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

// Each texel averages the log luminance of four bilinear reads spread over the part of the image it
// covers; the log keeps a single lamp from dragging the whole average up.
sampler2D sceneTex : register(s0);

float LogLuma(float2 uv)
{
    float3 c = Unclip(Linear(tex2Dlod(sceneTex, float4(uv, 0, 0)).rgb));
    return log(Luma(c) + 0.0001);
}

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 uv = (vpos + 0.5) * texel.zw;
    float2 q = texel.zw * 0.25;
    float sum = LogLuma(uv + float2(-q.x, -q.y)) + LogLuma(uv + float2(q.x, -q.y))
              + LogLuma(uv + float2(-q.x, q.y)) + LogLuma(uv + float2(q.x, q.y));
    return float4(sum * 0.25, 0.0, 0.0, 1.0);
}
