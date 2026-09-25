// wxl-forever post: the log luminance reduced four by four.
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

sampler2D source : register(s0);

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 base = vpos * 4.0;
    float sum = 0.0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            sum += tex2Dlod(source, float4((base + float2(x, y) + 0.5) * texel.xy, 0, 0)).r;
    return float4(sum / 16.0, 0.0, 0.0, 1.0);
}
