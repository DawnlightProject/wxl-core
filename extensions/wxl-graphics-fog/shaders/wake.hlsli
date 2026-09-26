// wxl-graphics-fog: reading the wake fluid, a 2D flow at the ground around the player in two levels
// (0.25 and 1 yd cells, 256 across, toroidal like the clipmaps). Each texel holds the air's velocity,
// the change it made to the fog (0 untouched, -1 cleared, above 0 thickened) and the height the
// change reaches over the ground.
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

#ifndef FOG_WAKE_HLSLI
#define FOG_WAKE_HLSLI

#include "common.hlsli"

static const float kWakeN = float(FOG_WAKE_N);

float WakeCell(uint k) { return wakeB[k].x; }

// How far inside a wake level's window a world xy lies, in cells (negative outside).
float WakeInside(uint k, float2 xy)
{
    float2 c = xy * wakeB[k].y;
    float2 lo = c - wakeA[k].xy;
    float2 hi = wakeA[k].xy + kWakeN - c;
    return min(min(lo.x, lo.y), min(hi.x, hi.y));
}

float3 WakeUvw(uint k, float2 xy)
{
    return float3(xy * wakeB[k].y / kWakeN, (float(k) + 0.5) / float(FOG_WAKE_LEVELS));
}

// The wake at a world xy: the fine level where it holds the point, the coarse one around it, faded
// out towards each window's edge. Zero outside both, or with the wakes off.
float4 WakeAt(Texture3D<float4> wakeTex, float2 xy)
{
    if (wakeD.w < 0.5) return 0.0;
    // Well inside the fine level, it alone: one fetch.
    float in0 = WakeInside(0u, xy);
    if (in0 >= 26.0) return wakeTex.SampleLevel(sLinearWrap, WakeUvw(0u, xy), 0);
    float in1 = WakeInside(1u, xy);
    if (in1 <= 2.0) return 0.0;
    float4 w = wakeTex.SampleLevel(sLinearWrap, WakeUvw(1u, xy), 0) * saturate((in1 - 2.0) / 16.0);
    if (in0 > 2.0) w = lerp(w, wakeTex.SampleLevel(sLinearWrap, WakeUvw(0u, xy), 0), saturate((in0 - 2.0) / 24.0));
    return w;
}

// How much of the wake reaches height h over the ground, for a wake whose change reaches height top.
float WakeReach(float h, float top) { return 1.0 - smoothstep(top, top + 1.5, h); }

// A wake's own height, and whether a torch made it (its height is stored past FOG_WAKE_TORCH).
bool  WakeIsTorch(float4 w) { return w.w >= FOG_WAKE_TORCH; }
float WakeTop(float4 w) { return WakeIsTorch(w) ? w.w - FOG_WAKE_TORCH : w.w; }

#endif
