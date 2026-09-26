// wxl-graphics-fog: the fog as a threat gameplay can drive -- density sources and volumes (add, carve,
// hold, smoke), fog fronts that advance or close in, wind and flow, global intensity and pulses, and
// what the camera stands in. Published by the wxl-graphics-fog extension as
// WXL_Api::GetInterface(WXL_GRAPHICS_FOG_API_NAME, WXL_GRAPHICS_FOG_API_VERSION); the same table is
// also published under version 1, whose three functions keep their places.
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

#ifndef WXL_GRAPHICS_FOG_API_H
#define WXL_GRAPHICS_FOG_API_H

#include <stdint.h>

// The fog is a simulated medium in the world (docs/design.md of the extension): what this API adds
// becomes part of it. Fog added by a source drifts with the wind and flows down the land; a carved
// hole refills by inflow over the fog's renewal time; a front pushes the fog ahead of it. Everything
// fades in and out (never a pop), and every call is cheap: the lists are read once per frame.
//
// Positions and directions are world space (yards, the client's axes: x north, y west, z up). Heights
// "over the ground" are measured from the terrain (or a lake's surface) under the point.
//
// Handles are non-zero and stay valid until removed; 0 means refused (full, or a bad argument). The
// capacities are per process, shared by every caller. Render thread (the client's main thread) only.

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_GRAPHICS_FOG_API_NAME    "wxl.graphics-fog"
#define WXL_GRAPHICS_FOG_API_VERSION 3

#define WXL_GFX_FOG_MAX_LIGHTS  8    ///< lights another extension may feed (SetLights)
#define WXL_GFX_FOG_MAX_VOLUMES 20   ///< density volumes of SetDensityVolumes
#define WXL_GFX_FOG_MAX_SOURCES 32   ///< AddSource
#define WXL_GFX_FOG_MAX_FRONTS  8    ///< AddFront
#define WXL_GFX_FOG_MAX_FLOWS   16   ///< AddFlow
#define WXL_GFX_FOG_MAX_PULSES  8    ///< Pulse
#define WXL_GFX_FOG_MAX_CASCADES 8   ///< AddCascade

#define WXL_GFX_FOG_SHAPE_SPHERE   0   ///< extent.x = radius
#define WXL_GFX_FOG_SHAPE_BOX      1   ///< extent = half sizes (x, y, z), yawed by `yaw`
#define WXL_GFX_FOG_SHAPE_CAPSULE  2   ///< v1: centre is the bottom, extent.x = radius, extent.z = height
#define WXL_GFX_FOG_SHAPE_CYLINDER 3   ///< vertical: centre is the bottom, extent.x = radius, extent.z = height

#define WXL_GFX_FOG_OP_ADD   0   ///< injects fog: strength = extinction per yard added per second
#define WXL_GFX_FOG_OP_CARVE 1   ///< removes fog: strength = share removed per second (8 clears in half a second)
#define WXL_GFX_FOG_OP_HOLD  2   ///< keeps at least a density: strength = extinction per yard
#define WXL_GFX_FOG_OP_SMOKE 3   ///< injects smoke (darker, thinning on its own): strength per second

#define WXL_GFX_FOG_FRONT_LINE    0   ///< a wall advancing along `direction`
#define WXL_GFX_FOG_FRONT_CLOSING 1   ///< a ring closing in on `origin`

#define WXL_GFX_FOG_PULSE_BREATH 0   ///< the density swells and ebbs, `period` seconds a cycle
#define WXL_GFX_FOG_PULSE_SURGE  1   ///< one rise over `attack` seconds, then a fall over `release`

// --- version 1 --------------------------------------------------------------------------------------

/// A point light scattering into the fog (and handed on to wxl-graphics-lights, so surfaces see it).
typedef struct WXL_GfxFogLight
{
    float   position[3];     ///< world space
    float   radius;          ///< influence radius, yards; nothing past it
    float   color[3];        ///< rgb 0..1
    float   intensity;       ///< multiplies color
    float   direction[3];    ///< spot axis; ignored for a point
    float   cosCone;         ///< cosine of the spot half-angle; <= -1 makes it a point light
    float   innerRadius;     ///< full colour up to here, fading to nothing at radius
    int32_t interior;        ///< 1 lights only indoor air, 0 only outdoor fog
} WXL_GfxFogLight;

/// A density volume replaced every call (v1): fog banks, graveyards, swamp pockets.
typedef struct WXL_GfxFogVolume
{
    int32_t shape;           ///< WXL_GFX_FOG_SHAPE_SPHERE, _BOX or _CAPSULE
    float   center[3];       ///< world space; a capsule's is its bottom
    float   extent[3];       ///< sphere: x = radius; box: half sizes; capsule: x = radius, z = height
    float   density;         ///< added: extinction per yard held; carved: share removed 0..1
    float   falloff;         ///< 0 hard edge .. 1 fades from the centre
    int32_t carve;           ///< 0 adds density, 1 removes that share of the fog there
    float   swirl;           ///< 0..1 churn inside it
} WXL_GfxFogVolume;

// --- version 2 --------------------------------------------------------------------------------------

/// Fog added, carved, held or smoke put in a shape; it may also push the fog.
typedef struct WXL_GfxFogSource
{
    uint32_t structSize;     ///< sizeof(WXL_GfxFogSource)
    int32_t  shape;          ///< WXL_GFX_FOG_SHAPE_*
    float    center[3];      ///< world space (the bottom of a capsule or cylinder)
    float    extent[3];      ///< see the shapes
    float    yaw;            ///< a box's rotation about the vertical, radians
    int32_t  op;             ///< WXL_GFX_FOG_OP_*
    float    strength;       ///< see the ops
    float    softness;       ///< 0 hard edge .. 1 the whole shape fades towards its edge
    float    billow;         ///< 0..1 how lumpy the edge and the injected fog are
    float    push[3];        ///< yd/s the fog inside is pushed along
    float    radialPush;     ///< yd/s outwards from the centre (negative pulls in)
    float    swirl;          ///< yd/s around the vertical axis
    float    duration;       ///< seconds, then it fades out; 0 = until removed
    float    fadeIn;         ///< seconds to full strength
    float    fadeOut;        ///< seconds from full strength to nothing
} WXL_GfxFogSource;

/// A fog front: a wall of fog that advances along a direction, or a ring that closes in on a point.
typedef struct WXL_GfxFogFront
{
    uint32_t structSize;     ///< sizeof(WXL_GfxFogFront)
    int32_t  kind;           ///< WXL_GFX_FOG_FRONT_*
    float    origin[3];      ///< line: a point of the front at the start; closing: the centre it closes on
    float    direction[2];   ///< line: horizontal travel direction (normalised for you)
    float    speed;          ///< yd/s
    float    startRadius;    ///< closing: the ring's radius at the start
    float    endRadius;      ///< closing: where it stops; line: yards travelled before it stops (0 = never)
    float    halfWidth;      ///< line: half its width, yards (0 = unbounded)
    float    height;         ///< yards over the ground the wall reaches
    float    depth;          ///< yards of fog behind the front (the body)
    float    density;        ///< extinction per yard in the body
    float    billow;         ///< 0..1 how lumpy its face is
    float    push;           ///< yd/s it drives the fog ahead of it
    float    duration;       ///< seconds, then it fades out; 0 = until removed
    float    fadeIn, fadeOut;
} WXL_GfxFogFront;

/// Wind or flow in a shape: pushes the fog through the simulation.
typedef struct WXL_GfxFogFlow
{
    uint32_t structSize;     ///< sizeof(WXL_GfxFogFlow)
    int32_t  shape;          ///< WXL_GFX_FOG_SHAPE_*
    float    center[3];
    float    extent[3];
    float    yaw;
    float    velocity[3];    ///< yd/s
    float    radial;         ///< yd/s outwards (negative pulls in)
    float    swirl;          ///< yd/s around the vertical
    float    softness;       ///< 0..1
    float    duration, fadeIn, fadeOut;
} WXL_GfxFogFlow;

/// A change of the fog's overall density over time.
typedef struct WXL_GfxFogPulse
{
    uint32_t structSize;     ///< sizeof(WXL_GfxFogPulse)
    int32_t  kind;           ///< WXL_GFX_FOG_PULSE_*
    float    amplitude;      ///< share of the density added at the peak (0.5 = half again; negative thins)
    float    period;         ///< breath: seconds per cycle
    float    attack;         ///< surge: seconds to the peak
    float    release;        ///< surge: seconds back to nothing
    float    duration;       ///< breath: seconds, then it fades out over `release`; 0 = until stopped
} WXL_GfxFogPulse;

/// A cascade: a bank of fog held behind a crest that pours over it and runs down the far side,
/// thinning as it falls. The fog runs where the terrain takes it, helped along `direction`.
typedef struct WXL_GfxFogCascade
{
    uint32_t structSize;     ///< sizeof(WXL_GfxFogCascade)
    int32_t  mapId;          ///< the map it belongs to (Map.dbc id); -1 on any map
    float    position[3];    ///< the spill point, on the crest (world space)
    float    direction[2];   ///< horizontal fall direction (normalised for you)
    float    width;          ///< yards across the spill and its bank
    float    depth;          ///< yards of fog the bank holds behind the crest (the flow rate follows)
    float    duration;       ///< seconds, then it fades out; 0 = until removed
    float    fadeIn, fadeOut;
} WXL_GfxFogCascade;

typedef struct WXL_GraphicsFogApi
{
    uint32_t structSize;
    uint32_t apiVersion;

    // --- version 1 (same places) ----------------------------------------------------------------

    /// Replaces the caller's lights for the coming frames (at most WXL_GFX_FOG_MAX_LIGHTS in all).
    void(__cdecl* SetLights)(const WXL_GfxFogLight* lights, int count);

    /// Replaces the density volumes (at most WXL_GFX_FOG_MAX_VOLUMES); count 0 clears them.
    void(__cdecl* SetDensityVolumes)(const WXL_GfxFogVolume* volumes, int count);

    /// Non-zero while the fog draws (enabled, DXVK available, drawn within the last second).
    int(__cdecl* Active)(void);

    // --- version 2: check structSize before reading these ------------------------------------------

    uint32_t(__cdecl* AddSource)(const WXL_GfxFogSource* source);
    /// Changes a source in place (moves it, retunes it); non-zero when the handle is live.
    int(__cdecl* UpdateSource)(uint32_t handle, const WXL_GfxFogSource* source);
    /// Fades a source out over fadeOut seconds (< 0: its own fadeOut), then frees it.
    void(__cdecl* RemoveSource)(uint32_t handle, float fadeOut);

    uint32_t(__cdecl* AddFront)(const WXL_GfxFogFront* front);
    void(__cdecl* RemoveFront)(uint32_t handle, float fadeOut);

    uint32_t(__cdecl* AddFlow)(const WXL_GfxFogFlow* flow);
    int(__cdecl* UpdateFlow)(uint32_t handle, const WXL_GfxFogFlow* flow);
    void(__cdecl* RemoveFlow)(uint32_t handle, float fadeOut);

    /// The fog's overall density, as a share of the profile's (1 = as tuned), reached over `seconds`.
    void(__cdecl* SetIntensity)(float intensity, float seconds);
    uint32_t(__cdecl* Pulse)(const WXL_GfxFogPulse* pulse);
    void(__cdecl* StopPulse)(uint32_t handle, float fadeOut);

    /// Overrides the wind (direction the wind blows towards, world xy; speed yd/s) over `seconds`;
    /// speed < 0 hands the wind back to the profile.
    void(__cdecl* SetWind)(const float direction[2], float speed, float seconds);

    /// Extinction per yard around the camera, all media together; read back from the GPU, a few
    /// frames late. 0 before the first reading.
    float(__cdecl* CameraDensity)(void);
    /// Yards at which the fog around the camera leaves 5 % of the light (3 / CameraDensity).
    float(__cdecl* Visibility)(void);
    /// Share 0..1 of the camera's surroundings inside a building (the indoor medium).
    float(__cdecl* CameraIndoor)(void);

    /// Removes every source, front, flow, pulse and volume, fading over `fadeOut` seconds, and hands
    /// the intensity and the wind back to the profile.
    void(__cdecl* ClearAll)(float fadeOut);

    // --- version 3: check structSize before reading these ------------------------------------------
    /// A cascade pouring from a spill point (at most WXL_GFX_FOG_MAX_CASCADES); 0 when full.
    uint32_t(__cdecl* AddCascade)(const WXL_GfxFogCascade* cascade);
    /// Changes a cascade in place; non-zero when the handle is live.
    int(__cdecl* UpdateCascade)(uint32_t handle, const WXL_GfxFogCascade* cascade);
    /// Fades a cascade out over fadeOut seconds (< 0: its own fadeOut); its fog runs on and thins.
    void(__cdecl* RemoveCascade)(uint32_t handle, float fadeOut);
} WXL_GraphicsFogApi;

#ifdef __cplusplus
}
#endif

#endif // WXL_GRAPHICS_FOG_API_H
