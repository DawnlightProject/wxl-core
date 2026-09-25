// wxl-graphics-extend: per-pixel noise -- the shared blue-noise tile, interleaved gradient noise and
// the R2 sequence that moves them frame to frame.
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

#ifndef WXL_GFX_NOISE_HLSLI
#define WXL_GFX_NOISE_HLSLI

// The n-th point of Martin Roberts' R2 sequence: successive points spread as evenly over the unit
// square as any sequence can, so whatever is offset by it converges quickly over frames. Keep n
// small (a frame counter reduced with fmod): past some hundred thousand, float precision leaves
// the fraction too coarse to be a sequence at all.
float2 WxlR2(float n)
{
    return frac(0.5 + n * float2(0.7548776662, 0.5698402910));
}

// The service's blue-noise tile (WXL_GFX_TEX_BLUE_NOISE: 128 x 128 A8R8G8B8, four independent
// void-and-cluster channels, every value 0..255 equally often) at an integer pixel, wrapping.
// Bind it point-sampled with D3DTADDRESS_WRAP on both axes; nothing here works through a filter.
float4 WxlBlueNoiseAt(sampler2D tile, float2 pixel)
{
    return tex2Dlod(tile, float4((pixel + 0.5) / 128.0, 0, 0));
}

// The tile at a pixel, shifted by the R2 point of the frame: the pattern stays blue over the screen
// on every frame, and a pixel's values over frames are a fresh sample each time (the shift walks
// whole texels for the point sampler), so a temporal filter averages the error away. The frame
// index cycles every 1024 frames, which keeps the sequence exact in float for any session length.
float4 WxlBlueNoise(sampler2D tile, float2 vpos, float frameIndex)
{
    return WxlBlueNoiseAt(tile, vpos + floor(WxlR2(fmod(frameIndex, 1024.0)) * 128.0));
}

// Jorge Jimenez's interleaved gradient noise: a 0..1 value per pixel with no texture at all, its
// pattern a fine slope that dithers well; the frame shifts it along its own gradient (the constant
// of his temporal variant, over a cycle of 64 frames).
float WxlInterleavedGradientNoise(float2 vpos, float frameIndex)
{
    float2 p = vpos + 5.588238 * fmod(frameIndex, 64.0);
    return frac(52.9829189 * frac(0.06711056 * p.x + 0.00583715 * p.y));
}

#endif
