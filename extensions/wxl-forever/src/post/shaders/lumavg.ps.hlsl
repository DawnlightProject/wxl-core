// wxl-forever post: the image's average luminance, one texel.
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

// The 16 x 9 log luminances averaged and brought back out of the log: the geometric mean. With
// meterC.x the centre weighs most (a Gaussian about a third of the view wide over a fifth for
// everything), so a bright sky or lamp at the edge barely moves the eye.
sampler2D source : register(s0);

float4 main(float2 vpos : VPOS) : COLOR0
{
    float sum = 0.0, total = 0.0;
    for (int y = 0; y < 9; ++y)
        for (int x = 0; x < 16; ++x)
        {
            float2 o = (float2(x, y) + 0.5) / float2(16.0, 9.0) - 0.5;
            float w = meterC.x > 0.5 ? 0.2 + exp(-dot(o, o) / 0.045) : 1.0;
            sum += tex2Dlod(source, float4((float2(x, y) + 0.5) * texel.xy, 0, 0)).r * w;
            total += w;
        }
    return float4(exp(sum / total), 0.0, 0.0, 1.0);
}
