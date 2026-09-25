// wxl-forever fog: projectiles punch holes through the fog that refill slowly, in billows.
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
#include "Projectiles.hpp"

#include "game/Effects.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
    namespace efx = wxl::game::effects;
    namespace pj  = wxl::forever::fog::projectiles;

    constexpr size_t kCollectCap    = 64;
    constexpr float  kSampleSeconds = 0.08f;   // one segment per this much flight
    constexpr int    kTileTexW      = pj::kTilesX * pj::kTileSlots / 4;

    struct Segment
    {
        float head[3], tail[3];
        float radius;
        float birth;
        float fire;                // 1 for a fire spell's trail
        const void* owner;         // the missile, while it flies; null once it landed
        uint32_t serial;           // order of creation within the missile's trail
    };

    struct Track
    {
        const void* owner;
        float last[3];             // where the last segment ended
        float lastTime;
        float position[3];         // where the missile was last seen
        float radius;
        float fire;
        uint32_t serial;
        bool  seen;
    };

    std::vector<Segment> g_pool;
    std::vector<Track>   g_tracks;
    pj::Tuning           g_tuning;
    float                g_now = 0.0f;
    int                  g_tracked = 0;
    float                g_reach = 0.0f;

    IDirect3DTexture9*   g_segmentTex = nullptr;   // kMaxSegments x 4, A32B32G32R32F
    IDirect3DTexture9*   g_tileTex    = nullptr;   // kTileTexW x kTilesY, A16B16G16R16
    bool                 g_texFailed  = false;

    /// A fire spell's missile, by its model: its hole widens and its billows are warmer.
    bool Fire(const efx::Missile& m)
    {
        if (m.kind != efx::MissileKind::Spell) return false;
        char lower[sizeof m.model];
        size_t i = 0;
        for (; i + 1 < sizeof lower && m.model[i]; ++i) lower[i] = char(std::tolower(uint8_t(m.model[i])));
        lower[i] = 0;
        for (const char* word : { "fire", "flame", "pyro", "lava", "meteor", "scorch", "immolat", "incinerat",
                                  "hellfire", "magma", "ember", "fel" })
            if (std::strstr(lower, word)) return true;
        return false;
    }

    float Life() { return g_tuning.hold + std::max(g_tuning.refill, 0.1f); }

    void Push(const float head[3], const float tail[3], float radius, float fire, const void* owner, uint32_t serial)
    {
        Segment s{};
        std::copy(head, head + 3, s.head);
        std::copy(tail, tail + 3, s.tail);
        s.radius = radius;
        s.birth  = g_now;
        s.fire   = fire;
        s.owner  = owner;
        s.serial = serial;
        g_pool.push_back(s);
    }

    /// A full pool merges the two consecutive segments of one trail whose ages differ least (their
    /// refill then differs by a hundredth, which shows nothing); failing that, the most refilled
    /// segment leaves.
    void Shrink()
    {
        while (int(g_pool.size()) > pj::kMaxSegments)
        {
            int best = -1, next = -1;
            float bestGap = 1e9f;
            for (int i = 0; i < int(g_pool.size()); ++i)
                for (int j = 0; j < int(g_pool.size()); ++j)
                {
                    const Segment& a = g_pool[i];
                    const Segment& b = g_pool[j];
                    if (i == j || a.fire != b.fire) continue;
                    // b continues a: its tail is a's head.
                    const float dx = a.head[0] - b.tail[0], dy = a.head[1] - b.tail[1], dz = a.head[2] - b.tail[2];
                    if (dx * dx + dy * dy + dz * dz > 0.01f) continue;
                    const float gap = std::fabs(b.birth - a.birth);
                    if (gap < bestGap) { bestGap = gap; best = i; next = j; }
                }
            if (best >= 0)
            {
                Segment& a = g_pool[best];
                const Segment& b = g_pool[next];
                std::copy(b.head, b.head + 3, a.head);
                a.radius = std::max(a.radius, b.radius);
                a.birth  = 0.5f * (a.birth + b.birth);
                a.serial = b.serial;
                g_pool.erase(g_pool.begin() + next);
                continue;
            }
            const auto oldest = std::min_element(g_pool.begin(), g_pool.end(),
                                                 [](const Segment& a, const Segment& b) { return a.birth < b.birth; });
            g_pool.erase(oldest);
        }
    }

    void ReleaseTex()
    {
        if (g_segmentTex) { g_segmentTex->Release(); g_segmentTex = nullptr; }
        if (g_tileTex)    { g_tileTex->Release();    g_tileTex = nullptr; }
    }
}

namespace wxl::forever::fog::projectiles
{
    void Update(const float eye[3], float seconds, const Tuning& tuning)
    {
        g_tuning = tuning;
        // The clock wrapped (hourly) or jumped back: the old trails cannot be aged; let them go.
        if (seconds < g_now - 1.0f)
        {
            g_pool.clear();
            g_tracks.clear();
        }
        g_now = seconds;

        static efx::Missile missiles[kCollectCap];
        efx::MissileQuery q;
        for (int k = 0; k < 3; ++k) q.center[k] = eye[k];
        q.radius = 150.0f;
        const size_t n = std::min(efx::CollectMissiles(q, missiles, kCollectCap), kCollectCap);

        for (Track& t : g_tracks) t.seen = false;
        g_tracked = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const efx::Missile& m = missiles[i];
            if (m.speed <= 0.5f) continue;
            ++g_tracked;
            auto it = std::find_if(g_tracks.begin(), g_tracks.end(), [&](const Track& t) { return t.owner == m.owner; });
            if (it == g_tracks.end())
            {
                Track t{};
                t.owner = m.owner;
                std::copy(m.position, m.position + 3, t.last);
                t.lastTime = g_now;
                g_tracks.push_back(t);
                it = g_tracks.end() - 1;
            }
            Track& t = *it;
            t.seen = true;
            std::copy(m.position, m.position + 3, t.position);
            t.radius = std::max(m.radius * tuning.radius, 0.35f);
            t.fire = Fire(m) ? 1.0f : 0.0f;
            if (g_now - t.lastTime >= kSampleSeconds)
            {
                Push(m.position, t.last, t.radius, t.fire, t.owner, t.serial++);
                std::copy(m.position, m.position + 3, t.last);
                t.lastTime = g_now;
            }
        }

        // A missile gone from the list has landed: its last stretch, then a crater where it stopped.
        for (auto it = g_tracks.begin(); it != g_tracks.end();)
        {
            if (it->seen) { ++it; continue; }
            Push(it->position, it->last, it->radius, it->fire, nullptr, it->serial++);
            const float crater = std::max(it->radius * 2.0f, 1.0f) * std::max(tuning.crater, 0.0f);
            if (crater > 0.0f) Push(it->position, it->position, crater, it->fire, nullptr, it->serial++);
            it = g_tracks.erase(it);
        }
        for (Segment& s : g_pool)
            if (s.owner && std::none_of(g_tracks.begin(), g_tracks.end(), [&](const Track& t) { return t.owner == s.owner; }))
                s.owner = nullptr;

        // Refilled segments leave; a full pool merges its oldest.
        const float life = Life();
        g_pool.erase(std::remove_if(g_pool.begin(), g_pool.end(), [&](const Segment& s) { return g_now - s.birth > life; }),
                     g_pool.end());
        Shrink();
    }

    bool Publish(IDirect3DDevice9* dev, const float eye[3], const float viewProjRel[16])
    {
        if (!dev) return false;
        if (!g_segmentTex || !g_tileTex)
        {
            if (g_texFailed) return false;
            if (FAILED(dev->CreateTexture(kMaxSegments, 4, 1, D3DUSAGE_DYNAMIC, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT,
                                          &g_segmentTex, nullptr))
                || FAILED(dev->CreateTexture(kTileTexW, kTilesY, 1, D3DUSAGE_DYNAMIC, D3DFMT_A16B16G16R16, D3DPOOL_DEFAULT,
                                             &g_tileTex, nullptr)))
            {
                ReleaseTex();
                g_texFailed = true;
                WLOG_WARN("fog: projectile trail textures unavailable; no trails");
                return false;
            }
        }

        static float rows[4][kMaxSegments][4];
        static uint16_t tiles[kTilesY][kTilesX][kTileSlots];
        uint8_t counts[kTilesY][kTilesX] = {};
        std::memset(rows, 0, sizeof rows);
        std::memset(tiles, 0xFF, sizeof tiles);

        const Tuning& t = g_tuning;
        const float windX = 0.944f, windY = 0.330f;
        g_reach = 0.0f;
        const int count = std::min(int(g_pool.size()), kMaxSegments);
        for (int i = 0; i < count; ++i)
        {
            const Segment& s = g_pool[i];
            const float age = std::max(g_now - s.birth, 0.0f);
            // Hold, then an ease in and out over the refill time.
            const float u = std::clamp((age - t.hold) / std::max(t.refill, 0.1f), 0.0f, 1.0f);
            const float progress = u * u * (3.0f - 2.0f * u);
            // A fire spell's hole widens slowly while it lasts.
            const float grow = 1.0f + s.fire * t.heat * std::min(age / 1.5f, 1.0f);
            const float radius = s.radius * grow;
            const float size = std::max(t.billowSize, 0.2f);
            const float drift = age * 0.8f / size * 0.25f;

            for (int k = 0; k < 3; ++k)
            {
                rows[0][i][k] = s.head[k] - eye[k];
                rows[1][i][k] = s.tail[k] - eye[k];
            }
            rows[0][i][3] = radius;
            rows[1][i][3] = progress;
            // Billows thicken as the refill runs and fade as it ends.
            rows[2][i][0] = t.billow * 4.0f * progress * (1.0f - progress);
            rows[2][i][1] = t.roll * age;
            rows[2][i][2] = s.fire * t.smoke;
            rows[2][i][3] = 1.0f / size;
            rows[3][i][0] = -windX * drift;
            rows[3][i][1] = -windY * drift;
            rows[3][i][2] = age * 0.02f;
            rows[3][i][3] = t.carve;

            // Bounds: the segment's sphere, widened for the billows at its edge.
            float mid[3], half = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                mid[k] = 0.5f * (rows[0][i][k] + rows[1][i][k]);
                const float h = 0.5f * (rows[0][i][k] - rows[1][i][k]);
                half += h * h;
            }
            const float bound = std::sqrt(half) + radius * 2.5f + 0.5f;
            const float dist = std::sqrt(mid[0] * mid[0] + mid[1] * mid[1] + mid[2] * mid[2]);
            g_reach = std::max(g_reach, dist + bound);

            float u0 = 0.0f, u1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
            if (dist > bound * 1.05f + 0.5f)
            {
                u0 = v0 = 1e9f;
                u1 = v1 = -1e9f;
                bool behind = false;
                for (int c = 0; c < 8 && !behind; ++c)
                {
                    const float p[3] = { mid[0] + ((c & 1) ? bound : -bound), mid[1] + ((c & 2) ? bound : -bound),
                                         mid[2] + ((c & 4) ? bound : -bound) };
                    float clip[4];
                    for (int j = 0; j < 4; ++j)
                        clip[j] = p[0] * viewProjRel[j] + p[1] * viewProjRel[4 + j] + p[2] * viewProjRel[8 + j] + viewProjRel[12 + j];
                    if (clip[3] <= 1e-3f) { behind = true; break; }
                    const float su = clip[0] / clip[3] * 0.5f + 0.5f, sv = 0.5f - clip[1] / clip[3] * 0.5f;
                    u0 = std::min(u0, su); u1 = std::max(u1, su);
                    v0 = std::min(v0, sv); v1 = std::max(v1, sv);
                }
                if (behind) { u0 = v0 = 0.0f; u1 = v1 = 1.0f; }
                if (u1 < 0.0f || u0 > 1.0f || v1 < 0.0f || v0 > 1.0f) continue;
            }
            const int x0 = std::clamp(int(std::floor(u0 * kTilesX)), 0, kTilesX - 1);
            const int x1 = std::clamp(int(std::floor(u1 * kTilesX)), 0, kTilesX - 1);
            const int y0 = std::clamp(int(std::floor(v0 * kTilesY)), 0, kTilesY - 1);
            const int y1 = std::clamp(int(std::floor(v1 * kTilesY)), 0, kTilesY - 1);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    if (counts[y][x] < kTileSlots) tiles[y][x][counts[y][x]++] = uint16_t(i);
        }

        D3DLOCKED_RECT rect{};
        if (SUCCEEDED(g_segmentTex->LockRect(0, &rect, nullptr, D3DLOCK_DISCARD)))
        {
            for (int row = 0; row < 4; ++row)
                std::memcpy(static_cast<uint8_t*>(rect.pBits) + size_t(row) * rect.Pitch, rows[row], sizeof rows[row]);
            g_segmentTex->UnlockRect(0);
        }
        if (SUCCEEDED(g_tileTex->LockRect(0, &rect, nullptr, D3DLOCK_DISCARD)))
        {
            for (int y = 0; y < kTilesY; ++y)
                std::memcpy(static_cast<uint8_t*>(rect.pBits) + size_t(y) * rect.Pitch, tiles[y], sizeof tiles[y]);
            g_tileTex->UnlockRect(0);
        }
        return true;
    }

    IDirect3DTexture9* SegmentTexture() { return g_segmentTex; }
    IDirect3DTexture9* TileTexture() { return g_tileTex; }
    int Live() { return int(std::min(g_pool.size(), size_t(kMaxSegments))); }
    float Reach() { return g_reach; }
    int Tracked() { return g_tracked; }

    void ReleaseTextures()
    {
        ReleaseTex();
        g_texFailed = false;
    }
}
