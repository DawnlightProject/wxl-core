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
#include <vector>

namespace
{
    namespace bd    = wxl::gfx::shadow::bodies;
    namespace world = wxl::game::world;

    bd::Capsule g_list[WXL_GFX_SHADOW_CAPSULES];
    int         g_count = 0;

    struct Seen
    {
        bd::Capsule c;
        float       d2;
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
}

namespace wxl::gfx::shadow::bodies
{
    void Update(const float eye[3], float range, float radiusPerYard)
    {
        g_seen.clear();
        const unsigned long long player = world::ActivePlayerGuid();
        const float range2 = range * range;
        world::ForEachObject(world::kTypeMaskUnit, [&](unsigned long long guid, void* obj) {
            float feet[3];
            world::UnitPosition(obj, feet);
            const float d2 = Dist2(feet, eye);
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
            s.d2 = d2;
            g_seen.push_back(s);
            return true;
        });
        std::sort(g_seen.begin(), g_seen.end(), [](const Seen& x, const Seen& y) { return x.d2 < y.d2; });
        g_count = std::min(int(g_seen.size()), WXL_GFX_SHADOW_CAPSULES);
        for (int i = 0; i < g_count; ++i) g_list[i] = g_seen[size_t(i)].c;
    }

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
