// wxl-graphics-shadow: the debug view drawn over the finished frame (ps_3_0, D3D9). s0 the view (an
// A16B16G16R16F copy of the debug image); alpha blends it over the frame.
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
#pragma once

sampler2D viewTex : register(s0);
float4 screen : register(c0);   // width, height, 1 / width, 1 / height

float4 main(float2 vpos : VPOS) : COLOR0
{
    float4 v = tex2Dlod(viewTex, float4((vpos + 0.5) * screen.zw, 0, 0));
    return float4(saturate(v.rgb), saturate(v.a));
}
