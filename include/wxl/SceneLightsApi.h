// The core's scene-light service: point lights injected into the world M2 scene, and the policy the
// engine uses to pick which lights each receiver gets. Published as WXL_Api::GetInterface(
// "wxl.scenelights", WXL_SCENELIGHTS_API_VERSION).
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

#ifndef WXL_SCENELIGHTS_API_H
#define WXL_SCENELIGHTS_API_H

#include <stdint.h>

// Injected lights are real CM2Light records linked into the world scene's light grid, so the stock
// shaders light M2 models (up to 4 lights, per vertex), WMO groups (4 per group) and terrain chunks
// (3 per chunk) with them, competing with the model lights under the active selection policy.
//
// Timing. Add/Update/Remove may be called any time on the main thread. The core applies the stored
// state once per world frame, from its detour on CM2Scene::Animate, right before the engine bumps
// the scene frame and selects lights; a change is therefore visible from the next world frame on.
// Everything is dropped when the world scene goes away (world leave, scene teardown).

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_SCENELIGHTS_API_VERSION 1

typedef struct WXL_SceneLight
{
    float position[3];  ///< world
    /// Diffuse the shaders add, intensity folded in: colour * intensity, may exceed 1.
    float color[3];
    /// Falloff 1 / (f0 + f1 d + f2 d^2), per vertex, d in yards. The engine's own model lights use
    /// (0, 0.7, 0.03). FalloffForRadius gives one that fades to ~1% at a chosen radius.
    float falloff[3];
} WXL_SceneLight;

typedef struct WXL_SelectedLight
{
    float position[3];
    float color[3];
    float falloff[3];
    float key;          ///< the selection key: squared distance (stock) or -influence (influence)
    int   injected;     ///< 1 when the light came from this service
} WXL_SelectedLight;

enum
{
    WXL_LIGHT_POLICY_STOCK     = 0,  ///< nearest to the receiver's centre, the engine's own
    WXL_LIGHT_POLICY_INFLUENCE = 1,  ///< brightest at the receiver's bounding sphere; scans wider
    WXL_LIGHT_POLICY_OFF       = 2,  ///< no point light reaches the world's receivers (models, map
                                     ///< objects, terrain, detail doodads); directional, ambient and
                                     ///< the sun and moon are untouched, and so is anything drawn
                                     ///< outside the world pass (UI models, character selection)
};

typedef struct WXL_SceneLightsApi
{
    uint32_t structSize;
    uint32_t apiVersion;

    /// Adds a light. Returns a handle, 0 when the pool (256) is full.
    uint32_t(__cdecl* Add)(const WXL_SceneLight* light);
    /// Replaces a light's parameters. Returns 0 for an unknown handle.
    int(__cdecl* Update)(uint32_t handle, const WXL_SceneLight* light);
    /// Removes a light and unlinks it at once.
    void(__cdecl* Remove)(uint32_t handle);
    /// Lights currently held.
    uint32_t(__cdecl* Count)(void);

    /// Fills f[3] so the light fades to about 1% of its colour at radius yards.
    void(__cdecl* FalloffForRadius)(float radius, float f[3]);

    void(__cdecl* SetPolicy)(int policy);
    int(__cdecl* Policy)(void);

    /// Reads what the engine selected into a CM2Lighting (a receiver's lighting block, e.g. from a
    /// draw callback). Returns how many were written, at most cap and at most 4.
    int(__cdecl* Selected)(const void* lighting, WXL_SelectedLight* out, int cap);
} WXL_SceneLightsApi;

#ifdef __cplusplus
}
#endif

#endif // WXL_SCENELIGHTS_API_H
