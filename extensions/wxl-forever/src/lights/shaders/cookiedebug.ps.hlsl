// wxl-forever lights: the cookie atlas drawn in a corner (Cookies.cpp, debug view 1).
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

sampler2D atlas : register(s0);
float4 rect : register(c0);   // viewport origin x, y and size in pixels

float4 main(float2 vpos : VPOS) : COLOR
{
    float2 uv = (vpos + 0.5 - rect.xy) / rect.zw;
    return float4(tex2D(atlas, uv).rgb, 1.0);
}
