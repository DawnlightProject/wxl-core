// wxl-graphics-lights: the one list of point and spot lights every render extension lights with --
// gathered from the client, faded, clustered, with cookies, interior rooms and omni shadow slots.
// Published by the wxl-graphics-lights extension as
// WXL_Api::GetInterface(WXL_GRAPHICS_LIGHTS_API_NAME, WXL_GRAPHICS_LIGHTS_API_VERSION).
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

#ifndef WXL_GRAPHICS_LIGHTS_API_H
#define WXL_GRAPHICS_LIGHTS_API_H

#include <stddef.h>
#include <stdint.h>

// Where the lights come from: the engine's M2 lights and the WMO lights files author (core's
// wxl::game::lights), the model table, which gives light to models that carry none (torches, lamp
// posts, candles: data/model-lights.csv), and lights consumers pass in (SetGiven). The service scores
// them, keeps the best WXL_GFX_LIGHTS_MAX, fades each in and out, merges the lights of one fixture,
// resolves their cookies and rooms, and publishes the list once per frame, before the world pass,
// for every consumer at once: as C structs (Current) and as textures a shader reads (the layouts of
// wxl-forever's lights service, unchanged: see LightTexture, ClusterTexture, OmniTexture).
//
// Demand. Nothing is gathered on a frame no consumer wants: call Want() from your
// WXL_GfxWantsFn (GraphicsExtendApi.h) on every frame you will read the list. The list of a frame is
// published in the begin phase of the graphics-extend scheduler, so it is ready for every begin,
// compute record and draw callback of that frame. The first frame after the first Want() has no list
// yet (Current returns 0).
//
// Textures. Every texture here is D3DPOOL_DEFAULT and filled with UpdateTexture, so a Vulkan compute
// pass may import it (GraphicsVulkanApi.h) as well as a D3D9 pass sample it. They are released before
// a device reset and come back with the next publish; re-read the pointers every frame.
//
// Threads. Render thread only.

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_GRAPHICS_LIGHTS_API_NAME    "wxl.graphics-lights"
#define WXL_GRAPHICS_LIGHTS_API_VERSION 1

#define WXL_GFX_LIGHTS_MAX        128   ///< lights in a published list (kLightCapacity of the shaders)
#define WXL_GFX_LIGHT_ROWS        4     ///< texel rows per light in the light texture
#define WXL_GFX_LIGHTS_OMNI_MAX   4     ///< omni shadow slots (the core's WXL_OMNISHADOWS_MAX)
#define WXL_GFX_LIGHTS_ROOMS_MAX  12

#define WXL_GFX_CLUSTERS_X 16
#define WXL_GFX_CLUSTERS_Y 9
#define WXL_GFX_CLUSTERS_Z 16
#define WXL_GFX_CLUSTER_TEX_W 256
#define WXL_GFX_CLUSTER_NEAR  0.5f     ///< yards: the clusters divide [near, far] exponentially in depth
#define WXL_GFX_CLUSTER_FAR   250.0f

#define WXL_GFX_LIGHT_KIND_M2    0u
#define WXL_GFX_LIGHT_KIND_WMO   1u
#define WXL_GFX_LIGHT_KIND_TABLE 2u
#define WXL_GFX_LIGHT_KIND_GIVEN 3u

#define WXL_GFX_LIGHT_FLICKER_NONE    0u
#define WXL_GFX_LIGHT_FLICKER_FIRE    1u
#define WXL_GFX_LIGHT_FLICKER_CANDLE  2u
#define WXL_GFX_LIGHT_FLICKER_LANTERN 3u

#define WXL_GFX_LIGHT_PROFILE_NONE      0u
#define WXL_GFX_LIGHT_PROFILE_CAGE      1u
#define WXL_GFX_LIGHT_PROFILE_DOWNLIGHT 2u
#define WXL_GFX_LIGHT_PROFILE_FLAME     3u
#define WXL_GFX_LIGHT_PROFILE_GRILLE    4u

/**
 * @brief One light, world space. Fields after `kind` are set by the service each frame; a given light
 *        (SetGiven) fills position through innerRadius and may leave the rest zero.
 *
 * Plain C with no 64-bit member, so its layout is the same for every compiler that builds a consumer.
 */
typedef struct WXL_GfxLight
{
    float       position[3];     ///< world space
    float       radius;          ///< nothing past it, yards
    float       color[3];        ///< the client's working (gamma) colour
    float       intensity;       ///< multiplies color
    float       direction[3];    ///< spot axis; ignored for a point
    float       cosCone;         ///< cosine of the spot half-angle; <= -1 makes it a point light
    float       innerRadius;     ///< reference radius: where the light has its own colour
    int32_t     interior;        ///< 1 for a light inside an interior room (with the interior gate on)
    uint8_t     kind;            ///< WXL_GFX_LIGHT_KIND_*
    uint8_t     flicker;         ///< WXL_GFX_LIGHT_FLICKER_*
    uint8_t     profile;         ///< WXL_GFX_LIGHT_PROFILE_*: its angular profile
    uint8_t     carried;         ///< 1 when its model rides another (a torch in a hand)
    float       size;            ///< radius of the glowing source, yards
    float       extent[3];       ///< half the length of a tube light; zero for a point
    const void* owner;           ///< identity for fading; null for a given light
    uint32_t    index;
    uint32_t    id;              ///< stable non-zero id while its owner lives (0 for a given light)
    float       rest[3];         ///< position before the flicker moved it
    uint32_t    cookieSourceLo;  ///< the cookie's source file hash (0 = unknown), low and high words
    uint32_t    cookieSourceHi;
    float       cookieRotation[4];   ///< world direction -> the cookie's frame (quaternion xyzw)
    uint32_t    cookieCell;      ///< 1-based atlas cell + 128 x its faded-in share (0 = none)
    float       cookieOpen;      ///< the cookie's mean transmittance, -1 unknown
    float       cookieTint[3];   ///< its glass tint, luma-preserving
    int32_t     room;            ///< the interior room holding it, -1 outside every room
    float       roomFloor, roomCeiling;   ///< that room's floor and ceiling at the light, world z
    float       haloRadius, haloInner;    ///< the fog halo's own reach and reference radius (0 = the light's)
} WXL_GfxLight;

/// One omni shadow map as the core rendered it for a light of this frame's list.
typedef struct WXL_GfxOmniSlot
{
    int32_t lightIndex;          ///< index in this frame's list
    float   weight;              ///< its shadow's share (fades as a slot changes hands)
    float   position[3];         ///< where it was rendered from, camera-relative
    float   radius;
    float   faceSize;
    void*   texture;             ///< IDirect3DTexture9*, the core's R32F 4 x 2 atlas (borrowed, this frame)
    float   rows[6][3][4];       ///< per face: camera-relative (x, y, z, 1) -> (u * w, v * w, w); zero if not rendered
} WXL_GfxOmniSlot;

/// A light that moved lately (a carried torch, a missile's glow), before the flicker.
typedef struct WXL_GfxMovingLight
{
    float position[3];
    float radius;
} WXL_GfxMovingLight;

typedef struct WXL_GraphicsLightsApi
{
    uint32_t structSize;
    uint32_t apiVersion;

    /// This frame (and the next) you will read the list: call it from your wants callback.
    void(__cdecl* Want)(void);

    /// The list published for the frame being drawn, and that frame's index (WXL_GfxFrame::frameIndex
    /// numbering); null and *count 0 before the first publish. Valid until the next publish.
    const WXL_GfxLight*(__cdecl* Current)(int* count, uint32_t* frameIndex);

    /**
     * @brief Lights a consumer adds, replacing its last set (a debug lantern, lights fed by others).
     *
     * They go first in every list, unfaded, until replaced. key identifies the caller (any address
     * unique to it); count 0 removes its set.
     */
    void(__cdecl* SetGiven)(const void* key, const WXL_GfxLight* lights, int count);

    /// Whether a light inside a room counts as interior (WXL_GfxLight::interior). The fog turns it on
    /// while it separates indoor from outdoor; off, every light is outdoor.
    void(__cdecl* SetInteriorGate)(int on);

    // --- textures (D3DPOOL_DEFAULT, filled with UpdateTexture: D3D9 and Vulkan consumers alike) -----------

    /**
     * @brief IDirect3DTexture9*, WXL_GFX_LIGHTS_MAX x WXL_GFX_LIGHT_ROWS, A32B32G32R32F, one column per light.
     *
     * Rows: (position - eye, radius, negative for an interior light) (linear colour * intensity,
     * cosCone) (spot axis, or a tube's half length when row 3 says so, or for a point light its fog
     * halo's reach and reference radius, 0 when the halo takes the light's own; reference radius)
     * (source size, profile, 1 = tube, flags: 1 an engine model light the engine already applies to
     * models, + 2 a light carried by another model, + 4 x the mask of the rooms it may light).
     */
    void*(__cdecl* LightTexture)(void);

    /**
     * @brief IDirect3DTexture9*, A32B32G32R32F, WXL_GFX_CLUSTER_TEX_W wide, every value exact.
     *
     * Rows: one texel per cluster (x + y * X + z * X * Y): its list's offset and length; then the
     * cookie rows: per light its rotation quaternion, then its cell code and mean transmittance rgb,
     * light i at column i % width, one block of ceil(WXL_GFX_LIGHTS_MAX / width) rows per attribute;
     * then the pool of every cluster's light indices, four to a texel. No list is ever cut.
     */
    void*(__cdecl* ClusterTexture)(void);
    void(__cdecl* ClusterTextureSize)(int* width, int* height);

    /**
     * @brief IDirect3DTexture9*, WXL_GFX_LIGHTS_MAX x 2, A32B32G32R32F: the omni slot table.
     *
     * Row 0, per light index, (slot + 1, the slot's share, receiver skip yards, 0), zero for a light
     * without a map; row 1, 20 texels a slot: (position, radius) (1 / atlas width, 1 / atlas height,
     * face size, own-housing yards) then 3 rows a face.
     *
     * What the maps hold. The core draws the casters of the engine's main sun map into them: the
     * player, NPCs and creatures (every extShadowQuality >= 1), buildings (>= 3) and doodads (5),
     * within about 40 yards of the player. Carried lights (a torch in a hand) take maps too
     * (WXL_GFX_LIGHTS_OMNI_CARRIED, on by default), and the chooser favours lights with a unit near.
     *
     * Per slot. Own-housing yards: a texel whose caster lies nearer the light than this lets the light
     * through (a lamp's cage and post; for a carried light, its item and the hand, OMNI_CARRIED_SELF).
     * Receiver skip yards (row 0 z, by light index): a receiver nearer the light than this ignores the
     * map. It is 0 for a still lamp and OMNI_CARRIED_SKIP for a carried light, so the carrier's body is
     * never darkened by its own torch while its shadow falls on everything further away. A consumer
     * tests |r - slot position| < skip and takes the light unshadowed there
     * (shaders/wxl/lights/shadows_vk.hlsli, WxlOmniSkipsReceiver).
     *
     * Which frame. The table is published before the world pass; the rows describe the maps as the
     * core renders them during that same pass: a light that moved is handed to the core at its new
     * place, its six faces are redrawn there, and its rows are shifted there already.
     */
    void*(__cdecl* OmniTexture)(void);

    /// This frame's omni slots (at most WXL_GFX_LIGHTS_OMNI_MAX); returns how many were written.
    int(__cdecl* OmniSlots)(WXL_GfxOmniSlot* out, int max);

    /// IDirect3DTexture9*, the cookie atlas (L8, or X8R8G8B8 when any cookie is tinted: read .r of an
    /// L8 one as luminance); a 1 x 1 white texture while no cookie is resident.
    void*(__cdecl* CookieAtlas)(void);

    // --- shader constants for the layouts above ------------------------------------------------------

    /// clusterC (clusters across, down, deep, texture width) and clusterD (1 / texture width,
    /// 1 / texture height, near distance, 1 / log(far / near)).
    void(__cdecl* ClusterConstants)(float clusterC[4], float clusterD[4]);

    /// A carried light's soft core, the near cap and the hot core: (core yards, cap, hot, 0).
    void(__cdecl* CarriedConstants)(float out[4]);

    /// cookieC (face width, face height in atlas coordinates, strength (0 = off), floor), cookieD (half
    /// a texel in u and v, debug view, a flame's spared share), cookieE (a glass colour's share, 0, 0, 0).
    void(__cdecl* CookieConstants)(float cookieC[4], float cookieD[4], float cookieE[4]);

    /// The share of a room's light that reaches another floor; 1 with the room gating off.
    float(__cdecl* RoomCross)(void);

    // --- rooms ------------------------------------------------------------------------------------------

    /// This frame's interior rooms, smallest first: 3 float4 rows each (q = dot(float4(r, 1), row),
    /// inside when every |q| <= 1, r camera-relative), padded. Returns the count; *rows valid until the
    /// next publish. The rooms are a working set kept by stable identity (the WMO placement and its
    /// group) with hysteresis: a room joins within 45 yd of the camera and leaves past 65 yd, whatever
    /// the view. A room joining fades in over 0.3 s and one leaving fades out before it goes (RoomWeights):
    /// a consumer that weighs its gate by it never flips. A room's index may change from frame to frame
    /// (the list stays sorted by size); the light texture's room masks follow the same frame's order.
    int(__cdecl* Rooms)(const float** rows);

    /// Whether the camera stands inside a room this frame.
    int(__cdecl* CameraIndoor)(void);

    /// The smallest room holding a camera-relative point and its floor and ceiling there
    /// (camera-relative); -1 outside every room.
    int(__cdecl* RoomOf)(const float r[3], float* floor, float* ceiling);

    // --- motion -----------------------------------------------------------------------------------------

    /// Lights that moved more than a few centimetres lately, nearest first; withCarried adds every
    /// carried light. Returns how many were written.
    int(__cdecl* MovingLights)(WXL_GfxMovingLight* out, int max, int withCarried);

    /// One line for a panel: found, of which WMO, table and carried, and used.
    const char*(__cdecl* Status)(void);

    // --- appended: present when structSize covers them --------------------------------------------------

    /// This frame's rooms' weights, 0..1, one per room in Rooms' order (1 settled, lower while fading in
    /// or out; a weight-0 room holds nothing). Returns the count; *weights valid until the next publish.
    int(__cdecl* RoomWeights)(const float** weights);
} WXL_GraphicsLightsApi;

#ifdef __cplusplus
}
#endif

#endif // WXL_GRAPHICS_LIGHTS_API_H
