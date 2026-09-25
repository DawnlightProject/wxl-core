// wxl-graphics-extend: screen, matrix and scalar helpers every pass shares.
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

#ifndef WXL_GFX_COMMON_HLSLI
#define WXL_GFX_COMMON_HLSLI

// Shader model 3 (ps_3_0 / vs_3_0). The library declares no register of its own: samplers and
// constants come in as parameters, so the program that includes it owns every s# and c#.

// D3D9 puts pixel centres on integer coordinates: the top-left pixel's VPOS is (0, 0) and its
// texel centre lies half a texel in, so a full-screen pass samples its own pixel at
// (vpos + 0.5) * (1 / width, 1 / height) exactly.
float2 WxlScreenUv(float2 vpos, float2 invSize)
{
    return (vpos + 0.5) * invSize;
}

// Texture space (0..1, y down) <-> normalised device coordinates (-1..1, y up).
float2 WxlUvToNdc(float2 uv)
{
    return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

float2 WxlNdcToUv(float2 ndc)
{
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

// v * M for a row-major, row-vector matrix (the engine's convention, v' = v * M) handed over as its
// four columns (wxl::gfx::matrix::Columns): one dp4 per result component, the form every constant
// table in the codebase uses. c0 is M's first column, so the result's x is dot(v, c0).
float4 WxlDp4Rows(float4 v, float4 c0, float4 c1, float4 c2, float4 c3)
{
    return float4(dot(v, c0), dot(v, c1), dot(v, c2), dot(v, c3));
}

// Rec. 709 luminance of a linear colour.
float WxlLuminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// 1 / x that stays finite: the magnitude is floored at 1e-6, the sign kept, so a zero divides as
// a tiny positive value instead of producing an infinity that spreads through a blend.
float WxlSafeRcp(float x)
{
    float s = x < 0.0 ? -1.0 : 1.0;
    return s / max(abs(x), 1e-6);
}

float WxlSquare(float x)
{
    return x * x;
}

float3 WxlSquare(float3 x)
{
    return x * x;
}

#endif
