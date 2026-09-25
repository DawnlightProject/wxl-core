// lights: walks the world M2 scene's model lights and the placed WMOs' MOLT tables.
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

#include "game/Lights.hpp"

#include "offsets/game/Lights.hpp"
#include "offsets/game/WMO.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>

namespace wxl::game::lights
{
    namespace
    {
        namespace off = wxl::offsets::game::lights;

        // Guards against a list the engine is rebuilding under us; far above any real scene.
        constexpr uint32_t kMaxModels     = 200000;
        constexpr uint32_t kMaxPlacements = 20000;
        constexpr uint32_t kMaxPerOwner   = 1024;
        constexpr uintptr_t kMinPointer   = 0x10000;

        template <class T>
        inline T At(uintptr_t base, size_t offset) { return *reinterpret_cast<const T*>(base + offset); }

        inline bool Plausible(uintptr_t p) { return p >= kMinPointer; }

        inline float DistSq(const float a[3], const float b[3])
        {
            const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            return dx * dx + dy * dy + dz * dz;
        }

        struct Sink
        {
            PointLight* out;
            size_t      cap;
            size_t      matched;
            Stats*      stats;

            PointLight* Next()
            {
                PointLight* slot = matched < cap ? &out[matched] : nullptr;
                ++matched;
                return slot;
            }
        };

        /// First key of a float track: animation slot 0, else the slot the bone plays, else 0.
        float TrackFirstKey(uintptr_t track, uint32_t animSlot)
        {
            const uint32_t  count = At<uint32_t>(track, off::kOffTrackValuesCount);
            const uintptr_t inner = At<uintptr_t>(track, off::kOffTrackValuesPtr);
            if (!count || !Plausible(inner)) return 0.0f;

            const uint32_t slots[2] = { 0, animSlot < count ? animSlot : 0 };
            for (uint32_t slot : slots)
            {
                const uintptr_t arr = inner + slot * off::kInnerArrayStride;
                const uint32_t  n   = At<uint32_t>(arr, 0);
                const uintptr_t p   = At<uintptr_t>(arr, 4);
                if (n && Plausible(p)) return At<float>(p, 0);
            }
            return 0.0f;
        }

        // Counts one outcome when statistics were asked for.
        #define WXL_LIGHT_COUNT(field) do { if (sink.stats) ++sink.stats->field; } while (0)

        void NoteLitModel(Sink& sink, uintptr_t model)
        {
            Stats* s = sink.stats;
            if (!s) return;
            ++s->litModels;
            if (s->litNameCount >= 8) return;
            const char* stem = reinterpret_cast<const char*>(model + off::kOffModelPathStem);
            char* dst = s->litNames[s->litNameCount++];
            size_t i = 0;
            for (; i + 1 < sizeof s->litNames[0] && stem[i]; ++i) dst[i] = stem[i];
            dst[i] = 0;
        }

        void CollectModelLights(const Query& q, Sink& sink)
        {
            const uintptr_t scene = *reinterpret_cast<const uintptr_t*>(off::kWorldM2Scene);
            if (!Plausible(scene)) return;
            const uint32_t frame    = At<uint32_t>(scene, off::kOffSceneFrame);
            const float    radiusSq = q.radius * q.radius;
            if (sink.stats) sink.stats->frame = frame;

            uintptr_t inst = At<uintptr_t>(scene, off::kOffSceneModelHead);
            for (uint32_t guard = 0; Plausible(inst) && guard < kMaxModels;
                 ++guard, inst = At<uintptr_t>(inst, off::kOffModelSceneNext))
            {
                WXL_LIGHT_COUNT(models);
                if (!(At<uint32_t>(inst, off::kOffInstFlags) & off::kInstFlagLoaded)) { WXL_LIGHT_COUNT(notLoaded); continue; }
                const uintptr_t model = At<uintptr_t>(inst, off::kOffInstModel);
                const uintptr_t header = Plausible(model) ? At<uintptr_t>(model, off::kOffModelHeader) : 0;
                if (!Plausible(header)) { WXL_LIGHT_COUNT(noRecords); continue; }
                const uint32_t  count = At<uint32_t>(header, off::kOffHeaderLightCount);
                const uintptr_t defs  = At<uintptr_t>(header, off::kOffHeaderLights);
                if (!count) { WXL_LIGHT_COUNT(noLights); continue; }
                const uintptr_t records = At<uintptr_t>(inst, off::kOffInstLightRecords);
                if (count > kMaxPerOwner || !Plausible(defs) || !Plausible(records)) { WXL_LIGHT_COUNT(noRecords); continue; }
                NoteLitModel(sink, model);

                for (uint32_t i = 0; i < count; ++i)
                {
                    const uintptr_t rec   = records + i * off::kRecordStride;
                    const uintptr_t light = rec + off::kOffRecLight;
                    if (At<uint32_t>(light, off::kOffLightType) != off::kLightTypePoint) { WXL_LIGHT_COUNT(notPoint); continue; }

                    const uint32_t age = frame - At<uint32_t>(light, off::kOffLightStamp);
                    if (age > q.maxStaleFrames) { WXL_LIGHT_COUNT(stale); continue; }
                    // This frame's lights carry the engine's own verdict; older ones their last tracks.
                    const bool on = age == 0
                        ? At<uint32_t>(light, off::kOffLightVisible) != 0
                        : At<uint32_t>(rec, off::kOffRecEnabled) != 0 && At<uint8_t>(rec, off::kOffRecVisible) != 0;
                    if (!on) { WXL_LIGHT_COUNT(hidden); continue; }

                    const float* pos = reinterpret_cast<const float*>(light + off::kOffLightPosition);
                    if (DistSq(pos, q.center) > radiusSq) { WXL_LIGHT_COUNT(tooFar); continue; }

                    WXL_LIGHT_COUNT(accepted);
                    PointLight* o = sink.Next();
                    if (!o) continue;

                    const float* diffuse = reinterpret_cast<const float*>(light + off::kOffLightDiffuse);
                    const float* ambient = reinterpret_cast<const float*>(light + off::kOffLightAmbient);
                    const float* falloff = reinterpret_cast<const float*>(light + off::kOffLightAtten);
                    const float* hue     = reinterpret_cast<const float*>(rec + off::kOffRecDiffuseColor);
                    for (int k = 0; k < 3; ++k)
                    {
                        o->position[k] = pos[k];
                        o->color[k]    = diffuse[k];
                        o->ambient[k]  = ambient[k];
                        o->falloff[k]  = falloff[k];
                    }
                    // What was folded in: the ratio on the strongest channel of the colour track.
                    int best = 0;
                    for (int k = 1; k < 3; ++k) if (hue[k] > hue[best]) best = k;
                    o->intensity = hue[best] > 1e-6f ? diffuse[best] / hue[best] : 0.0f;

                    const uintptr_t def   = defs + i * off::kDefStride;
                    const uint16_t  bone  = At<uint16_t>(def, off::kOffDefBone);
                    const uintptr_t bones = At<uintptr_t>(inst, off::kOffInstBoneStates);
                    const uint32_t  slot  = Plausible(bones)
                        ? At<uint16_t>(bones + bone * off::kBoneStateStride, off::kOffBoneStateAnimIdx)
                        : 0;
                    o->attenStart = TrackFirstKey(def + off::kOffDefAttenStart, slot);
                    o->attenEnd   = TrackFirstKey(def + off::kOffDefAttenEnd, slot);
                    o->kind  = Kind::M2;
                    o->owner = reinterpret_cast<const void*>(inst);
                    o->index = i;
                }
            }
        }

        bool SphereTouchesBox(const float c[3], float r, const float* mn, const float* mx)
        {
            float d = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                const float v = c[k] < mn[k] ? mn[k] - c[k] : (c[k] > mx[k] ? c[k] - mx[k] : 0.0f);
                d += v * v;
            }
            return d <= r * r;
        }

        void CollectWmoLights(const Query& q, Sink& sink)
        {
            const uint32_t linkOffset = *reinterpret_cast<const uint32_t*>(off::kMapObjDefLinkOffset);
            const float    radiusSq   = q.radius * q.radius;

            uintptr_t def = *reinterpret_cast<const uintptr_t*>(off::kMapObjDefHead);
            for (uint32_t guard = 0; guard < kMaxPlacements; ++guard)
            {
                if ((def & 1) || !Plausible(def)) return;
                const uintptr_t next = At<uintptr_t>(def + linkOffset, 4);
                WXL_LIGHT_COUNT(wmoPlacements);

                const float* mn = reinterpret_cast<const float*>(def + off::kOffDefBoundsMin);
                const float* mx = reinterpret_cast<const float*>(def + off::kOffDefBoundsMax);
                const uintptr_t root = At<uintptr_t>(def, off::kOffDefMapObj);
                if (!(At<uint32_t>(def, off::kOffDefFlags) & off::kDefFlagNoLights) && Plausible(root) &&
                    At<uint32_t>(root, off::kOffRootLoaded) && SphereTouchesBox(q.center, q.radius, mn, mx))
                {
                    const uintptr_t molt  = At<uintptr_t>(root, off::kOffRootMolt);
                    const uint32_t  count = At<uint32_t>(root, off::kOffRootMoltCount);
                    const float*    m     = reinterpret_cast<const float*>(def + off::kOffDefToWorld);
                    for (uint32_t i = 0; Plausible(molt) && i < count && i < kMaxPerOwner; ++i)
                    {
                        const uintptr_t e = molt + i * off::kMoltStride;
                        WXL_LIGHT_COUNT(wmoEntries);
                        if (At<uint8_t>(e, off::kOffMoltType) > 1) { WXL_LIGHT_COUNT(wmoNotOmni); continue; } // omni and spot only

                        const float* p = reinterpret_cast<const float*>(e + off::kOffMoltPosition);
                        const float world[3] = {
                            p[0] * m[0] + p[1] * m[4] + p[2] * m[8]  + m[12],
                            p[0] * m[1] + p[1] * m[5] + p[2] * m[9]  + m[13],
                            p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14],
                        };
                        if (DistSq(world, q.center) > radiusSq) { WXL_LIGHT_COUNT(wmoTooFar); continue; }

                        WXL_LIGHT_COUNT(wmoAccepted);
                        PointLight* o = sink.Next();
                        if (!o) continue;

                        const uint8_t* bgra      = reinterpret_cast<const uint8_t*>(e + off::kOffMoltColor);
                        const float    intensity = At<float>(e, off::kOffMoltIntensity);
                        for (int k = 0; k < 3; ++k)
                        {
                            o->position[k] = world[k];
                            o->color[k]    = float(bgra[2 - k]) / 255.0f * intensity;
                            o->ambient[k]  = 0.0f;
                            o->falloff[k]  = 0.0f;
                        }
                        o->intensity  = intensity;
                        o->attenStart = At<float>(e, off::kOffMoltAttenStart);
                        o->attenEnd   = At<float>(e, off::kOffMoltAttenEnd);
                        o->kind  = Kind::Wmo;
                        o->owner = reinterpret_cast<const void*>(def);
                        o->index = i;
                    }
                }
                else WXL_LIGHT_COUNT(wmoSkipped);
                def = next;
            }
        }

        // No C++ objects here, so structured exception handling may guard the raw reads.
        void CollectGuarded(const Query& q, Sink& sink)
        {
            __try
            {
                if (q.m2) CollectModelLights(q, sink);
                if (q.wmo) CollectWmoLights(q, sink);
            }
            __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                       : EXCEPTION_CONTINUE_SEARCH)
            {
                if (sink.stats) ++sink.stats->faults;
            }
        }

        /// Copies a model's path stem, which the engine keeps inline and zero-terminated.
        void CopyStem(uintptr_t model, char* dst, size_t cap)
        {
            const char* stem = reinterpret_cast<const char*>(model + off::kOffModelPathStem);
            const size_t limit = std::min(cap - 1, size_t(off::kOffModelHeader - off::kOffModelPathStem));
            size_t i = 0;
            for (; i < limit && stem[i]; ++i) dst[i] = stem[i];
            dst[i] = 0;
        }

        size_t CollectModelsGuarded(const ModelQuery& q, ModelInstance* out, size_t cap)
        {
            size_t matched = 0;
            __try
            {
                const uintptr_t scene = *reinterpret_cast<const uintptr_t*>(off::kWorldM2Scene);
                if (!Plausible(scene)) return 0;
                const uint32_t frame    = At<uint32_t>(scene, off::kOffSceneFrame);
                const float    radiusSq = q.radius * q.radius;
                uintptr_t inst = At<uintptr_t>(scene, off::kOffSceneModelHead);
                for (uint32_t guard = 0; Plausible(inst) && guard < kMaxModels;
                     ++guard, inst = At<uintptr_t>(inst, off::kOffModelSceneNext))
                {
                    if (!(At<uint32_t>(inst, off::kOffInstFlags) & off::kInstFlagLoaded)) continue;
                    const uintptr_t model  = At<uintptr_t>(inst, off::kOffInstModel);
                    const uintptr_t header = Plausible(model) ? At<uintptr_t>(model, off::kOffModelHeader) : 0;
                    if (!Plausible(header)) continue;
                    const uint32_t lightCount = At<uint32_t>(header, off::kOffHeaderLightCount);
                    if (q.unlitOnly && lightCount) continue;
                    const bool attached = At<uintptr_t>(inst, off::kOffInstParent) != 0;
                    if (attached && !q.includeAttached) continue;
                    const uint32_t age = frame - At<uint32_t>(inst, off::kOffInstLastAnimFrame);
                    if (age > q.maxStaleFrames) continue;
                    const float* m = reinterpret_cast<const float*>(inst + off::kOffInstPlacement);
                    if (DistSq(m + 12, q.center) > radiusSq) continue;

                    ModelInstance* o = matched < cap ? &out[matched] : nullptr;
                    ++matched;
                    if (!o) continue;
                    o->owner = reinterpret_cast<const void*>(inst);
                    CopyStem(model, o->stem, sizeof o->stem);
                    for (int k = 0; k < 16; ++k) o->toWorld[k] = m[k];
                    o->age        = age;
                    o->lightCount = lightCount;
                    o->attached   = attached;
                }
            }
            __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                       : EXCEPTION_CONTINUE_SEARCH)
            {
            }
            return matched;
        }

        #undef WXL_LIGHT_COUNT
    }

    size_t CollectModels(const ModelQuery& q, ModelInstance* out, size_t cap)
    {
        return CollectModelsGuarded(q, out, out ? cap : 0);
    }

    size_t Collect(const Query& q, PointLight* out, size_t cap, Stats* stats)
    {
        if (stats) *stats = Stats{};
        Sink sink{ out, out ? cap : 0, 0, stats };
        CollectGuarded(q, sink);
        return sink.matched;
    }

    uint32_t SceneFrame()
    {
        const uintptr_t scene = *reinterpret_cast<const uintptr_t*>(off::kWorldM2Scene);
        return scene >= kMinPointer ? At<uint32_t>(scene, off::kOffSceneFrame) : 0;
    }

    namespace
    {
        // The same placement list CollectWmoLights walks, reporting the placements themselves.
        size_t CollectWmoPlacementsGuarded(const float center[3], float radius, WmoPlacement* out, size_t cap)
        {
            size_t matched = 0;
            __try
            {
                const uint32_t linkOffset = *reinterpret_cast<const uint32_t*>(off::kMapObjDefLinkOffset);
                uintptr_t def = *reinterpret_cast<const uintptr_t*>(off::kMapObjDefHead);
                for (uint32_t guard = 0; guard < kMaxPlacements; ++guard)
                {
                    if ((def & 1) || !Plausible(def)) break;
                    const uintptr_t next = At<uintptr_t>(def + linkOffset, 4);
                    const float* mn = reinterpret_cast<const float*>(def + off::kOffDefBoundsMin);
                    const float* mx = reinterpret_cast<const float*>(def + off::kOffDefBoundsMax);
                    const uintptr_t root = At<uintptr_t>(def, off::kOffDefMapObj);
                    if (Plausible(root) && SphereTouchesBox(center, radius, mn, mx))
                    {
                        WmoPlacement* o = matched < cap ? &out[matched] : nullptr;
                        ++matched;
                        if (o)
                        {
                            o->owner = reinterpret_cast<const void*>(def);
                            o->root  = reinterpret_cast<const void*>(root);
                            const char* name = reinterpret_cast<const char*>(root + wxl::offsets::game::wmo::kOffNameInline);
                            size_t n = 0;
                            while (n + 1 < sizeof o->path && name[n]) { o->path[n] = name[n]; ++n; }
                            o->path[n] = '\0';
                            const float* m = reinterpret_cast<const float*>(def + off::kOffDefToWorld);
                            for (int k = 0; k < 16; ++k) o->toWorld[k] = m[k];
                            for (int k = 0; k < 3; ++k) { o->boundsMin[k] = mn[k]; o->boundsMax[k] = mx[k]; }
                            o->lightCount = At<uint32_t>(root, off::kOffRootLoaded) ? At<uint32_t>(root, off::kOffRootMoltCount) : 0;
                        }
                    }
                    def = next;
                }
            }
            __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                       : EXCEPTION_CONTINUE_SEARCH)
            {
            }
            return matched;
        }
    }

    size_t CollectWmoPlacements(const float center[3], float radius, WmoPlacement* out, size_t cap)
    {
        return CollectWmoPlacementsGuarded(center, radius, out, out ? cap : 0);
    }
}
