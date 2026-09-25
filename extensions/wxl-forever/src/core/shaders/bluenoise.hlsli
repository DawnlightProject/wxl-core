// wxl-forever core: reading the suite's blue-noise tile (core/BlueNoise.cpp).
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

#ifndef WXL_FOREVER_BLUENOISE_HLSLI
#define WXL_FOREVER_BLUENOISE_HLSLI

// Four independent channels of the 128 x 128 void-and-cluster tile at an integer pixel, wrapping.
// The caller binds the tile to a point-sampled, wrapping sampler of its choice.
float4 BlueNoiseAt(sampler2D tile, float2 pixel)
{
    return tex2Dlod(tile, float4((pixel + 0.5) / 128.0, 0, 0));
}

#endif
