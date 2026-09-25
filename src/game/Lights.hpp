// lights: the point light sources the client knows about this frame, in world space.
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
 * @brief Enumerates point lights near a place: the M2 model lights the engine evaluated this frame,
 *        and, on request, the lights WMO files author but the 3.3.5a renderer never uses.
 *
 * M2 lights are the engine's own: the same CM2Light objects it files into the world scene's light
 * grid and lights nearby models with, read after its animate pass wrote them. Torches, braziers,
 * lanterns and spell glows are all of this kind -- the client has no other dynamic light. They are
 * rare: about a hundred of the 3.3.5a client's models carry one (campfires, orc braziers, Dalaran
 * lamps, fire totems, a few spell missiles); street lamps, wall torches and windows do not.
 * CollectModels lists the world scene's model instances, so a caller can give such models light of
 * its own (the content decision stays with the caller).
 *
 * Valid from OnWorldSceneEnd (the frame's animate pass has run) until the next frame's; anywhere
 * else reads the previous frame's values. Main thread only.
 */
namespace wxl::game::lights
{
    enum class Kind : uint8_t
    {
        M2,  ///< a model light, evaluated by the engine this frame
        Wmo, ///< a MOLT entry, transformed by its placement; static, never lit by the engine
    };

    struct PointLight
    {
        float position[3];    ///< world space
        /// Diffuse the engine shades with, intensity already applied, so it may exceed 1. The
        /// client works without gamma conversion; these are its working values, not linearised.
        float color[3];
        float ambient[3];     ///< M2 ambient term, intensity applied; zero for WMO
        float intensity;      ///< the factor folded into color, so color / intensity is the hue
        /// Authored radius where the light starts to fade and where it reaches zero, in yards.
        /// Taken from the file (M2 attenuation tracks, MOLT fields); the 3.3.5a renderer ignores
        /// both and uses falloff below. Zero when the file carries none.
        float attenStart;
        float attenEnd;
        /// Falloff the engine hands its shaders, 1 / (f0 + f1 d + f2 d^2); zero for WMO.
        float falloff[3];
        Kind  kind;
        /// Owner identity: the M2 instance or the WMO placement. Stable while the owner lives,
        /// never dereferenced by this API's callers.
        const void* owner;
        uint32_t    index;    ///< light index within the owner
    };

    struct Query
    {
        float center[3] = { 0.0f, 0.0f, 0.0f };
        float radius    = 100.0f;   ///< lights farther than this from center are skipped
        bool  m2        = true;
        bool  wmo       = false;
        /// M2 lights whose model last animated up to this many frames ago are kept, with their last
        /// values. 0 keeps exactly the models the engine animated this frame;
        /// it does not animate a model it culled (typically an off-screen one).
        uint32_t maxStaleFrames = 0;
    };

    /// Where each candidate went during one Collect, for diagnosing an empty result.
    struct Stats
    {
        uint32_t frame;          ///< scene frame the walk compared stamps against
        uint32_t models;         ///< instances walked in the world scene
        uint32_t notLoaded;
        uint32_t noRecords;      ///< loaded, but no light records or no header
        uint32_t noLights;       ///< the model file carries no M2Light at all
        uint32_t litModels;      ///< models carrying at least one M2Light
        uint32_t notPoint;       ///< directional lights
        uint32_t stale;          ///< stamp older than maxStaleFrames
        uint32_t hidden;         ///< switched off (visibility track or engine verdict)
        uint32_t tooFar;
        uint32_t accepted;
        uint32_t wmoPlacements;
        uint32_t wmoSkipped;     ///< placement flagged, not loaded, or outside the radius
        uint32_t wmoEntries;     ///< MOLT entries examined
        uint32_t wmoNotOmni;
        uint32_t wmoTooFar;
        uint32_t wmoAccepted;
        uint32_t faults;         ///< access violations that stopped a walk
        /// Path stems of the first lit models met, in walk order.
        char     litNames[8][96];
        uint32_t litNameCount;
    };

    /**
     * @brief Collects the point lights matching a query.
     * @param q      what and where to look.
     * @param out    receives up to cap lights, in no particular order.
     * @param cap    capacity of out.
     * @param stats  when given, receives a count of every candidate by outcome; costs a little.
     * @return how many lights matched, which may exceed cap; only the first cap are written.
     *
     * Returns 0 without a world. A fault while reading engine memory stops the walk and returns
     * what was collected before it.
     */
    size_t Collect(const Query& q, PointLight* out, size_t cap, Stats* stats = nullptr);

    /// One model instance of the world M2 scene.
    struct ModelInstance
    {
        const void* owner;        ///< the instance; stable while it lives, never to be dereferenced
        char     stem[96];        ///< the model's path stem as the engine keeps it (case as stored)
        float    toWorld[16];     ///< placement, row vectors: world = p * M, translation in 12..14
        uint32_t age;             ///< scene frames since the instance last animated (0 = this frame)
        uint32_t lightCount;      ///< M2 lights its model file carries
        bool     attached;        ///< rides another model's matrix (a parent is set)
    };

    struct ModelQuery
    {
        float center[3] = { 0.0f, 0.0f, 0.0f };
        float radius    = 100.0f;   ///< instances whose origin is farther than this are skipped
        uint32_t maxStaleFrames = 0;
        bool  unlitOnly       = true;    ///< only models that carry no M2 light
        bool  includeAttached = false;   ///< include instances attached to another model
    };

    /**
     * @brief Lists the world scene's loaded model instances matching a query.
     * @return how many matched, which may exceed cap; only the first cap are written. A fault while
     *         reading engine memory stops the walk.
     */
    size_t CollectModels(const ModelQuery& q, ModelInstance* out, size_t cap);

    /// The world M2 scene's frame counter, so a caller can tell whether a new frame has animated.
    uint32_t SceneFrame();

    /// One placed WMO (a map-object definition): the owner PointLight::owner reports for a WMO light.
    struct WmoPlacement
    {
        const void* owner;        ///< the placement; stable while it lives, never to be dereferenced
        /// The CMapObj root it places, shared by every placement of the same file: what
        /// wxl::game::wmo::GetGroupInfo reads the group table from, drawn or not.
        const void* root;
        char     path[128];       ///< the root file path as the client keeps it (case as stored)
        float    toWorld[16];     ///< root space to world, row vectors: world = p * M, translation in 12..14
        float    boundsMin[3];    ///< world AABB
        float    boundsMax[3];
        uint32_t lightCount;      ///< MOLT entries of the root (0 until it is parsed)
    };

    /**
     * @brief Lists the placed WMOs whose world box touches a sphere.
     * @return how many matched, which may exceed cap; only the first cap are written. A fault while
     *         reading engine memory stops the walk.
     */
    size_t CollectWmoPlacements(const float center[3], float radius, WmoPlacement* out, size_t cap);
}
