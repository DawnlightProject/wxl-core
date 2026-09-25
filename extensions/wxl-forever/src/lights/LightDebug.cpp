// wxl-forever lights: the light service's panel and debug overlay (the former wxl-lightdebug).
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

#include "../core/BakedAssets.hpp"
#include "../core/ExtensionApi.hpp"
#include "../core/Panel.hpp"
#include "Cookies.hpp"
#include "Rooms.hpp"
#include "Lights.hpp"
#include "ModelTable.hpp"

#include "wxl/EventScript.hpp"
#include "game/Camera.hpp"
#include "game/Effects.hpp"
#include "game/Gfx.hpp"
#include "game/Gx.hpp"
#include "game/Lights.hpp"
#include "game/World.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace
{
    namespace ev  = wxl::events;
    namespace gfx = wxl::game::gfx;
    namespace gx  = wxl::game::gx;
    namespace cam = wxl::game::camera;
    namespace world = wxl::game::world;
    namespace lt  = wxl::game::lights;
    namespace efx = wxl::game::effects;
    namespace fl  = wxl::forever::lights;
    namespace ui  = wxl::forever::ui;

    constexpr size_t kCap = 512;
    constexpr size_t kMissileCap = 64;
    constexpr size_t kEmitterCap = 512;

    struct Settings
    {
        int   enabled   = 0;     // draw the overlay
        int   wmo       = 0;     // include MOLT lights
        int   table     = 0;     // include model-table lights
        int   through   = 1;     // draw through walls
        float radius    = 120.0f;
        int   staleFrames = 0;
        int   logStats  = 0;     // the histogram to the log once a second
        int   missiles  = 0;     // missiles in flight: an arrow along the flight, a sphere of the model
        int   emitters  = 0;     // particle emitters: a coloured tick along the emission axis
        float emitterRadius = 40.0f;
    };

    Settings            g_cfg;
    lt::PointLight      g_engine[kCap];
    fl::Light           g_table[kCap];
    size_t              g_engineFound = 0, g_tableFound = 0;
    size_t              g_m2 = 0, g_wmo = 0;
    lt::Stats           g_stats{};
    fl::table::Stats    g_tableStats{};
    bool                g_logNext = false;
    unsigned long long  g_lastStatsLog = 0;

    efx::Missile        g_missiles[kMissileCap];
    size_t              g_missileCount = 0;
    efx::MissileStats   g_missileStats{};
    efx::Emitter        g_emitters[kEmitterCap];
    size_t              g_emitterCount = 0;
    efx::EmitterStats   g_emitterStats{};
    bool                g_logEffects = false;

    /// WXL_FOREVER_LIGHTS_<name>.
    bool Raw(const char* name, char* buf, size_t cap)
    {
        return wxl_forever::ConfigRaw((std::string("WXL_FOREVER_LIGHTS_") + name).c_str(), buf, cap);
    }

    int Bool(const char* name, bool fallback)
    {
        char buf[32];
        return (Raw(name, buf, sizeof buf) ? wxl::ext::config::Truthy(buf, fallback) : fallback) ? 1 : 0;
    }

    float Float(const char* name, float fallback, float lo, float hi)
    {
        char buf[32];
        if (!Raw(name, buf, sizeof buf)) return fallback;
        char* end = nullptr;
        const float v = std::strtof(buf, &end);
        return end == buf ? fallback : std::clamp(v, lo, hi);
    }


    const char* MissileKindName(efx::MissileKind k)
    {
        return k == efx::MissileKind::Spell ? "spell" : (k == efx::MissileKind::Ranged ? "ranged" : "other");
    }

    const char* EmitterKindName(efx::EmitterKind k)
    {
        static const char* const kNames[] = { "smoke", "steam", "dust", "fire", "other" };
        return kNames[uint32_t(k)];
    }

    gfx::Color EmitterColor(efx::EmitterKind k)
    {
        switch (k)
        {
        case efx::EmitterKind::Smoke: return 0xE0A0A0A0;
        case efx::EmitterKind::Steam: return 0xE0C0E0FF;
        case efx::EmitterKind::Dust:  return 0xE0C8A064;
        case efx::EmitterKind::Fire:  return 0xE0FF7020;
        default:                      return 0xE0B060FF;
        }
    }

    gfx::Color ToColor(const float rgb[3], uint8_t alpha)
    {
        const float peak = std::max({ rgb[0], rgb[1], rgb[2] });
        if (peak <= 1e-6f) return 0x00808080u | (uint32_t(alpha) << 24);
        uint32_t c = uint32_t(alpha) << 24;
        for (int k = 0; k < 3; ++k)
            c |= uint32_t(std::lround(std::clamp(rgb[k] / peak, 0.0f, 1.0f) * 255.0f)) << (16 - 8 * k);
        return c;
    }

    void FormatStats(char* buf, size_t cap)
    {
        const lt::Stats& s = g_stats;
        const fl::table::Stats& t = g_tableStats;
        std::snprintf(buf, cap,
            "frame %u | models %u: not loaded %u, no records %u, no lights %u, lit %u | lights: "
            "not point %u, stale %u, hidden %u, too far %u, accepted %u | wmo: placements %u, "
            "skipped %u, entries %u, not omni %u, too far %u, accepted %u | table: rows %u, unlit instances %u, "
            "matched %u, too far %u, accepted %u | faults %u",
            s.frame, s.models, s.notLoaded, s.noRecords, s.noLights, s.litModels, s.notPoint, s.stale,
            s.hidden, s.tooFar, s.accepted, s.wmoPlacements, s.wmoSkipped, s.wmoEntries, s.wmoNotOmni,
            s.wmoTooFar, s.wmoAccepted, t.rows, t.instances, t.matched, t.tooFar, t.accepted, s.faults);
    }

    void FormatEffectStats(char* buf, size_t cap)
    {
        const efx::MissileStats& m = g_missileStats;
        const efx::EmitterStats& e = g_emitterStats;
        std::snprintf(buf, cap,
            "missiles: in flight %u, stopped %u, too far %u, accepted %u, with model %u, faults %u | "
            "emitters: models %u, with emitters %u, stale %u, emitters %u, disabled %u, too far %u, "
            "accepted %u (smoke %u, steam %u, dust %u, fire %u, other %u), faults %u",
            m.inFlight, m.stopped, m.tooFar, m.accepted, m.withModel, m.faults,
            e.models, e.withEmitters, e.stale, e.emitters, e.disabled, e.tooFar, e.accepted,
            e.byKind[0], e.byKind[1], e.byKind[2], e.byKind[3], e.byKind[4], e.faults);
    }

    void LogStats()
    {
        char line[800];
        FormatStats(line, sizeof line);
        WLOG_INFO("lights: %s", line);
        for (uint32_t i = 0; i < g_stats.litNameCount; ++i) WLOG_INFO("  lit model: %s", g_stats.litNames[i]);
    }

    void LogLights(const float center[3])
    {
        float player[3] = { 0.0f, 0.0f, 0.0f };
        const unsigned long long guid = world::ActivePlayerGuid();
        if (void* unit = guid ? world::ResolveObject(guid, world::kTypeMaskPlayer) : nullptr)
            world::UnitPosition(unit, player);

        struct Entry { const char* kind; const float* position; const float* color; float atten0, atten1; const void* owner; uint32_t index; float d2; };
        static Entry entries[kCap * 2];
        size_t n = 0;
        auto d2 = [&](const float* p) {
            const float dx = p[0] - player[0], dy = p[1] - player[1], dz = p[2] - player[2];
            return dx * dx + dy * dy + dz * dz;
        };
        for (size_t i = 0; i < std::min(g_engineFound, kCap); ++i)
        {
            const lt::PointLight& l = g_engine[i];
            entries[n++] = { l.kind == lt::Kind::M2 ? "m2   " : "wmo  ", l.position, l.color, l.attenStart, l.attenEnd, l.owner, l.index, d2(l.position) };
        }
        for (size_t i = 0; i < std::min(g_tableFound, kCap); ++i)
        {
            const fl::Light& l = g_table[i];
            entries[n++] = { "table", l.position, l.color, l.innerRadius, l.radius, l.owner, l.index, d2(l.position) };
        }
        std::sort(entries, entries + n, [](const Entry& a, const Entry& b) { return a.d2 < b.d2; });

        LogStats();
        WLOG_INFO("lights: frame %u, camera (%.2f, %.2f, %.2f), player (%.2f, %.2f, %.2f), %u found",
                  lt::SceneFrame(), center[0], center[1], center[2], player[0], player[1], player[2], unsigned(n));
        for (size_t k = 0; k < n && k < 16; ++k)
        {
            const Entry& e = entries[k];
            WLOG_INFO("  %s owner %p #%u at (%.2f, %.2f, %.2f) %.1f yd from player, colour (%.3f, %.3f, %.3f), atten %.2f-%.2f",
                      e.kind, e.owner, e.index, e.position[0], e.position[1], e.position[2], std::sqrt(e.d2),
                      e.color[0], e.color[1], e.color[2], e.atten0, e.atten1);
        }
    }

    void LogEffects()
    {
        char line[800];
        FormatEffectStats(line, sizeof line);
        WLOG_INFO("effects: %s", line);
        for (size_t i = 0; i < std::min(g_missileCount, kMissileCap); ++i)
        {
            const efx::Missile& m = g_missiles[i];
            WLOG_INFO("  missile %s spell %u at (%.2f, %.2f, %.2f) dir (%.2f, %.2f, %.2f) %.1f yd/s, radius %.2f%s, model '%s'",
                      MissileKindName(m.kind), m.spellId, m.position[0], m.position[1], m.position[2],
                      m.direction[0], m.direction[1], m.direction[2], m.speed, m.radius,
                      m.ballistic ? " ballistic" : "", m.model);
        }
        for (size_t i = 0; i < std::min(g_emitterCount, size_t(24)); ++i)
        {
            const efx::Emitter& e = g_emitters[i];
            WLOG_INFO("  emitter %s at (%.2f, %.2f, %.2f) dir (%.2f, %.2f, %.2f) rate %.1f life %.2f speed %.2f"
                      " size %.2f-%.2f live %u blend %u '%s'",
                      EmitterKindName(e.kind), e.position[0], e.position[1], e.position[2],
                      e.direction[0], e.direction[1], e.direction[2], e.rate, e.lifespan, e.speed,
                      e.sizeMin, e.sizeMax, e.liveParticles, e.blend, e.texture);
        }
    }

    /// Collects missiles and emitters and queues the enabled ones; true when anything was queued.
    bool QueueEffects(const float center[3], gfx::Depth depth)
    {
        bool queued = false;
        efx::MissileQuery mq;
        for (int k = 0; k < 3; ++k) mq.center[k] = center[k];
        mq.radius = g_cfg.radius;
        g_missileCount = efx::CollectMissiles(mq, g_missiles, kMissileCap, &g_missileStats);

        efx::EmitterQuery eq;
        for (int k = 0; k < 3; ++k) eq.center[k] = center[k];
        eq.radius = g_cfg.emitterRadius;
        eq.maxStaleFrames = uint32_t(g_cfg.staleFrames);
        g_emitterCount = efx::CollectEmitters(eq, g_emitters, kEmitterCap, &g_emitterStats);

        if (g_cfg.missiles)
            for (size_t i = 0; i < std::min(g_missileCount, kMissileCap); ++i)
            {
                // An arrow a quarter second of flight long, and the model's extent.
                const efx::Missile& m = g_missiles[i];
                const float len = std::max(0.5f, m.speed * 0.25f);
                const float tip[3] = { m.position[0] + m.direction[0] * len, m.position[1] + m.direction[1] * len,
                                       m.position[2] + m.direction[2] * len };
                const gfx::Color c = m.kind == efx::MissileKind::Spell ? 0xF0FF40FF
                                   : (m.kind == efx::MissileKind::Ranged ? 0xF040FF40 : 0xF0FFFFFF);
                gfx::Arrow(m.position, tip, 0.3f, c, depth);
                gfx::Sphere(m.position, m.radius > 0.05f ? m.radius : 0.25f, c, depth, 12);
                queued = true;
            }
        if (g_cfg.emitters)
            for (size_t i = 0; i < std::min(g_emitterCount, kEmitterCap); ++i)
            {
                // A tick along the emission axis and a ring of the largest particle size.
                const efx::Emitter& e = g_emitters[i];
                const gfx::Color c = EmitterColor(e.kind);
                const float tip[3] = { e.position[0] + e.direction[0] * 0.8f, e.position[1] + e.direction[1] * 0.8f,
                                       e.position[2] + e.direction[2] * 0.8f };
                gfx::Arrow(e.position, tip, 0.15f, c, depth);
                gfx::Circle(e.position, std::clamp(e.sizeMax, 0.1f, 5.0f), c, depth, 10);
                queued = true;
            }
        return queued;
    }

    void OverlayTab()
    {
        char line[200];
        std::snprintf(line, sizeof line, "scene frame %u | m2 %u, wmo %u, table %u within %.0f yd of the camera",
                      lt::SceneFrame(), unsigned(g_m2), unsigned(g_wmo), unsigned(std::min(g_tableFound, kCap)), g_cfg.radius);
        ui::Text(line);
        ui::Text(fl::Status());
        std::snprintf(line, sizeof line, "published to shaders: %d lights, longest cluster list %d",
                      fl::Published(), fl::ClusterPeak());
        ui::Text(line);
        char stats[800];
        FormatStats(stats, sizeof stats);
        ui::Text(stats);
        for (uint32_t i = 0; i < g_stats.litNameCount; ++i) ui::Text(g_stats.litNames[i]);
        ui::Separator();
        ui::Check("Show lights", &g_cfg.enabled,
                  "Draws every light near the camera: a cross for model lights, a small box for WMO lights, a ring on a stem for model-table lights, and a sphere of its radius in its colour.");
        ui::Check("Include WMO lights", &g_cfg.wmo,
                  "Adds the lights buildings author (MOLT). The game's own renderer never uses them; the fog does.");
        ui::Check("Include model-table lights", &g_cfg.table,
                  "Adds the lights the model table gives to torches, lamp posts and candles that carry none of their own.");
        ui::Check("Draw through walls", &g_cfg.through, "Shows the markers even behind geometry.");
        ui::Slider("Search radius", &g_cfg.radius, 10.0f, 600.0f, "How far around the camera lights are shown, in yards.");
        ui::Slider("Keep lights stale for (frames)", &g_cfg.staleFrames, 0, 600,
                   "Also shows lights of models the game did not animate this frame, up to this many frames back.");
        ui::Check("Log the histogram every second", &g_cfg.logStats,
                  "Writes where every candidate light went (accepted, too far, hidden...) to the log once a second.");
        if (ui::Button("Log lights near me", "Writes the 16 lights nearest your character, with positions, colours and radii, to the log."))
            g_logNext = true;
    }

    void EffectsTab()
    {
        char stats[800];
        FormatEffectStats(stats, sizeof stats);
        ui::Text(stats);
        ui::Check("Show missiles in flight", &g_cfg.missiles,
                  "An arrow along each missile's flight and a sphere of its model: magenta for spells, green for arrows and bullets.");
        ui::Check("Show particle emitters", &g_cfg.emitters,
                  "A tick along each emitter's axis and a ring of its particle size: grey smoke, pale blue steam, tan dust, orange fire, purple other.");
        ui::Slider("Emitter search radius", &g_cfg.emitterRadius, 5.0f, 200.0f, "How far around the camera emitters are shown, in yards.");
        if (ui::Button("Log effects near me", "Writes the missiles in flight and the nearest emitters, with their parameters, to the log."))
            g_logEffects = true;
    }

    /// The service's own settings: what every consumer gets.
    void GatherTab()
    {
        fl::Options& o = fl::Settings();
        int engine = o.engine ? 1 : 0, wmo = o.wmo ? 1 : 0, table = o.table ? 1 : 0;
        ui::Text(fl::Status());
        char line[160];
        std::snprintf(line, sizeof line, "published to shaders: %d lights, longest cluster list %d",
                      fl::Published(), fl::ClusterPeak());
        ui::Text(line);
        ui::Separator();
        if (ui::Check("Client lights", &engine,
                      "Gathers the game's own lights (torches, lamps, braziers) for the fog and the surfaces.")) o.engine = engine != 0;
        if (ui::Check("WMO lights", &wmo,
                      "Includes the lights built into buildings and cities, most of a town's lamps.")) o.wmo = wmo != 0;
        if (ui::Check("Model-table lights", &table,
                      "Includes lights the model table gives to street lamps, lanterns and candles that carry none.")) o.table = table != 0;
        ui::Slider("Flicker", &o.flicker, 0.0f, 2.0f,
                   "How much lights flicker: fires breathe and gutter, candles tremble, lanterns barely move. The fog glow and the lit surfaces flicker together. Set 0 for steady lights.");
        ui::Slider("Gather radius", &o.radius, 10.0f, 150.0f,
                   "How far around the camera lights are gathered, in yards. Up to 128 are used, the brightest and nearest first.");
        ui::Slider("Carried light core (yards)", &o.carriedCore, 0.0f, 2.0f,
                   "How wide the bright heart of a light a character holds is (a torch, a lantern), on surfaces and in the fog. Wider cores keep it from flaring on whatever passes right next to it; the light further out is unchanged. 0 treats it like any light.");
        ui::Slider("Near-light cap", &o.nearCap, 0.5f, 5.0f,
                   "The most any light gives right next to its source, on surfaces and in the fog, in multiples of its own colour. Lower it if surfaces or fog touching a lamp or torch burn white. 5 is uncapped.");
        ui::Slider("Hot core", &o.hotCore, 0.0f, 1.0f,
                   "How much a flame's light runs whiter and brighter close to its source, on surfaces and in the fog: a torch's heart reads yellow-white, its falloff deep orange. Lamps take a third of it. 0 keeps one colour throughout.");
        ui::Slider("Brightest light", &o.maxPeak, 0.5f, 10.0f,
                   "The most any single light may give, in its brightest colour channel. The game's own model and building lights carry their intensity in their colour and some reach far past what a lamp should; this keeps one of them from washing everything in reach.");
        ui::Slider("Widest light (yards)", &o.maxRadius, 5.0f, 100.0f,
                   "The furthest any single light may reach. Some model lights declare a reach of dozens of yards.");
        int mergeReach = o.mergeReach ? 1 : 0;
        if (ui::Check("Merged lamps take the larger reach", &mergeReach,
                      "A lamp is often lit by two sources at once: the curated light this suite gives its model, and a light the game or the building's author placed on it. They are merged into one, keeping the curated colour and shape. On, the merged light also takes the larger reach and brightness of the two, so the ground around the lamp is lit as far as the game's own light went; off, it keeps the curated reach, with a tighter pool and halo."))
            o.mergeReach = mergeReach != 0;
        ui::Slider("Light between floors", &o.roomLeak, 0.0f, 1.0f,
                   "How much of a lamp's light inside a building reaches a room above or below its own, on surfaces and in the fog. Floors and ceilings are not drawn between a lamp and the room over it, so without this a lamp downstairs lights the floor upstairs. 0 stops it at the floor, 1 lets it all through. Rooms on the same floor and open double-height halls keep their light; lights outdoors are never affected.");
        char roomLine[120];
        std::snprintf(roomLine, sizeof roomLine, "rooms near the camera: %d of %d interior groups, camera %s", fl::rooms::Count(),
                      fl::rooms::Collected(), fl::rooms::CameraIndoor() ? "indoors" : "outdoors");
        ui::Text(roomLine);
    }

    class LightsModule final : public wxl::ext::EventScript
    {
    public:
        LightsModule()
        {
            on<&LightsModule::OnWorldSceneEnd>(ev::Event::OnWorldSceneEnd);
            on<&LightsModule::OnDeviceLost>(ev::Event::OnDeviceLost);
        }

        void OnDeviceLost(const ev::DeviceResetArgs&)
        {
            fl::ReleaseTextures();
            fl::cookies::ReleaseTextures();
            wxl::forever::baked::ReleaseAll();
        }

        void OnWorldSceneEnd(const ev::WorldSceneEndArgs& a)
        {
            // The panel's histogram is live whenever the panel is open, even with the overlay off.
            if (!g_cfg.enabled && !g_logNext && !g_cfg.logStats && !g_cfg.missiles && !g_cfg.emitters &&
                !g_logEffects && !wxl_forever::g_api->UiIsOpen()) return;

            lt::Query q;
            cam::GetPosition(q.center);
            q.radius         = g_cfg.radius;
            q.m2             = true;
            q.wmo            = g_cfg.wmo != 0;
            q.maxStaleFrames = uint32_t(g_cfg.staleFrames);
            g_engineFound = lt::Collect(q, g_engine, kCap, &g_stats);
            g_tableFound  = g_cfg.table
                ? fl::table::Collect(q.center, g_cfg.radius, uint32_t(g_cfg.staleFrames), g_table, kCap, &g_tableStats) : 0;

            const unsigned long long now = GetTickCount64();
            if (g_cfg.logStats && now - g_lastStatsLog >= 1000)
            {
                g_lastStatsLog = now;
                LogStats();
            }

            g_m2 = g_wmo = 0;
            const size_t n = std::min(g_engineFound, kCap);
            for (size_t i = 0; i < n; ++i) (g_engine[i].kind == lt::Kind::M2 ? g_m2 : g_wmo)++;

            if (g_logNext)
            {
                g_logNext = false;
                LogLights(q.center);
            }
            const gfx::Depth depth = g_cfg.through ? gfx::Depth::Through : gfx::Depth::Tested;
            const bool effects = QueueEffects(q.center, depth);
            if (g_logEffects)
            {
                g_logEffects = false;
                LogEffects();
            }
            if (!g_cfg.enabled)
            {
                if (effects) gfx::Flush(gx::Device9(a.device), a.sceneDepth);
                return;
            }

            // M2: a cross. WMO: a small box. Model table: a ring with a stem, so it reads as guessed.
            for (size_t i = 0; i < n; ++i)
            {
                const lt::PointLight& l = g_engine[i];
                const gfx::Color c = ToColor(l.color, 0xE0);
                if (l.kind == lt::Kind::M2) gfx::Cross(l.position, 0.4f, c, depth);
                else
                {
                    const float ext[3] = { 0.25f, 0.25f, 0.25f };
                    gfx::BoxAt(l.position, ext, c, depth);
                }
                if (l.attenEnd > 0.0f) gfx::Sphere(l.position, l.attenEnd, ToColor(l.color, 0x90), depth, 20);
            }
            for (size_t i = 0; i < std::min(g_tableFound, kCap); ++i)
            {
                const fl::Light& l = g_table[i];
                const gfx::Color c = ToColor(l.color, 0xE0);
                const float top[3] = { l.position[0], l.position[1], l.position[2] + 0.6f };
                gfx::Circle(l.position, 0.35f, c, depth, 12);
                gfx::Line(l.position, top, c, depth);
                if (l.radius > 0.0f) gfx::Sphere(l.position, l.radius, ToColor(l.color, 0x90), depth, 20);
            }
            gfx::Flush(gx::Device9(a.device), a.sceneDepth);
        }
    };
}

namespace wxl::forever::lights
{
    void UiOverview()
    {
        ui::Scope scope("lights.overview");
        ui::Text(fl::Status());
        char line[160];
        std::snprintf(line, sizeof line, "published to shaders: %d lights, longest cluster list %d | %s",
                      fl::Published(), fl::ClusterPeak(), fl::cookies::Status());
        ui::Text(line);
    }

    void UiSettings()
    {
        ui::Scope scope("lights");
        if (!ui::BeginTabs("forever.lights")) return;
        if (ui::BeginTab("Gather"))  { GatherTab(); ui::EndTab(); }
        if (ui::BeginTab("Cookies")) { fl::cookies::Panel(); ui::EndTab(); }
        ui::EndTabs();
    }

    void UiDebug()
    {
        ui::Scope scope("lights.debug");
        ui::Text("Light markers");
        OverlayTab();
        ui::Separator();
        int noRooms = fl::Settings().rooms ? 0 : 1;
        if (ui::Check("No room gating", &noRooms,
                      "Lights inside buildings reach every room in their radius again, through floors and ceilings, on surfaces and in the fog."))
            fl::Settings().rooms = noRooms == 0;
        int noMerge = fl::Settings().merge ? 0 : 1;
        if (ui::Check("No fixture merge", &noMerge,
                      "Every source of light on a lamp is kept apart again: the curated light, the game's model light and the building's light each count, and which one shows changes with the camera."))
            fl::Settings().merge = noMerge == 0;
        ui::Separator();
        ui::Text("Missiles and emitters");
        EffectsTab();
    }

    void Install()
    {
        g_cfg.enabled       = Bool("ENABLED", false);
        g_cfg.wmo           = Bool("WMO", false);
        g_cfg.table         = Bool("TABLE", false);
        g_cfg.through       = Bool("THROUGH", true);
        g_cfg.radius        = Float("RADIUS", g_cfg.radius, 10.0f, 600.0f);
        g_cfg.logStats      = Bool("LOG_STATS", false);
        g_cfg.missiles      = Bool("MISSILES", false);
        g_cfg.emitters      = Bool("EMITTERS", false);
        g_cfg.emitterRadius = Float("EMITTER_RADIUS", g_cfg.emitterRadius, 5.0f, 200.0f);

        // What the service gathers.
        fl::Options& o = fl::Settings();
        o.engine = Bool("GATHER_ENGINE", true) != 0;
        o.wmo    = Bool("GATHER_WMO", true) != 0;
        o.table  = Bool("GATHER_TABLE", true) != 0;
        o.radius = Float("GATHER_RADIUS", o.radius, 5.0f, 300.0f);
        o.flicker = Float("FLICKER", o.flicker, 0.0f, 4.0f);
        o.carriedCore = Float("CARRIED_CORE", o.carriedCore, 0.0f, 3.0f);
        o.nearCap = Float("NEAR_CAP", o.nearCap, 0.1f, 10.0f);
        o.hotCore = Float("HOT_CORE", o.hotCore, 0.0f, 1.0f);
        o.maxPeak = Float("MAX_PEAK", o.maxPeak, 0.5f, 20.0f);
        o.maxRadius = Float("MAX_RADIUS", o.maxRadius, 5.0f, 200.0f);
        o.rooms = Bool("ROOMS", true) != 0;
        o.roomLeak = Float("ROOM_LEAK", o.roomLeak, 0.0f, 1.0f);
        o.merge = Bool("MERGE", true) != 0;
        o.mergeReach = Bool("MERGE_REACH", true) != 0;

        static LightsModule module;
        fl::cookies::Install();
        fl::rooms::Install();
        WLOG_INFO("lights: service installed (overlay %d, wmo %d, table %d, radius %.0f)", g_cfg.enabled, g_cfg.wmo,
                  g_cfg.table, g_cfg.radius);
    }
}
