// wxl-forever post: the eye's adaptation, one texel eased over time.
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

// The luminance the eye is adapted to moves towards the image's, in the log, at one speed going
// brighter and another going darker: stepping into a dark building, the eye opens slowly.
sampler2D measured : register(s0);
sampler2D previous : register(s1);

float4 main(float2 vpos : VPOS) : COLOR0
{
    float now = max(tex2Dlod(measured, float4(0.5, 0.5, 0, 0)).r, 0.0001);
    float before = max(tex2Dlod(previous, float4(0.5, 0.5, 0, 0)).r, 0.0001);
    float share = now > before ? adaptC.x : adaptC.y;
    return float4(exp(lerp(log(before), log(now), share)), 0.0, 0.0, 1.0);
}
