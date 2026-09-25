// wxl-graphics-extend: transfer curves (sRGB, the engine's gamma 2), tonemaps and dithering.
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

#ifndef WXL_GFX_COLOR_HLSLI
#define WXL_GFX_COLOR_HLSLI

// --- sRGB, the exact piecewise curve (IEC 61966-2-1), on 0..1 ---------------------------------------

float3 WxlSrgbToLinear(float3 c)
{
    c = saturate(c);
    float3 low = c / 12.92;
    float3 high = pow((c + 0.055) / 1.055, 2.4);
    return lerp(high, low, step(c, 0.04045));
}

// The pow's base is floored because lerp carries a NaN from the branch it discards; the floor lies
// inside the linear segment, so it never reaches the result.
float3 WxlLinearToSrgb(float3 c)
{
    c = saturate(c);
    float3 low = c * 12.92;
    float3 high = 1.055 * pow(max(c, 1e-6), 1.0 / 2.4) - 0.055;
    return lerp(high, low, step(c, 0.0031308));
}

// --- the engine's gamma 2 ----------------------------------------------------------------------------
// The client writes display-encoded colour, and the rewritten materials treat it as gamma 2 when they
// add the light buffer: out = sqrt(colour^2 + added) (src/game/GBuffer.hpp). A pass that adds light
// to what the engine drew does the same -- square to decode, square root to encode back -- so its
// light and the materials' own agree; the exact sRGB curve above is for data authored in sRGB.

float WxlGamma2ToLinear(float c)
{
    return c * c;
}

float3 WxlGamma2ToLinear(float3 c)
{
    return c * c;
}

float WxlLinearToGamma2(float c)
{
    return sqrt(max(c, 0.0));
}

float3 WxlLinearToGamma2(float3 c)
{
    return sqrt(max(c, 0.0));
}

// --- tonemaps, linear light in, 0..1 out -----------------------------------------------------------------

// The ACES filmic fit by Krzysztof Narkowicz: a toe, a shoulder reaching white around 16.
float3 WxlTonemapAces(float3 x)
{
    x = max(x, 0.0);
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

// Reinhard: no toe, white only at infinity, so the darks the scene already shaped stay as they are.
float3 WxlTonemapReinhard(float3 x)
{
    x = max(x, 0.0);
    return x / (1.0 + x);
}

// Extended Reinhard: the same, but a value of white maps to 1 (white > 1; 1 is plain clipping).
float3 WxlTonemapReinhardWhite(float3 x, float white)
{
    x = max(x, 0.0);
    float w = max(white, 1.0);
    return saturate(x * (1.0 + x / (w * w)) / (1.0 + x));
}

// John Hable's Uncharted 2 curve. The curve alone (unnormalised) and the usual use: the exposed
// value over the curve at the white point (11.2 in the original), so white lands on 1.
float3 WxlUncharted2Curve(float3 x)
{
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float3 WxlTonemapUncharted2(float3 x, float exposureBias, float white)
{
    x = max(x, 0.0);
    float w = max(white, 0.01);
    float3 curved = WxlUncharted2Curve(x * exposureBias);
    float3 whiteScale = 1.0 / WxlUncharted2Curve(float3(w, w, w));
    return saturate(curved * whiteScale);
}

// --- dithering ------------------------------------------------------------------------------------------

// One uniform value in 0..1 folded into a triangular distribution over -1..1: unlike a flat offset
// it hides quantisation without a visible noise floor at the band edges.
float WxlTriangularNoise(float u)
{
    float t = u * 2.0 - 1.0;
    return sign(t) * (1.0 - sqrt(max(1.0 - abs(t), 0.0)));
}

// Adds one quantisation step of triangular noise before the output is rounded: apply to the
// encoded value (the space the target stores), with levels the target's steps per unit (255 for an
// 8-bit target) and u a per-pixel uniform in 0..1 (wxl/gfx/noise.hlsli).
float3 WxlDither(float3 encoded, float u, float levels)
{
    return encoded + WxlTriangularNoise(u) / levels;
}

#endif
