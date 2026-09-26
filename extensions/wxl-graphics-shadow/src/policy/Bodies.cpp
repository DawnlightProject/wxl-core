// wxl-graphics-shadow: the body capsules, from the core's unit walk.
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

#include "Bodies.hpp"

#include "game/World.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
    namespace bd    = wxl::gfx::shadow::bodies;
    namespace world = wxl::game::world;

    constexpr int   kCapsules = WXL_GFX_SHADOW_CAPSULES;
    constexpr float kKeep = 1.25f;   // a listed unit ranks as if this many times nearer than it is
    constexpr float kFade = 0.3f;    // seconds a unit's occlusion takes to fade in or out

    bd::Capsule g_list[kCapsules];
    int         g_count = 0;
    float       g_focus[3] = {};

    struct Seen
    {
        bd::Capsule c;
        float       rank;       // squared distance to the focus, less for a listed unit
    };
    std::vector<Seen> g_seen;   // kept for its capacity

    float Dist2(const float a[3], const float b[3])
    {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return dx * dx + dy * dy + dz * dz;
    }

    /// Distance from p to the capsule's axis segment.
    float AxisDistance(const bd::Capsule& c, const float p[3])
    {
        float ab[3], ap[3];
        for (int k = 0; k < 3; ++k) { ab[k] = c.b[k] - c.a[k]; ap[k] = p[k] - c.a[k]; }
        const float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
        const float t = len2 > 1e-6f ? std::clamp((ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len2, 0.0f, 1.0f) : 0.0f;
        float q[3];
        for (int k = 0; k < 3; ++k) q[k] = c.a[k] + ab[k] * t;
        return std::sqrt(Dist2(p, q));
    }

    bool Listed(unsigned long long guid)
    {
        for (int i = 0; i < g_count; ++i)
            if (g_list[i].guid == guid) return true;
        return false;
    }

    int SeenIndex(unsigned long long guid)
    {
        for (size_t k = 0; k < g_seen.size(); ++k)
            if (g_seen[k].c.guid == guid) return int(k);
        return -1;
    }
}

namespace wxl::gfx::shadow::bodies
{
    void Update(const float eye[3], float range, float radiusPerYard, float dt, bool fade)
    {
        // Ranked from the player: the camera orbits it in third person, so a list ranked from the eye
        // changed hands, and unit shadows popped, whenever the view turned.
        const unsigned long long player = world::ActivePlayerGuid();
        void* self = player ? world::ResolveObject(player, world::kTypeMaskPlayer) : nullptr;
        if (self) world::UnitPosition(self, g_focus);
        else std::memcpy(g_focus, eye, sizeof g_focus);

        g_seen.clear();
        const float range2 = range * range;
        world::ForEachObject(world::kTypeMaskUnit, [&](unsigned long long guid, void* obj) {
            float feet[3];
            world::UnitPosition(obj, feet);
            const float d2 = Dist2(feet, g_focus);
            if (d2 > range2) return true;
            float head[3];
            world::NamePosition(obj, head);
            const float h = head[2] - feet[2];
            const float height = (h > 0.4f && h < 12.0f) ? h : 2.0f;
            const float r = std::clamp(height * radiusPerYard, 0.15f, 1.5f);
            Seen s{};
            s.c.radius = r;
            s.c.guid = guid;
            s.c.player = guid == player;
            for (int k = 0; k < 3; ++k) s.c.a[k] = s.c.b[k] = feet[k];
            // A capsule standing on the feet, its caps inside the body's height.
            s.c.a[2] = feet[2] + r;
            s.c.b[2] = feet[2] + std::max(height - r, r);
            // A listed unit keeps its row unless a newcomer is clearly nearer, so two units near the
            // cut-off do not trade places every frame.
            s.rank = Listed(guid) ? d2 / (kKeep * kKeep) : d2;
            g_seen.push_back(s);
            return true;
        });
        std::sort(g_seen.begin(), g_seen.end(), [](const Seen& x, const Seen& y) { return x.rank < y.rank; });
        const int wanted = std::min(int(g_seen.size()), kCapsules);

        // Listed units fade in while wanted, else fade out (following the unit while it is still seen,
        // at its last place once it is gone) and give their row up at nothing. The order of the rows is
        // kept, though nothing relies on it across frames: the slots find their capsules every frame.
        const float step = fade ? dt / kFade : 1.0f;
        bd::Capsule kept[kCapsules];
        int n = 0;
        for (int i = 0; i < g_count; ++i)
        {
            const int k = SeenIndex(g_list[i].guid);
            const float weight = k >= 0 && k < wanted ? std::min(g_list[i].weight + step, 1.0f) : g_list[i].weight - step;
            if (weight <= 0.0f) continue;
            kept[n] = k >= 0 ? g_seen[size_t(k)].c : g_list[i];
            kept[n].weight = weight;
            ++n;
        }
        // Newcomers take the rows left, fading in from nothing.
        for (int k = 0; k < wanted && n < kCapsules; ++k)
        {
            const bd::Capsule& c = g_seen[size_t(k)].c;
            if (std::any_of(kept, kept + n, [&](const bd::Capsule& x) { return x.guid == c.guid; })) continue;
            kept[n] = c;
            kept[n].weight = fade ? 0.0f : 1.0f;
            ++n;
        }
        std::copy(kept, kept + n, g_list);
        g_count = n;
    }

    const float* Focus() { return g_focus; }

    int Count() { return g_count; }
    const Capsule* List() { return g_list; }

    int Find(unsigned long long guid)
    {
        if (!guid) return -1;
        for (int i = 0; i < g_count; ++i)
            if (g_list[i].guid == guid) return i;
        return -1;
    }

    int Nearest(const float p[3], float reach)
    {
        int best = -1;
        float bestGap = reach;
        for (int i = 0; i < g_count; ++i)
        {
            const float gap = AxisDistance(g_list[i], p) - g_list[i].radius;
            if (gap < bestGap)
            {
                bestGap = gap;
                best = i;
            }
        }
        return best;
    }

    uint32_t Mask(const float light[3], float radius)
    {
        uint32_t m = 0;
        for (int i = 0; i < g_count; ++i)
        {
            const float reach = radius + g_list[i].radius + 0.5f;
            if (AxisDistance(g_list[i], light) < reach) m |= 1u << i;
        }
        return m;
    }
}
