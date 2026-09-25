// wxl-forever lights: the frame's light list, gathered and published once for every consumer.
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

#include "../core/ExtensionApi.hpp"
#include "../core/Passes.hpp"
#include "Lights.hpp"
#include "Cookies.hpp"
#include "Rooms.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace
{
    namespace fl = wxl::forever::lights;

    fl::Options g_options;
    fl::Light   g_given[fl::kMaxLights];
    int         g_givenCount = 0;
    bool        g_interiorGate = false;

    fl::Light   g_list[fl::kMaxLights];
    int         g_count = 0;

    // Where each light was last frame, before the flicker, and the frame it last moved.
    struct Seen { const void* owner; uint32_t index; float position[3]; uint32_t moved; };
    std::vector<Seen> g_seen;
    std::vector<fl::Moving> g_moving;
    std::vector<uint8_t>    g_movingCarriedOnly;   // 1 where a carried light is listed without moving
    constexpr uint32_t kMoveHold = 12;       // frames a light still counts as moving once it stops
    constexpr float    kMoveStep = 0.03f;    // yards per frame that count as a move

    /// Which lights moved since the last frame. Given lights have no owner: their place in the list
    /// is their identity.
    void TrackMoves(const float eye[3], uint32_t frame)
    {
        std::vector<Seen> seen;
        seen.reserve(size_t(g_count));
        struct Entry { float key; fl::Moving m; uint8_t carriedOnly; };
        std::vector<Entry> moving;
        for (int i = 0; i < g_count; ++i)
        {
            const fl::Light& l = g_list[i];
            Seen now{ l.owner, l.owner ? l.index : uint32_t(i), { l.position[0], l.position[1], l.position[2] }, 0 };
            for (const Seen& s : g_seen)
            {
                if (s.owner != now.owner || s.index != now.index) continue;
                const float dx = l.position[0] - s.position[0], dy = l.position[1] - s.position[1], dz = l.position[2] - s.position[2];
                now.moved = dx * dx + dy * dy + dz * dz > kMoveStep * kMoveStep ? frame : s.moved;
                break;
            }
            seen.push_back(now);
            const bool moved = now.moved && frame - now.moved <= kMoveHold;
            if (moved || l.carried)
            {
                const float dx = l.position[0] - eye[0], dy = l.position[1] - eye[1], dz = l.position[2] - eye[2];
                moving.push_back({ std::sqrt(dx * dx + dy * dy + dz * dz) - l.radius,
                                   fl::Moving{ { l.position[0], l.position[1], l.position[2] }, l.radius },
                                   uint8_t(moved ? 0 : 1) });
            }
        }
        g_seen.swap(seen);
        std::sort(moving.begin(), moving.end(), [](const Entry& a, const Entry& b) { return a.key < b.key; });
        g_moving.clear();
        g_movingCarriedOnly.clear();
        for (const Entry& e : moving)
        {
            g_moving.push_back(e.m);
            g_movingCarriedOnly.push_back(e.carriedOnly);
        }
    }
    uint32_t    g_frame = 0;
    bool        g_published = false;
    LONGLONG    g_lastCount = 0;
    LONGLONG    g_startCount = 0;

    float Hash(uint32_t x)
    {
        x ^= x >> 16; x *= 0x7FEB352Du;
        x ^= x >> 15; x *= 0x846CA68Bu;
        x ^= x >> 16;
        return float(x & 0xFFFFFF) / 16777216.0f;
    }

    /// Smooth value noise in -1..1 along time, its own sequence per seed.
    float Wave(float t, uint32_t seed)
    {
        const float i = std::floor(t);
        const float f = t - i;
        const uint32_t k = uint32_t(int64_t(i)) * 0x9E3779B1u + seed * 0x85EBCA77u;
        const float a = Hash(k), b = Hash(k + 0x9E3779B1u);
        const float s = f * f * (3.0f - 2.0f * f);
        return (a + (b - a) * s) * 2.0f - 1.0f;
    }

    /// A light's flicker at time t: a factor on its intensity and, for a fire, a small shift of
    /// its position. Seeded by the light's identity, so every consumer sees the same flicker.
    void Animate(fl::Light& l, float t, float strength)
    {
        if (l.flicker == fl::Flicker::None || strength <= 0.0f) return;
        const uint32_t seed = uint32_t(reinterpret_cast<uintptr_t>(l.owner)) * 2654435761u + l.index * 40503u;
        float v = 0.0f;
        switch (l.flicker)
        {
        case fl::Flicker::Fire:
            // Layered breathing, and now and then a dip as the flame gutters.
            v = 0.14f * Wave(t * 1.3f, seed) + 0.09f * Wave(t * 3.7f, seed + 1) + 0.05f * Wave(t * 9.1f, seed + 2);
            v -= 0.5f * std::max(Wave(t * 0.7f, seed + 3) - 0.6f, 0.0f);
            for (int k = 0; k < 3; ++k) l.position[k] += 0.05f * strength * Wave(t * 2.3f, seed + 4 + k);
            break;
        case fl::Flicker::Candle:
            v = 0.05f * Wave(t * 7.9f, seed) + 0.03f * Wave(t * 15.3f, seed + 1);
            break;
        default:
            v = 0.02f * Wave(t * 0.9f, seed) + 0.01f * Wave(t * 3.1f, seed + 1);
            break;
        }
        l.intensity *= std::max(1.0f + v * strength, 0.3f);
    }
}

namespace wxl::forever::lights
{
    Options& Settings() { return g_options; }

    void SetGiven(const Light* lights, int count)
    {
        g_givenCount = lights ? std::clamp(count, 0, kMaxLights) : 0;
        std::copy(lights, lights + g_givenCount, g_given);
    }

    void SetInteriorGate(bool on) { g_interiorGate = on; }

    bool Frame(const passes::Frame& frame)
    {
        if (frame.index == g_frame) return g_published;
        g_frame = frame.index;

        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const float dt = g_lastCount ? float(double(now.QuadPart - g_lastCount) / double(frequency.QuadPart)) : 0.0f;
        g_lastCount = now.QuadPart;

        g_count = Gather(frame.eye, g_given, g_givenCount, std::min(dt, 0.25f), g_options, g_list);
        // One identity and one order for every consumer: the list sorted by id (given lights, id 0,
        // first in the order they were given), so the same lights keep the same indices.
        for (int i = 0; i < g_count; ++i) g_list[i].id = LightId(g_list[i]);
        std::stable_sort(g_list, g_list + g_count, [](const Light& a, const Light& b) {
            if (a.id != b.id) return a.id < b.id;
            if (a.kind != b.kind) return a.kind < b.kind;
            if (a.owner != b.owner) return std::less<const void*>()(a.owner, b.owner);
            return a.index < b.index;
        });
        TrackMoves(frame.eye, frame.index);
        if (!g_startCount) g_startCount = now.QuadPart;
        const float t = float(double(now.QuadPart - g_startCount) / double(frequency.QuadPart));
        for (int i = 0; i < g_count; ++i)
        {
            for (int k = 0; k < 3; ++k) g_list[i].rest[k] = g_list[i].position[k];
            Animate(g_list[i], t, g_options.flicker);
        }
        // Each light's room, from the interior groups of the map objects this frame drew.
        rooms::Build(frame.eye);
        // One test for both: a light's room, and interior means in a room (where it rests, so the
        // flicker cannot move it across a wall).
        for (int i = 0; i < g_count; ++i)
        {
            Light& l = g_list[i];
            const float r[3] = { l.rest[0] - frame.eye[0], l.rest[1] - frame.eye[1], l.rest[2] - frame.eye[2] };
            float lo = 0.0f, hi = 0.0f;
            l.room = rooms::RoomOf(r, lo, hi);
            l.roomFloor = lo + frame.eye[2];
            l.roomCeiling = hi + frame.eye[2];
            l.interior = g_interiorGate && l.room >= 0 ? 1 : 0;
        }
        cookies::Frame(frame.device, g_list, g_count, frame.eye, frame.index);

        ClusterSpace space{ frame.viewProjRel, { frame.eye[0], frame.eye[1], frame.eye[2] }, kClusterNear,
                            std::log(kClusterFar / kClusterNear) };
        g_published = Publish(frame.device, g_list, g_count, space);
        return g_published;
    }

    const Light* Current(int& count)
    {
        count = g_count;
        return g_list;
    }

    int MovingLights(Moving* out, int max, bool withCarried)
    {
        int n = 0;
        for (size_t i = 0; i < g_moving.size() && n < max; ++i)
            if (withCarried || !g_movingCarriedOnly[i]) out[n++] = g_moving[i];
        return n;
    }

    void CarriedConstants(float out[4])
    {
        out[0] = std::clamp(g_options.carriedCore, 0.0f, 3.0f);
        out[1] = std::max(g_options.nearCap, 0.1f);
        out[2] = std::clamp(g_options.hotCore, 0.0f, 1.0f);
        out[3] = 0.0f;
    }

    float RoomCross()
    {
        return g_options.rooms ? std::clamp(g_options.roomLeak, 0.0f, 1.0f) : 1.0f;
    }

    uint32_t LightId(const Light& l)
    {
        if (!l.owner) return 0;
        const uint32_t id = uint32_t(reinterpret_cast<uintptr_t>(l.owner) >> 2) * 2654435761u ^ (l.index + 1u) * 40503u
                          ^ (uint32_t(l.kind) + 1u) * 0x9E3779B1u;
        return id ? id : 1u;
    }

    void ClusterConstants(float clusterC[4], float clusterD[4])
    {
        clusterC[0] = float(kClustersX);
        clusterC[1] = float(kClustersY);
        clusterC[2] = float(kClustersZ);
        clusterC[3] = float(kClusterTexW);
        clusterD[0] = 1.0f / float(ClusterTextureWidth());
        clusterD[1] = 1.0f / float(ClusterTextureHeight());
        clusterD[2] = kClusterNear;
        clusterD[3] = 1.0f / std::log(kClusterFar / kClusterNear);
    }
}
