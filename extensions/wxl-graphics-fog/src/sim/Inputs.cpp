// wxl-graphics-fog: the built-in inputs. Bodies (and the panel's phantoms), missiles, blasts, fires and
// frost become splats of the wake fluid; missiles' tunnels, blasts, plumes and fires' heat become
// simulation primitives in the 3D volume.
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

#include "Inputs.hpp"
#include "Primitives.hpp"
#include "../core/Settings.hpp"

#include "game/Effects.hpp"
#include "game/World.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
    namespace pr    = wxl::gfx::fog::prims;
    namespace efx   = wxl::game::effects;
    namespace world = wxl::game::world;
    using namespace wxl::gfx::fog;

    float Dist2(const float a[3], const float b[3])
    {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return dx * dx + dy * dy + dz * dz;
    }

    float Dist2XY(const float a[3], const float b[3])
    {
        const float dx = a[0] - b[0], dy = a[1] - b[1];
        return dx * dx + dy * dy;
    }

    bool Contains(const char* text, std::initializer_list<const char*> words)
    {
        char lower[128];
        size_t i = 0;
        for (; i + 1 < sizeof lower && text[i]; ++i) lower[i] = char(std::tolower(uint8_t(text[i])));
        lower[i] = 0;
        for (const char* w : words)
            if (std::strstr(lower, w)) return true;
        return false;
    }

    bool FireName(const char* name)
    {
        return Contains(name, { "fire", "flame", "pyro", "lava", "meteor", "scorch", "immolat", "incinerat", "hellfire", "magma",
                                "ember", "fel", "burn" });
    }

    bool FrostName(const char* name)
    {
        return Contains(name, { "frost", "ice", "icy", "blizzard", "frozen", "freez", "chill", "cold", "glacial", "snow",
                                "winter" });
    }

    // --- bodies and phantoms -----------------------------------------------------------------------------

    constexpr int kMaxWalkers = 192;

    struct Body
    {
        unsigned long long guid = 0;
        float pos[3] = {};
        float vel[3] = {};
        float height = 2.0f;
        bool  present = false;
    };

    struct Phantom
    {
        float pos[3], vel[3], to[3];
        float height;
    };

    std::vector<Body>    g_bodies;
    std::vector<Phantom> g_phantoms;

    // --- missiles, blasts, hot and cold spots ----------------------------------------------------------

    constexpr size_t kCollect  = 64;
    constexpr int    kSegments = 40;
    constexpr double kSampleEvery = 0.08;
    constexpr double kShockLife = 2.5;

    struct Segment
    {
        float  head[3], tail[3];
        float  radius;
        double birth;
        float  fire, frost;
    };

    struct Track
    {
        const void* owner;
        float  last[3];
        double lastTime;
        float  position[3], velocity[3];
        float  radius;
        float  fire, frost;
        bool   seen;
    };

    struct Shock
    {
        float  pos[3];
        double birth;
        float  strength, radius;
    };

    struct Spot
    {
        float pos[3];
        float radius;
        bool  frost;
    };

    struct SeenOwner
    {
        const void* owner;
        double      last;
    };

    std::vector<Segment>   g_segments;
    std::vector<Track>     g_tracks;
    std::vector<Shock>     g_shocks;
    std::vector<Spot>      g_spots;
    std::vector<SeenOwner> g_spellOwners;
    int                    g_missiles = 0;
    int                    g_plumes = 0, g_fires = 0;

    void Push(const float head[3], const float tail[3], float radius, float fire, float frost, double now)
    {
        Segment s{};
        std::copy(head, head + 3, s.head);
        std::copy(tail, tail + 3, s.tail);
        s.radius = radius;
        s.birth = now;
        s.fire = fire;
        s.frost = frost;
        g_segments.push_back(s);
        // A full pool merges its two oldest consecutive segments, or lets the oldest go.
        while (int(g_segments.size()) > kSegments)
        {
            bool merged = false;
            for (size_t a = 0; a + 1 < g_segments.size() && !merged; ++a)
                for (size_t b = a + 1; b < g_segments.size() && !merged; ++b)
                {
                    Segment& x = g_segments[a];
                    const Segment& y = g_segments[b];
                    if (Dist2(x.head, y.tail) > 0.01f || std::fabs(float(x.birth - y.birth)) > 0.3f) continue;
                    std::copy(y.head, y.head + 3, x.head);
                    x.radius = std::max(x.radius, y.radius);
                    g_segments.erase(g_segments.begin() + std::ptrdiff_t(b));
                    merged = true;
                }
            if (!merged) g_segments.erase(g_segments.begin());
        }
    }

    void AddShock(const float pos[3], float strength, float radius, double now)
    {
        if (strength <= 0.0f) return;
        for (const Shock& s : g_shocks)
            if (Dist2(s.pos, pos) < 1.0f && now - s.birth < 0.3) return;   // one blast, not one per emitter
        Shock s{};
        std::copy(pos, pos + 3, s.pos);
        s.birth = now;
        s.strength = strength;
        s.radius = radius;
        g_shocks.push_back(s);
        if (g_shocks.size() > 24) g_shocks.erase(g_shocks.begin());
    }

    // A splat of the wake fluid: two rows (position and size; velocity or strength, height, kind).
    struct Splat
    {
        float r0[4], r1[4];
        float d2;
    };
}

namespace wxl::gfx::fog::inputs
{
    void UpdateBodies(float dt, const float player[3], bool havePlayer, float range)
    {
        // Phantoms walk to their goal, then go.
        for (Phantom& p : g_phantoms)
            for (int k = 0; k < 3; ++k) p.pos[k] += p.vel[k] * dt;
        g_phantoms.erase(std::remove_if(g_phantoms.begin(), g_phantoms.end(), [](const Phantom& p) {
                             const float d[2] = { p.to[0] - p.pos[0], p.to[1] - p.pos[1] };
                             return d[0] * p.vel[0] + d[1] * p.vel[1] <= 0.0f;
                         }),
                         g_phantoms.end());

        for (Body& b : g_bodies) b.present = false;
        if (!havePlayer)
        {
            g_bodies.clear();
            return;
        }
        struct Seen { unsigned long long guid; float pos[3]; float height; float d2; };
        static std::vector<Seen> seen;
        seen.clear();
        const float range2 = range * range;
        world::ForEachObject(world::kTypeMaskUnit, [&](unsigned long long guid, void* obj) {
            Seen s{ guid, {}, 2.0f, 0.0f };
            world::Position(obj, s.pos);
            s.d2 = Dist2(s.pos, player);
            if (s.d2 > range2) return true;
            float head[3];
            world::NamePosition(obj, head);
            const float h = head[2] - s.pos[2];
            s.height = (h > 0.5f && h < 12.0f) ? h : 2.0f;
            seen.push_back(s);
            return true;
        });
        std::sort(seen.begin(), seen.end(), [](const Seen& a, const Seen& b) { return a.d2 < b.d2; });
        if (seen.size() > size_t(kMaxWalkers)) seen.resize(kMaxWalkers);

        const float ease = dt > 0.0f ? std::min(dt / 0.2f, 1.0f) : 0.0f;
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
            for (int k = 0; k < 3; ++k)
            {
                const float v = dt > 0.0f ? (s.pos[k] - b.pos[k]) / dt : 0.0f;
                b.vel[k] += (std::clamp(v, -30.0f, 30.0f) - b.vel[k]) * ease;
                b.pos[k] = s.pos[k];
            }
            b.height = s.height;
            b.present = true;
        }
        g_bodies.erase(std::remove_if(g_bodies.begin(), g_bodies.end(), [](const Body& b) { return !b.present; }), g_bodies.end());
    }

    int Occluders(const float centre[3], float* rows, int cap)
    {
        struct O { float row[4]; float d2; };
        static std::vector<O> list;
        list.clear();
        for (const Body& b : g_bodies) list.push_back({ { b.pos[0], b.pos[1], b.pos[2], b.height }, Dist2(b.pos, centre) });
        for (const Phantom& p : g_phantoms) list.push_back({ { p.pos[0], p.pos[1], p.pos[2], p.height }, Dist2(p.pos, centre) });
        std::sort(list.begin(), list.end(), [](const O& a, const O& b) { return a.d2 < b.d2; });
        const int n = std::min(int(list.size()), cap);
        for (int i = 0; i < n; ++i) std::memcpy(rows + i * 4, list[size_t(i)].row, sizeof(float) * 4);
        return n;
    }

    void SpawnPhantoms(const float from[3], const float to[3], int count, float speed, float spread)
    {
        const float dx = to[0] - from[0], dy = to[1] - from[1];
        const float len = std::max(std::sqrt(dx * dx + dy * dy), 0.1f);
        const float ux = dx / len, uy = dy / len;
        count = std::clamp(count, 1, kMaxWalkers);
        uint32_t seed = 0x9E3779B9u ^ uint32_t(count * 7919);
        auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / 16777216.0f; };
        for (int i = 0; i < count; ++i)
        {
            Phantom p{};
            const float across = (rnd() - 0.5f) * spread, behind = rnd() * spread * 0.6f;
            p.pos[0] = from[0] - uy * across - ux * behind;
            p.pos[1] = from[1] + ux * across - uy * behind;
            p.pos[2] = from[2];
            const float sp = speed * (0.8f + 0.4f * rnd());
            p.vel[0] = ux * sp;
            p.vel[1] = uy * sp;
            p.vel[2] = (to[2] - from[2]) / len * sp;
            p.to[0] = to[0] - uy * across;
            p.to[1] = to[1] + ux * across;
            p.to[2] = to[2];
            p.height = 1.6f + 0.6f * rnd();
            g_phantoms.push_back(p);
        }
        if (g_phantoms.size() > size_t(kMaxWalkers)) g_phantoms.erase(g_phantoms.begin(), g_phantoms.end() - kMaxWalkers);
    }

    void ClearPhantoms() { g_phantoms.clear(); }

    void UpdateMissiles(const float eye[3], double clock)
    {
        const Settings& s = Config();
        const OutdoorProfile& o = Outdoor();
        const double life = o.projectileHold + std::max(o.projectileRefill, 0.5f);
        g_shocks.erase(std::remove_if(g_shocks.begin(), g_shocks.end(),
                                      [&](const Shock& k) { return clock - k.birth > kShockLife || clock < k.birth - 1.0; }),
                       g_shocks.end());
        if (!s.projectiles)
        {
            g_segments.clear();
            g_tracks.clear();
            g_missiles = 0;
        }
        else
        {
            static efx::Missile missiles[kCollect];
            efx::MissileQuery q;
            std::copy(eye, eye + 3, q.center);
            q.radius = 150.0f;
            const size_t n = std::min(efx::CollectMissiles(q, missiles, kCollect), kCollect);
            for (Track& t : g_tracks) t.seen = false;
            g_missiles = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const efx::Missile& m = missiles[i];
                if (m.speed <= 0.5f) continue;
                ++g_missiles;
                auto it = std::find_if(g_tracks.begin(), g_tracks.end(), [&](const Track& t) { return t.owner == m.owner; });
                if (it == g_tracks.end())
                {
                    Track t{};
                    t.owner = m.owner;
                    std::copy(m.position, m.position + 3, t.last);
                    t.lastTime = clock;
                    g_tracks.push_back(t);
                    it = g_tracks.end() - 1;
                }
                Track& t = *it;
                t.seen = true;
                std::copy(m.position, m.position + 3, t.position);
                for (int k = 0; k < 3; ++k) t.velocity[k] = m.direction[k] * m.speed;
                t.radius = std::max(m.radius * o.projectileRadius, 0.35f);
                const bool spell = m.kind == efx::MissileKind::Spell;
                t.fire = spell && FireName(m.model) ? 1.0f : 0.0f;
                t.frost = spell && t.fire == 0.0f && FrostName(m.model) ? 1.0f : 0.0f;
                if (clock - t.lastTime >= kSampleEvery)
                {
                    Push(m.position, t.last, t.radius, t.fire, t.frost, clock);
                    std::copy(m.position, m.position + 3, t.last);
                    t.lastTime = clock;
                }
            }
            // A missile gone from the list has landed: its last stretch, a crater, and a blast.
            for (auto it = g_tracks.begin(); it != g_tracks.end();)
            {
                if (it->seen) { ++it; continue; }
                Push(it->position, it->last, it->radius, it->fire, it->frost, clock);
                const float crater = std::max(it->radius * 2.0f, 1.0f) * 1.3f;
                Push(it->position, it->position, crater, it->fire, it->frost, clock);
                AddShock(it->position, o.shockStrength * (it->fire > 0.0f ? 1.2f : 1.0f), std::clamp(3.0f + it->radius * 3.0f, 3.0f, 12.0f), clock);
                it = g_tracks.erase(it);
            }
            g_segments.erase(std::remove_if(g_segments.begin(), g_segments.end(),
                                            [&](const Segment& sg) { return clock - sg.birth > life || clock < sg.birth - 1.0; }),
                             g_segments.end());
        }

        // Spell effects: a new one nearby is a cast landing (a blast); flames burn, icy ones chill.
        g_spots.clear();
        if (!s.wakes && !s.heat) return;
        static efx::Emitter emitters[256];
        efx::EmitterQuery eq;
        std::copy(eye, eye + 3, eq.center);
        eq.radius = 60.0f;
        const size_t en = std::min(efx::CollectEmitters(eq, emitters, 256), size_t(256));
        for (size_t i = 0; i < en; ++i)
        {
            const efx::Emitter& e = emitters[i];
            const bool spell = Contains(e.texture, { "spells" });
            const bool frost = FrostName(e.texture);
            if (e.kind == efx::EmitterKind::Fire || frost)
            {
                if (g_spots.size() < 24) g_spots.push_back({ { e.position[0], e.position[1], e.position[2] },
                                                             std::clamp(e.sizeMax * 0.8f, 0.5f, 4.0f), frost && e.kind != efx::EmitterKind::Fire });
            }
            if (!spell) continue;
            auto it = std::find_if(g_spellOwners.begin(), g_spellOwners.end(), [&](const SeenOwner& so) { return so.owner == e.owner; });
            if (it != g_spellOwners.end())
            {
                it->last = clock;
                continue;
            }
            g_spellOwners.push_back({ e.owner, clock });
            if (Dist2(e.position, eye) < 45.0f * 45.0f)
                AddShock(e.position, o.shockStrength * 0.7f, std::clamp(2.5f + e.sizeMax * 2.0f, 3.0f, 10.0f), clock);
        }
        g_spellOwners.erase(std::remove_if(g_spellOwners.begin(), g_spellOwners.end(), [&](const SeenOwner& so) { return clock - so.last > 3.0; }),
                            g_spellOwners.end());
    }

    int WakeSplats(const float centre[3], double clock, const WXL_GfxLight* lights, int lightCount, float* out, int cap)
    {
        const Settings& s = Config();
        const OutdoorProfile& o = Outdoor();
        if (!s.wakes || cap <= 0) return 0;
        static std::vector<Splat> list;
        list.clear();
        auto add = [&](const float pos[3], float radius, float a, float b, float c, int kind) {
            Splat sp{};
            sp.r0[0] = pos[0]; sp.r0[1] = pos[1]; sp.r0[2] = pos[2]; sp.r0[3] = radius;
            sp.r1[0] = a; sp.r1[1] = b; sp.r1[2] = c; sp.r1[3] = float(kind);
            sp.d2 = Dist2XY(pos, centre);
            list.push_back(sp);
        };
        const float reach = std::max(s.wakeRange, 10.0f);
        const float reach2 = reach * reach;

        // Walkers: the air moves with them, the fog is pushed out of their way.
        for (const Body& b : g_bodies)
        {
            const float r = std::clamp(b.height * 0.28f, 0.35f, 3.0f) * o.wakeRadius;
            add(b.pos, r, b.vel[0], b.vel[1], b.height + o.wakeHeight, FOG_BODY_WALKER);
        }
        for (const Phantom& p : g_phantoms)
            add(p.pos, 0.45f * o.wakeRadius, p.vel[0], p.vel[1], p.height + o.wakeHeight, FOG_BODY_WALKER);

        // Missiles near the ground drag the air along their path; burning ones evaporate, icy ones chill.
        for (const Track& t : g_tracks)
        {
            if (std::fabs(t.position[2] - centre[2]) > 6.0f) continue;
            add(t.position, t.radius * 1.2f, t.velocity[0], t.velocity[1], 0.0f, FOG_BODY_MISSILE);
            if (t.fire > 0.0f) add(t.position, t.radius * 1.6f, o.fireEvaporation, 0.0f, 4.0f, FOG_BODY_FIRE);
            if (t.frost > 0.0f) add(t.position, t.radius * 1.6f, o.frostFog, 0.0f, 3.0f, FOG_BODY_FROST);
        }

        // Carried flames (torches and lanterns in hands): a pocket the fog cannot reform in.
        if (o.torchClear > 0.0f)
        {
            for (int i = 0; lights && i < lightCount; ++i)
            {
                const WXL_GfxLight& l = lights[i];
                if (!l.carried) continue;
                add(l.position, std::clamp(std::fabs(l.radius) * o.torchRadius, 1.5f, 15.0f), o.torchClear, 0.0f, 12.0f, FOG_BODY_TORCH);
            }
            if (s.debugTorch)
            {
                const float hand[3] = { centre[0], centre[1], centre[2] + 1.3f };
                add(hand, std::clamp(12.0f * o.torchRadius, 1.5f, 15.0f), o.torchClear, 0.0f, 12.0f, FOG_BODY_TORCH);
            }
        }

        // Blasts: a ring running out and slowing, fading as it goes.
        for (const Shock& k : g_shocks)
        {
            const float age = float(clock - k.birth);
            if (age < 0.0f || age > 1.6f) continue;
            const float R = k.radius * (1.0f - std::exp(-age / 0.12f)) + 0.3f;
            add(k.pos, R, k.strength * std::exp(-age / 0.5f), 0.6f + 0.3f * R, 5.0f, FOG_BODY_SHOCK);
        }

        // Fires and icy effects.
        for (const Spot& sp : g_spots)
            add(sp.pos, sp.radius, sp.frost ? o.frostFog : o.fireEvaporation, 0.0f, sp.frost ? 3.0f : 4.0f,
                sp.frost ? FOG_BODY_FROST : FOG_BODY_FIRE);
        if (s.heat && lights)
            for (int i = 0; i < lightCount; ++i)
            {
                const WXL_GfxLight& l = lights[i];
                if (l.flicker != WXL_GFX_LIGHT_FLICKER_FIRE) continue;
                add(l.position, std::clamp(l.size * 3.0f, 0.8f, 3.0f), o.fireEvaporation * o.heat, 0.0f, 4.0f, FOG_BODY_FIRE);
            }

        list.erase(std::remove_if(list.begin(), list.end(), [&](const Splat& sp) { return sp.d2 > reach2 + sp.r0[3] * sp.r0[3]; }), list.end());
        std::sort(list.begin(), list.end(), [](const Splat& a, const Splat& b) { return a.d2 < b.d2; });
        const int n = std::min(int(list.size()), cap);
        for (int i = 0; i < n; ++i)
        {
            std::memcpy(out + i * 8, list[size_t(i)].r0, sizeof(float) * 4);
            std::memcpy(out + i * 8 + 4, list[size_t(i)].r1, sizeof(float) * 4);
        }
        return n;
    }

    void Emit(const float eye[3], double clock, const WXL_GfxLight* lights, int lightCount)
    {
        const Settings& s = Config();
        const OutdoorProfile& o = Outdoor();
        const float zero[3] = { 0.0f, 0.0f, 0.0f };

        // Missiles: the tunnel holds, then the fog rolls back in around its axis. Burning ones evaporate
        // the fog and leave smoke; icy ones leave a trail of fog.
        if (s.projectiles)
        {
            for (const Segment& sg : g_segments)
            {
                const float age = float(clock - sg.birth);
                const float grow = 1.0f + sg.fire * 0.35f * std::min(age / 1.5f, 1.0f);
                pr::Prim p = pr::Capsule(sg.head, sg.tail, sg.radius * grow);
                const float u = std::clamp((age - o.projectileHold) / std::max(o.projectileRefill, 0.5f), 0.0f, 1.0f);
                const float fade = 1.0f - u * u * (3.0f - 2.0f * u);
                if (sg.frost > 0.0f)
                {
                    const float cap = std::max(o.density, o.groundDensity) * (1.0f + o.frostFog);
                    pr::SetDensity(p, 0.3f * o.frostFog * fade, 0.0f, cap, 0.8f, 0.8f);
                    pr::SetFlow(p, zero, 0.0f, 0.6f * fade);
                }
                else if (age < o.projectileHold)
                {
                    pr::SetDensity(p, -14.0f * o.projectileCarve * (1.0f + sg.fire * o.fireEvaporation), 0.0f, 0.0f, 0.4f, 0.3f);
                    pr::SetFlow(p, zero, 0.6f, 0.0f);
                }
                else if (sg.fire > 0.0f)
                {
                    pr::SetDensity(p, -4.0f * o.fireEvaporation * fade, 0.06f * fade, 0.0f, 0.8f, 1.0f);
                    const float up[3] = { 0.0f, 0.0f, 0.8f * fade };
                    pr::SetFlow(p, up, 0.3f * fade, 0.8f * fade);
                }
                else
                {
                    // Rolling in: drawn towards the axis and turned round it, with lumpy fog at the seam.
                    const float cap = std::max(o.density, o.groundDensity) * (1.0f + 0.6f * o.trailBillow);
                    pr::SetDensity(p, o.trailBillow * 0.08f * fade, 0.0f, cap, 0.8f, 1.0f);
                    pr::SetFlow(p, zero, -1.8f * fade, 2.2f * fade);
                }
                if (!pr::Add(p)) break;
            }
        }

        // Blasts: blown out at once, then the fog flows back in, turning.
        for (const Shock& k : g_shocks)
        {
            const float age = float(clock - k.birth);
            if (age < 0.0f || age > float(kShockLife)) continue;
            const float R = k.radius * (0.35f + 0.65f * (1.0f - std::exp(-age / 0.15f)));
            pr::Prim p = pr::Sphere(k.pos, R);
            const bool out = age < 0.35f;
            pr::SetDensity(p, out ? -10.0f * k.strength : 0.0f, 0.0f, 0.0f, 0.7f, 0.8f);
            const float radial = out ? 7.0f * k.strength * std::exp(-age / 0.2f) : -1.6f * k.strength * std::exp(-(age - 0.35f) / 0.8f);
            pr::SetFlow(p, zero, radial, out ? 0.0f : 0.8f * k.strength * std::exp(-(age - 0.35f) / 0.8f));
            if (!pr::Add(p)) break;
        }

        // Plumes: smoke put in at the emitter, lifted by its heat; the flow does the rest.
        g_plumes = 0;
        if (s.plumes && (o.smokeDensity > 0.0f || o.fireSmoke > 0.0f))
        {
            static efx::Emitter emitters[256];
            efx::EmitterQuery q;
            std::copy(eye, eye + 3, q.center);
            q.radius = 60.0f;
            q.maxStaleFrames = 600;
            const size_t n = std::min(efx::CollectEmitters(q, emitters, 256), size_t(256));
            struct Candidate { const efx::Emitter* e; bool fire; float d2; };
            std::vector<Candidate> list;
            for (size_t i = 0; i < n; ++i)
            {
                const efx::Emitter& e = emitters[i];
                const bool smoky = e.kind == efx::EmitterKind::Smoke || e.kind == efx::EmitterKind::Steam;
                const bool fire = e.kind == efx::EmitterKind::Fire && o.fireSmoke > 0.0f;
                if (!smoky && !fire) continue;
                list.push_back({ &e, !smoky, Dist2(e.position, eye) });
            }
            std::sort(list.begin(), list.end(), [](const Candidate& a, const Candidate& b) { return a.d2 < b.d2; });
            for (const Candidate& c : list)
            {
                if (g_plumes >= 6) break;
                const efx::Emitter& e = *c.e;
                const float live = std::max(e.rate * e.lifespan, float(e.liveParticles));
                const float radius = std::clamp(e.sizeMin * 0.5f, 0.3f, 1.5f);
                const float base[3] = { e.position[0], e.position[1], e.position[2] + (c.fire ? 0.8f : 0.0f) };
                pr::Prim p = pr::Cylinder(base, radius, 1.2f);
                const float rate = c.fire ? o.fireSmoke * 0.08f : std::clamp(live * 0.004f, 0.05f, 0.5f) * o.smokeDensity;
                if (e.kind == efx::EmitterKind::Steam) pr::SetDensity(p, rate * 0.5f, 0.0f, 0.0f, 0.6f, 0.6f);
                else pr::SetDensity(p, 0.0f, rate, 0.0f, 0.6f, 0.6f);
                const float up[3] = { 0.0f, 0.0f, c.fire ? 2.0f : 1.4f };
                pr::SetFlow(p, up, 0.3f, 0.4f);
                if (!pr::Add(p)) break;
                // The column above keeps the smoke rising until the wind takes it.
                const float column[3] = { base[0], base[1], base[2] + 1.2f };
                pr::Prim lift = pr::Cylinder(column, radius * 2.5f, 7.0f);
                pr::SetDensity(lift, 0.0f, 0.0f, 0.0f, 0.8f, 0.0f);
                const float rise[3] = { 0.0f, 0.0f, 1.0f };
                pr::SetFlow(lift, rise, 0.2f, 0.3f);
                pr::Add(lift);
                ++g_plumes;
            }
        }

        // Fires thin the fog around them and lift it; icy effects leave fog.
        g_fires = 0;
        if (s.heat && o.heat > 0.0f)
        {
            for (int i = 0; lights && i < lightCount && g_fires < 8; ++i)
            {
                const WXL_GfxLight& l = lights[i];
                if (l.flicker != WXL_GFX_LIGHT_FLICKER_FIRE || Dist2(l.position, eye) > 60.0f * 60.0f) continue;
                const float r = std::clamp(l.size * 4.0f, 1.2f, 4.0f);
                pr::Prim p = pr::Sphere(l.position, r);
                pr::SetDensity(p, -o.heat * 1.5f * o.fireEvaporation, 0.0f, 0.0f, 0.8f, 0.5f);
                const float up[3] = { 0.0f, 0.0f, 1.5f * o.heat };
                pr::SetFlow(p, up, 0.0f, 0.0f);
                if (!pr::Add(p)) break;
                ++g_fires;
            }
            for (const Spot& sp : g_spots)
            {
                if (g_fires >= 12) break;
                pr::Prim p = pr::Sphere(sp.pos, sp.radius * (sp.frost ? 1.5f : 1.2f));
                if (sp.frost)
                {
                    const float cap = std::max(o.density, o.groundDensity) * (1.0f + o.frostFog);
                    pr::SetDensity(p, 0.25f * o.frostFog, 0.0f, cap, 0.8f, 0.8f);
                    const float sink[3] = { 0.0f, 0.0f, -0.4f };
                    pr::SetFlow(p, sink, 0.3f, 0.0f);
                }
                else
                {
                    pr::SetDensity(p, -o.heat * 1.5f * o.fireEvaporation, 0.0f, 0.0f, 0.8f, 0.5f);
                    const float up[3] = { 0.0f, 0.0f, 1.5f * o.heat };
                    pr::SetFlow(p, up, 0.0f, 0.0f);
                }
                if (!pr::Add(p)) break;
                ++g_fires;
            }
        }
    }

    int Bodies() { return int(g_bodies.size()); }
    int Phantoms() { return int(g_phantoms.size()); }
    int Missiles() { return g_missiles; }
    int Segments() { return int(g_segments.size()); }
    int Shocks() { return int(g_shocks.size()); }
    int Plumes() { return g_plumes; }
    int Fires() { return g_fires; }
}
