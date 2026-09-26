// wxl-graphics-lights: gathering, scoring and fading this frame's light list.
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
#include "Lights.hpp"
#include "ModelTable.hpp"
#include "Families.hpp"

#include "game/Lights.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <unordered_map>
#include <vector>

namespace
{
    namespace gl = wxl::gfx::lights;
    namespace lt = wxl::game::lights;

    // Room for every light and model around the camera, so the cut below is the ranking's, never the
    // engine's walk order.
    constexpr size_t   kCollectCap  = 4096;
    constexpr size_t   kModelCap    = 4096;
    constexpr float    kFadeSeconds = 1.0f;
    constexpr float    kKeepBonus   = 1.5f;    // score factor of a light chosen last frame, against swaps at the 128th place
    constexpr double   kHoldSeconds = 2.0;     // a light once chosen stays chosen this long while it is found
    // Models the engine culled (behind the camera, or just off screen) keep their lights this long,
    // with their last values, so the set does not change as the camera turns. The engine stamps its
    // lights in scene frames; StaleFrames turns the seconds into frames at the rate the scene runs.
    constexpr double   kStaleSeconds = 10.0;
    // The model table's lights stand on placed doodads that never move: however long ago the engine
    // last animated one, its light is where it was, so their age is not asked at all.
    constexpr uint32_t kAnyAge = 0xFFFFFFFFu;

    struct Tracked
    {
        const void* owner;
        uint32_t    index;
        gl::Kind    kind;
        gl::Light   light;
        float       score;
        float       weight;   // 0..1, eased towards 1 while chosen and 0 while not
        bool        chosen;
        double      since;    // g_clock when it was last chosen after not being chosen
    };

    std::vector<Tracked> g_tracked;
    double               g_clock = 0.0;   // seconds of gathers, for the hold

    /// The scene frames of the last kStaleSeconds. A fixed count of frames would keep a culled lamp
    /// 2.5 s at 240 frames a second and 20 s at 30. The scene's frame counter is sampled every quarter
    /// second; the newest sample at least kStaleSeconds old gives the count, and until there is one, the
    /// rate so far.
    uint32_t StaleFrames()
    {
        struct Sample { double t; uint32_t frame; };
        constexpr int kSamples = 64;   // 16 s at one a quarter second
        static Sample samples[kSamples];
        static int count = 0, head = 0;
        LARGE_INTEGER counter{}, frequency{};
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);
        const double now = double(counter.QuadPart) / double(frequency.QuadPart);
        const uint32_t frame = lt::SceneFrame();
        const Sample* newest = count ? &samples[(head + kSamples - 1) % kSamples] : nullptr;
        if (newest && frame < newest->frame) count = 0;   // a new world scene counts from its start
        if (!count || now - newest->t >= 0.25)
        {
            samples[head] = Sample{ now, frame };
            head = (head + 1) % kSamples;
            count = std::min(count + 1, kSamples);
        }
        for (int i = 0; i < count; ++i)
        {
            const Sample& s = samples[(head + kSamples - 1 - i) % kSamples];
            if (now - s.t >= kStaleSeconds) return frame - s.frame;
        }
        const Sample& oldest = samples[(head + kSamples - count) % kSamples];
        const double span = now - oldest.t;
        if (span < 0.5) return 600;   // no rate yet: ten seconds at 60 frames a second
        return uint32_t(std::clamp(double(frame - oldest.frame) / span * kStaleSeconds, 60.0, 1e6));
    }

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
        s.hash = gl::table::StemHash(path, maxLen);
        s.upright = gl::table::RotationQuat(toWorld, s.quat);
        g_sources.push_back(s);
    }
    char g_status[160]  = "client lights: none gathered";
    char g_nearest[224] = "nearest client light: none gathered";

    const char* KindName(gl::Kind k)
    {
        switch (k)
        {
        case gl::Kind::M2:    return "M2 (model light)";
        case gl::Kind::Wmo:   return "WMO (MOLT)";
        case gl::Kind::Table: return "model table";
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
        gl::Kind    kind;
        bool operator==(const Key& o) const { return owner == o.owner && index == o.index && kind == o.kind; }
    };

    struct KeyHash
    {
        size_t operator()(const Key& k) const
        {
            return std::hash<const void*>()(k.owner) ^ (size_t(k.index) * 40503u + size_t(k.kind) * 0x9E3779B1u);
        }
    };

    Key KeyOf(const gl::Light& l) { return Key{ l.owner, l.index, l.kind }; }

    int Rank(gl::Kind k) { return k == gl::Kind::Table ? 0 : (k == gl::Kind::M2 ? 1 : 2); }

    struct Pair
    {
        Key       winner, loser;
        gl::Light winnerLight, loserLight;   // as last seen together
        uint32_t  seen;                      // the gather both were last found in
    };
    std::vector<Pair> g_pairs;
    uint32_t          g_gathers = 0;
    DWORD             g_mergeLogged[3] = {};   // table + M2, table + WMO, M2 + WMO

    float Luma(const gl::Light& l)
    {
        return (l.color[0] * 0.299f + l.color[1] * 0.587f + l.color[2] * 0.114f) * l.intensity;
    }

    /// The winner takes the loser's reach and brightness where they are larger; its hue stays, and
    /// its fog halo keeps its own reach. Whatever reach says, it keeps what the engine does with the
    /// loser: the engine still lights with an M2 light a table light stands in for, and a MOLT light
    /// stays baked into the interior's vertex colours.
    void Absorb(gl::Light& w, const gl::Light& l, bool reach)
    {
        w.engineLit = uint8_t(w.engineLit | l.engineLit);
        w.baked = uint8_t(w.baked | l.baked);
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

    void LogMerge(const gl::Light& w, const gl::Light& l, float d)
    {
        const int type = w.kind == gl::Kind::Table ? (l.kind == gl::Kind::M2 ? 0 : 1) : 2;
        if (GetTickCount() - g_mergeLogged[type] < 10000) return;
        g_mergeLogged[type] = GetTickCount();
        static const char* const kTypes[] = { "model-table light + M2 light", "model-table light + WMO light", "M2 light + WMO light" };
        LIGHTS_LOG_INFO("lights: merged %s %.2f yd apart at (%.1f %.1f %.1f): kept the %s colour (%.2f %.2f %.2f) reach %.1f, "
                        "dropped colour (%.2f %.2f %.2f) reach %.1f",
                        kTypes[type], d, w.position[0], w.position[1], w.position[2], w.kind == gl::Kind::Table ? "table" : "M2",
                        w.color[0], w.color[1], w.color[2], w.radius, l.color[0], l.color[1], l.color[2], l.radius);
    }

    /// Merges the lights of one fixture in found[0..n), keeping the order of the rest; returns the new count.
    size_t MergeFixtures(gl::Light* found, size_t n, bool reach)
    {
        ++g_gathers;
        static std::vector<char> gone;   // kept between frames: its capacity, never a fresh allocation
        gone.assign(n, 0);
        for (size_t i = 0; i < n; ++i)
        {
            if (gone[i] || found[i].kind == gl::Kind::Given) continue;
            for (size_t j = i + 1; j < n; ++j)
            {
                if (gone[j] || found[j].kind == found[i].kind || found[j].kind == gl::Kind::Given) continue;
                const gl::Light& a = found[i];
                const gl::Light& b = found[j];
                const bool table = a.kind == gl::Kind::Table || b.kind == gl::Kind::Table;
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
                    if (std::any_of(found, found + n, [&](const gl::Light& o) { return KeyOf(o) == p.winner; })) break;
                    gl::Light stand = p.winnerLight;
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
    gl::Light FromEngine(const lt::PointLight& l)
    {
        gl::Light o{};
        for (int k = 0; k < 3; ++k)
        {
            o.position[k] = l.position[k];
            o.color[k] = std::min(l.color[k], 8.0f);
        }
        o.radius      = l.attenEnd > 0.0f ? l.attenEnd : 10.0f;
        o.innerRadius = std::clamp(l.attenStart, 0.0f, o.radius * 0.95f);
        o.intensity   = 1.0f;
        o.cosCone     = -2.0f;
        o.kind  = l.kind == lt::Kind::Wmo ? gl::Kind::Wmo : gl::Kind::M2;
        // The engine lights the scene with its M2 lights itself; a MOLT light is baked into the
        // building's interior vertex colours instead.
        o.engineLit = o.kind == gl::Kind::M2 ? 1 : 0;
        o.baked     = o.kind == gl::Kind::Wmo ? 1 : 0;
        // The engine's lights carry no family: a warm model light is taken for a fire, a warm WMO
        // light for a lantern.
        const bool warm = l.color[0] > 1.3f * l.color[2] && l.color[0] > 0.05f;
        o.flicker = !warm ? gl::Flicker::None : (o.kind == gl::Kind::M2 ? gl::Flicker::Fire : gl::Flicker::Lantern);
        o.profile = warm && o.kind == gl::Kind::M2 ? gl::Profile::Flame : gl::Profile::None;
        o.size    = o.kind == gl::Kind::M2 ? 0.25f : 0.2f;
        o.family  = uint8_t(!warm ? WXL_GFX_LIGHT_FAMILY_TINT
                                  : (o.kind == gl::Kind::M2 ? WXL_GFX_LIGHT_FAMILY_ENGINE_FIRE : WXL_GFX_LIGHT_FAMILY_ENGINE_LAMP));
        o.owner = l.owner;
        o.index = l.index;
        return o;
    }
}

namespace wxl::gfx::lights
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
            const uint32_t stale = StaleFrames();
            lt::Query q;
            for (int k = 0; k < 3; ++k) q.center[k] = eye[k];
            q.radius = options.radius;
            q.m2  = true;
            q.wmo = options.wmo;
            q.maxStaleFrames = stale;
            const size_t matched = lt::Collect(q, engine, kCollectCap);
            const size_t e = std::min(matched, kCollectCap);
            static bool warned = false;
            if (matched > kCollectCap && !warned)
            {
                warned = true;
                LIGHTS_LOG_WARN("lights: %u engine lights within %.0f yd, %u kept (walk order past that)", unsigned(matched),
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
                mq.maxStaleFrames = stale;
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
                table = std::min(table::Collect(eye, options.radius, kAnyAge, found + n, kCollectCap - n, nullptr),
                                 kCollectCap - n);
                n += table;
            }
        }

        const size_t gathered = n;
        if (options.merge) n = MergeFixtures(found, n, options.mergeReach);

        // Score everything in range by brightness over distance, then mark the best as chosen. A light
        // chosen last frame counts kKeepBonus times, so two near-equal lights do not swap every frame,
        // and one chosen less than kHoldSeconds ago stays chosen whatever outranks it.
        g_clock += double(std::max(dt, 0.0f));
        const size_t cap = size_t(std::max(slots, 0));
        static std::unordered_map<Key, size_t, KeyHash> where;   // tracked light by identity; kept for its buckets
        where.clear();
        for (size_t t = 0; t < g_tracked.size(); ++t) where[Key{ g_tracked[t].owner, g_tracked[t].index, g_tracked[t].kind }] = t;
        struct Pick { float score; size_t index; int tracked; bool incumbent, held; };
        static std::vector<Pick> picks;   // kept between frames for its capacity
        picks.clear();
        for (size_t i = 0; i < n; ++i)
        {
            const Light& l = found[i];
            const float dx = l.position[0] - eye[0], dy = l.position[1] - eye[1], dz = l.position[2] - eye[2];
            const float luma = (l.color[0] * 0.299f + l.color[1] * 0.587f + l.color[2] * 0.114f) * l.intensity;
            float score = luma / (1.0f + (dx * dx + dy * dy + dz * dz) / (l.radius * l.radius));
            const auto it = where.find(KeyOf(l));
            const int tracked = it == where.end() ? -1 : int(it->second);
            const bool incumbent = tracked >= 0 && g_tracked[size_t(tracked)].chosen;
            if (incumbent && n > cap) score *= kKeepBonus;
            const bool held = incumbent && g_clock - g_tracked[size_t(tracked)].since < kHoldSeconds;
            picks.push_back({ score, i, tracked, incumbent, held });
        }
        std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) {
            if (a.held != b.held) return a.held;
            return a.score > b.score;
        });
        if (picks.size() > cap) picks.resize(cap);

        // The wanted lights already tracked (chosen, or fading out) are chosen; the rest fade out.
        for (Tracked& t : g_tracked) t.chosen = false;
        for (const Pick& p : picks)
        {
            if (p.tracked < 0) continue;
            Tracked& t = g_tracked[size_t(p.tracked)];
            if (!p.incumbent) t.since = g_clock;   // chosen again while fading out: a new hold
            t.light = found[p.index];
            t.score = p.score;
            t.chosen = true;
        }
        // A light fading out keeps its slot until its weight reaches 0, so it always finishes its fade
        // on screen and, chosen again, comes back from the weight it shows. Newcomers take only the
        // slots nothing holds: one that outranks a lamp waits for that lamp's fade.
        size_t occupied = 0;
        for (const Tracked& t : g_tracked)
            if (t.chosen || t.weight > 0.0f) ++occupied;
        for (const Pick& p : picks)
        {
            if (p.tracked >= 0 || occupied >= cap) continue;
            const Light& l = found[p.index];
            g_tracked.push_back({ l.owner, l.index, l.kind, l, p.score, 0.0f, true, g_clock });
            ++occupied;
        }

        const float step = dt / kFadeSeconds;
        for (Tracked& t : g_tracked)
            t.weight = std::clamp(t.weight + (t.chosen ? step : -step), 0.0f, 1.0f);
        g_tracked.erase(std::remove_if(g_tracked.begin(), g_tracked.end(),
                                       [](const Tracked& t) { return !t.chosen && t.weight <= 0.0f; }),
                        g_tracked.end());

        // Chosen lights first, then the ones still fading out. All fit, unless the given lights took
        // more slots this frame; then the faintest fading lights are the ones cut.
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
            l.fade = t.weight;
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

    void ApplyFamily(Light& l, const Options& o)
    {
        namespace fm = families;
        const uint32_t family = l.kind == Kind::Given ? WXL_GFX_LIGHT_FAMILY_GIVEN : l.family;
        l.family = uint8_t(family);
        const fm::Family& f = fm::Get(family);
        // A Kelvin family keeps its temperature for a warm colour; a cold or coloured one (magic
        // flames) keeps its own hue, as does a family with a tint of its own.
        const bool warm = fm::Warm(l.color);
        const bool ownTint = f.tint[0] + f.tint[1] + f.tint[2] > 0.0f;
        if (f.kelvin > 0.0f && warm)
        {
            fm::Blackbody(f.kelvin, l.chroma);
            fm::Adapt(l.chroma, o.adaptation);
            l.kelvin = f.kelvin;
        }
        else
        {
            fm::TintOf(ownTint ? f.tint : l.color, l.chroma);
            l.kelvin = 0.0f;
        }
        // A warm colour at luminance 1 runs its red far past 1 (2 at 2000 K even after the adaptation,
        // 1.6 at 2700 K): on reddish ground the red channel alone meets the curve's shoulder and a pool
        // reads red, not orange. Its strongest channel is eased back, luminance kept (1.44 at 2000 K by
        // default). Magic colours keep their saturation.
        if (warm && (l.kelvin > 0.0f || !ownTint)) fm::SoftCap(l.chroma, o.warmthCap);
        // Engine lights carry their brightness in their colour; given ones in their intensity. An
        // engine light's colour sets only a little of its strength, so a model light authored at 4 is
        // not twice the family's lamp.
        float scale = 1.0f;
        if (l.kind == Kind::M2 || l.kind == Kind::Wmo)
        {
            const float luma = 0.299f * l.color[0] + 0.587f * l.color[1] + 0.114f * l.color[2];
            scale = std::clamp(std::sqrt(std::max(luma, 1e-3f)), 0.75f, 1.33f);
        }
        else if (l.kind == Kind::Given)
        {
            float lin[3];
            for (int k = 0; k < 3; ++k) lin[k] = std::pow(std::max(l.color[k], 0.0f), 2.2f);
            scale = std::max(l.intensity, 0.0f) * std::sqrt(std::max(fm::Luma(lin), 0.01f));
        }
        l.power = f.intensity * scale;
        l.softRadius = std::max(f.softRadius, 0.02f);
        l.emissiveRadius = std::clamp(2.0f * f.softRadius, 0.05f, 0.4f);
        if (l.carried)
        {
            l.softRadius = std::max(l.softRadius, o.carriedCore);
            // A torch in a hand sits a few inches from the fingers: the glow of an engine fire's
            // 0.4 yd head would light the hand itself.
            l.emissiveRadius = std::min(l.emissiveRadius, 0.12f);
        }
        const float lit = std::max(l.power * std::max(o.gain, 0.0f), 0.0f);
        l.reach = std::clamp(std::sqrt(lit / std::max(o.cutoff, 1e-4f)), 4.0f, std::max(o.maxRadius, 4.0f));
        // A given light never reaches past what it asked for.
        if (l.kind == Kind::Given && l.radius > 0.0f) l.reach = std::min(l.reach, std::max(l.radius, 1.0f));
    }
}
