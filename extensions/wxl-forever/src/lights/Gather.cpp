// wxl-forever lights: gathering, scoring and fading this frame's light list.
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

#include "Lights.hpp"
#include "ModelTable.hpp"

#include "../core/ExtensionApi.hpp"
#include "game/Lights.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    namespace fl = wxl::forever::lights;
    namespace lt = wxl::game::lights;

    // Room for every light and model around the camera, so the cut below is the ranking's, never the
    // engine's walk order.
    constexpr size_t   kCollectCap  = 4096;
    constexpr size_t   kModelCap    = 4096;
    constexpr float    kFadeSeconds = 1.0f;
    constexpr float    kKeepBonus   = 1.25f;   // score factor of a light chosen last frame, against swaps at the 128th place
    // About ten seconds: models the engine culled (behind the camera) keep their lights, so the set
    // does not change as the camera turns.
    constexpr uint32_t kStaleFrames = 600;

    struct Tracked
    {
        const void* owner;
        uint32_t    index;
        fl::Kind    kind;
        fl::Light   light;
        float       score;
        float       weight;   // 0..1, eased towards 1 while chosen and 0 while not
        bool        chosen;
    };

    std::vector<Tracked> g_tracked;
    std::vector<const void*> g_attached;   // model instances riding another, sorted

    /// The file and rotation behind an engine light's owner (an M2 instance or a WMO placement).
    struct Source
    {
        const void* owner;
        uint64_t    hash;      // table::StemHash of the file's stem
        float       quat[4];   // world direction -> the file's frame
        bool        upright;   // false for a mirrored placement
    };
    std::vector<Source> g_sources;   // sorted by owner

    void AddSource(const void* owner, const char* path, size_t maxLen, const float* toWorld)
    {
        Source s{};
        s.owner = owner;
        s.hash = fl::table::StemHash(path, maxLen);
        s.upright = fl::table::RotationQuat(toWorld, s.quat);
        g_sources.push_back(s);
    }
    char g_status[160]  = "client lights: none gathered";
    char g_nearest[224] = "nearest client light: none gathered";

    const char* KindName(fl::Kind k)
    {
        switch (k)
        {
        case fl::Kind::M2:    return "M2 (model light)";
        case fl::Kind::Wmo:   return "WMO (MOLT)";
        case fl::Kind::Table: return "model table";
        default:              return "given";
        }
    }

    // One light per fixture. A lamp may be lit by several sources at once (the model table's row for
    // its model, a light of the engine's own model, a WMO light the building's author placed on it);
    // gathered apart, they compete for the slots and the one shown changes with the camera. Lights of
    // different sources this close merge into the one of the first source in: table, M2, WMO. The
    // winner keeps its colour, family and cookie and, with mergeReach, takes the larger reach and
    // brightness of the two. A pair is remembered for a while: when one of the two drops out of the
    // gather (its radius, a cull), the merged light stays as it was instead of swapping.
    constexpr float    kMergeTable  = 1.5f;   // yards between a table light and an engine light (the table's offsets are approximate)
    constexpr float    kMergeEngine = 0.5f;   // yards between an M2 and a WMO light
    constexpr uint32_t kPairHold    = 600;    // gathers a pair is remembered once one of it is missing

    struct Key
    {
        const void* owner;
        uint32_t    index;
        fl::Kind    kind;
        bool operator==(const Key& o) const { return owner == o.owner && index == o.index && kind == o.kind; }
    };

    Key KeyOf(const fl::Light& l) { return Key{ l.owner, l.index, l.kind }; }

    int Rank(fl::Kind k) { return k == fl::Kind::Table ? 0 : (k == fl::Kind::M2 ? 1 : 2); }

    struct Pair
    {
        Key       winner, loser;
        fl::Light winnerLight, loserLight;   // as last seen together
        uint32_t  seen;                      // the gather both were last found in
    };
    std::vector<Pair> g_pairs;
    uint32_t          g_gathers = 0;
    DWORD             g_mergeLogged[3] = {};   // table + M2, table + WMO, M2 + WMO

    float Luma(const fl::Light& l)
    {
        return (l.color[0] * 0.299f + l.color[1] * 0.587f + l.color[2] * 0.114f) * l.intensity;
    }

    /// The winner takes the loser's reach and brightness where they are larger; its hue stays, and
    /// its fog halo keeps its own reach.
    void Absorb(fl::Light& w, const fl::Light& l, bool reach)
    {
        if (!reach) return;
        if (w.haloRadius <= 0.0f && l.radius > w.radius)
        {
            w.haloRadius = w.radius;
            w.haloInner = w.innerRadius;
        }
        w.radius = std::max(w.radius, l.radius);
        w.innerRadius = std::max(w.innerRadius, l.innerRadius);
        const float lw = Luma(w), ll = Luma(l);
        if (ll > lw && lw > 1e-4f)
            for (int k = 0; k < 3; ++k) w.color[k] *= ll / lw;
    }

    void LogMerge(const fl::Light& w, const fl::Light& l, float d)
    {
        const int type = w.kind == fl::Kind::Table ? (l.kind == fl::Kind::M2 ? 0 : 1) : 2;
        if (GetTickCount() - g_mergeLogged[type] < 10000) return;
        g_mergeLogged[type] = GetTickCount();
        static const char* const kTypes[] = { "model-table light + M2 light", "model-table light + WMO light", "M2 light + WMO light" };
        WLOG_INFO("lights: merged %s %.2f yd apart at (%.1f %.1f %.1f): kept the %s colour (%.2f %.2f %.2f) reach %.1f, "
                  "dropped colour (%.2f %.2f %.2f) reach %.1f",
                  kTypes[type], d, w.position[0], w.position[1], w.position[2], w.kind == fl::Kind::Table ? "table" : "M2",
                  w.color[0], w.color[1], w.color[2], w.radius, l.color[0], l.color[1], l.color[2], l.radius);
    }

    /// Merges the lights of one fixture in found[0..n), keeping the order of the rest; returns the new count.
    size_t MergeFixtures(fl::Light* found, size_t n, bool reach)
    {
        ++g_gathers;
        std::vector<char> gone(n, 0);
        for (size_t i = 0; i < n; ++i)
        {
            if (gone[i] || found[i].kind == fl::Kind::Given) continue;
            for (size_t j = i + 1; j < n; ++j)
            {
                if (gone[j] || found[j].kind == found[i].kind || found[j].kind == fl::Kind::Given) continue;
                const fl::Light& a = found[i];
                const fl::Light& b = found[j];
                const bool table = a.kind == fl::Kind::Table || b.kind == fl::Kind::Table;
                const float limit = table ? kMergeTable : kMergeEngine;
                const float dx = a.position[0] - b.position[0], dy = a.position[1] - b.position[1], dz = a.position[2] - b.position[2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > limit * limit) continue;
                const size_t w = Rank(a.kind) <= Rank(b.kind) ? i : j, l = w == i ? j : i;
                const Key wk = KeyOf(found[w]), lk = KeyOf(found[l]);
                auto it = std::find_if(g_pairs.begin(), g_pairs.end(), [&](const Pair& p) { return p.winner == wk && p.loser == lk; });
                if (it == g_pairs.end())
                {
                    LogMerge(found[w], found[l], std::sqrt(d2));
                    g_pairs.push_back(Pair{ wk, lk, found[w], found[l], g_gathers });
                }
                else
                {
                    it->winnerLight = found[w];
                    it->loserLight = found[l];
                    it->seen = g_gathers;
                }
                Absorb(found[w], found[l], reach);
                gone[l] = 1;
                if (l == i) break;
            }
        }
        // A remembered pair with one side missing: the winner alone keeps what it took; the loser
        // alone stands in for the winner, as the merged light it was, under the winner's identity.
        for (Pair& p : g_pairs)
        {
            if (p.seen == g_gathers) continue;
            for (size_t i = 0; i < n; ++i)
            {
                if (gone[i]) continue;
                const Key k = KeyOf(found[i]);
                if (k == p.winner) { Absorb(found[i], p.loserLight, reach); break; }
                if (k == p.loser)
                {
                    // Both here but apart: each stays itself, so no two lights share an identity.
                    if (std::any_of(found, found + n, [&](const fl::Light& o) { return KeyOf(o) == p.winner; })) break;
                    fl::Light stand = p.winnerLight;
                    for (int c = 0; c < 3; ++c)
                        stand.position[c] = found[i].position[c] + (p.winnerLight.position[c] - p.loserLight.position[c]);
                    Absorb(stand, found[i], reach);
                    found[i] = stand;
                    break;
                }
            }
        }
        g_pairs.erase(std::remove_if(g_pairs.begin(), g_pairs.end(),
                                     [](const Pair& p) { return g_gathers - p.seen > kPairHold; }),
                      g_pairs.end());
        size_t kept = 0;
        for (size_t i = 0; i < n; ++i)
            if (!gone[i]) found[kept++] = found[i];
        return kept;
    }

    /// The engine's light as the service keeps it: its working colour, capped, its authored radii.
    fl::Light FromEngine(const lt::PointLight& l)
    {
        fl::Light o{};
        for (int k = 0; k < 3; ++k)
        {
            o.position[k] = l.position[k];
            o.color[k] = std::min(l.color[k], 8.0f);
        }
        o.radius      = l.attenEnd > 0.0f ? l.attenEnd : 10.0f;
        o.innerRadius = std::clamp(l.attenStart, 0.0f, o.radius * 0.95f);
        o.intensity   = 1.0f;
        o.cosCone     = -2.0f;
        o.kind  = l.kind == lt::Kind::Wmo ? fl::Kind::Wmo : fl::Kind::M2;
        // The engine's lights carry no family: a warm model light is taken for a fire, a warm WMO
        // light for a lantern.
        const bool warm = l.color[0] > 1.3f * l.color[2] && l.color[0] > 0.05f;
        o.flicker = !warm ? fl::Flicker::None : (o.kind == fl::Kind::M2 ? fl::Flicker::Fire : fl::Flicker::Lantern);
        o.profile = warm && o.kind == fl::Kind::M2 ? fl::Profile::Flame : fl::Profile::None;
        o.size    = o.kind == fl::Kind::M2 ? 0.25f : 0.2f;
        o.owner = l.owner;
        o.index = l.index;
        return o;
    }
}

namespace wxl::forever::lights
{
    int Gather(const float eye[3], const Light* given, int givenCount, float dt, const Options& options,
               Light out[kMaxLights])
    {
        int count = 0;
        for (int i = 0; i < givenCount && count < kMaxLights; ++i) out[count++] = given[i];
        const int slots = kMaxLights - count;

        static lt::PointLight engine[kCollectCap];
        static std::vector<Light> foundStore(kCollectCap);   // on the heap: a Light has initialisers
        Light* found = foundStore.data();
        size_t n = 0, wmo = 0, table = 0, carried = 0;
        if (options.engine && slots > 0)
        {
            lt::Query q;
            for (int k = 0; k < 3; ++k) q.center[k] = eye[k];
            q.radius = options.radius;
            q.m2  = true;
            q.wmo = options.wmo;
            q.maxStaleFrames = kStaleFrames;
            const size_t matched = lt::Collect(q, engine, kCollectCap);
            const size_t e = std::min(matched, kCollectCap);
            static bool warned = false;
            if (matched > kCollectCap && !warned)
            {
                warned = true;
                WLOG_WARN("lights: %u engine lights within %.0f yd, %u kept (walk order past that)", unsigned(matched),
                          options.radius, unsigned(kCollectCap));
            }
            // A model light whose instance rides another model is carried: a torch in a hand, a
            // glow on a unit. Its light sits a few inches from the carrier's body. The same walk
            // gives each engine light the file and rotation its cookie is keyed and oriented by.
            g_attached.clear();
            g_sources.clear();
            if (e > wmo)
            {
                static lt::ModelInstance instances[kModelCap];
                lt::ModelQuery mq;
                for (int k = 0; k < 3; ++k) mq.center[k] = eye[k];
                mq.radius = options.radius + 4.0f;
                mq.maxStaleFrames = kStaleFrames;
                mq.unlitOnly = false;
                mq.includeAttached = true;
                const size_t m = std::min(lt::CollectModels(mq, instances, kModelCap), kModelCap);
                for (size_t i = 0; i < m; ++i)
                {
                    if (instances[i].attached && instances[i].lightCount > 0) g_attached.push_back(instances[i].owner);
                    if (instances[i].lightCount > 0) AddSource(instances[i].owner, instances[i].stem, sizeof instances[i].stem, instances[i].toWorld);
                }
                std::sort(g_attached.begin(), g_attached.end());
                if (options.wmo)
                {
                    static lt::WmoPlacement placements[256];
                    const size_t p = std::min(lt::CollectWmoPlacements(eye, options.radius, placements, 256), size_t(256));
                    for (size_t i = 0; i < p; ++i)
                        if (placements[i].lightCount > 0) AddSource(placements[i].owner, placements[i].path, sizeof placements[i].path, placements[i].toWorld);
                }
                std::sort(g_sources.begin(), g_sources.end(), [](const Source& a, const Source& b) { return a.owner < b.owner; });
            }
            for (size_t i = 0; i < e; ++i)
            {
                found[n] = FromEngine(engine[i]);
                if (engine[i].kind == lt::Kind::Wmo) ++wmo;
                else if (std::binary_search(g_attached.begin(), g_attached.end(), engine[i].owner))
                {
                    found[n].carried = 1;
                    ++carried;
                }
                const auto src = std::lower_bound(g_sources.begin(), g_sources.end(), engine[i].owner,
                                                  [](const Source& s, const void* o) { return s.owner < o; });
                if (src != g_sources.end() && src->owner == engine[i].owner && src->upright)
                {
                    found[n].cookieSource = src->hash;
                    for (int k = 0; k < 4; ++k) found[n].cookieRotation[k] = src->quat[k];
                }
                ++n;
            }
            if (options.table)
            {
                table = std::min(table::Collect(eye, options.radius, kStaleFrames, found + n, kCollectCap - n, nullptr),
                                 kCollectCap - n);
                n += table;
            }
        }

        const size_t gathered = n;
        if (options.merge) n = MergeFixtures(found, n, options.mergeReach);

        // Score everything in range by brightness over distance, then mark the best as chosen; a light
        // chosen last frame counts kKeepBonus times, so two near-equal lights do not swap every frame.
        struct Pick { float score; size_t index; };
        std::vector<Pick> picks;
        picks.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            const Light& l = found[i];
            const float dx = l.position[0] - eye[0], dy = l.position[1] - eye[1], dz = l.position[2] - eye[2];
            const float luma = (l.color[0] * 0.299f + l.color[1] * 0.587f + l.color[2] * 0.114f) * l.intensity;
            float score = luma / (1.0f + (dx * dx + dy * dy + dz * dz) / (l.radius * l.radius));
            if (n > size_t(std::max(slots, 0)) && std::any_of(g_tracked.begin(), g_tracked.end(), [&](const Tracked& t) {
                    return t.chosen && t.owner == l.owner && t.index == l.index && t.kind == l.kind; }))
                score *= kKeepBonus;
            picks.push_back({ score, i });
        }
        std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.score > b.score; });
        if (picks.size() > size_t(std::max(slots, 0))) picks.resize(size_t(std::max(slots, 0)));

        for (Tracked& t : g_tracked) t.chosen = false;
        for (const Pick& p : picks)
        {
            const Light& l = found[p.index];
            auto it = std::find_if(g_tracked.begin(), g_tracked.end(), [&](const Tracked& t) {
                return t.owner == l.owner && t.index == l.index && t.kind == l.kind;
            });
            if (it == g_tracked.end()) g_tracked.push_back({ l.owner, l.index, l.kind, l, p.score, 0.0f, true });
            else
            {
                it->light  = l;
                it->score  = p.score;
                it->chosen = true;
            }
        }

        const float step = dt / kFadeSeconds;
        for (Tracked& t : g_tracked)
            t.weight = std::clamp(t.weight + (t.chosen ? step : -step), 0.0f, 1.0f);
        g_tracked.erase(std::remove_if(g_tracked.begin(), g_tracked.end(),
                                       [](const Tracked& t) { return !t.chosen && t.weight <= 0.0f; }),
                        g_tracked.end());

        // Chosen lights first, then the ones still fading out, while slots remain.
        std::sort(g_tracked.begin(), g_tracked.end(), [](const Tracked& a, const Tracked& b) {
            if (a.chosen != b.chosen) return a.chosen;
            return a.weight * a.score > b.weight * b.score;
        });
        int used = 0;
        for (const Tracked& t : g_tracked)
        {
            if (count >= kMaxLights) break;
            if (t.weight <= 0.0f) continue;
            Light l = t.light;
            l.intensity *= t.weight;
            out[count++] = l;
            ++used;
        }

        std::snprintf(g_status, sizeof g_status,
                      "client lights: %u found (%u WMO, %u model table, %u carried), %u merged, %d used of %d, %d given",
                      unsigned(gathered), unsigned(wmo), unsigned(table), unsigned(carried), unsigned(gathered - n), used,
                      kMaxLights, givenCount);

        size_t nearest = n;
        float nearestSq = 1e30f;
        for (size_t i = 0; i < n; ++i)
        {
            const float dx = found[i].position[0] - eye[0], dy = found[i].position[1] - eye[1], dz = found[i].position[2] - eye[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < nearestSq) { nearestSq = d2; nearest = i; }
        }
        if (nearest == n)
            std::snprintf(g_nearest, sizeof g_nearest, "nearest client light: none within %.0f yd", options.radius);
        else
        {
            const Light& l = found[nearest];
            const bool chosen = std::any_of(g_tracked.begin(), g_tracked.end(), [&](const Tracked& t) {
                return t.chosen && t.owner == l.owner && t.index == l.index && t.kind == l.kind;
            });
            std::snprintf(g_nearest, sizeof g_nearest,
                          "nearest client light: %s, %.1f yd from the camera at (%.1f %.1f %.1f), radius %.1f (inner %.1f), "
                          "colour (%.2f %.2f %.2f), %s",
                          KindName(l.kind), std::sqrt(nearestSq), l.position[0], l.position[1], l.position[2], l.radius,
                          l.innerRadius, l.color[0], l.color[1], l.color[2], chosen ? "chosen" : "not chosen");
        }
        return count;
    }

    const char* Status() { return g_status; }

    const char* Nearest() { return g_nearest; }
}
