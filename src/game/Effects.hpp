// effects: the missiles in flight and the particle emitters alive in the world, in world space.
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
 * @brief Read-only views of two engine lists: CMissile's in-flight missiles and the particle
 *        emitters of the models in the world M2 scene.
 *
 * Both are read as the engine left them. Valid from OnWorldSceneEnd (missiles have moved this
 * tick, models have animated this frame) until the next frame. Main thread only.
 */
namespace wxl::game::effects
{
    // --- missiles -----------------------------------------------------------------------------

    enum class MissileKind : uint8_t
    {
        Spell,   ///< a spell's missile model (model path under Spells\)
        Ranged,  ///< a weapon projectile: arrows, bullets, thrown weapons
        Other,   ///< anything else, including missiles without a model
    };

    struct Missile
    {
        float position[3];    ///< world space, as the engine advanced it this tick
        float direction[3];   ///< unit vector towards the impact point; zero once there
        float speed;          ///< yards per second, the missile's own
        float impact[3];      ///< where it is heading
        float radius;         ///< half the model's bounding diagonal times its scale; 0 without a model
        MissileKind kind;
        bool     ballistic;   ///< arcs under gravity; direction then points at the impact, not along the arc
        uint32_t spellId;     ///< the cast's spell, as the missile records it
        unsigned long long caster, target;  ///< object guids
        char     model[96];   ///< path stem of the missile model, empty without one
        const void* owner;    ///< the CMissile, stable while it flies
    };

    struct MissileQuery
    {
        float center[3] = { 0.0f, 0.0f, 0.0f };
        float radius    = 200.0f;
    };

    struct MissileStats
    {
        uint32_t inFlight;    ///< missiles on the engine's list
        uint32_t stopped;     ///< speed zero: not moving this tick
        uint32_t tooFar;
        uint32_t accepted;
        uint32_t withModel;
        uint32_t faults;
    };

    /**
     * @brief Collects the missiles in flight near a point.
     * @return how many matched, which may exceed cap; only the first cap are written.
     */
    size_t CollectMissiles(const MissileQuery& q, Missile* out, size_t cap, MissileStats* stats = nullptr);

    // --- particle emitters --------------------------------------------------------------------

    enum class EmitterKind : uint8_t
    {
        Smoke,  ///< smoke, cloud, ash, and mist drawn alpha-blended (chimneys use a mist texture)
        Steam,  ///< steam, vapour, additive mist
        Dust,   ///< dust, sand, dirt, debris
        Fire,   ///< fire, flame, embers, lava, sparks
        Other,  ///< glows, stars, water, magic
    };

    struct Emitter
    {
        float position[3];    ///< world space, through the emitter's bone as animated this frame
        float direction[3];   ///< unit emission axis (the emitter's local +Z) in world space
        float rate;           ///< particles per second, sampled this frame
        float lifespan;       ///< seconds, sampled this frame
        float speed;          ///< emission speed, sampled this frame
        float sizeMin, sizeMax;  ///< particle scale over its life, from the file, times model scale
        uint32_t liveParticles;  ///< particles currently alive
        uint8_t  blend;       ///< the record's blend mode: 2 alpha, 4 additive, ...
        uint8_t  shape;       ///< 1 plane, 2 sphere, 3 spline
        EmitterKind kind;
        char     texture[96]; ///< texture path, empty when the texture is replaceable
        const void* owner;    ///< the M2 instance
        uint32_t index;       ///< emitter index within the model
    };

    struct EmitterQuery
    {
        float center[3] = { 0.0f, 0.0f, 0.0f };
        float radius    = 60.0f;
        /// Models last animated up to this many frames ago still count; their bones are from then.
        uint32_t maxStaleFrames = 0;
        bool includeDisabled = false;   ///< keep emitters whose enable track is off this frame
    };

    struct EmitterStats
    {
        uint32_t models;       ///< instances walked in the world scene
        uint32_t withEmitters; ///< loaded, animated and carrying emitters
        uint32_t stale;        ///< models not animated recently
        uint32_t emitters;
        uint32_t disabled;
        uint32_t tooFar;
        uint32_t accepted;
        uint32_t byKind[5];    ///< accepted, indexed by EmitterKind
        uint32_t faults;
    };

    /**
     * @brief Collects the particle emitters of world-scene models near a point.
     * @return how many matched, which may exceed cap; only the first cap are written.
     */
    size_t CollectEmitters(const EmitterQuery& q, Emitter* out, size_t cap, EmitterStats* stats = nullptr);

    /// The classification CollectEmitters applies, from a texture path and a blend mode.
    EmitterKind ClassifyEmitter(const char* texture, uint8_t blend);
}
