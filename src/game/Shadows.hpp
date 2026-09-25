// shadows: the engine's own exterior shadow maps, read after it rendered them this frame.
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

#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief Read-only view of CShadowCache: the sun shadow maps the client renders when the
 *        extShadowQuality CVar is above 0, and a way to run code after its four callbacks.
 *
 * Modes (extShadowQuality): 0 none; 1-2 characters only, 1024/2048 map; 3-4 adds WMOs and doodads
 * with three cascade bands updated a tile per frame; 5 renders the three cascades every frame.
 * Terrain receives but never casts. The light follows the sun with its height exaggerated five times.
 *
 * Encoding. Without hwPCF a map is an R32F colour texture holding linear light-space depth times
 * 1/4000 (depth from the light's near plane; the light sits 2000 yd back from the centre). With
 * hwPCF it is a D24X8 depth texture, meant for hardware comparison (tex2Dproj / texldb).
 *
 * Lookup. For a position p in the space the receiver shaders use (view space: p * view, where view
 * is Snapshot::view), s = (dot(rows[0], p1), dot(rows[1], p1), dot(rows[2], p1)) with p1 = (p, 1);
 * uv = s.xy * 0.5 + 0.5; the texel is in shadow when stored depth < s.z. The rows already carry the
 * light's orthographic projection, the y flip and the depth bias.
 *
 * Call from OnWorldSceneEnd. Main thread only.
 */
namespace wxl::game::shadows
{
    struct Map
    {
        void* texture;       ///< IDirect3DTexture9*, what the engine's receivers sample; null when absent
        float rows[3][4];    ///< view-space position -> (u', v', depth) as described above
        float halfExtent;    ///< yards from centre to edge
        float centre[3];     ///< world position the map was rendered around
        float depthBias;     ///< yards the engine pulls depth back by for this map (already in rows)
        bool  present;
    };

    enum class Slot : uint8_t { Main = 0, Interior = 1, Band0 = 2, Band1 = 3, Band2 = 4, Count = 5 };

    struct Snapshot
    {
        bool     valid;        ///< mode > 0, textures in place, and the engine updated them this frame
        uint32_t generation;   ///< changes whenever a texture is recreated; drop cached pointers then
        int      mode;         ///< extShadowQuality, 0..5
        int      tier;         ///< receiver shader tier, 0..3
        bool     hwPcf;        ///< maps are D24X8 depth textures rather than R32F
        uint32_t size;         ///< map edge in texels
        float    lightDir[3];     ///< world direction the light travels
        float    lightDirView[3]; ///< the same in view space (what the receivers' pixel c4 holds)
        float    view[16];        ///< the view matrix current at the call, row-vector convention
        float    cameraPos[3];    ///< world camera position; view is relative to it
        Map      maps[size_t(Slot::Count)];
    };

    /**
     * @brief Reads the shadow state.
     * @return out.valid. Everything else is filled as far as it exists even when invalid.
     */
    bool Get(Snapshot& out);

    // --- extension point: run after the engine's callbacks ---------------------------------------

    enum class Callback : uint8_t { Matrix = 0, Frustum = 1, Query = 2, Render = 3 };

    /// The raw arguments the engine passed (see offsets/engine/Shadows.hpp for each signature) and
    /// the stock callback's return value (meaningless for Matrix and Frustum).
    struct CallbackArgs
    {
        Callback  which;
        uintptr_t arg[5];
        int       result;
    };

    using AfterFn = void(__cdecl*)(const CallbackArgs& args, void* user);

    /**
     * @brief Runs fn after the stock callback every time the engine calls it.
     * @return false when the engine has not installed its callbacks yet (retry later) or the slot
     *         already holds 8 subscribers. The wrapper re-installs itself if the engine re-initialises.
     */
    bool ChainAfter(Callback which, AfterFn fn, void* user);

    /**
     * @brief One Render callback described: which map it drew into and the transforms it drew with,
     *        for a subscriber that adds casters of its own to that map (ChainAfter(Callback::Render)).
     *
     * view * proj takes a camera-relative world position (world - cameraPos) to the clip space the
     * engine's casters used: xy from the pass's own orthographic rectangle (a sub-rectangle of the
     * map below mode 5, matched by viewport), z = (light-view z - 1) / 3999 in 0..1 as the D3D
     * device stores it. The engine's caster pixel shader writes light-view z * depthScale to the
     * colour map (max 0); a subscriber must write the same to stay comparable.
     */
    struct RenderPass
    {
        Slot  slot;            ///< the map: Main, Interior or a band; Count when none matched
        int   index;           ///< the engine's slot argument (0 main and interior, the band index)
        float centre[3];       ///< world centre the pass renders around (texel snapped)
        float halfExtent;      ///< yards from centre to edge of the whole map
        float lightDir[3];     ///< world direction the light travels
        float up[3];           ///< the up vector the pass's look-at used
        float cameraPos[3];    ///< world camera position the view is relative to
        float view[16];        ///< camera-relative world -> light view, row vectors (the engine's look-at)
        float proj[16];        ///< light view -> clip, row vectors, D3D depth range
        float viewport[4];     ///< x0, x1, y0, y1 fractions of the map the pass drew into
        float depthScale;      ///< 1 / 4000, what the colour map holds per yard of light-view z
        void* colour;          ///< IDirect3DTexture9* the pass drew colour into, or null
        void* depth;           ///< IDirect3DTexture9* the pass drew depth into, or null (the scene's)
        bool  hwPcf;
        bool  drewNothing;     ///< the engine's caster lists were empty (it only cleared the map)
    };

    /// Fills out from a Render callback's arguments; false for any other callback or when the
    /// query pointer is missing. Read inside the callback: the query is the caller's stack frame.
    bool DescribeRender(const CallbackArgs& args, RenderPass& out);
}
