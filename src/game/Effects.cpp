// effects: walks CMissile's in-flight list and the world M2 scene's particle emitters.
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

#include "game/Effects.hpp"

#include "offsets/game/Effects.hpp"
#include "offsets/game/Lights.hpp"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <initializer_list>

namespace wxl::game::effects
{
    namespace
    {
        namespace off   = wxl::offsets::game::effects;
        namespace scene = wxl::offsets::game::lights;

        constexpr uint32_t  kMaxMissiles   = 4096;
        constexpr uint32_t  kMaxModels     = 200000;
        constexpr uint32_t  kMaxEmitters   = 256;
        constexpr uintptr_t kMinPointer    = 0x10000;

        template <class T>
        inline T At(uintptr_t base, size_t offset) { return *reinterpret_cast<const T*>(base + offset); }

        inline bool Plausible(uintptr_t p) { return p >= kMinPointer; }

        /// An M2Array pointer that the loader may or may not have rebased onto the file buffer.
        inline uintptr_t Resolve(uintptr_t header, uint32_t fileSize, uintptr_t v)
        {
            return v && v < fileSize ? header + v : v;
        }

        inline float DistSq(const float a[3], const float b[3])
        {
            const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            return dx * dx + dy * dy + dz * dz;
        }

        inline float Normalize(float v[3])
        {
            const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len > 1e-6f) { v[0] /= len; v[1] /= len; v[2] /= len; }
            else v[0] = v[1] = v[2] = 0.0f;
            return len;
        }

        /// Row-vector product: out = a * b.
        void Mul(const float* a, const float* b, float out[16])
        {
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
        }

        void Transform(const float p[3], const float* m, float out[3])
        {
            for (int k = 0; k < 3; ++k) out[k] = p[0] * m[k] + p[1] * m[4 + k] + p[2] * m[8 + k] + m[12 + k];
        }

        void CopyText(char* dst, size_t cap, const char* src, size_t maxLen)
        {
            size_t i = 0;
            for (; i + 1 < cap && i < maxLen && src[i]; ++i) dst[i] = src[i];
            dst[i] = 0;
        }

        bool Contains(const char* s, const char* needle)
        {
            const size_t n = std::strlen(needle);
            for (; *s; ++s)
                if (_strnicmp(s, needle, n) == 0) return true;
            return false;
        }

        bool ContainsAny(const char* s, std::initializer_list<const char*> needles)
        {
            for (const char* n : needles) if (Contains(s, n)) return true;
            return false;
        }

        /// Path stem of an instance's model, or empty.
        void ModelStem(uintptr_t inst, char* dst, size_t cap)
        {
            dst[0] = 0;
            const uintptr_t model = Plausible(inst) ? At<uintptr_t>(inst, scene::kOffInstModel) : 0;
            if (!Plausible(model)) return;
            CopyText(dst, cap, reinterpret_cast<const char*>(model + scene::kOffModelPathStem),
                     scene::kOffModelHeader - scene::kOffModelPathStem);
        }

        /// Half the model's bounding diagonal times the instance's scale.
        float ModelRadius(uintptr_t inst)
        {
            const uintptr_t model = At<uintptr_t>(inst, scene::kOffInstModel);
            if (!Plausible(model) || !(At<uint32_t>(inst, scene::kOffInstFlags) & scene::kInstFlagLoaded)) return 0.0f;
            const uintptr_t header = At<uintptr_t>(model, scene::kOffModelHeader);
            if (!Plausible(header)) return 0.0f;
            const float* b = reinterpret_cast<const float*>(header + off::kOffHeaderBounds);
            const float dx = b[3] - b[0], dy = b[4] - b[1], dz = b[5] - b[2];
            const float* m = reinterpret_cast<const float*>(inst + scene::kOffInstPlacement);
            const float scale = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
            return 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz) * (scale > 1e-4f ? scale : 1.0f);
        }

        // --- missiles ---------------------------------------------------------------------------

        void WalkMissiles(const MissileQuery& q, Missile* out, size_t cap, size_t& matched, MissileStats* s)
        {
            const float radiusSq = q.radius * q.radius;
            uintptr_t m = *reinterpret_cast<const uintptr_t*>(off::kInFlightMissiles);
            for (uint32_t guard = 0; Plausible(m) && guard < kMaxMissiles; ++guard, m = At<uintptr_t>(m, off::kOffMissileNext))
            {
                if (s) ++s->inFlight;
                const float speed = At<float>(m, off::kOffMissileSpeed);
                if (speed == 0.0f) { if (s) ++s->stopped; continue; }
                const float* pos = reinterpret_cast<const float*>(m + off::kOffMissilePosition);
                if (DistSq(pos, q.center) > radiusSq) { if (s) ++s->tooFar; continue; }

                if (s) ++s->accepted;
                Missile* o = matched < cap ? &out[matched] : nullptr;
                ++matched;
                if (!o) continue;

                const bool onTransport = At<unsigned long long>(m, off::kOffMissileTransport) != 0;
                const float* impact = reinterpret_cast<const float*>(m + (onTransport ? off::kOffMissileImpactT : off::kOffMissileImpact));
                for (int k = 0; k < 3; ++k)
                {
                    o->position[k]  = pos[k];
                    o->impact[k]    = impact[k];
                    o->direction[k] = impact[k] - pos[k];
                }
                Normalize(o->direction);
                o->speed     = speed;
                o->ballistic = (At<uint32_t>(m, off::kOffMissileFlags) & off::kMissileBallistic) != 0;
                o->spellId   = At<uint32_t>(m, off::kOffMissileSpell);
                o->caster    = At<unsigned long long>(m, off::kOffMissileCaster);
                o->target    = At<unsigned long long>(m, off::kOffMissileTarget);
                o->owner     = reinterpret_cast<const void*>(m);

                const uintptr_t inst = At<uintptr_t>(m, off::kOffMissileModel);
                ModelStem(inst, o->model, sizeof o->model);
                o->radius = Plausible(inst) ? ModelRadius(inst) : 0.0f;
                if (o->model[0] && s) ++s->withModel;

                if (ContainsAny(o->model, { "ammo", "arrow", "bullet", "thrown" })) o->kind = MissileKind::Ranged;
                else if (_strnicmp(o->model, "spells\\", 7) == 0 || _strnicmp(o->model, "spells/", 7) == 0)
                    o->kind = MissileKind::Spell;
                else o->kind = MissileKind::Other;
            }
        }

        // --- emitters ---------------------------------------------------------------------------

        void WalkEmitters(const EmitterQuery& q, Emitter* out, size_t cap, size_t& matched, EmitterStats* s)
        {
            const uintptr_t sc = *reinterpret_cast<const uintptr_t*>(scene::kWorldM2Scene);
            if (!Plausible(sc)) return;
            const uint32_t frame    = At<uint32_t>(sc, scene::kOffSceneFrame);
            const float*   toWorld  = reinterpret_cast<const float*>(sc + off::kOffScenePaletteToWorld);
            const float    radiusSq = q.radius * q.radius;

            uintptr_t inst = At<uintptr_t>(sc, scene::kOffSceneModelHead);
            for (uint32_t guard = 0; Plausible(inst) && guard < kMaxModels;
                 ++guard, inst = At<uintptr_t>(inst, scene::kOffModelSceneNext))
            {
                if (s) ++s->models;
                if (!(At<uint32_t>(inst, scene::kOffInstFlags) & scene::kInstFlagLoaded)) continue;
                const uintptr_t model  = At<uintptr_t>(inst, scene::kOffInstModel);
                const uintptr_t header = Plausible(model) ? At<uintptr_t>(model, scene::kOffModelHeader) : 0;
                if (!Plausible(header)) continue;
                const uint32_t count = At<uint32_t>(header, off::kOffHeaderEmitterCount);
                if (!count || count > kMaxEmitters) continue;
                const uint32_t  fileSize = At<uint32_t>(model, off::kOffModelFileSize);
                const uintptr_t defs     = Resolve(header, fileSize, At<uintptr_t>(header, off::kOffHeaderEmitters));
                const uintptr_t cells    = At<uintptr_t>(inst, off::kOffInstEmitterCells);
                const uintptr_t objects  = At<uintptr_t>(inst, off::kOffInstEmitters);
                const uintptr_t palette  = At<uintptr_t>(inst, off::kOffInstBonePalette);
                if (!Plausible(defs) || !Plausible(cells) || !Plausible(objects) || !Plausible(palette)) continue;
                if (frame - At<uint32_t>(inst, scene::kOffInstLastAnimFrame) > q.maxStaleFrames) { if (s) ++s->stale; continue; }
                if (s) ++s->withEmitters;

                const uint32_t  bones    = At<uint32_t>(header, off::kOffHeaderBoneCount);
                const uint32_t  texCount = At<uint32_t>(header, off::kOffHeaderTextureCount);
                const uintptr_t textures = Resolve(header, fileSize, At<uintptr_t>(header, off::kOffHeaderTextures));

                for (uint32_t i = 0; i < count; ++i)
                {
                    if (s) ++s->emitters;
                    const uintptr_t def  = defs + i * off::kEmitterRecordStride;
                    const uintptr_t cell = cells + i * off::kEmitterCellStride;
                    if (!q.includeDisabled && !At<uint8_t>(cell, off::kOffCellEnabled)) { if (s) ++s->disabled; continue; }
                    const uint16_t bone = At<uint16_t>(def, off::kOffEmBone);
                    if (bone >= bones) continue;

                    float m[16];
                    Mul(reinterpret_cast<const float*>(palette + bone * 0x40), toWorld, m);
                    float pos[3];
                    Transform(reinterpret_cast<const float*>(def + off::kOffEmPosition), m, pos);
                    if (DistSq(pos, q.center) > radiusSq) { if (s) ++s->tooFar; continue; }

                    if (s) ++s->accepted;
                    Emitter* o = matched < cap ? &out[matched] : nullptr;
                    ++matched;

                    char tex[96] = {};
                    const uint32_t ti = At<uint16_t>(def, off::kOffEmTexture) & 0x1F;
                    if (Plausible(textures) && ti < texCount)
                    {
                        const uintptr_t t   = textures + ti * off::kTextureStride;
                        const uint32_t  len = At<uint32_t>(t, off::kOffTextureName);
                        const uintptr_t str = Resolve(header, fileSize, At<uintptr_t>(t, off::kOffTextureName + 4));
                        if (len && Plausible(str)) CopyText(tex, sizeof tex, reinterpret_cast<const char*>(str), len);
                    }
                    const uint8_t     blend = At<uint8_t>(def, off::kOffEmBlend);
                    const EmitterKind kind  = ClassifyEmitter(tex, blend);
                    if (s) ++s->byKind[uint32_t(kind)];
                    if (!o) continue;

                    for (int k = 0; k < 3; ++k) { o->position[k] = pos[k]; o->direction[k] = m[8 + k]; }
                    Normalize(o->direction);
                    const float scale = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);

                    // Scale keys over the particle's life: {count, pointer} of C2Vector at +8 of the block.
                    float lo = 0.0f, hi = 0.0f;
                    const uint32_t  keys = At<uint32_t>(def, off::kOffEmScaleBlock + 8);
                    const uintptr_t vals = Resolve(header, fileSize, At<uintptr_t>(def, off::kOffEmScaleBlock + 12));
                    for (uint32_t k = 0; Plausible(vals) && k < keys && k < 16; ++k)
                    {
                        const float v = At<float>(vals, k * 8);
                        if (k == 0 || v < lo) lo = v;
                        if (k == 0 || v > hi) hi = v;
                    }
                    o->sizeMin = lo * scale;
                    o->sizeMax = hi * scale;

                    o->rate     = At<float>(cell, off::kOffCellRate);
                    o->lifespan = At<float>(cell, off::kOffCellLifespan);
                    o->speed    = At<float>(cell, off::kOffCellSpeed);
                    const uintptr_t em = At<uintptr_t>(objects, i * 4);
                    o->liveParticles = Plausible(em) ? At<uint32_t>(em, off::kOffEmitterLiveCount) : 0;
                    o->blend = blend;
                    o->shape = At<uint8_t>(def, off::kOffEmType);
                    o->kind  = kind;
                    std::memcpy(o->texture, tex, sizeof tex);
                    o->owner = reinterpret_cast<const void*>(inst);
                    o->index = i;
                }
            }
        }

        // No C++ objects here, so structured exception handling may guard the raw reads.
        void GuardMissiles(const MissileQuery& q, Missile* out, size_t cap, size_t& matched, MissileStats* s)
        {
            __try { WalkMissiles(q, out, cap, matched, s); }
            __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
            { if (s) ++s->faults; }
        }

        void GuardEmitters(const EmitterQuery& q, Emitter* out, size_t cap, size_t& matched, EmitterStats* s)
        {
            __try { WalkEmitters(q, out, cap, matched, s); }
            __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
            { if (s) ++s->faults; }
        }
    }

    EmitterKind ClassifyEmitter(const char* texture, uint8_t blend)
    {
        if (!texture || !*texture) return EmitterKind::Other;
        const char* name = texture;
        for (const char* p = texture; *p; ++p) if (*p == '\\' || *p == '/') name = p + 1;

        if (ContainsAny(name, { "steam", "vapor", "vapour" })) return EmitterKind::Steam;
        if (Contains(name, "mist")) return blend == 2 ? EmitterKind::Smoke : EmitterKind::Steam;
        if (ContainsAny(name, { "smoke", "cloud", "fog", "haze", "puff", "soot", "spore" })) return EmitterKind::Smoke;
        if (ContainsAny(name, { "dust", "sand", "dirt", "debris", "partrock", "pebble" })) return EmitterKind::Dust;
        if (Contains(name, "sparkle")) return EmitterKind::Other; // glitter, not sparks
        if (ContainsAny(name, { "fire", "flame", "ember", "lava", "burn", "spark", "torch", "candle", "blaze" }))
            return EmitterKind::Fire;
        return EmitterKind::Other;
    }

    size_t CollectMissiles(const MissileQuery& q, Missile* out, size_t cap, MissileStats* stats)
    {
        if (stats) *stats = MissileStats{};
        size_t matched = 0;
        GuardMissiles(q, out, out ? cap : 0, matched, stats);
        return matched;
    }

    size_t CollectEmitters(const EmitterQuery& q, Emitter* out, size_t cap, EmitterStats* stats)
    {
        if (stats) *stats = EmitterStats{};
        size_t matched = 0;
        GuardEmitters(q, out, out ? cap : 0, matched, stats);
        return matched;
    }
}
