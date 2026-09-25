// wxl-forever fog: bodies moving through the fog push it away and leave a short fading wake.
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

#include "Wakes.hpp"

#include "game/World.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    namespace in    = wxl::forever::fog::inputs;
    namespace world = wxl::game::world;

    constexpr int   kTrailPoints   = 8;
    constexpr float kSampleSeconds = 0.12f;  // a trail point at most this often, and at least a share of the trail apart
    constexpr float kSampleSpacing = 0.35f;  // and only once the body has moved this far, yards

    struct TrailPoint { float pos[3]; float age; };

    struct Body
    {
        unsigned long long guid = 0;
        float pos[3] = {};
        float height = 2.0f;
        float sinceSample = 0.0f;
        bool  present = false;
        TrailPoint trail[kTrailPoints];
        int   trailCount = 0;
    };

    std::vector<Body> g_bodies;
    int               g_tracked = 0;

    float Dist2(const float a[3], const float b[3])
    {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return dx * dx + dy * dy + dz * dz;
    }

    void Age(Body& b, float dt, float trailSeconds)
    {
        int kept = 0;
        for (int i = 0; i < b.trailCount; ++i)
        {
            b.trail[i].age += dt;
            if (b.trail[i].age < trailSeconds) b.trail[kept++] = b.trail[i];
        }
        b.trailCount = kept;
    }
}

namespace wxl::forever::fog::wakes
{
    void Update(float dt, const float player[3], bool havePlayer, float range, float trailSeconds)
    {
        for (Body& b : g_bodies)
        {
            b.present = false;
            Age(b, dt, trailSeconds);
        }
        if (!havePlayer)
        {
            g_bodies.clear();
            g_tracked = 0;
            return;
        }

        struct Seen { unsigned long long guid; float pos[3]; float height; float d2; };
        std::vector<Seen> seen;
        const float range2 = range * range;
        world::ForEachObject(world::kTypeMaskUnit, [&](unsigned long long guid, void* obj) {
            Seen s{ guid, {}, 2.0f, 0.0f };
            world::Position(obj, s.pos);
            s.d2 = Dist2(s.pos, player);
            if (s.d2 > range2) return true;
            float head[3];
            world::NamePosition(obj, head);
            const float h = head[2] - s.pos[2];
            s.height = (h > 0.5f && h < 8.0f) ? h : 2.0f;
            seen.push_back(s);
            return true;
        });
        std::sort(seen.begin(), seen.end(), [](const Seen& a, const Seen& b) { return a.d2 < b.d2; });
        if (seen.size() > size_t(kMaxBodies)) seen.resize(kMaxBodies);

        for (const Seen& s : seen)
        {
            auto it = std::find_if(g_bodies.begin(), g_bodies.end(), [&](const Body& b) { return b.guid == s.guid; });
            if (it == g_bodies.end())
            {
                Body b;
                b.guid = s.guid;
                std::copy(s.pos, s.pos + 3, b.pos);
                g_bodies.push_back(b);
                it = g_bodies.end() - 1;
            }
            Body& b = *it;
            b.present = true;
            b.height = s.height;
            b.sinceSample += dt;

            // A body that moved leaves the place it was as the newest trail point.
            // Points are spread over the whole trail, so the ring only drops a point once it has
            // faded: a running body's wake no longer loses half-strength capsules every few frames.
            const float every = std::max(kSampleSeconds, trailSeconds / float(kTrailPoints));
            if (b.sinceSample >= every && Dist2(b.pos, s.pos) > kSampleSpacing * kSampleSpacing)
            {
                if (b.trailCount == kTrailPoints)
                {
                    std::rotate(b.trail, b.trail + 1, b.trail + kTrailPoints);
                    --b.trailCount;
                }
                TrailPoint& p = b.trail[b.trailCount++];
                std::copy(b.pos, b.pos + 3, p.pos);
                p.age = 0.0f;
                std::copy(s.pos, s.pos + 3, b.pos);
                b.sinceSample = 0.0f;
            }
            else
            {
                std::copy(s.pos, s.pos + 3, b.pos);
            }
        }

        // A body gone from range keeps its wake until the wake has faded.
        g_bodies.erase(std::remove_if(g_bodies.begin(), g_bodies.end(),
                                      [](const Body& b) { return !b.present && b.trailCount == 0; }),
                       g_bodies.end());
        g_tracked = int(seen.size());
    }

    int Emit(const float eye[3], float radius, float trailSeconds, inputs::Volume* out, int cap)
    {
        if (cap <= 0) return 0;
        std::vector<const Body*> order;
        for (const Body& b : g_bodies) order.push_back(&b);
        std::sort(order.begin(), order.end(),
                  [eye](const Body* a, const Body* b) { return Dist2(a->pos, eye) < Dist2(b->pos, eye); });

        int count = 0;
        auto capsule = [&](const float pos[3], float r, float height, float strength, float falloff, float swirl) {
            if (count >= cap) return;
            in::Volume& v = out[count++];
            v = in::Volume{};
            v.shape = int(in::VolumeShape::Capsule);
            std::copy(pos, pos + 3, v.center);
            v.extent[0] = r;
            v.extent[1] = r;
            v.extent[2] = height;
            v.density = strength;
            v.falloff = falloff;
            v.carve = 1;
            v.swirl = swirl;
        };

        for (const Body* b : order)
            if (b->present) capsule(b->pos, radius, b->height, 1.0f, 0.5f, 0.3f);

        // Trails fill what is left, freshest first.
        struct Point { const Body* body; const TrailPoint* p; };
        std::vector<Point> points;
        for (const Body* b : order)
            for (int i = 0; i < b->trailCount; ++i) points.push_back({ b, &b->trail[i] });
        std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.p->age < b.p->age; });
        for (const Point& pt : points)
        {
            const float life = 1.0f - std::clamp(pt.p->age / std::max(trailSeconds, 0.01f), 0.0f, 1.0f);
            // An old wake point carves less and churns more: the hole closes into a swirl.
            capsule(pt.p->pos, radius * (0.8f + 0.6f * (1.0f - life)), pt.body->height, 0.8f * life * life, 0.8f,
                    std::sin(3.14159f * life));
        }
        return count;
    }

    int Tracked() { return g_tracked; }
}
