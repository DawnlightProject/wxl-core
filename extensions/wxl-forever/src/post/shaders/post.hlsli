// wxl-forever post: the registers the post passes share.
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

#ifndef WXL_FOREVER_POST_HLSLI
#define WXL_FOREVER_POST_HLSLI

// Registers, set by post/Post.cpp:
//   c0  1 / source width, 1 / source height, 1 / target width, 1 / target height
//   c1  bloom threshold (linear luminance), knee, share of the light buffer, 1 = light buffer bound
//   c2  bloom tint rgb, intensity
//   c3  0, 0, view (0 image, 1 bloom only, 2 luminance), 0
//   c4  exposure floor, exposure ceiling, mid-grey key, filmic amount (0 off)
//   c5  1 = adapted luminance bound, pre-tonemap scale, inverse tonemap strength, 0
//   c6  adaptation share this frame going brighter, going darker, 0, 0
//   c7  weight of the pyramid level the up pass adds, 0, 0, 0
//   c8  1 = the bright pass averages its source pixels by brightness, 0, 0, 0
//   c9  curve (0 ACES, 1 gentle), gentle white point, desaturation after the curve, 1 - adaptation
//       strength (all zero: the look before these existed)
//   c10 1 = centre-weighted metering, 0, 0, 0
float4 texel : register(c0);
float4 brightC : register(c1);
float4 bloomC : register(c2);
float4 viewC : register(c3);
float4 exposureC : register(c4);
float4 toneC : register(c5);
float4 adaptC : register(c6);
float4 upC : register(c7);
float4 stableC : register(c8);
float4 curveC : register(c9);
float4 meterC : register(c10);

float3 Linear(float3 c) { return pow(max(c, 0.0), 2.2); }
float3 Encode(float3 c) { return pow(max(c, 0.0), 1.0 / 2.2); }
float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

// The LDR scene back to an estimate of the light it stood for: gentle, so darks and mids stay as
// they are and only the brights open up. toneC.z is 0 once the world draws into an HDR target.
float3 Unclip(float3 c)
{
    return c / max(1.0 - c * toneC.z, 0.2);
}

// ACES filmic fit (Narkowicz) on one value.
float Aces(float x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

// Gentle curve (extended Reinhard): no toe, so the darks the game already shaped stay as they are,
// and a shoulder reaching 1 at the white point.
float Gentle(float x)
{
    float w = max(curveC.y, 1.0);
    return x * (1.0 + x / (w * w)) / (1.0 + x);
}

// The curve applied to the luminance and the colour scaled with it, so a warm light keeps its hue
// instead of each channel bending on its own (which turns skin under a lamp orange). Only where a
// channel would still pass 1 does the colour move towards white, as a bright light does; then the
// saturation.
float3 Filmic(float3 x)
{
    float l = Luma(x);
    float3 c = x * ((curveC.x > 0.5 ? Gentle(l) : Aces(l)) / max(l, 0.0001));
    float peak = max(c.r, max(c.g, c.b));
    c = peak > 1.0 ? lerp(c / peak, 1.0, saturate((peak - 1.0) * 0.5)) : c;
    return max(lerp(c, Luma(c).xxx, curveC.z), 0.0);
}

#endif
