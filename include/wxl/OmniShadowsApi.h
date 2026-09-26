// The core's omni shadow maps: 6-face depth maps rendered from a few chosen point lights with the
// engine's own shadow draw path. Published as WXL_Api::GetInterface("wxl.omnishadows",
// WXL_OMNISHADOWS_API_VERSION).
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

#ifndef WXL_OMNISHADOWS_API_H
#define WXL_OMNISHADOWS_API_H

#include <stdint.h>

// What renders. Right after the engine renders its main exterior shadow map (inside the world pass,
// through CShadowQuery::Render), the core re-issues the same casters -- that pass's draw lists -- from
// each chosen light, one cube face per cell of a 4 x 2 atlas, with the engine's ShadowMapRenderSL
// effect. The casters are therefore what the engine's main map holds: the ~40 x 40 yd around the
// player, and only what extShadowQuality puts there (1-2: characters; 3-4: + WMOs; 5: + doodads).
// Lights far from the player shadow only what falls inside that area. Needs extShadowQuality >= 1.
//
// Encoding. Each light owns an R32F atlas 4 * faceSize wide, 2 * faceSize high (powers of two);
// face f (0..5 = +X, -X, +Y, -Y, +Z, -Z in world axes) is the cell (f % 4, f / 4), the last two
// cells unused. A texel holds the depth along the
// face's axis divided by the light's radius (1 where nothing was drawn). To look a point up: pick
// the face whose axis best matches (point - light), s = faceRows[f] . (point, 1), uv = s.xy / s.w
// (already in atlas coordinates), and compare s.w / radius with the texel (in shadow when larger,
// minus a bias). Everything the pass has not drawn holds 1 (lit): the two spare cells, and any face
// not yet rendered since the atlas was created or its slot reassigned (the whole atlas is cleared
// to 1 then).

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_OMNISHADOWS_API_VERSION 3 // 2 appends SetLightsEx and GetState; 3 appends Claim, SetLightsV3, ...
#define WXL_OMNISHADOWS_MAX 4         // lights a v1 / v2 caller sets
#define WXL_OMNISHADOWS_MAX_V3 12     // lights SetLightsV3 sets

// v3 caster classes: which of the engine's casters a light's map holds (0 = both, as v1 and v2).
#define WXL_OMNI_CASTERS_STATIC   0x1u  ///< WMOs and every model that belongs to no unit
#define WXL_OMNI_CASTERS_DYNAMIC  0x2u  ///< units and every model attached to one (weapons, torches)
/// v3: with CASTERS_DYNAMIC alone, the faces that see a unit are redrawn every frame (animation shows)
/// and a face whose last unit left is redrawn once, empty. Faces that never saw one cost nothing.
#define WXL_OMNI_REDRAW_OCCUPIED  0x4u

typedef struct WXL_OmniLight
{
    float position[3];  ///< world
    float radius;       ///< far plane and depth scale, yards
} WXL_OmniLight;

/// SetLightsEx's light: a stable id lets the core tell a moving light from a reassigned slot.
typedef struct WXL_OmniLightEx
{
    float    position[3];
    float    radius;
    uint32_t id;          ///< nonzero and stable per light; 0 = match by slot index
    uint32_t reserved;
} WXL_OmniLightEx;

/// SetLightsV3's light.
typedef struct WXL_OmniLightV3
{
    float    position[3];
    float    radius;
    uint32_t id;          ///< nonzero and stable per light (and per map when a light takes two)
    uint32_t flags;       ///< WXL_OMNI_CASTERS_* | WXL_OMNI_REDRAW_OCCUPIED
    uint32_t faceMask;    ///< bit f: face f is rendered; 0 = all six. Faces left out stay 1 (lit)
    uint32_t faceSize;    ///< 0 = the SetFaceSize default; else rounded down to a power of two, 64..1024
} WXL_OmniLightV3;

/// Per-light change tracking, from GetState.
typedef struct WXL_OmniShadowState
{
    uint32_t staleFaces;      ///< bit f set: face f does not yet reflect the light's latest position or casters
    uint32_t lastMovedFrame;  ///< scene frame the light or a caster in its radius last moved (0 = never)
    uint32_t lastClearFrame;  ///< scene frame the atlas was last cleared to 1 (creation, slot reassigned)
    uint32_t id;
} WXL_OmniShadowState;

typedef struct WXL_OmniShadow
{
    void*    texture;          ///< IDirect3DTexture9*, R32F atlas; null until first rendered
    uint32_t faceSize;
    float    position[3];      ///< where it was rendered from
    float    radius;
    float    faceRows[6][4][4]; ///< world (x, y, z, 1) -> (u*w, v*w, -, w), atlas coordinates, w = depth along the face axis
    uint32_t faceFrame[6];     ///< scene frame each face was last rendered on (0 = never)
} WXL_OmniShadow;

typedef struct WXL_OmniShadowsApi
{
    uint32_t structSize;
    uint32_t apiVersion;

    /// Chooses the lights (up to WXL_OMNISHADOWS_MAX); replaces the previous set. Main thread. Call it
    /// every frame: the set is dropped when no call arrives for a second. WXL_OMNI_SHADOWS=0 in
    /// WarcraftXL.cfg makes it a no-op.
    void(__cdecl* SetLights)(const WXL_OmniLight* lights, uint32_t count);
    /// Faces rendered per frame across all lights (default 6); faces refresh round-robin.
    void(__cdecl* SetBudget)(uint32_t facesPerFrame);
    /// Face edge in texels (default 512, rounded down to a power of two, 64..1024); applies when the
    /// atlases are next created.
    void(__cdecl* SetFaceSize)(uint32_t size);
    /// Reads one light's maps. Returns 0 when index is out of range.
    int(__cdecl* Get)(uint32_t index, WXL_OmniShadow* out);
    uint32_t(__cdecl* Count)(void);
    /// Changes whenever the atlases are recreated; drop cached texture pointers then. A replaced
    /// atlas stays alive four more frames, so a pointer read before the world pass is safe to use
    /// in that frame's later passes.
    uint32_t(__cdecl* Generation)(void);

    // --- appended; present when structSize covers them ---
    /// SetLights with stable ids. A light (matched by id, or by slot when id is 0) that moved more than
    /// 0.05 yd, or that has a unit inside its radius that moved, gets all six faces re-rendered that
    /// frame ahead of the round-robin. A slot given a different light is cleared to 1 first.
    void(__cdecl* SetLightsEx)(const WXL_OmniLightEx* lights, uint32_t count);
    /// Reads one light's change tracking. Returns 0 when index is out of range.
    int(__cdecl* GetState)(uint32_t index, WXL_OmniShadowState* out);

    // --- v3 ---
    /**
     * @brief Takes exclusive driving of the maps for owner (any address unique to the caller).
     *
     * While held, SetLights, SetLightsEx, SetBudget and SetFaceSize from anyone are accepted and
     * ignored (Get, Count, GetState and Generation still answer everyone), and only the owner's
     * SetLightsV3 and SetBudgetV3 apply. Returns 1 when owner holds it, 0 when another does.
     */
    int(__cdecl* Claim)(const void* owner);
    /// Gives the claim back (only its owner can); the light set is dropped.
    void(__cdecl* Release)(const void* owner);
    /// Like SetLightsEx with caster classes, face masks and face sizes, up to WXL_OMNISHADOWS_MAX_V3.
    /// Only while the claim is free or held by owner; a light's slot index is its index here.
    void(__cdecl* SetLightsV3)(const void* owner, const WXL_OmniLightV3* lights, uint32_t count);
    /// SetBudget for the claim's owner: faces of still lights refreshed per frame, in turn.
    void(__cdecl* SetBudgetV3)(const void* owner, uint32_t facesPerFrame);
    /// Faces rendered on the last frame, all lights together (priority and round-robin).
    uint32_t(__cdecl* FacesLastFrame)(void);
} WXL_OmniShadowsApi;

#ifdef __cplusplus
}
#endif

#endif // WXL_OMNISHADOWS_API_H
