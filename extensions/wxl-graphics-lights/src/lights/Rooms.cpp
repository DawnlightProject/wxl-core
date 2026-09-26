// wxl-graphics-lights: the interior rooms near the camera, for indoor fog and for keeping a lamp's light in its room.
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

#include "../core/Extension.hpp"
#include "Rooms.hpp"

#include "wxl/gfx/Matrix.hpp"
#include "game/Lights.hpp"
#include "game/Wmo.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

namespace
{
    namespace wmo   = wxl::game::wmo;
    namespace lt    = wxl::game::lights;
    namespace rooms = wxl::gfx::lights::rooms;

    constexpr size_t   kMaxPlacements = 512;
    constexpr uint32_t kMaxGroups     = 512;   // groups read from one map object's table
    constexpr float    kReach         = 80.0f; // yards from the camera past which a room is not collected
    constexpr float    kEnter         = 45.0f; // a room joins the working set within this many yards
    constexpr float    kLeave         = 65.0f; // and leaves it past this many (hysteresis)
    constexpr float    kDisplace      = 15.0f; // a newcomer displaces a kept room only when this much nearer
    constexpr double   kMissing       = 1.0;   // seconds a kept room may go uncollected before it leaves
    constexpr float    kFade          = 0.3f;  // seconds a room fades in or out over
    constexpr float    kFloorSlack    = 0.1f;  // yards past a room's floor or ceiling still in it

    /// One interior group as unit-cube rows over world positions (q = row . (world, 1)), and its size.
    struct Box
    {
        float  rows[3][4];
        double distance;           // camera to box in yards, 0 when inside
        double volume;
    };

    /// A room of the working set, by its stable identity: the placement and its group. It keeps its box
    /// while briefly uncollected, and fades in on joining and out on leaving, so no gate ever flips.
    struct Tracked
    {
        const void* owner;
        uint32_t    group;
        Box         box;
        float       weight;        // 0..1, eased towards target
        bool        target;        // in the working set (true) or leaving it
        double      lastSeen;      // seconds, when last collected
    };

    struct Found { const void* owner; uint32_t group; Box box; };

    std::vector<Tracked> g_tracked;
    std::vector<Found>   g_found;   // this frame's candidates; kept between frames for its capacity
    int g_listChanges = 0;   // rooms that joined since the last read of Changes()

    float  g_rows[3 * rooms::kMaxRooms][4];
    float  g_weights[rooms::kMaxRooms];
    struct Id { const void* owner; uint32_t group; };
    Id     g_ids[rooms::kMaxRooms];
    int    g_count = 0, g_collected = 0;
    bool   g_cameraIndoor = false;
    double g_last = -1.0;

    double Now()
    {
        LARGE_INTEGER c{}, f{};
        QueryPerformanceCounter(&c);
        QueryPerformanceFrequency(&f);
        return double(c.QuadPart) / double(f.QuadPart);
    }

    /// The group's box as world-space rows into the unit cube, padded; its distance from the eye and volume.
    Box MakeBox(const float* w, const wmo::GroupInfo& g, const float eye[3])
    {
        Box box{};
        double outside = 0.0, volume = 1.0;
        for (int j = 0; j < 3; ++j)
        {
            const double c    = 0.5 * (double(g.bboxMin[j]) + g.bboxMax[j]);
            const double half = std::max(0.5 * (double(g.bboxMax[j]) - g.bboxMin[j]), 0.01);
            const double h    = half + rooms::kPad;
            box.rows[j][0] = float(w[0 * 4 + j] / h);
            box.rows[j][1] = float(w[1 * 4 + j] / h);
            box.rows[j][2] = float(w[2 * 4 + j] / h);
            box.rows[j][3] = float((double(w[12 + j]) - c) / h);
            // The eye in model space, for the distance.
            const double m = double(eye[0]) * w[0 * 4 + j] + double(eye[1]) * w[1 * 4 + j] + double(eye[2]) * w[2 * 4 + j] + w[12 + j];
            const double d = std::fabs(m - c) - h;
            if (d > 0.0) outside += d * d;
            volume *= 2.0 * half;
        }
        box.distance = std::sqrt(outside);
        box.volume   = volume;
        return box;
    }

    /// Whether published room b holds the camera-relative point r: `inset` yards inside its padded
    /// faces horizontally, |q| <= 1 - inset * |row| (each row's length is 1 / its half size), and
    /// between its floor and ceiling; lo and hi receive those, unpadded.
    bool Holds(int b, const float r[3], float inset, float& lo, float& hi)
    {
        if (b < 0 || b >= g_count) return false;
        auto inside = [&](const float* row) {
            const float q = r[0] * row[0] + r[1] * row[1] + r[2] * row[2] + row[3];
            const float len = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
            return std::fabs(q) <= 1.0f - inset * len;
        };
        if (!inside(g_rows[b * 3]) || !inside(g_rows[b * 3 + 1])) return false;
        float a = 0.0f, c = 0.0f;
        if (!rooms::Height(b, r, a, c) || r[2] < a - kFloorSlack || r[2] > c + kFloorSlack) return false;
        lo = a;
        hi = c;
        return true;
    }

    /// The distance of a kept box from the eye, from its rows (each row's length is 1 / its half size).
    double Distance(const Box& b, const float eye[3])
    {
        double outside = 0.0;
        for (int j = 0; j < 3; ++j)
        {
            const float* r = b.rows[j];
            const double scale = std::sqrt(double(r[0]) * r[0] + double(r[1]) * r[1] + double(r[2]) * r[2]);
            const double q = double(r[0]) * eye[0] + double(r[1]) * eye[1] + double(r[2]) * eye[2] + r[3];
            const double d = (std::fabs(q) - 1.0) / std::max(scale, 1e-9);
            if (d > 0.0) outside += d * d;
        }
        return std::sqrt(outside);
    }
}

namespace wxl::gfx::lights::rooms
{
    void Install()
    {
        LIGHTS_LOG_INFO("lights: interior rooms taken from the placed WMOs around the camera, drawn or not; stable identities, "
                        "joining within %.0f yd and leaving past %.0f yd, each fading over %.1f s", kEnter, kLeave, kFade);
    }

    int Build(const float eye[3])
    {
        const double t = Now();
        const float dt = g_last >= 0.0 ? float(std::clamp(t - g_last, 0.0, 0.25)) : 0.0f;
        g_last = t;

        // Every placed map object near the camera, whatever the view culled: its interior groups.
        static lt::WmoPlacement placements[kMaxPlacements];
        const size_t n = std::min(lt::CollectWmoPlacements(eye, kReach, placements, kMaxPlacements), kMaxPlacements);
        std::vector<Found>& found = g_found;
        found.clear();
        for (size_t p = 0; p < n; ++p)
        {
            const lt::WmoPlacement& pl = placements[p];
            float worldToModel[16];
            if (!pl.root || !wxl::gfx::matrix::Invert4(pl.toWorld, worldToModel)) continue;
            wmo::GroupInfo info;
            for (uint32_t g = 0; g < kMaxGroups && wmo::GetGroupInfo(pl.root, g, info); ++g)
            {
                if (!(info.flags & wmo::groupflag::kInterior)) continue;
                Box box = MakeBox(worldToModel, info, eye);
                if (box.distance < kReach) found.push_back(Found{ pl.owner, g, box });
            }
        }
        g_collected = int(found.size());

        // The kept rooms: refreshed from what was collected, or left as they were while briefly missing.
        for (Tracked& r : g_tracked)
        {
            auto it = std::find_if(found.begin(), found.end(), [&](const Found& f) { return f.owner == r.owner && f.group == r.group; });
            if (it != found.end())
            {
                r.box = it->box;
                r.lastSeen = t;
            }
            else r.box.distance = Distance(r.box, eye);
            if (r.target && (r.box.distance > kLeave || t - r.lastSeen > kMissing)) r.target = false;
        }
        // Newcomers, nearest first: a free place, or a kept room far enough behind them gives way.
        std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
            if (a.box.distance != b.box.distance) return a.box.distance < b.box.distance;
            if (a.owner != b.owner) return std::less<const void*>()(a.owner, b.owner);
            return a.group < b.group;
        });
        for (const Found& f : found)
        {
            if (f.box.distance > kEnter) break;
            auto it = std::find_if(g_tracked.begin(), g_tracked.end(), [&](const Tracked& r) { return r.owner == f.owner && r.group == f.group; });
            if (it != g_tracked.end())
            {
                it->target = true;   // a room fading out that is wanted again fades back in
                continue;
            }
            if (int(g_tracked.size()) < rooms::kMaxRooms)
            {
                g_tracked.push_back(Tracked{ f.owner, f.group, f.box, 0.0f, true, t });
                ++g_listChanges;
                continue;
            }
            // Full: the farthest kept room leaves (fading out first) when this one is clearly nearer.
            Tracked* farthest = nullptr;
            for (Tracked& r : g_tracked)
                if (r.target && (!farthest || r.box.distance > farthest->box.distance)) farthest = &r;
            if (farthest && f.box.distance + kDisplace < farthest->box.distance) farthest->target = false;
        }
        // Fades; a room faded out is gone.
        const float step = dt / kFade;
        for (Tracked& r : g_tracked) r.weight = std::clamp(r.weight + (r.target ? step : -step), 0.0f, 1.0f);
        g_tracked.erase(std::remove_if(g_tracked.begin(), g_tracked.end(), [](const Tracked& r) { return !r.target && r.weight <= 0.0f; }),
                        g_tracked.end());

        // Published smallest first (the first holding a point is its own room), camera-relative.
        static std::vector<const Tracked*> order;
        order.clear();
        for (const Tracked& r : g_tracked) order.push_back(&r);
        std::sort(order.begin(), order.end(), [](const Tracked* a, const Tracked* b) {
            if (a->box.volume != b->box.volume) return a->box.volume < b->box.volume;
            if (a->owner != b->owner) return std::less<const void*>()(a->owner, b->owner);
            return a->group < b->group;
        });
        g_cameraIndoor = false;
        g_count = std::min(int(order.size()), rooms::kMaxRooms);
        for (int i = 0; i < g_count; ++i)
        {
            const Tracked& r = *order[i];
            if (r.box.distance <= 0.0 && r.weight > 0.5f) g_cameraIndoor = true;
            for (int j = 0; j < 3; ++j)
            {
                const float* w = r.box.rows[j];
                float* out = g_rows[i * 3 + j];
                out[0] = w[0];
                out[1] = w[1];
                out[2] = w[2];
                out[3] = float(double(w[3]) + double(w[0]) * eye[0] + double(w[1]) * eye[1] + double(w[2]) * eye[2]);
            }
            g_weights[i] = r.weight;
            g_ids[i] = Id{ r.owner, r.group };
        }
        return g_count;
    }

    const float* Weights() { return g_weights; }

    const float (*Rows())[4] { return g_rows; }
    int  Count() { return g_count; }
    bool CameraIndoor() { return g_cameraIndoor; }
    int  Collected() { return g_collected; }

    int Changes()
    {
        const int n = g_listChanges;
        g_listChanges = 0;
        return n;
    }

    bool Height(int b, const float r[3], float& lo, float& hi)
    {
        if (b < 0 || b >= g_count) return false;
        const float* z = g_rows[b * 3 + 2];
        if (std::fabs(z[2]) < 1e-6f) return false;
        // Where the vertical row reaches -1 and 1 at this point, less the padding.
        const float base = r[0] * z[0] + r[1] * z[1] + z[3];
        float a = (-1.0f - base) / z[2], c = (1.0f - base) / z[2];
        if (a > c) std::swap(a, c);
        lo = a + kPad;
        hi = c - kPad;
        return true;
    }

    bool Identity(int b, const void*& owner, uint32_t& group)
    {
        if (b < 0 || b >= g_count) return false;
        owner = g_ids[b].owner;
        group = g_ids[b].group;
        return true;
    }

    int IndexOf(const void* owner, uint32_t group)
    {
        for (int b = 0; b < g_count; ++b)
            if (g_ids[b].owner == owner && g_ids[b].group == group) return b;
        return -1;
    }

    bool Contains(int b, const float r[3], float inset)
    {
        float lo = 0.0f, hi = 0.0f;
        return Holds(b, r, inset, lo, hi);
    }

    int RoomOf(const float r[3], float& lo, float& hi, float inset)
    {
        for (int b = 0; b < g_count; ++b)
            if (g_weights[b] > 0.0f && Holds(b, r, inset, lo, hi)) return b;
        return -1;
    }

    int SettledRoomOf(const float r[3], float& lo, float& hi, float inset)
    {
        int best = -1;
        float a = 0.0f, c = 0.0f;
        for (int b = 0; b < g_count; ++b)
        {
            if (g_weights[b] <= 0.0f || !Holds(b, r, inset, a, c)) continue;
            // Smallest first: the first room fully in wins outright, else the one furthest faded in.
            const bool settled = g_weights[b] >= 1.0f;
            if (settled || best < 0 || g_weights[b] > g_weights[best])
            {
                best = b;
                lo = a;
                hi = c;
            }
            if (settled) break;
        }
        return best;
    }

    uint32_t Mask(int b, const float r[3])
    {
        if (b < 0 || b >= g_count) return 0;
        uint32_t mask = 1u << b;
        float ownLo = 0.0f, ownHi = 0.0f;
        if (!Height(b, r, ownLo, ownHi)) return mask;
        for (int o = 0; o < g_count; ++o)
        {
            float lo = 0.0f, hi = 0.0f;
            if (o == b || !Height(o, r, lo, hi)) continue;
            if (r[2] >= lo - 0.25f && r[2] <= hi + 0.25f && lo >= ownLo - 1.0f && lo <= ownHi + 1.0f) mask |= 1u << o;
        }
        return mask;
    }
}
