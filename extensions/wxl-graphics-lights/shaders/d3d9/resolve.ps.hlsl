// wxl-graphics-lights: the interim resolve (ps_3_0, D3D9, WXL_GFX_ORDER_RESOLVE). The FP16 scene,
// scene-referred and gamma-encoded as GraphicsLightsResolveApi.h describes, through the curve with
// exposure, to the 8-bit back buffer with blue-noise dither. wxl-graphics-post takes this over by
// claiming the resolve.
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

sampler2D hdrTex  : register(s0);    // the FP16 scene, point
sampler2D blueTex : register(s1);    // 128 x 128 blue noise, wrapping

float4 screen : register(c0);        // w, h, 1 / w, 1 / h
float4 curve  : register(c1);        // knee, gamma, exposure, 1 to pass the values through (a debug view)
float4 look   : register(c2);        // noise offset x, y, dither on, 0

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 uv = (vpos + 0.5) * screen.zw;
    float3 c = tex2Dlod(hdrTex, float4(uv, 0, 0)).rgb;
    float3 encoded = curve.w > 0.5 ? saturate(c) : WxlHdrEncode(WxlHdrCurve(WxlHdrDecode(c, curve.y) * curve.z, curve.x), curve.y);
    if (look.z > 0.5)
    {
        float u = tex2Dlod(blueTex, float4((vpos + look.xy + 0.5) / 128.0, 0, 0)).g;
        float t = u * 2.0 - 1.0;
        encoded += sign(t) * (1.0 - sqrt(max(1.0 - abs(t), 0.0))) / 255.0;
    }
    return float4(encoded, 1.0);
}
