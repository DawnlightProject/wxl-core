// wxl-forever fog: smoke and steam emitters become plumes in the volume.
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

#include "Plumes.hpp"

#include "game/Effects.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
    namespace efx = wxl::game::effects;
    namespace in  = wxl::forever::fog::inputs;

    constexpr size_t kCollectCap = 256;
    char g_status[200] = "plumes: none yet";

    struct Candidate
    {
        const efx::Emitter* e;
        bool  fire;
        float dist2;
    };
}

namespace wxl::forever::fog::plumes
{
    int Emit(const float eye[3], const Tuning& tuning, inputs::Volume* out, int cap)
    {
        static efx::Emitter emitters[kCollectCap];
        efx::EmitterQuery q;
        for (int k = 0; k < 3; ++k) q.center[k] = eye[k];
        q.radius = 60.0f;
        q.maxStaleFrames = 600;   // a chimney behind the camera keeps smoking
        efx::EmitterStats stats{};
        const size_t n = std::min(efx::CollectEmitters(q, emitters, kCollectCap, &stats), kCollectCap);

        static Candidate candidates[kCollectCap];
        size_t count = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const efx::Emitter& e = emitters[i];
            const bool smoky = e.kind == efx::EmitterKind::Smoke || e.kind == efx::EmitterKind::Steam;
            const bool fire  = e.kind == efx::EmitterKind::Fire && tuning.fireSmoke > 0.0f;
            if (!smoky && !fire) continue;
            const float dx = e.position[0] - eye[0], dy = e.position[1] - eye[1], dz = e.position[2] - eye[2];
            candidates[count++] = { &e, !smoky, dx * dx + dy * dy + dz * dz };
        }
        std::sort(candidates, candidates + count, [](const Candidate& a, const Candidate& b) { return a.dist2 < b.dist2; });

        int written = 0;
        for (size_t i = 0; i < count && written < cap && written < kMaxPlumes; ++i)
        {
            const efx::Emitter& e = *candidates[i].e;
            const bool fire = candidates[i].fire;

            // Height from how far the particles travel in their life, width from their size.
            float height = e.speed > 0.2f ? e.speed * e.lifespan : 4.0f;
            height = std::clamp(height, 1.5f, 15.0f);
            const float baseRadius = std::clamp(e.sizeMin * 0.5f, 0.2f, 2.0f);
            const float topRadius  = std::clamp(std::max(e.sizeMax * 1.2f, baseRadius * 2.0f), 0.5f, 6.0f);
            // Denser the more particles it keeps alive (rate times life).
            const float live = std::max(e.rate * e.lifespan, float(e.liveParticles));
            float density = std::clamp(live * 0.002f, 0.01f, 0.15f) * tuning.smoke;
            if (fire) density = 0.03f * tuning.fireSmoke;

            // Smoke rises: mostly up, leaning a little along the emitter's own axis.
            float axis[3] = { e.direction[0] * 0.3f, e.direction[1] * 0.3f, 1.0f };
            const float len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
            for (float& c : axis) c /= len;

            in::Volume& v = out[written++];
            v = in::Volume{};
            v.shape = int(in::VolumeShape::Plume);
            for (int k = 0; k < 3; ++k)
            {
                v.center[k] = e.position[k] + (fire ? (k == 2 ? 1.0f : 0.0f) : 0.0f);
                v.extent[k] = axis[k] * (fire ? 5.0f : height);
            }
            v.density = density;
            v.falloff = fire ? 2.0f : topRadius;          // a plume keeps its top radius here
            v.radius  = fire ? 0.5f : baseRadius;
            v.carve   = e.kind == efx::EmitterKind::Steam ? 0 : 1;   // a plume uses it as the smoke tint
            v.swirl   = 0.6f;
        }

        std::snprintf(g_status, sizeof g_status,
                      "plumes: %d written | emitters %u (smoke %u, steam %u, fire %u), disabled %u, stale %u",
                      written, stats.accepted, stats.byKind[0], stats.byKind[1], stats.byKind[3], stats.disabled, stats.stale);
        return written;
    }

    const char* Status() { return g_status; }
}
