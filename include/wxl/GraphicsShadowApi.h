// wxl-graphics-shadow: every shadow of the render stack -- the lamps' shadow maps, body capsules for
// lamps without one, the sun and the moon (the engine's cascades, the terrain in them, the baked
// horizon), contact shadows -- as screen-space masks for surfaces and as a Vulkan binding set plus an
// HLSL include for any point (fog froxels, a lamp field). Published by the wxl-graphics-shadow
// extension as WXL_Api::GetInterface(WXL_GRAPHICS_SHADOW_API_NAME, WXL_GRAPHICS_SHADOW_API_VERSION).
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

#ifndef WXL_GRAPHICS_SHADOW_API_H
#define WXL_GRAPHICS_SHADOW_API_H

#include <stddef.h>
#include <stdint.h>

#include "wxl/GraphicsVulkanApi.h"

// Absent means lit. Every consumer must work without this service: when the interface is missing, when
// DXVK is not there, or when GetFrame returns 0, everything is fully lit. The HLSL include
// (extensions/wxl-graphics-shadow/shaders/wxl/shadow/shadow.hlsli) returns 1 everywhere while the
// uniform block written by WriteBindings says "off".
//
// Identity. A shadow slot is keyed by the light id wxl-graphics-lights publishes (WXL_GfxLight::id):
// one id space for the service, the surfaces and the fog. Slot numbers are stable while a light keeps
// its slot; a light gaining or losing a slot, or a map, fades over about half a second, and the
// fade is already folded into every visibility this service returns.
//
// The frame. Before the world pass (scheduler begin phase, at WXL_GFX_SHADOW_ORDER) the service
// chooses the slots and the maps from this frame's lights; the core renders the maps inside the world
// pass; then the service's compute pass (WXL_GFX_SHADOW_ORDER) filters the maps and writes the masks.
// A compute pass ordered after it in the same block reads them (GetFrame, WriteBindings). A D3D9 pass
// does not: the masks are Vulkan images only.
//
// Positions. Every position a shader hands the include is camera-relative (world - eye, the
// WXL_GfxView "rel" convention); ShadowRel(world) converts. CPU structs are in world space.
//
// Threads. Render thread only. Callbacks of wxl-graphics-extend run on it.

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_GRAPHICS_SHADOW_API_NAME    "wxl.graphics-shadow"
#define WXL_GRAPHICS_SHADOW_API_VERSION 1

#define WXL_GFX_SHADOW_SLOTS     16   ///< lights shadowed at once (a map, capsules, or both)
#define WXL_GFX_SHADOW_MAPS      8    ///< filtered cube maps (one per mapped light)
#define WXL_GFX_SHADOW_CAPSULES  32   ///< body capsules (player, NPCs, creatures) near the camera
#define WXL_GFX_SHADOW_CASCADES  4    ///< the engine's sun maps: main, then bands 0..2
#define WXL_GFX_SHADOW_MASK_LAYERS 5  ///< layers of the mask image (see WXL_GfxShadowFrame)

/// The compute pass order: after nothing that lights surfaces, before wxl-graphics-lights (100) and
/// the fog (200). Order your compute pass higher to read this frame's shadows.
#define WXL_GFX_SHADOW_ORDER 90

// --- what a consumer wants (Want) ---------------------------------------------------------------------

/// Screen-space masks for this frame's surfaces (costs a full-screen pass).
#define WXL_GFX_SHADOW_WANT_MASKS  0x00000001u
/// The maps and the uniform block for point lookups (fog froxels, a lamp field): no screen pass.
#define WXL_GFX_SHADOW_WANT_POINTS 0x00000002u

// --- a light handed in (SetLights) ---------------------------------------------------------------------

#define WXL_GFX_SHADOW_LIGHT_CARRIED 0x00000001u  ///< rides a unit (a torch in a hand)
#define WXL_GFX_SHADOW_LIGHT_NO_MAP  0x00000002u  ///< capsules and contact only, never a cube map
#define WXL_GFX_SHADOW_LIGHT_NEVER   0x00000004u  ///< cast no shadow at all (never takes a slot)

/**
 * @brief One light that may cast shadows, world space. Plain C, no 64-bit member.
 *
 * `importance` is the caller's ranking (larger first); the service adds hysteresis, so a light near
 * the cut-off does not flip. A light missing from a frame's set fades its slot out.
 */
typedef struct WXL_GfxShadowLight
{
    uint32_t id;             ///< WXL_GfxLight::id: nonzero, stable while the light lives
    float    position[3];    ///< where it rests (WXL_GfxLight::rest: before the flicker moves it)
    float    radius;         ///< nothing past it, yards
    float    direction[3];   ///< spot axis (unit); ignored for a point light
    float    cosCone;        ///< cosine of the spot half-angle; <= -1 for a point light
    float    sourceSize;     ///< radius of the glowing source, yards: how wide penumbrae grow
    float    importance;     ///< >= 0; larger takes a slot and a map first
    uint32_t flags;          ///< WXL_GFX_SHADOW_LIGHT_*
    int32_t  listIndex;      ///< its index in wxl-graphics-lights' list of this frame, -1 when not listed
    uint32_t carrierLo;      ///< the carrying unit's GUID (low, high words) when known; 0 = found by position
    uint32_t carrierHi;
} WXL_GfxShadowLight;

/// A slot as this frame uses it.
typedef struct WXL_GfxShadowSlot
{
    uint32_t lightId;        ///< 0 when the slot is free
    int32_t  listIndex;      ///< in wxl-graphics-lights' list this frame, -1 when not listed
    float    weight;         ///< 0..1 the slot's shadow share (fades in and out)
    float    mapWeight;      ///< 0..1 how much of it comes from a cube map (the rest from capsules)
    int32_t  map;            ///< the filtered map it reads, -1 none
    uint32_t flags;          ///< WXL_GFX_SHADOW_LIGHT_* it was handed with
} WXL_GfxShadowSlot;

// --- the frame, for a compute pass ordered after WXL_GFX_SHADOW_ORDER ------------------------------------

#define WXL_GFX_SHADOW_FRAME_MASKS   0x00000001u  ///< the masks were written this frame
#define WXL_GFX_SHADOW_FRAME_POINTS  0x00000002u  ///< maps and uniform block current this frame
#define WXL_GFX_SHADOW_FRAME_SUN     0x00000004u  ///< the sun or the moon casts this frame
#define WXL_GFX_SHADOW_FRAME_HALFRES 0x00000008u  ///< the masks were traced at half resolution and upsampled

/**
 * @brief What the service wrote this frame.
 *
 * masks: a VK_IMAGE_TYPE_3D image, VK_FORMAT_R8G8B8A8_UNORM, the world's size x WXL_GFX_SHADOW_MASK_LAYERS,
 * in GENERAL, full resolution whatever the tracing resolution (read it with Load(int4(x, y, layer, 0))).
 * Value 1 is lit. Every value already includes its fade.
 *   layer 0  x the sun's visibility (terrain horizon x cascades x contact), y the moon's (the same
 *            towards the moon), z the terrain horizon alone towards the body that shines, w the
 *            contact shadow alone towards it
 *   layer 1 + s / 4, channel s % 4: shadow slot s's visibility (map or capsules, times contact)
 * Pixels without geometry (the sky) hold 1.
 */
typedef struct WXL_GfxShadowFrame
{
    uint32_t          structSize;
    uint32_t          frameIndex;       ///< WXL_GfxFrame::frameIndex it belongs to
    uint32_t          flags;            ///< WXL_GFX_SHADOW_FRAME_*
    uint32_t          width, height;    ///< of the masks (the world's render target)
    WXL_GfxVkImage    masks;            ///< image VK_NULL_HANDLE without WXL_GFX_SHADOW_FRAME_MASKS
    float             toSun[3];         ///< towards the sun, world, unit
    float             sunWeight;        ///< 0..1: how much the sun shines (0 at night)
    float             toMoon[3];
    float             moonWeight;
    uint32_t          slotCount;        ///< WXL_GFX_SHADOW_SLOTS
    WXL_GfxShadowSlot slots[WXL_GFX_SHADOW_SLOTS];
} WXL_GfxShadowFrame;

typedef struct WXL_GraphicsShadowApi
{
    uint32_t structSize;
    uint32_t apiVersion;

    /// This frame (and the next) you read shadows: WXL_GFX_SHADOW_WANT_*. Call it from your wants
    /// callback every frame you will; nothing is computed on a frame nobody wanted.
    void(__cdecl* Want)(uint32_t what);

    /**
     * @brief The lights that may cast this frame, replacing the previous set.
     *
     * wxl-graphics-lights calls it once a frame before the world pass (after it publishes its list).
     * Without a call for two frames, the service reads WXL_GraphicsLightsApi::Current itself and ranks
     * by brightness over distance. count 0 with a non-null pointer means "no light casts".
     */
    void(__cdecl* SetLights)(const WXL_GfxShadowLight* lights, int count);

    /// This frame's slots (WXL_GFX_SHADOW_SLOTS at most); returns how many were written.
    int(__cdecl* Slots)(WXL_GfxShadowSlot* out, int max);

    /// The slot of a light id this frame, -1 when it has none.
    int(__cdecl* SlotOf)(uint32_t lightId);

    /// Inside a record callback ordered after WXL_GFX_SHADOW_ORDER: what was written this frame.
    /// Returns 0 when the service did not run this frame (then everything is lit). Set structSize first.
    int(__cdecl* GetFrame)(WXL_GfxShadowFrame* out);

    // --- point lookups: the binding set shadow.hlsli declares -----------------------------------------

    /// How many consecutive set-0 bindings shadow.hlsli uses from WXL_SHADOW_BINDING on.
    uint32_t(__cdecl* BindingCount)(void);

    /// Their descriptions from firstBinding on, for your pipeline's layout (WXL_GfxVkPipelineDesc).
    /// Returns how many were written (BindingCount when max allows).
    uint32_t(__cdecl* DescribeBindings)(uint32_t firstBinding, WXL_GfxVkBinding* out, uint32_t max);

    /**
     * @brief Writes the shadow resources into your descriptor set from firstBinding on.
     *
     * Inside a record callback, on a set from AllocDescriptorSet. Always writes something valid: when
     * the service did not run this frame it writes neutral images and an "off" uniform block, so
     * every lookup returns 1. Returns 0 only when nothing could be written (no Vulkan yet); do not
     * dispatch a pipeline that uses the include then.
     */
    int(__cdecl* WriteBindings)(VkDescriptorSet set, uint32_t firstBinding);

    // --- status ----------------------------------------------------------------------------------------

    /// One line for a panel.
    const char*(__cdecl* Status)(void);

    /// Smoothed GPU milliseconds of the service's compute pass; -1 until measured.
    float(__cdecl* GpuMs)(void);
} WXL_GraphicsShadowApi;

#ifdef __cplusplus
}
#endif

#endif // WXL_GRAPHICS_SHADOW_API_H
