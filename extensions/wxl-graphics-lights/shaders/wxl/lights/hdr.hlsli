// wxl-graphics-lights: the HDR scene contract in HLSL (GraphicsLightsResolveApi.h) -- the resolve curve,
// its inverse and the encoding of the FP16 scene between the lights' composite and the resolve.
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

#ifndef WXL_LIGHTS_HDR_HLSLI
#define WXL_LIGHTS_HDR_HLSLI

// Per channel, linear light: identity up to the knee k, then an exponential shoulder towards 1 with a
// slope of 1 at the knee. No toe: the darks stay as the engine drew them. SM3 and SM6 alike.

static const float kWxlHdrTop = 1.0 - 1.0 / 512.0;   // the highest display value the inverse maps

float3 WxlHdrCurve(float3 x, float k)
{
    float3 over = max(x - k, 0.0);
    float room = max(1.0 - k, 1e-4);
    float3 shoulder = k + room * (1.0 - exp(-over / room));
    return lerp(shoulder, max(x, 0.0), step(x, k));
}

// The inverse: a display value back to scene-referred light. Past the top it grows four times the
// excess, so an additive glow the engine stacked past 1 keeps some of its energy.
float3 WxlHdrCurveInverse(float3 y, float k)
{
    float room = max(1.0 - k, 1e-4);
    float3 c = clamp(y, 0.0, kWxlHdrTop);
    float3 expanded = k - room * log(max(1.0 - (c - k) / room, 1e-6));
    float3 x = lerp(expanded, c, step(c, k));
    return x + max(y - kWxlHdrTop, 0.0) * 4.0;
}

// The FP16 scene holds pow(x, 1 / gamma) of the linear scene-referred x.
float3 WxlHdrDecode(float3 c, float gamma) { return pow(max(c, 0.0), gamma); }
float3 WxlHdrEncode(float3 x, float gamma) { return pow(max(x, 0.0), 1.0 / gamma); }

#endif
