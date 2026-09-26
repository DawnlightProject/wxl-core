// wxl-graphics-lights: the HDR source texture (GraphicsLightsSourcesApi.h SourceTexture) read from a shader.
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

#ifndef WXL_LIGHTS_SOURCES_HLSLI
#define WXL_LIGHTS_SOURCES_HLSLI

// WXL_GFX_LIGHTS_MAX columns, one per light (the list's index), of ten RGBA32F rows. Positions are
// camera-relative to the eye the list was published around; intensities are linear, scene-referred,
// at one yard (see GraphicsLightsSourcesApi.h). Read with Load: every value is exact.
//   0  position (flicker included), reach (yards; the window reaches zero there)
//   1  intensity rgb, soft core radius (yards)
//   2  spot axis, cos of the cone's edge (<= -1 a point light)
//   3  cos where the cone is full, profile (WXL_GFX_LIGHT_PROFILE_*), flags (WXL_GFX_LIGHT_SOURCE_*), family
//   4  a tube's half length (world axes), emissive radius (yards)
//   5  emissive luminance, room (-1 none), room gate weight 0..1, room mask (rooms it may light, 12 bits)
//   6  rest position (before the flicker), flicker gain
//   7  cookie rotation, world direction -> the cookie's frame (quaternion xyzw)
//   8  cookie cell code (atlas cell + 1 + 128 x faded-in share; 0 not resident), cookie mean rgb (0: none)
//   9  hot core share, kelvin (0 a tint), fade, 0
// A column past the list's end holds zeros (reach 0: no light).

static const int kWxlSourceRows = 10;

struct WxlLightSource
{
    float3 position; float reach;
    float3 intensity; float softRadius;
    float3 axis; float cosOuter;
    float cosInner; float profile; float flags; float family;
    float3 extent; float emissiveRadius;
    float emissive; float room; float roomGate; float roomMask;
    float3 rest; float flickerGain;
    float4 cookieRotation;
    float4 cookie;
    float hot; float kelvin; float fade; float pad;
};

WxlLightSource WxlReadSource(Texture2D<float4> sources, uint index)
{
    WxlLightSource s;
    float4 r0 = sources.Load(int3(index, 0, 0));
    float4 r1 = sources.Load(int3(index, 1, 0));
    float4 r2 = sources.Load(int3(index, 2, 0));
    float4 r3 = sources.Load(int3(index, 3, 0));
    float4 r4 = sources.Load(int3(index, 4, 0));
    float4 r5 = sources.Load(int3(index, 5, 0));
    float4 r6 = sources.Load(int3(index, 6, 0));
    float4 r9 = sources.Load(int3(index, 9, 0));
    s.position = r0.xyz; s.reach = r0.w;
    s.intensity = r1.xyz; s.softRadius = r1.w;
    s.axis = r2.xyz; s.cosOuter = r2.w;
    s.cosInner = r3.x; s.profile = r3.y; s.flags = r3.z; s.family = r3.w;
    s.extent = r4.xyz; s.emissiveRadius = r4.w;
    s.emissive = r5.x; s.room = r5.y; s.roomGate = r5.z; s.roomMask = r5.w;
    s.rest = r6.xyz; s.flickerGain = r6.w;
    s.cookieRotation = sources.Load(int3(index, 7, 0));
    s.cookie = sources.Load(int3(index, 8, 0));
    s.hot = r9.x; s.kelvin = r9.y; s.fade = r9.z; s.pad = 0.0;
    return s;
}

bool WxlSourceHas(float flags, uint bit) { return (uint(flags + 0.5) & bit) != 0u; }

// The window that takes a light to zero at its reach with a zero slope: saturate(1 - x^4)^2.
float WxlSourceWindow(float d2, float reach)
{
    float x2 = d2 / max(reach * reach, 1e-4);
    float w = saturate(1.0 - x2 * x2);
    return w * w;
}

// The irradiance at squared distance d2 on a surface facing the light, per unit of its intensity.
float WxlSourceFalloff(float d2, float reach, float softRadius)
{
    return WxlSourceWindow(d2, reach) / (d2 + softRadius * softRadius);
}

#endif
