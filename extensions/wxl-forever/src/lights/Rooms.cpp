// wxl-forever lights: the interior rooms near the camera, for indoor fog and for keeping a lamp's light in its room.
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
#include "../core/Matrix.hpp"
#include "Rooms.hpp"

#include "game/Lights.hpp"
#include "game/Wmo.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

namespace
{
    namespace wmo   = wxl::game::wmo;
    namespace lt    = wxl::game::lights;
    namespace rooms = wxl::forever::lights::rooms;

    constexpr size_t   kMaxPlacements = 512;
    constexpr uint32_t kMaxGroups     = 512;   // groups read from one map object's table
    constexpr float    kReach         = 80.0f; // yards from the camera past which a room is ignored
    constexpr float    kFloorSlack    = 0.1f;  // yards past a room's floor or ceiling still in it
    constexpr double   kKeepBonus     = 10.0;  // yards nearer a room already in the list counts

    struct Room
    {
        float  rows[3][4];
        double distance;           // camera to box in yards, 0 when inside (less the keep bonus while sorting)
        double inside;             // the true distance, for the camera's own test
        double volume;
        const void* owner;         // identity: the placement and its group
        uint32_t    group;
    };

    // Last frame's rooms, by identity: they keep their place among the kept ones while in reach, so
    // a room does not drop out and back as the camera moves between two near-equal distances.
    struct RoomId { const void* owner; uint32_t group; };
    std::vector<RoomId> g_kept;
    int g_listChanges = 0;   // rooms that entered the list since the last read of Changes()

    float  g_rows[3 * rooms::kMaxRooms][4];
    int    g_count = 0, g_collected = 0;
    bool   g_cameraIndoor = false;

    /// The group's box as camera-relative rows into the unit cube, padded; its distance and volume.
    Room MakeRoom(const float* w, const wmo::GroupInfo& g, const float eye[3])
    {
        Room room{};
        double outside = 0.0, volume = 1.0;
        for (int j = 0; j < 3; ++j)
        {
            // The camera in model space: the constant term of the camera-relative transform.
            const double m0 = double(eye[0]) * w[0 * 4 + j] + double(eye[1]) * w[1 * 4 + j]
                            + double(eye[2]) * w[2 * 4 + j] + w[12 + j];
            const double c  = 0.5 * (double(g.bboxMin[j]) + g.bboxMax[j]);
            const double half = std::max(0.5 * (double(g.bboxMax[j]) - g.bboxMin[j]), 0.01);
            const double h  = half + rooms::kPad;
            room.rows[j][0] = float(w[0 * 4 + j] / h);
            room.rows[j][1] = float(w[1 * 4 + j] / h);
            room.rows[j][2] = float(w[2 * 4 + j] / h);
            room.rows[j][3] = float((m0 - c) / h);
            const double d = std::fabs(m0 - c) - h;
            if (d > 0.0) outside += d * d;
            volume *= 2.0 * half;
        }
        room.distance = std::sqrt(outside);
        room.volume   = volume;
        return room;
    }
}

namespace wxl::forever::lights::rooms
{
    bool Install()
    {
        WLOG_INFO("lights: interior rooms taken from the placed WMOs around the camera, drawn or not");
        return true;
    }

    int Build(const float eye[3])
    {
        // Every placed map object near the camera, whatever the view culled: its interior groups.
        static lt::WmoPlacement placements[kMaxPlacements];
        const size_t n = std::min(lt::CollectWmoPlacements(eye, kReach, placements, kMaxPlacements), kMaxPlacements);
        std::vector<Room> found;
        for (size_t p = 0; p < n; ++p)
        {
            const lt::WmoPlacement& pl = placements[p];
            float worldToModel[16];
            if (!pl.root || !wxl::forever::matrix::Invert4(pl.toWorld, worldToModel)) continue;
            wmo::GroupInfo info;
            for (uint32_t g = 0; g < kMaxGroups && wmo::GetGroupInfo(pl.root, g, info); ++g)
            {
                if (!(info.flags & wmo::groupflag::kInterior)) continue;
                Room room = MakeRoom(worldToModel, info, eye);
                room.owner = pl.owner;
                room.group = g;
                if (room.distance < kReach) found.push_back(room);
            }
        }
        g_collected = int(found.size());
        // The nearest rooms, last frame's counted kKeepBonus yards nearer, then smallest first: the
        // first holding a point is its own room. Equal volumes keep a fixed order by identity.
        auto wasKept = [](const Room& r) {
            return std::any_of(g_kept.begin(), g_kept.end(), [&](const RoomId& k) { return k.owner == r.owner && k.group == r.group; });
        };
        for (Room& r : found)
        {
            r.inside = r.distance;
            if (wasKept(r)) r.distance -= kKeepBonus;
        }
        const size_t keep = std::min<size_t>(found.size(), kMaxRooms);
        std::stable_sort(found.begin(), found.end(), [](const Room& a, const Room& b) {
            if (a.distance != b.distance) return a.distance < b.distance;
            if (a.owner != b.owner) return std::less<const void*>()(a.owner, b.owner);
            return a.group < b.group;
        });
        found.resize(keep);
        for (const Room& r : found) if (!wasKept(r)) ++g_listChanges;
        g_kept.clear();
        for (const Room& r : found) g_kept.push_back(RoomId{ r.owner, r.group });
        std::sort(found.begin(), found.end(), [](const Room& a, const Room& b) {
            if (a.volume != b.volume) return a.volume < b.volume;
            if (a.owner != b.owner) return std::less<const void*>()(a.owner, b.owner);
            return a.group < b.group;
        });
        g_cameraIndoor = false;
        g_count = int(keep);
        for (int i = 0; i < g_count; ++i)
        {
            if (found[i].inside <= 0.0) g_cameraIndoor = true;
            std::memcpy(g_rows[i * 3], found[i].rows, sizeof found[i].rows);
        }
        return g_count;
    }

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

    int RoomOf(const float r[3], float& lo, float& hi)
    {
        for (int b = 0; b < g_count; ++b)
        {
            const float* x = g_rows[b * 3];
            const float* y = g_rows[b * 3 + 1];
            if (std::fabs(r[0] * x[0] + r[1] * x[1] + r[2] * x[2] + x[3]) > 1.0f) continue;
            if (std::fabs(r[0] * y[0] + r[1] * y[1] + r[2] * y[2] + y[3]) > 1.0f) continue;
            float a = 0.0f, c = 0.0f;
            if (!Height(b, r, a, c) || r[2] < a - kFloorSlack || r[2] > c + kFloorSlack) continue;
            lo = a;
            hi = c;
            return b;
        }
        return -1;
    }
}
