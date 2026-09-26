// wxl-graphics-lights: reading the lamp light field (GraphicsLightsFieldApi.h) from a Vulkan compute shader.
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

#ifndef WXL_LIGHTS_FIELD_HLSLI
#define WXL_LIGHTS_FIELD_HLSLI

// The consumer binds the three images (WXL_GfxLightsField) as Texture3D<float4> with a linear clamp
// sampler, and hands over, from the same struct:
//   fieldViewProj  the four columns of viewProjRel (the dp4 form: clip = dot(float4(r, 1), column))
//   fieldGrid      (width, height, depth, 0)
//   fieldRange     (nearDistance, 1 / log(farDistance / nearDistance), farDistance, 0)
// Positions are camera-relative to the field's own eye (WXL_GfxLightsField::eye); a consumer whose eye
// differs adds the difference first.
//
// In a march, per sample at r with scattering coefficient sigmaS (1/yd) and transmittance T to the eye:
//     radiance += T * sigmaS * WxlLightsFieldInscatter(...) * stepLength
// The field is already an average over each froxel's depth span, so the march may step coarsely.

// Texture coordinates of a camera-relative point: screen uv (y down) and exponential depth.
float3 WxlLightsFieldUvw(float3 r, float4 fieldViewProj[4], float4 fieldRange)
{
    float4 h = float4(r, 1.0);
    float4 c = float4(dot(h, fieldViewProj[0]), dot(h, fieldViewProj[1]), dot(h, fieldViewProj[2]), dot(h, fieldViewProj[3]));
    float iw = 1.0 / max(abs(c.w), 1e-5);
    float2 uv = float2(c.x * iw * 0.5 + 0.5, 0.5 - c.y * iw * 0.5);
    float z = log(max(length(r), fieldRange.x) / fieldRange.x) * fieldRange.y;
    return float3(uv, saturate(z));
}

// The lamps' light scattered towards the eye at r, per unit scattering coefficient, with the phase the
// field was computed with (SetPhase).
float3 WxlLightsFieldInscatter(Texture3D<float4> inscatter, SamplerState linearClamp, float3 r, float4 fieldViewProj[4],
                               float4 fieldRange)
{
    return inscatter.SampleLevel(linearClamp, WxlLightsFieldUvw(r, fieldViewProj, fieldRange), 0).rgb;
}

// The same for any phase: the two-lobe model of ambient and direction. phaseToward is the consumer's
// phase function value for the cosine between the mean propagation direction and the direction the light
// leaves towards (view, unit, from the point to the eye).
float3 WxlLightsFieldTwoLobe(Texture3D<float4> ambient, Texture3D<float4> direction, SamplerState linearClamp, float3 r,
                             float3 view, float4 fieldViewProj[4], float4 fieldRange, out float cosTheta)
{
    float3 uvw = WxlLightsFieldUvw(r, fieldViewProj, fieldRange);
    float4 a = ambient.SampleLevel(linearClamp, uvw, 0);
    float3 d = direction.SampleLevel(linearClamp, uvw, 0).xyz;
    cosTheta = dot(normalize(d + 1e-6), view);
    return a.rgb;   // times lerp(1, 4 pi phase(cosTheta), a.a) by the caller, with its own phase
}

// The directionality of the two-lobe model at r (ambient.a).
float WxlLightsFieldDirectionality(Texture3D<float4> ambient, SamplerState linearClamp, float3 r, float4 fieldViewProj[4],
                                   float4 fieldRange)
{
    return ambient.SampleLevel(linearClamp, WxlLightsFieldUvw(r, fieldViewProj, fieldRange), 0).a;
}

#endif
