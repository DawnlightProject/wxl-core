// The core's sun shadow light: a way to adjust the direction the engine's shadow maps are rendered
// along, each frame, before it is used. Published as WXL_Api::GetInterface("wxl.shadowlight",
// WXL_SHADOWLIGHT_API_VERSION).
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

#ifndef WXL_SHADOWLIGHT_API_H
#define WXL_SHADOWLIGHT_API_H

#include <stdint.h>

// The engine derives its shadow light from the sun as normalize(sun.x, sun.y, max(sun.z * 5, -1.2))
// (CShadowQuery::Update): the sun's height is exaggerated five times and never falls below 50
// degrees, so shadows stay short at dusk. Once per world frame, before the engine renders its maps,
// the adjuster receives that direction (unit length, pointing down along the light's travel) with
// the true sun direction beside it, and may overwrite dir; the core normalises it and hands it to
// the engine. Every consumer of the shadow maps (the engine's receivers, wxl::game::shadows) then
// sees the adjusted direction.

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_SHADOWLIGHT_API_VERSION 2 // 2 appends Claim, Release, SetAdjustOwned

/// dir: in the engine's direction, out the one to render with. sun: the engine's lighting
/// direction this frame (points down, unit length; zero when unknown). Main thread.
typedef void (__cdecl* WXL_ShadowLightAdjustFn)(float dir[3], const float sun[3], void* user);

typedef struct WXL_ShadowLightApi
{
    uint32_t structSize;
    uint32_t version;

    /// Installs the one adjuster (null removes it). Returns 0 when the engine function could not
    /// be hooked; the engine then keeps its own direction.
    int (__cdecl* SetAdjust)(WXL_ShadowLightAdjustFn fn, void* user);

    /// The direction the maps were last rendered along (after adjustment). Returns 0 before the
    /// first frame.
    int (__cdecl* GetDirection)(float dir[3]);

    // --- v2 ---
    /// Takes the adjuster for owner (any address unique to the caller): while held, SetAdjust from
    /// anyone is accepted (returns 1) and ignored, and only SetAdjustOwned by owner applies. Returns 1
    /// when owner holds it, 0 when another does.
    int (__cdecl* Claim)(const void* owner);
    /// Gives the claim back (only its owner can) and removes its adjuster.
    void (__cdecl* Release)(const void* owner);
    /// SetAdjust for the claim's holder (or anyone while it is free).
    int (__cdecl* SetAdjustOwned)(const void* owner, WXL_ShadowLightAdjustFn fn, void* user);
} WXL_ShadowLightApi;

#ifdef __cplusplus
}
#endif

#endif
