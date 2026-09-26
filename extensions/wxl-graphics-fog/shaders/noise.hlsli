// wxl-graphics-fog: tileable gradient (Perlin) and cellular (Worley) noise, used once to bake the noise
// textures, and the curl of a vector potential.
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

#ifndef FOG_NOISE_HLSLI
#define FOG_NOISE_HLSLI

#include "common.hlsli"

uint3 WrapCell(int3 c, int period) { return uint3((c % period + period) % period); }

float3 Gradient(uint3 cell, uint seed)
{
    float3 h = HashFloat3(cell + uint3(seed * 131u, seed * 17u, seed * 7u));
    float3 g = h * 2.0 - 1.0;
    return g * rsqrt(max(dot(g, g), 1e-4));
}

float3 Fade(float3 t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

// Gradient noise with `period` cells over p's unit cube, roughly -1..1.
float Perlin(float3 p, int period, uint seed)
{
    int3 i = int3(floor(p));
    float3 f = p - float3(i);
    float3 u = Fade(f);
    float n000 = dot(Gradient(WrapCell(i + int3(0, 0, 0), period), seed), f - float3(0, 0, 0));
    float n100 = dot(Gradient(WrapCell(i + int3(1, 0, 0), period), seed), f - float3(1, 0, 0));
    float n010 = dot(Gradient(WrapCell(i + int3(0, 1, 0), period), seed), f - float3(0, 1, 0));
    float n110 = dot(Gradient(WrapCell(i + int3(1, 1, 0), period), seed), f - float3(1, 1, 0));
    float n001 = dot(Gradient(WrapCell(i + int3(0, 0, 1), period), seed), f - float3(0, 0, 1));
    float n101 = dot(Gradient(WrapCell(i + int3(1, 0, 1), period), seed), f - float3(1, 0, 1));
    float n011 = dot(Gradient(WrapCell(i + int3(0, 1, 1), period), seed), f - float3(0, 1, 1));
    float n111 = dot(Gradient(WrapCell(i + int3(1, 1, 1), period), seed), f - float3(1, 1, 1));
    float x00 = lerp(n000, n100, u.x), x10 = lerp(n010, n110, u.x);
    float x01 = lerp(n001, n101, u.x), x11 = lerp(n011, n111, u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z) * 1.4;
}

// Perlin fBm over `octaves`, each doubling the frequency; the tile stays seamless. 0..1.
float PerlinFbm(float3 uvw, int freq, int octaves, uint seed)
{
    float sum = 0.0, amp = 0.5, norm = 0.0;
    for (int o = 0; o < octaves; ++o)
    {
        sum += Perlin(uvw * float(freq), freq, seed + uint(o)) * amp;
        norm += amp;
        amp *= 0.5;
        freq *= 2;
    }
    return saturate(sum / norm * 0.5 + 0.5);
}

// Distance to the nearest feature point, one point per cell, `period` cells over the unit cube:
// 0 at a point, about 1 at the farthest.
float Worley(float3 p, int period, uint seed)
{
    int3 i = int3(floor(p));
    float3 f = p - float3(i);
    float best = 8.0;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                int3 o = int3(x, y, z);
                float3 feature = HashFloat3(WrapCell(i + o, period) + uint3(seed * 977u, seed * 211u, seed * 53u));
                float3 d = float3(o) + feature - f;
                best = min(best, dot(d, d));
            }
    return sqrt(best);
}

// Inverted Worley fBm: bright, round billows. 0..1.
float WorleyFbm(float3 uvw, int freq, uint seed)
{
    float a = 1.0 - Worley(uvw * float(freq), freq, seed);
    float b = 1.0 - Worley(uvw * float(freq * 2), freq * 2, seed + 1u);
    float c = 1.0 - Worley(uvw * float(freq * 4), freq * 4, seed + 2u);
    return saturate(a * 0.625 + b * 0.25 + c * 0.125);
}

#endif
