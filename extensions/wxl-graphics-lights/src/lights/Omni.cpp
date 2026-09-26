// wxl-graphics-lights: the omni shadow slots -- which lights hold the core's four maps, and the table every consumer reads them from.
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
#include "Omni.hpp"
#include "Rooms.hpp"

#include "wxl/GraphicsShadowApi.h"
#include "wxl/OmniShadowsApi.h"
#include "wxl/gfx/Ui.hpp"
#include "game/Lights.hpp"
#include "game/World.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace
{
    namespace gl    = wxl::gfx::lights;
    namespace ui    = wxl::gfx::ui;
    namespace world = wxl::game::world;

    constexpr int kSlots = WXL_GFX_LIGHTS_OMNI_MAX;
    static_assert(kSlots == WXL_OMNISHADOWS_MAX, "the omni table has one row per core slot");

    // The atlas layout OmniShadowsApi.h documents: face f is the cell (f % 4, f / 4) of a 4 x 2 grid.
    constexpr int    kMaxUnits    = 16;
    constexpr double kSlotHold    = 1.0;     // seconds a light keeps its slot at least
    constexpr float  kSlotFade    = 0.5f;    // seconds a slot's shadow takes to fade in or out
    constexpr double kNearHold    = 1.5;     // seconds a light keeps its unit after it left
    constexpr int    kLeavingRank = 99;      // a leaving slot yields to any candidate
    constexpr float  kBodyReach   = 1.5f;    // yards past a light's radius a unit still counts as near it
    constexpr const char* kForeverInterface = "wxl.forever.fog.inputs";   // published by wxl-forever's fog

    gl::omni::Settings g_cfg;

    const WXL_OmniShadowsApi* g_core = nullptr;
    uint32_t g_coreMisses = 0;
    bool     g_coreLogged = false;
    bool     g_forever = false;          // another extension owns the slots (g_owner)
    const char* g_owner = nullptr;
    uint32_t g_foreverChecks = 0;
    bool     g_foreverLogged = false;
    int      g_faceSizeSet = 0;          // last face size asked of the core
    int      g_budgetSet = 0;            // last round-robin budget asked of the core

    // The four omni slots, held: the core clears and redraws a slot given a different light, so a
    // light keeps its slot until a clearly better one has waited out the hold, and the slot's
    // shadow fades out before it changes hands and in after: a map is a bonus on a light that
    // always shines, never a switch.
    struct SlotState
    {
        uint32_t id = 0;        // the light held, 0 none
        uint32_t next = 0;      // the light it goes to once faded out, 0 none
        double   since = 0.0;   // when it was taken, seconds
        float    weight = 0.0f; // its shadow's share, 0..1
        bool     leaving = false; // still listed but no longer a candidate: fading out to nobody
    };
    SlotState g_slots[kSlots];
    int       g_changes = 0;      // hand-overs since the module started
    std::unordered_map<uint32_t, double> g_unitNear;   // when (seconds) a unit was last near each light, by id

    // The units near the camera this frame, world feet and height, for the tiers.
    float g_unitWorld[kMaxUnits][4] = {};
    int   g_unitCount = 0;

    struct Candidate { int rank; int index; float score; };
    std::vector<Candidate> g_candidates;   // kept for its capacity

    // With wxl-forever choosing, a slot's share fades in as the core hands it a new light; forever
    // fades its own out before that, so the hand-over is soft on both sides.
    uint32_t g_seenId[kSlots] = {};
    float    g_seenWeight[kSlots] = {};

    WXL_GfxOmniSlot g_pub[kSlots] = {};
    float g_housing[kSlots] = {};      // per slot: casters this near the light do not shadow it
    float g_skip[kSlots] = {};         // per slot: receivers this near the light ignore its map
    int  g_pubCount = 0;

    // Where this frame's chosen lights were handed to the core, by id: the core renders a moved light's
    // six faces from there during this frame's world pass, so its rows are shifted there too.
    struct Handed { uint32_t id; float position[3]; };
    Handed g_handed[kSlots] = {};
    int    g_handedCount = 0;
    int  g_held = 0;
    char g_status[224] = "omni shadow maps: not started";
    double g_last = 0.0;

    double Now()
    {
        LARGE_INTEGER counter{}, frequency{};
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);
        return double(counter.QuadPart) / double(frequency.QuadPart);
    }

    const WXL_OmniShadowsApi* Core()
    {
        if (g_core) return g_core;
        if ((g_coreMisses++ & 63u) != 0) return nullptr;
        // Only the version 2 functions are used here (a v3 core answers them too).
        auto* api = static_cast<const WXL_OmniShadowsApi*>(gl::g_api->GetInterface("wxl.omnishadows", 2));
        const size_t v2 = offsetof(WXL_OmniShadowsApi, GetState) + sizeof(void*);
        if (api && (api->structSize < v2 || !api->SetLightsEx || !api->GetState)) api = nullptr;
        if (!g_coreLogged)
        {
            g_coreLogged = true;
            LIGHTS_LOG_INFO("omni: shadow maps %s", api ? "available from the core, with change tracking" : "not offered by the core");
        }
        g_core = api;
        return g_core;
    }

    /// The units nearest the camera, world feet and height: a light with one of them near it takes a
    /// map first (a passer-by must cast a shadow).
    void GatherUnits(const float eye[3])
    {
        struct Seen { float pos[3]; float height; float d2; };
        static std::vector<Seen> seen;
        seen.clear();
        world::ForEachObject(world::kTypeMaskUnit, [&](unsigned long long, void* obj) {
            Seen s{};
            world::Position(obj, s.pos);
            const float dx = s.pos[0] - eye[0], dy = s.pos[1] - eye[1], dz = s.pos[2] - eye[2];
            s.d2 = dx * dx + dy * dy + dz * dz;
            if (s.d2 > 80.0f * 80.0f) return true;
            float head[3];
            world::NamePosition(obj, head);
            const float h = head[2] - s.pos[2];
            s.height = (h > 0.5f && h < 8.0f) ? h : 2.2f;
            seen.push_back(s);
            return true;
        });
        std::sort(seen.begin(), seen.end(), [](const Seen& a, const Seen& b) { return a.d2 < b.d2; });
        g_unitCount = std::min(int(seen.size()), kMaxUnits);
        for (int i = 0; i < g_unitCount; ++i)
        {
            for (int k = 0; k < 3; ++k) g_unitWorld[i][k] = seen[i].pos[k];
            g_unitWorld[i][3] = seen[i].height;
        }
    }

    /// The chooser wxl-forever's surface lighting ran: the four maps go, in tiers, to point lights
    /// with a real reach that have a unit near them, then lights without a cookie (nothing else
    /// shapes their light); a still lamp whose cookie already throws its own cage takes none. Within
    /// a tier the camera's own room first, then importance (brightness over distance; the list itself
    /// is in id order). A unit counts as near for a short while after it leaves, so a slot does not
    /// flap as someone walks past. Hands the held lights to the core, from where they rest.
    void Choose(const WXL_OmniShadowsApi* core, const gl::Light* list, int count, const float eye[3], double now, float dt)
    {
        // The face size applies when the core next creates its atlases; faces whose light or casters
        // moved are redrawn at once, the rest refreshed this many a frame.
        const int faceSize = std::clamp(g_cfg.faceSize, 64, 1024);
        if (faceSize != g_faceSizeSet && core->SetFaceSize) { core->SetFaceSize(uint32_t(faceSize)); g_faceSizeSet = faceSize; }
        const int budget = std::clamp(g_cfg.refresh, 1, 6);
        if (budget != g_budgetSet && core->SetBudget) { core->SetBudget(uint32_t(budget)); g_budgetSet = budget; }

        if (g_cfg.unitsFirst) GatherUnits(eye);
        else g_unitCount = 0;

        const float origin[3] = { 0.0f, 0.0f, 0.0f };
        float lo = 0.0f, hi = 0.0f;
        const int cameraRoom = gl::rooms::RoomOf(origin, lo, hi);
        std::vector<Candidate>& candidates = g_candidates;
        candidates.clear();
        for (int i = 0; i < count; ++i)
        {
            const gl::Light& l = list[i];
            if (l.radius < 2.0f || l.extent[0] != 0.0f || l.extent[1] != 0.0f || l.extent[2] != 0.0f) continue;
            // A carried light's map is filled by its own carrier and item, a few inches away.
            if (l.carried && !g_cfg.carried) continue;
            int rank = cameraRoom >= 0 && l.room != cameraRoom ? 1 : 0;
            if (g_cfg.unitsFirst)
            {
                bool unitNear = false;
                for (int u = 0; u < g_unitCount && !unitNear; ++u)
                {
                    const float dx = g_unitWorld[u][0] - l.rest[0], dy = g_unitWorld[u][1] - l.rest[1];
                    const float dz = g_unitWorld[u][2] + 0.5f * g_unitWorld[u][3] - l.rest[2];
                    const float reach = l.radius + kBodyReach;
                    unitNear = dx * dx + dy * dy + dz * dz < reach * reach;
                }
                if (l.id)
                {
                    double& seen = g_unitNear[l.id];
                    if (unitNear) seen = now;
                    else unitNear = seen > 0.0 && now - seen <= kNearHold;
                }
                // A lamp whose cookie already throws its own cage takes a map only for a passer-by.
                if (!unitNear && l.cookieOpen >= 0.0f) continue;
                rank += unitNear ? 0 : 2;
            }
            const float dx = l.rest[0] - eye[0], dy = l.rest[1] - eye[1], dz = l.rest[2] - eye[2];
            const float luma = (l.color[0] * 0.299f + l.color[1] * 0.587f + l.color[2] * 0.114f) * l.intensity;
            candidates.push_back({ rank, i, luma / (1.0f + (dx * dx + dy * dy + dz * dz) / (l.radius * l.radius)) });
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.rank != b.rank ? a.rank < b.rank : a.score > b.score;
        });
        // Forgotten lights leave the hold table now and then, so it cannot grow without bound.
        if (g_unitNear.size() > 512)
            for (auto it = g_unitNear.begin(); it != g_unitNear.end();)
                it = now - it->second > kNearHold ? g_unitNear.erase(it) : std::next(it);
        // Where each candidate stands: its place and rank by id; and where a light is in the list.
        auto find = [&](uint32_t id, int& rank) -> int {
            for (size_t k = 0; k < candidates.size(); ++k)
                if (list[candidates[k].index].id == id) { rank = candidates[k].rank; return int(k); }
            return -1;
        };
        auto listed = [&](uint32_t id) -> int {
            for (int i = 0; i < count; ++i)
                if (list[i].id == id) return i;
            return -1;
        };
        // A held light gone from the list frees its slot (its light has faded out with it); one still
        // listed but no longer a candidate fades its shadow out first, like a hand-over to nobody.
        for (SlotState& s : g_slots)
        {
            int rank = 0;
            if (s.id && listed(s.id) < 0) { s = SlotState{}; ++g_changes; continue; }
            if (s.id && find(s.id, rank) < 0) s.leaving = true;
            if (s.next && find(s.next, rank) < 0) s.next = 0;
        }
        // The best candidates not held: an empty slot takes one; otherwise one held light, past its
        // hold, out of the top four and in a worse tier, starts fading out for it.
        bool started = false;
        for (size_t k = 0; k < candidates.size() && k < size_t(kSlots); ++k)
        {
            const uint32_t id = list[candidates[k].index].id;
            if (!id || std::any_of(std::begin(g_slots), std::end(g_slots), [&](const SlotState& s) { return s.id == id || s.next == id; }))
                continue;
            SlotState* empty = nullptr;
            for (SlotState& s : g_slots) if (!s.id) { empty = &s; break; }
            if (empty) { *empty = SlotState{ id, 0, now, 0.0f, false }; ++g_changes; continue; }
            if (started) continue;
            SlotState* worst = nullptr;
            int worstRank = -1;
            for (SlotState& s : g_slots)
            {
                int rank = kLeavingRank;
                const int at = s.leaving ? kSlots : find(s.id, rank);
                if (s.next || (!s.leaving && now - s.since < kSlotHold) || at < kSlots || rank <= candidates[k].rank)
                    continue;
                if (rank > worstRank) { worstRank = rank; worst = &s; }
            }
            if (worst) { worst->next = id; started = true; }
        }
        // Shadows fade in and out over kSlotFade seconds, whatever the frame rate.
        const float step = std::min(dt / kSlotFade, 1.0f);
        for (SlotState& s : g_slots)
        {
            if (!s.id) continue;
            if (s.next || s.leaving)
            {
                s.weight -= step;
                if (s.weight > 0.0f) continue;
                s = s.next ? SlotState{ s.next, 0, now, 0.0f, false } : SlotState{};
                ++g_changes;
            }
            else s.weight = std::min(s.weight + step, 1.0f);
        }
        // Held lights in slot order; a freed slot in the middle closes up. The core keeps each map
        // with its light's id, so this only reorders the slots.
        for (int k = 0; k + 1 < kSlots; ++k)
            if (!g_slots[k].id && g_slots[k + 1].id) { std::swap(g_slots[k], g_slots[k + 1]); k = -1; }
        WXL_OmniLightEx chosen[kSlots];
        uint32_t n = 0;
        for (const SlotState& s : g_slots)
        {
            if (!s.id) break;
            const int index = listed(s.id);
            if (index < 0) break;
            const gl::Light& l = list[index];
            // Rendered from where the light rests: the flicker's sway does not redraw the map.
            chosen[n++] = WXL_OmniLightEx{ { l.rest[0], l.rest[1], l.rest[2] }, l.radius, s.id, 0 };
        }
        core->SetLightsEx(chosen, n);
        g_handedCount = int(n);
        for (uint32_t k = 0; k < n; ++k)
        {
            g_handed[k].id = chosen[k].id;
            std::memcpy(g_handed[k].position, chosen[k].position, sizeof g_handed[k].position);
        }
    }

    /// The share a light's map has this frame: the chooser's slot weight when this extension holds
    /// the slots, else a fade in from the moment the core's slot took a new light.
    float Weight(int slot, uint32_t id, float dt)
    {
        if (!g_forever)
        {
            for (const SlotState& s : g_slots)
                if (s.id == id) return s.weight;
            return 0.0f;
        }
        if (g_seenId[slot] != id)
        {
            g_seenId[slot] = id;
            g_seenWeight[slot] = 0.0f;
        }
        g_seenWeight[slot] = std::min(g_seenWeight[slot] + dt / kSlotFade, 1.0f);
        return g_seenWeight[slot];
    }

    /// The maps as the core rendered them, each matched to this frame's list by the id the core keeps
    /// with the slot (the LightId both extensions compute), into the table every consumer reads.
    void Read(const WXL_OmniShadowsApi* core, const gl::Light* list, int count, const float eye[3], float dt)
    {
        g_held = 0;
        const float self = std::clamp(g_cfg.self, 0.0f, 2.0f);
        const uint32_t ready = std::min(core->Count(), uint32_t(kSlots));
        g_pubCount = int(ready);
        for (uint32_t i = 0; i < ready; ++i)
        {
            WXL_GfxOmniSlot& pub = g_pub[i];
            pub = WXL_GfxOmniSlot{};
            pub.lightIndex = -1;
            WXL_OmniShadow s{};
            if (!core->Get(i, &s) || !s.texture || s.faceSize == 0) continue;
            WXL_OmniShadowState st{};
            const bool tracked = core->GetState(i, &st) != 0;
            int match = -1;
            if (tracked && st.id)
                for (int j = 0; j < count && match < 0; ++j)
                    if (list[j].id == st.id) match = j;
            pub.lightIndex = match;
            if (match >= 0) ++g_held;
            // A carried light: the item and the hand are its housing, and the carrier's own body does not
            // take its map (it shadows the surroundings, never itself).
            const bool carried = match >= 0 && list[match].carried;
            g_housing[i] = carried ? std::clamp(g_cfg.carriedSelf, 0.0f, 2.0f) : self;
            g_skip[i] = carried ? std::clamp(g_cfg.carriedSkip, 0.0f, 3.0f) : 0.0f;
            // A light handed to the core this frame from elsewhere than its maps were last rendered: the
            // core draws all six faces there in this frame's world pass, so the rows follow it now.
            float shift[3] = { 0.0f, 0.0f, 0.0f };
            if (!g_forever && tracked && st.id)
                for (int h = 0; h < g_handedCount; ++h)
                    if (g_handed[h].id == st.id)
                    {
                        for (int k = 0; k < 3; ++k) shift[k] = g_handed[h].position[k] - s.position[k];
                        if (shift[0] * shift[0] + shift[1] * shift[1] + shift[2] * shift[2] <= 0.05f * 0.05f)
                            shift[0] = shift[1] = shift[2] = 0.0f;
                        break;
                    }
            // A map no listed light owns shadows nothing.
            pub.weight = match >= 0 ? Weight(int(i), st.id, dt) : 0.0f;
            pub.radius = s.radius;
            pub.faceSize = float(s.faceSize);
            pub.texture = s.texture;
            for (int k = 0; k < 3; ++k) pub.position[k] = s.position[k] + shift[k] - eye[k];
            // Three rows per face: u * w, v * w and w (the projected z is not read), taken to
            // camera-relative input: the eye folds into the constant term.
            for (int f = 0; f < 6; ++f)
                for (int j = 0; j < 3; ++j)
                {
                    float* row = pub.rows[f][j];
                    if (!s.faceFrame[f]) { row[0] = row[1] = row[2] = row[3] = 0.0f; continue; }
                    const float* r = s.faceRows[f][j == 2 ? 3 : j];
                    row[0] = r[0]; row[1] = r[1]; row[2] = r[2];
                    row[3] = float(double(r[3]) + double(r[0]) * (eye[0] - shift[0]) + double(r[1]) * (eye[1] - shift[1])
                                   + double(r[2]) * (eye[2] - shift[2]));
                }
        }
    }

    void UpdateStatus(const char* owner)
    {
        std::snprintf(g_status, sizeof g_status, "omni shadow maps: %s; %d of %d maps on this frame's lights, %d hand-overs",
                      owner, g_held, g_pubCount, g_changes);
    }
}

namespace wxl::gfx::lights::omni
{
    Settings& Get() { return g_cfg; }

    void Install()
    {
        g_cfg.enabled    = ConfigBool("WXL_GFX_LIGHTS_OMNI_SHADOWS", g_cfg.enabled != 0) ? 1 : 0;
        g_cfg.unitsFirst = ConfigBool("WXL_GFX_LIGHTS_OMNI_UNITS_FIRST", g_cfg.unitsFirst != 0) ? 1 : 0;
        g_cfg.carried    = ConfigBool("WXL_GFX_LIGHTS_OMNI_CARRIED", g_cfg.carried != 0) ? 1 : 0;
        g_cfg.self       = ConfigFloat("WXL_GFX_LIGHTS_OMNI_SELF", g_cfg.self, 0.0f, 2.0f);
        g_cfg.carriedSelf = ConfigFloat("WXL_GFX_LIGHTS_OMNI_CARRIED_SELF", g_cfg.carriedSelf, 0.0f, 2.0f);
        g_cfg.carriedSkip = ConfigFloat("WXL_GFX_LIGHTS_OMNI_CARRIED_SKIP", g_cfg.carriedSkip, 0.0f, 3.0f);
        g_cfg.faceSize   = ConfigInt("WXL_GFX_LIGHTS_OMNI_FACE_SIZE", g_cfg.faceSize, 64, 1024);
        g_cfg.refresh    = ConfigInt("WXL_GFX_LIGHTS_OMNI_REFRESH", g_cfg.refresh, 1, 6);
        LIGHTS_LOG_INFO("omni: %s, units first %d, carried %d, own housing %.2f yd, face %d, refresh %d",
                        g_cfg.enabled ? "on" : "off", g_cfg.unitsFirst, g_cfg.carried, g_cfg.self, g_cfg.faceSize, g_cfg.refresh);
    }

    bool ForeverLoaded()
    {
        if (g_forever) return true;
        if ((g_foreverChecks++ & 63u) != 0) return false;
        // wxl-graphics-shadow owns every shadow, the core's maps included; wxl-forever did before it.
        if (g_api->GetInterface(WXL_GRAPHICS_SHADOW_API_NAME, WXL_GRAPHICS_SHADOW_API_VERSION)) g_owner = "wxl-graphics-shadow";
        else if (g_api->GetInterface(kForeverInterface, 2)) g_owner = "wxl-forever";
        g_forever = g_owner != nullptr;
        if (g_forever && !g_foreverLogged)
        {
            g_foreverLogged = true;
            LIGHTS_LOG_INFO("omni: %s is loaded and owns the core's shadow map slots; this extension only reads them back", g_owner);
        }
        return g_forever;
    }

    bool CoreAvailable() { return Core() != nullptr; }

    void Frame(IDirect3DDevice9* dev, const Light* list, int count, const float eye[3], uint32_t frame)
    {
        const double now = Now();
        const float dt = g_last > 0.0 ? float(std::clamp(now - g_last, 0.0, 0.25)) : 0.0f;
        g_last = now;
        const bool forever = ForeverLoaded();
        const WXL_OmniShadowsApi* core = Core();
        if (!core || (!forever && !g_cfg.enabled))
        {
            g_pubCount = g_held = 0;
            PublishOmni(dev, nullptr, 0, nullptr, nullptr, frame);
            UpdateStatus(core ? "off" : "not offered by the core");
            return;
        }
        if (!forever) Choose(core, list, count, eye, now, dt);
        Read(core, list, count, eye, dt);
        PublishOmni(dev, g_pub, g_pubCount, g_housing, g_skip, frame);
        char owner[64];
        std::snprintf(owner, sizeof owner, "chosen by %s", forever && g_owner ? g_owner : "this extension");
        UpdateStatus(owner);
    }

    const char* Status() { return g_status; }

    void Panel()
    {
        ui::Text(g_status);
        if (g_forever && g_owner)
            ui::Textf("%s is loaded and chooses the maps. The settings below apply without it.", g_owner);
        ui::Separator();
        ui::Check("Shadow maps", &g_cfg.enabled,
                  "Six-face depth maps the core renders for the four most important lights, so a lantern's cage, a lamp post and passers-by throw shadows on surfaces and in the fog.");
        ui::Check("Shadow maps to lights with units first", &g_cfg.unitsFirst,
                  "The four maps go first to lights with a character or creature near them (a passer-by must cast a shadow), then to lights without a cookie. A still lamp whose cookie already throws its own cage waits. Off: the brightest and nearest lights take them.");
        ui::Check("Shadow maps for carried lights", &g_cfg.carried,
                  "A torch or lantern in a hand may take a map, so its carrier casts a shadow on the ground and walls around. Off, carried lights never take one.");
        ui::Slider("Carried item housing (yards)", &g_cfg.carriedSelf, 0.0f, 1.0f,
                   "How far around a carried light its own item and the hand holding it are ignored by its shadow map, so they do not throw a huge shadow.");
        ui::Slider("Carrier self-shadow skip (yards)", &g_cfg.carriedSkip, 0.0f, 2.5f,
                   "Surfaces this near a carried light ignore its shadow map: the carrier's own body is lit by its torch, and its shadow falls on everything further away.");
        ui::Slider("Own housing (yards)", &g_cfg.self, 0.0f, 1.5f,
                   "How far around a light its own fixture (the post, the bracket, the cage) is ignored by its shadow map, published for every consumer.");
        static const char* const kFaces[] = { "256", "512", "1024" };
        int face = g_cfg.faceSize >= 1024 ? 2 : (g_cfg.faceSize >= 512 ? 1 : 0);
        if (ui::Combo("Omni face size", &face, kFaces, 3, "Texels per cube face of each map. Applies when the core next creates its atlases."))
            g_cfg.faceSize = face == 2 ? 1024 : (face == 1 ? 512 : 256);
        ui::Slider("Omni refresh (faces per frame)", &g_cfg.refresh, 1, 6,
                   "How many faces of still lights the core redraws each frame, in turn. Faces whose light or casters moved are redrawn at once.");
    }
}
