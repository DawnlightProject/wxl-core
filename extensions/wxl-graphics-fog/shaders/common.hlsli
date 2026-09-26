// wxl-graphics-fog: the constant buffer, the shared samplers, the push constants and small helpers
// every compute pass includes.
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

#ifndef FOG_COMMON_HLSLI
#define FOG_COMMON_HLSLI

#include "shared.h"

#define FOG_DECL1(name) float4 name;
#define FOG_DECLN(name, n) float4 name[n];
[[vk::binding(FOG_B_CONSTANTS, 0)]] cbuffer FogConstants
{
    FOG_CONSTANTS(FOG_DECL1, FOG_DECLN)
};
#undef FOG_DECL1
#undef FOG_DECLN

[[vk::binding(FOG_B_SAMP_POINT_CLAMP, 0)]]  SamplerState sPointClamp;
[[vk::binding(FOG_B_SAMP_LINEAR_CLAMP, 0)]] SamplerState sLinearClamp;
[[vk::binding(FOG_B_SAMP_LINEAR_WRAP, 0)]]  SamplerState sLinearWrap;
[[vk::binding(FOG_B_SAMP_POINT_WRAP, 0)]]   SamplerState sPointWrap;

struct FogPush
{
    uint4  a;    // per pass: level, mode, ...
    float4 b;    // per pass: dt, ...
};
[[vk::push_constant]] FogPush pc;

#define FOG_TEX(slot) [[vk::binding(FOG_B_TEX0 + (slot), 0)]]
#define FOG_OUT(slot) [[vk::binding(FOG_B_OUT0 + (slot), 0)]]

static const float kPi = 3.14159265;
static const float kInv4Pi = 0.0795774715;

bool Isolated(uint bit) { return (asuint(debug2.x) & bit) != 0u; }

float Remap(float v, float lo, float hi, float newLo, float newHi)
{
    return newLo + (v - lo) / max(hi - lo, 1e-5) * (newHi - newLo);
}

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Integer hashes (PCG-style): stable across drivers, no sin.
uint Hash1(uint x)
{
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

uint Hash3(uint3 v) { return Hash1(v.x ^ Hash1(v.y ^ Hash1(v.z))); }

float HashFloat(uint3 v) { return float(Hash3(v) & 0x00ffffffu) / 16777216.0; }

float3 HashFloat3(uint3 v)
{
    uint h = Hash3(v);
    return float3(float(h & 0x3ffu), float((h >> 10) & 0x3ffu), float((h >> 20) & 0x3ffu)) / 1024.0;
}

// v * M for a matrix handed over as its four columns (the dp4 form).
float4 Mul4(float4 v, float4 c0, float4 c1, float4 c2, float4 c3)
{
    return float4(dot(v, c0), dot(v, c1), dot(v, c2), dot(v, c3));
}

// Camera-relative point -> clip, this frame.
float4 ToClip(float3 r) { return Mul4(float4(r, 1.0), viewProj[0], viewProj[1], viewProj[2], viewProj[3]); }

// Screen uv (0..1, y down) and the D3D depth (0..1) -> camera-relative point.
float3 FromScreen(float2 uv, float ndcZ)
{
    float4 h = Mul4(float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, ndcZ, 1.0), invViewProj[0], invViewProj[1], invViewProj[2],
                    invViewProj[3]);
    return h.xyz / h.w;
}

// The unit view ray through a screen uv.
float3 RayDir(float2 uv) { return normalize(FromScreen(uv, 1.0)); }

// The INTZ value -> the projection's 0..1 depth (the world is drawn into depthRange.x..z).
float DepthToNdc(float d) { return saturate((d - depthRange.x) * depthRange.y); }

bool DepthIsSky(float d) { return d > depthRange.z + 0.00001; }

// Distance along the ray from the camera to the surface at uv with INTZ value d; sky gives far.
float SceneDistance(float2 uv, float d, float far)
{
    if (DepthIsSky(d)) return far;
    float3 r = FromScreen(uv, DepthToNdc(d));
    return min(length(r), far);
}

// Last frame's uv of a camera-relative point; w <= 0 behind last frame's camera.
float3 ReprojectUv(float3 r)
{
    float4 c = Mul4(float4(r, 1.0), reproject[0], reproject[1], reproject[2], reproject[3]);
    float2 ndc = c.xy / max(abs(c.w), 1e-5) * (c.w < 0.0 ? -1.0 : 1.0);
    return float3(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, c.w);
}

// Henyey-Greenstein, normalised so that isotropic scattering is 1 (4 pi p).
float HG(float cosTheta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (denom * sqrt(max(denom, 1e-4)));
}

float PhaseDual(float cosTheta, float4 ph)
{
    return lerp(HG(cosTheta, ph.x), HG(cosTheta, ph.y), ph.z);
}

// Smootherstep, for eased blends with no visible start or stop.
float Smoother(float x)
{
    x = saturate(x);
    return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

#endif
