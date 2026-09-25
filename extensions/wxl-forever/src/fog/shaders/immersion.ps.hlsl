// wxl-forever fog, froxel pass: immersion.
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

#include "fog/shaders/common.hlsli"

// How deep the camera stands in fog or smoke, one texel eased over time: the resolved volume's
// first yards, read at two distances across a small cone ahead, as (fog radiance rgb, immersion
// 0..1). Nothing here is modelled apart from the volume; apply draws the veil and the softened
// lights from it, in the volume's own colour.
sampler2D injected : register(s1);
sampler2D previous : register(s2);

float4 Cone(float slice)
{
    float4 sum = SampleVolume(injected, float2(0.5, 0.5), slice);
    sum += SampleVolume(injected, float2(0.35, 0.5), slice);
    sum += SampleVolume(injected, float2(0.65, 0.5), slice);
    sum += SampleVolume(injected, float2(0.5, 0.35), slice);
    sum += SampleVolume(injected, float2(0.5, 0.65), slice);
    return sum * 0.2;
}

float4 main(float2 vpos : VPOS) : COLOR0
{
    float4 sum = (Cone(DistToSlice(1.0)) + Cone(DistToSlice(2.5))) * 0.5;
    float sigma = sum.a;
    float4 now = float4(sum.rgb / max(sigma, 0.000001), saturate(sigma / max(immersionC.y, 0.0001)));
    float4 before = tex2Dlod(previous, float4(0.5, 0.5, 0, 0));
    return lerp(before, now, immersionC.x);
}
