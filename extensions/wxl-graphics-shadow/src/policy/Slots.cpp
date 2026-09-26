// wxl-graphics-shadow: the slot and map policy, and the core's omni maps driven under its claim.
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

#include "Slots.hpp"
#include "Bodies.hpp"
#include "../Shadow.hpp"
#include "../core/Extension.hpp"
#include "../core/Settings.hpp"

#include "wxl/OmniShadowsApi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace
{
    namespace sh = wxl::gfx::shadow;
    namespace sl = wxl::gfx::shadow::slots;
    namespace bd = wxl::gfx::shadow::bodies;

    sl::Slot g_slots[sl::kSlots];
    sl::Map  g_maps[sl::kMaps];

    const int g_owner = 0;                   // this extension's identity for the core's claims
    const WXL_OmniShadowsApi* g_core = nullptr;
    uint32_t g_coreMisses = 0;
    bool     g_claimed = false;
    bool     g_claimRefused = false;

    std::vector<WXL_GfxShadowLight> g_pushed;
    uint32_t g_pushFrame = 0;
    bool     g_everPushed = false;
    uint32_t g_frame = 0;                    // counts Choose calls
    bool     g_pulled = false;               // this frame's lights came from wxl-graphics-lights' list

    struct Candidate
    {
        WXL_GfxShadowLight l;
        float              rank;
    };
    std::vector<Candidate> g_candidates;

    // Motion per light id: where it was, and when it last moved.
    struct Motion { float pos[3]; double movedAt; double seenAt; };
    std::unordered_map<uint32_t, Motion> g_motion;

    int  g_changes = 0;                      // slot hand-overs since start
    int  g_mapChanges = 0;
    char g_status[256] = "slots: idle";

    float Dist2(const float a[3], const float b[3])
    {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return dx * dx + dy * dy + dz * dz;
    }

    /// The lights of this frame: handed in when a call came within two frames, else read from
    /// wxl-graphics-lights and ranked by brightness over distance.
    void Gather(const float eye[3])
    {
        g_candidates.clear();
        g_pulled = false;
        const sh::Settings& s = sh::Config();
        if (g_everPushed && g_frame - g_pushFrame <= 2)
        {
            for (const WXL_GfxShadowLight& l : g_pushed)
                if (l.id && !(l.flags & WXL_GFX_SHADOW_LIGHT_NEVER) && l.radius > 0.5f)
                    g_candidates.push_back({ l, std::max(l.importance, 0.0f) });
        }
        else if (const WXL_GraphicsLightsApi* lights = sh::Lights())
        {
            int count = 0;
            uint32_t frame = 0;
            const WXL_GfxLight* list = lights->Current(&count, &frame);
            g_pulled = true;
            for (int i = 0; list && i < count; ++i)
            {
                const WXL_GfxLight& g = list[i];
                if (!g.id || g.radius < 1.5f || g.intensity <= 0.0f) continue;
                WXL_GfxShadowLight l{};
                l.id = g.id;
                std::memcpy(l.position, g.rest, sizeof l.position);
                l.radius = std::fabs(g.radius);
                std::memcpy(l.direction, g.direction, sizeof l.direction);
                l.cosCone = g.cosCone;
                l.sourceSize = std::max(g.size, 0.03f);
                l.flags = g.carried ? WXL_GFX_SHADOW_LIGHT_CARRIED : 0u;
                l.listIndex = i;
                const float luma = (g.color[0] * 0.299f + g.color[1] * 0.587f + g.color[2] * 0.114f) * g.intensity;
                l.importance = luma / (1.0f + Dist2(l.position, eye) / (l.radius * l.radius)) * (g.carried ? 2.0f : 1.0f);
                g_candidates.push_back({ l, l.importance });
            }
        }
        (void)s;
    }

    /// Whether a light moves: carried, or displaced by more than 0.1 yd within the last two seconds.
    bool Moving(const WXL_GfxShadowLight& l, double now)
    {
        if (l.flags & WXL_GFX_SHADOW_LIGHT_CARRIED) return true;
        auto it = g_motion.find(l.id);
        if (it == g_motion.end())
        {
            g_motion[l.id] = Motion{ { l.position[0], l.position[1], l.position[2] }, -1e9, now };
            return false;
        }
        Motion& m = it->second;
        m.seenAt = now;
        if (Dist2(m.pos, l.position) > 0.1f * 0.1f)
        {
            std::memcpy(m.pos, l.position, sizeof m.pos);
            m.movedAt = now;
        }
        return now - m.movedAt < 2.0;
    }

    /// The carrying unit's capsule: by GUID when handed in, else the nearest capsule to a carried light.
    int Carrier(const WXL_GfxShadowLight& l)
    {
        const unsigned long long guid = (static_cast<unsigned long long>(l.carrierHi) << 32) | l.carrierLo;
        if (guid) return bd::Find(guid);
        if (!(l.flags & WXL_GFX_SHADOW_LIGHT_CARRIED)) return -1;
        return bd::Nearest(l.position, 0.9f);
    }

    /// The faces a spot light's cone may reach; all six for a point light.
    uint32_t SpotFaces(const WXL_GfxShadowLight& l)
    {
        if (l.cosCone <= -0.999f) return 0x3F;
        const float half = std::acos(std::clamp(l.cosCone, -1.0f, 1.0f));
        // A face's frustum reaches 54.7 degrees off its axis at the corners.
        const float reach = std::cos(std::min(half + 0.9553f, 3.14159265f));
        uint32_t mask = 0;
        for (int f = 0; f < 6; ++f)
        {
            float axis[3] = { 0.0f, 0.0f, 0.0f };
            axis[f / 2] = (f % 2) ? -1.0f : 1.0f;
            const float d = axis[0] * l.direction[0] + axis[1] * l.direction[1] + axis[2] * l.direction[2];
            if (d >= reach) mask |= 1u << f;
        }
        return mask ? mask : 0x3F;
    }

    void UpdateSlots(double now, float dt)
    {
        const sh::Settings& s = sh::Config();
        const float step = std::min(dt / std::max(s.slotFade, 0.05f), 1.0f);
        // Rank: a holder counts its margin, and one younger than the hold cannot be beaten.
        for (Candidate& c : g_candidates)
        {
            c.rank = std::max(c.l.importance, 0.0f);
            for (const sl::Slot& slot : g_slots)
                if (slot.id == c.l.id && !slot.leaving)
                {
                    c.rank *= s.slotMargin;
                    if (now - slot.since < s.slotHold) c.rank = 1e30f;
                }
        }
        std::stable_sort(g_candidates.begin(), g_candidates.end(), [](const Candidate& a, const Candidate& b) { return a.rank > b.rank; });
        const size_t wanted = std::min(g_candidates.size(), size_t(sl::kSlots));
        auto wantedIndex = [&](uint32_t id) -> int {
            for (size_t k = 0; k < wanted; ++k)
                if (g_candidates[k].l.id == id) return int(k);
            return -1;
        };
        // Holders: stay and fade in, or fade out and free the slot.
        for (sl::Slot& slot : g_slots)
        {
            if (!slot.id) continue;
            const int k = wantedIndex(slot.id);
            if (k >= 0)
            {
                slot.light = g_candidates[size_t(k)].l;
                slot.leaving = false;
                slot.weight = std::min(slot.weight + step, 1.0f);
            }
            else
            {
                // Still listed (just not among the best): keep its latest place while it fades. Gone from
                // the list: its index no longer names it.
                bool listed = false;
                for (const Candidate& c : g_candidates)
                    if (c.l.id == slot.id) { slot.light = c.l; listed = true; break; }
                if (!listed) slot.light.listIndex = -1;
                slot.leaving = true;
                slot.weight -= step;
                if (slot.weight <= 0.0f)
                {
                    if (slot.map >= 0) g_maps[slot.map] = sl::Map{};
                    slot = sl::Slot{};
                    ++g_changes;
                }
            }
        }
        // Newcomers take free slots, fading in from nothing.
        for (size_t k = 0; k < wanted; ++k)
        {
            const uint32_t id = g_candidates[k].l.id;
            if (std::any_of(std::begin(g_slots), std::end(g_slots), [&](const sl::Slot& x) { return x.id == id; })) continue;
            for (sl::Slot& slot : g_slots)
                if (!slot.id)
                {
                    slot = sl::Slot{};
                    slot.id = id;
                    slot.light = g_candidates[k].l;
                    slot.since = now;
                    ++g_changes;
                    break;
                }
        }
        if (sh::Isolated(sh::kIsoNoFade))
            for (sl::Slot& slot : g_slots)
                if (slot.id) slot.weight = slot.leaving ? 0.0f : 1.0f;
    }

    int FreeMap()
    {
        for (int m = 0; m < sl::kMaps; ++m)
            if (!g_maps[m].lightId) return m;
        return -1;
    }

    void UpdateMaps(double now, float dt)
    {
        const sh::Settings& s = sh::Config();
        const float step = std::min(dt / std::max(s.slotFade, 0.05f), 1.0f);
        const bool on = s.maps && g_claimed && !sh::Isolated(sh::kIsoNoMaps);
        // Who deserves a map: the best still and the best moving slots, holders favoured.
        struct Want { int slot; float rank; };
        Want still[sl::kSlots], moving[sl::kSlots];
        int nStill = 0, nMoving = 0;
        for (int i = 0; i < sl::kSlots && on; ++i)
        {
            sl::Slot& slot = g_slots[i];
            slot.moving = slot.id && Moving(slot.light, now);
            if (!slot.id || slot.leaving || (slot.light.flags & WXL_GFX_SHADOW_LIGHT_NO_MAP)) continue;
            float rank = std::max(slot.light.importance, 1e-6f);
            // The player's own carried light first of all.
            if (slot.carrier >= 0 && bd::List()[slot.carrier].player) rank *= 100.0f;
            if (slot.map >= 0 && !slot.mapLeaving) rank *= s.slotMargin;
            (slot.moving ? moving[nMoving++] : still[nStill++]) = Want{ i, rank };
        }
        auto byRank = [](const Want& a, const Want& b) { return a.rank > b.rank; };
        std::sort(still, still + nStill, byRank);
        std::sort(moving, moving + nMoving, byRank);
        bool chosen[sl::kSlots] = {};
        for (int k = 0; k < std::min(nStill, s.stillMaps); ++k) chosen[still[k].slot] = true;
        for (int k = 0; k < std::min(nMoving, s.movingMaps); ++k) chosen[moving[k].slot] = true;

        for (int i = 0; i < sl::kSlots; ++i)
        {
            sl::Slot& slot = g_slots[i];
            const int kind = slot.moving ? sl::kMapMoving : sl::kMapStill;
            if (slot.map >= 0)
            {
                sl::Map& map = g_maps[slot.map];
                // A map of the other kind (the light started or stopped moving) goes and comes back.
                const bool keep = chosen[i] && map.kind == kind;
                slot.mapLeaving = !keep;
                if (keep) slot.mapWeight = map.ready ? std::min(slot.mapWeight + step, 1.0f) : slot.mapWeight;
                else slot.mapWeight -= step;
                if (slot.mapWeight <= 0.0f && slot.mapLeaving)
                {
                    map = sl::Map{};
                    slot.map = -1;
                    slot.mapWeight = 0.0f;
                    slot.mapLeaving = false;
                    ++g_mapChanges;
                }
                continue;
            }
            if (!chosen[i]) continue;
            const int m = FreeMap();
            if (m < 0) continue;
            sl::Map& map = g_maps[m];
            map = sl::Map{};
            map.lightId = slot.id;
            map.kind = kind;
            map.idMain = slot.id;
            map.idUnits = (slot.id * 2654435761u) | 1u;
            slot.map = m;
            slot.mapWeight = 0.0f;
            slot.mapLeaving = false;
            ++g_mapChanges;
        }
        if (sh::Isolated(sh::kIsoNoFade))
            for (sl::Slot& slot : g_slots)
                if (slot.map >= 0) slot.mapWeight = slot.mapLeaving ? 0.0f : (g_maps[slot.map].ready ? 1.0f : 0.0f);
    }

    /// Hands the core every map's lights: main maps first (so a reader of the first four core slots,
    /// such as wxl-graphics-lights' omni table, finds each light's static map under its own id), then
    /// the unit maps of still lamps.
    void DriveCore()
    {
        const sh::Settings& s = sh::Config();
        const WXL_OmniShadowsApi* core = sl::Core();
        if (!core || !g_claimed) return;
        WXL_OmniLightV3 lights[WXL_OMNISHADOWS_MAX_V3] = {};
        uint32_t n = 0;
        for (sl::Map& map : g_maps) { map.coreMain = map.coreUnits = -1; }
        for (int pass = 0; pass < 2; ++pass)
            for (int m = 0; m < sl::kMaps; ++m)
            {
                sl::Map& map = g_maps[m];
                if (!map.lightId) continue;
                if (pass == 1 && map.kind != sl::kMapStill) continue;
                const sl::Slot* slot = nullptr;
                for (const sl::Slot& x : g_slots)
                    if (x.id == map.lightId) { slot = &x; break; }
                if (!slot || n >= WXL_OMNISHADOWS_MAX_V3) continue;
                WXL_OmniLightV3& l = lights[n];
                std::memcpy(l.position, slot->light.position, sizeof l.position);
                l.radius = slot->light.radius;
                l.faceMask = SpotFaces(slot->light);
                map.faceMask = l.faceMask;
                const bool carried = (slot->light.flags & WXL_GFX_SHADOW_LIGHT_CARRIED) != 0;
                map.housing = carried ? s.carriedHousing : s.housing;
                if (pass == 0)
                {
                    l.id = map.idMain;
                    l.flags = map.kind == sl::kMapStill ? WXL_OMNI_CASTERS_STATIC : 0u;
                    l.faceSize = uint32_t(map.kind == sl::kMapStill ? s.staticFace : s.movingFace);
                    map.coreMain = int(n);
                }
                else
                {
                    l.id = map.idUnits;
                    l.flags = WXL_OMNI_CASTERS_DYNAMIC | WXL_OMNI_REDRAW_OCCUPIED;
                    l.faceSize = uint32_t(s.unitFace);
                    map.coreUnits = int(n);
                }
                ++n;
            }
        core->SetBudgetV3(&g_owner, uint32_t(std::clamp(s.refresh, 1, 6)));
        core->SetLightsV3(&g_owner, lights, n);
    }

    void Claim()
    {
        const WXL_OmniShadowsApi* core = sl::Core();
        const bool want = sh::Config().enabled && sh::Config().maps;
        if (!core) return;
        if (want && !g_claimed && !g_claimRefused)
        {
            g_claimed = core->Claim(&g_owner) != 0;
            if (g_claimed) SHADOW_LOG_INFO("slots: the core's omni maps are driven here now (claimed)");
            else
            {
                g_claimRefused = true;
                SHADOW_LOG_WARN("slots: another extension holds the core's omni maps; lamps shadow with capsules only");
            }
        }
        else if (!want && g_claimed) sl::ReleaseCore();
    }
}

namespace wxl::gfx::shadow::slots
{
    const WXL_OmniShadowsApi* Core()
    {
        if (g_core) return g_core;
        if ((g_coreMisses++ & 63u) != 0) return nullptr;
        auto* api = static_cast<const WXL_OmniShadowsApi*>(g_api->GetInterface("wxl.omnishadows", WXL_OMNISHADOWS_API_VERSION));
        if (api && (api->structSize < sizeof(WXL_OmniShadowsApi) || !api->Claim || !api->SetLightsV3)) api = nullptr;
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            SHADOW_LOG_INFO("slots: omni shadow maps %s", api ? "available from the core (v3)" : "not offered by the core: capsules only");
        }
        g_core = api;
        return g_core;
    }

    void ReleaseCore()
    {
        for (Slot& slot : g_slots)
        {
            slot.map = -1;
            slot.mapWeight = 0.0f;
            slot.mapLeaving = false;
        }
        for (Map& map : g_maps) map = Map{};
        if (g_claimed && g_core)
        {
            g_core->SetLightsV3(&g_owner, nullptr, 0);
            g_core->Release(&g_owner);
            SHADOW_LOG_INFO("slots: the core's omni maps are released");
        }
        g_claimed = false;
        g_claimRefused = false;
    }

    void Idle()
    {
        if (g_claimed && g_core) g_core->SetLightsV3(&g_owner, nullptr, 0);
        for (Map& map : g_maps)
        {
            map.coreMain = map.coreUnits = -1;
            map.reset = true;
            map.ready = false;
        }
        for (Slot& slot : g_slots) slot.mapWeight = 0.0f;
    }

    void SetPushed(const WXL_GfxShadowLight* lights, int count, uint32_t)
    {
        g_pushed.assign(lights, lights + std::max(count, 0));
        g_pushFrame = g_frame;
        g_everPushed = true;
    }

    void Choose(const float eye[3], double now, float dt)
    {
        ++g_frame;
        const Settings& s = Config();
        Claim();
        Gather(eye);
        UpdateSlots(now, dt);
        // Carriers and capsules in reach, from this frame's bodies.
        const bool capsules = s.capsules && !Isolated(kIsoNoCapsules);
        for (Slot& slot : g_slots)
        {
            slot.carrier = slot.id ? Carrier(slot.light) : -1;
            slot.capsuleMask = slot.id && capsules ? bd::Mask(slot.light.position, slot.light.radius) : 0u;
            slot.contact = false;
        }
        UpdateMaps(now, dt);
        DriveCore();
        // Contact shadows for the most important slots.
        int order[kSlots];
        int n = 0;
        for (int i = 0; i < kSlots; ++i)
            if (g_slots[i].id && g_slots[i].weight > 0.0f) order[n++] = i;
        std::sort(order, order + n, [](int a, int b) { return g_slots[a].light.importance > g_slots[b].light.importance; });
        for (int k = 0; k < std::min(n, s.contactSlots); ++k) g_slots[order[k]].contact = true;

        // Forget motion of lights unseen for a while.
        if (g_motion.size() > 1024)
            for (auto it = g_motion.begin(); it != g_motion.end();)
                it = now - it->second.seenAt > 5.0 ? g_motion.erase(it) : std::next(it);

        int held = 0, mapped = 0;
        for (const Slot& slot : g_slots)
        {
            held += slot.id ? 1 : 0;
            mapped += slot.map >= 0 ? 1 : 0;
        }
        const WXL_OmniShadowsApi* core = Core();
        std::snprintf(g_status, sizeof g_status, "slots: lights %s (%d), %d slots held, %d maps, %d / %d hand-overs; core %s, %u faces last frame",
                      g_pulled ? "read from wxl-graphics-lights" : "handed in", int(g_candidates.size()), held, mapped, g_changes,
                      g_mapChanges, !core ? "absent" : (g_claimed ? "claimed" : (g_claimRefused ? "held by another" : "free")),
                      core && core->FacesLastFrame ? core->FacesLastFrame() : 0u);
    }

    const Slot* List() { return g_slots; }
    Map* Maps() { return g_maps; }
    const char* Status() { return g_status; }
}
