// wxl-graphics-lights: the composite (ps_3_0, D3D9, WXL_GFX_ORDER_LIGHTING, before the fog). Per pixel:
// the engine's colour times the sun and moon factor (in its own gamma space), decoded to linear and
// expanded to scene-referred light by the inverse of the resolve curve, plus the lamps' light from the
// surface pass. On the FP16 scene the sum is re-encoded as is (the resolve maps it); on an 8-bit target
// it goes through the curve here, with dither. docs/design.md, sections 2 and 6.
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

#include "../wxl/lights/hdr.hlsli"

sampler2D sceneTex : register(s0);   // the scene as the world pass left it (a copy), point
sampler2D lightTex : register(s1);   // the surface pass: rgb added light (linear), a sun factor; point
sampler2D blueTex  : register(s2);   // 128 x 128 blue noise, wrapping

float4 screen : register(c0);        // w, h, 1 / w, 1 / h
float4 curve  : register(c1);        // knee, gamma, 1 on the FP16 scene (0 an 8-bit target), exposure
float4 look   : register(c2);        // noise offset x, y, dither on, debug view (0 none)
float4 inputs : register(c3);        // 1 when lightTex holds this frame's light, 0, 0, 0

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 uv = (vpos + 0.5) * screen.zw;
    float3 scene = tex2Dlod(sceneTex, float4(uv, 0, 0)).rgb;
    float4 light = inputs.x > 0.5 ? tex2Dlod(lightTex, float4(uv, 0, 0)) : float4(0.0, 0.0, 0.0, 1.0);

    // A debug view: the surface pass wrote its colour in linear light.
    if (look.w > 0.5) return float4(WxlHdrEncode(light.rgb, curve.y), 1.0);

    float3 x = WxlHdrCurveInverse(WxlHdrDecode(scene * saturate(light.a), curve.y), curve.x) + max(light.rgb, 0.0);
    if (curve.z > 0.5) return float4(WxlHdrEncode(x, curve.y), 1.0);

    float3 encoded = WxlHdrEncode(WxlHdrCurve(x * curve.w, curve.x), curve.y);
    if (look.z > 0.5)
    {
        float u = tex2Dlod(blueTex, float4((vpos + look.xy + 0.5) / 128.0, 0, 0)).r;
        float t = u * 2.0 - 1.0;
        encoded += sign(t) * (1.0 - sqrt(max(1.0 - abs(t), 0.0))) / 255.0;
    }
    return float4(encoded, 1.0);
}
