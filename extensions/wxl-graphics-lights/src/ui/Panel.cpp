// wxl-graphics-lights: the "Graphics Lights" panel and the light markers overlay (wxl-forever's LightDebug).
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
#include "Panel.hpp"
#include "../lights/Cookies.hpp"
#include "../lights/Lights.hpp"
#include "../lights/ModelTable.hpp"
#include "../lights/Omni.hpp"
#include "../lights/Families.hpp"
#include "../lights/Rooms.hpp"
#include "../lights/Sky.hpp"
#include "../gpu/Gpu.hpp"
#include "../render/Field.hpp"
#include "../render/Hdr.hpp"
#include "../render/Render.hpp"
#include "../render/Surface.hpp"

#include "wxl/EventScript.hpp"
#include "wxl/gfx/Ui.hpp"
#include "game/Camera.hpp"
#include "game/Effects.hpp"
#include "game/Gfx.hpp"
#include "game/Gx.hpp"
#include "game/Lights.hpp"
#include "game/World.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <new>

namespace
{
    namespace ev    = wxl::events;
    namespace gfx   = wxl::game::gfx;
    namespace gx    = wxl::game::gx;
    namespace cam   = wxl::game::camera;
    namespace world = wxl::game::world;
    namespace lt    = wxl::game::lights;
    namespace efx   = wxl::game::effects;
    namespace gl    = wxl::gfx::lights;
    namespace ui    = wxl::gfx::ui;

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
    gl::Light           g_table[kCap];
    size_t              g_engineFound = 0, g_tableFound = 0;
    size_t              g_m2 = 0, g_wmo = 0;
    lt::Stats           g_stats{};
    gl::table::Stats    g_tableStats{};
    bool                g_logNext = false;
    unsigned long long  g_lastStatsLog = 0;
    int                 g_describe = 0;       // the light the self-check line describes
    char                g_describeLine[640] = "";

    efx::Missile        g_missiles[kMissileCap];
    size_t              g_missileCount = 0;
    efx::MissileStats   g_missileStats{};
    efx::Emitter        g_emitters[kEmitterCap];
    size_t              g_emitterCount = 0;
    efx::EmitterStats   g_emitterStats{};
    bool                g_logEffects = false;

    /// Runs an event handler so that no exception leaves it into the core.
    template <class F>
    void Guarded(const char* what, F&& call) noexcept
    {
        try { call(); }
        catch (const std::bad_alloc&) { LIGHTS_LOG_ERROR("%s: out of memory", what); }
        catch (...) { LIGHTS_LOG_ERROR("%s: unexpected exception", what); }
    }

    const char* MissileKindName(efx::MissileKind k)
    {
        return k == efx::MissileKind::Spell ? "spell" : (k == efx::MissileKind::Ranged ? "ranged" : "other");
    }

    const char* EmitterKindName(efx::EmitterKind k)
    {
        static const char* const kNames[] = { "smoke", "steam", "dust", "fire", "other" };
        return kNames[std::min(uint32_t(k), 4u)];
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
        const gl::table::Stats& t = g_tableStats;
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
        LIGHTS_LOG_INFO("lights: %s", line);
        for (uint32_t i = 0; i < g_stats.litNameCount; ++i) LIGHTS_LOG_INFO("  lit model: %s", g_stats.litNames[i]);
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
            const gl::Light& l = g_table[i];
            entries[n++] = { "table", l.position, l.color, l.innerRadius, l.radius, l.owner, l.index, d2(l.position) };
        }
        std::sort(entries, entries + n, [](const Entry& a, const Entry& b) { return a.d2 < b.d2; });

        LogStats();
        LIGHTS_LOG_INFO("lights: frame %u, camera (%.2f, %.2f, %.2f), player (%.2f, %.2f, %.2f), %u found",
                        lt::SceneFrame(), center[0], center[1], center[2], player[0], player[1], player[2], unsigned(n));
        for (size_t k = 0; k < n && k < 16; ++k)
        {
            const Entry& e = entries[k];
            LIGHTS_LOG_INFO("  %s owner %p #%u at (%.2f, %.2f, %.2f) %.1f yd from player, colour (%.3f, %.3f, %.3f), atten %.2f-%.2f",
                            e.kind, e.owner, e.index, e.position[0], e.position[1], e.position[2], std::sqrt(e.d2),
                            e.color[0], e.color[1], e.color[2], e.atten0, e.atten1);
        }
    }

    void LogEffects()
    {
        char line[800];
        FormatEffectStats(line, sizeof line);
        LIGHTS_LOG_INFO("effects: %s", line);
        for (size_t i = 0; i < std::min(g_missileCount, kMissileCap); ++i)
        {
            const efx::Missile& m = g_missiles[i];
            LIGHTS_LOG_INFO("  missile %s spell %u at (%.2f, %.2f, %.2f) dir (%.2f, %.2f, %.2f) %.1f yd/s, radius %.2f%s, model '%s'",
                            MissileKindName(m.kind), m.spellId, m.position[0], m.position[1], m.position[2],
                            m.direction[0], m.direction[1], m.direction[2], m.speed, m.radius,
                            m.ballistic ? " ballistic" : "", m.model);
        }
        for (size_t i = 0; i < std::min(g_emitterCount, size_t(24)); ++i)
        {
            const efx::Emitter& e = g_emitters[i];
            LIGHTS_LOG_INFO("  emitter %s at (%.2f, %.2f, %.2f) dir (%.2f, %.2f, %.2f) rate %.1f life %.2f speed %.2f"
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

    /// One line on the list as published: how many, the longest cluster list, the pool.
    void PublishedLine()
    {
        int count = 0;
        uint32_t frame = 0;
        gl::CurrentAbi(count, frame);
        ui::Textf("published to shaders: %d lights (frame %u), longest cluster list %d, %d cluster entries",
                  gl::Published(), frame, gl::ClusterPeak(), gl::ClusterEntries());
    }

    void OverlayTab()
    {
        ui::Textf("scene frame %u | m2 %u, wmo %u, table %u within %.0f yd of the camera",
                  lt::SceneFrame(), unsigned(g_m2), unsigned(g_wmo), unsigned(std::min(g_tableFound, kCap)), g_cfg.radius);
        ui::Text(gl::Status());
        ui::Text(gl::Nearest());
        PublishedLine();
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
        ui::Separator();
        // The self-check: one published light as a shader reads it back, beside its source.
        int count = 0;
        const gl::Light* list = gl::Current(count);
        ui::Slider("Self-check light", &g_describe, 0, std::max(count - 1, 0),
                   "Which light of this frame's list the line below describes: its texture columns, its cluster at its own place on screen and the layout, beside the light it came from.");
        if (count > 0)
        {
            g_describe = std::clamp(g_describe, 0, count - 1);
            gl::DescribePublished(g_describe, &list[g_describe], g_describeLine, sizeof g_describeLine);
            ui::Text(g_describeLine);
        }
        else ui::Text("no list published this frame (no consumer asked for one)");
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
        gl::Options& o = gl::Settings();
        int engine = o.engine ? 1 : 0, wmo = o.wmo ? 1 : 0, table = o.table ? 1 : 0;
        ui::Text(gl::Status());
        PublishedLine();
        ui::Text(gl::cookies::Status());
        ui::Text(gl::omni::Status());
        ui::Separator();
        if (ui::Check("Client lights", &engine,
                      "Gathers the game's own lights (torches, lamps, braziers) for the fog and the surfaces.")) o.engine = engine != 0;
        if (ui::Check("WMO lights", &wmo,
                      "Includes the lights built into buildings and cities, most of a town's lamps.")) o.wmo = wmo != 0;
        if (ui::Check("Model-table lights", &table,
                      "Includes lights the model table gives to street lamps, lanterns and candles that carry none.")) o.table = table != 0;
        ui::Slider("Gather radius", &o.radius, 10.0f, 150.0f,
                   "How far around the camera lights are gathered, in yards. Up to 128 are used, the brightest and nearest first. A light chosen stays at least two seconds; one leaving the set fades out over a second and keeps its place until it is gone, and a newcomer fades in once a place is free, so no lamp pops. A lamp whose model the game stops animating (off screen) keeps its light for ten seconds; the model table's lamps stay while their model is loaded.");
        ui::Slider("Near-light cap (current fog)", &o.nearCap, 0.5f, 5.0f,
                   "The current fog's cap on a lamp's glow right next to its source, in multiples of its colour. The lit surfaces have no cap: the curve at the end handles bright light.");
        ui::Slider("Brightest light (current fog)", &o.maxPeak, 0.5f, 10.0f,
                   "The most any single light may give the current fog, in its brightest colour channel. The game's own model and building lights carry their intensity in their colour and some reach far past what a lamp should; this keeps one of them from washing everything in reach.");
        ui::Slider("Widest light (yards)", &o.maxRadius, 5.0f, 100.0f,
                   "The furthest any single light may reach. Some model lights declare a reach of dozens of yards.");
        int mergeReach = o.mergeReach ? 1 : 0;
        if (ui::Check("Merged lamps take the larger reach", &mergeReach,
                      "A lamp is often lit by two sources at once: the curated light the model table gives its model, and a light the game or the building's author placed on it. They are merged into one, keeping the curated colour and shape. On, the merged light also takes the larger reach and brightness of the two, so the ground around the lamp is lit as far as the game's own light went; off, it keeps the curated reach, with a tighter pool and halo."))
            o.mergeReach = mergeReach != 0;
        ui::Slider("Light between floors", &o.roomLeak, 0.0f, 1.0f,
                   "How much of a lamp's light inside a building reaches a room above or below its own, on surfaces and in the fog. Floors and ceilings are not drawn between a lamp and the room over it, so without this a lamp downstairs lights the floor upstairs. 0 stops it at the floor, 1 lets it all through. Rooms on the same floor and open double-height halls keep their light; lights outdoors are never affected.");
        ui::Textf("rooms near the camera: %d of %d interior groups, camera %s", gl::rooms::Count(), gl::rooms::Collected(),
                  gl::rooms::CameraIndoor() ? "indoors" : "outdoors");
    }

    void TimersLine();

    /// The HDR chain and the light model: what every lamp looks like.
    void LightTab()
    {
        namespace hd = gl::hdr;
        namespace fm = gl::families;
        gl::Options& o = gl::Settings();
        hd::Settings& h = hd::Config();
        ui::Text(hd::Status());
        ui::Text(gl::render::Status());
        ui::Text(gl::sky::Status());
        TimersLine();
        ui::Separator();
        ui::Check("Lit surfaces and HDR", &gl::render::Enabled(),
                  "The lamps light the world's surfaces, and the world is drawn in high dynamic range, then mapped to the screen once at the end. Off, the service still publishes its lights for the fog.");
        ui::Check("HDR world", &h.hdr,
                  "Draws the world into a 16-bit float image so light adds without clipping, then maps it to the screen with one curve. Off, the curve is applied in the composite on the 8-bit image. Applies from the next frame.");
        ui::Slider("Curve knee", &h.knee, 0.3f, 0.85f,
                   "Where the curve starts to bend bright light towards white. Below it the image is exactly the game's own. Lower gives softer highlights and whiter lamp cores; higher keeps more colour in bright pools.");
        ui::Slider("Exposure", &h.exposure, 0.25f, 4.0f,
                   "Brightness of the whole image before the curve, the game's own light included. 1 leaves the game's look untouched. wxl-graphics-post will take this over.");
        ui::Check("Dither", &h.dither, "Blue-noise dither against banding in dark gradients on the 8-bit screen.");
        ui::Separator();
        ui::Slider("Lamp intensity", &o.gain, 0.0f, 4.0f,
                   "Every lamp's brightness, times this. At 0.35 a torch lights the ground to about 0.4 of the game's white at two yards, bright beside the game's own night light, and fades into the night within about ten. 1 is the families' full intensity: white at two yards. A lamp's reach follows its brightness.");
        ui::Slider("Lamps by day", &o.dayGain, 0.0f, 1.0f,
                   "How strong a lamp outside every building is by full day, against the night: the eye adapted to the sun sees a street lamp far weaker. Follows the sun through dusk and dawn; lamps inside rooms keep their full strength. 1 lamps are as strong by day as by night.");
        ui::Slider("Warmth adaptation", &o.adaptation, 0.0f, 1.0f,
                   "How much the eye adapts to warm light. 0 shows each lamp at its true colour temperature (a candle deep orange); higher moves them towards white, as a room lit by lamps looks after a while.");
        ui::Slider("Warm colour cap", &o.warmthCap, 0.0f, 4.0f,
                   "How far a warm lamp's red may run past its brightness. A flame's colour at full brightness is mostly red, and on reddish ground or stone the red channel alone meets the curve: the pool reads red, not orange. This eases the strongest channel back, keeping the brightness: lower gives yellower pools, higher redder ones, 0 no cap. Magic colours are never capped.");
        ui::Slider("Light cutoff", &o.cutoff, 0.002f, 0.05f,
                   "The brightness where a lamp's reach ends, smoothly. Lower lets light carry further (and costs more); higher keeps pools tighter.");
        ui::Slider("Carried light core (yards)", &o.carriedCore, 0.0f, 2.0f,
                   "How wide the soft heart of a light a character holds is, so a torch never flares on its carrier or on what passes beside it.");
        ui::Slider("Flame hot core", &o.hotCore, 0.0f, 1.0f,
                   "How much a flame's light runs whiter close to its source, on surfaces and in the air.");
        ui::Slider("Flicker", &o.flicker, 0.0f, 2.0f,
                   "How much lights flicker: fires breathe and gutter, candles tremble, lanterns barely move. Surfaces, air and shadows flicker together. 0 for steady lights.");
        int legacy = o.legacyChroma ? 1 : 0;
        if (ui::Check("Fog takes the families' colours", &legacy,
                      "The current fog's lamp halos take the same colour temperature as the lit surfaces, at the brightness they always had."))
            o.legacyChroma = legacy != 0;
        if (ui::Section("Families"))
        {
            ui::Scope scope("families");
            for (uint32_t f = 1; f < fm::kCount; ++f)
            {
                const fm::Family& fam = fm::Get(f);
                char label[64], help[256];
                std::snprintf(label, sizeof label, "%s", fm::Name(f));
                if (fam.kelvin > 0.0f)
                    std::snprintf(help, sizeof help, "The %s family's intensity, times this: %.1f K, %.1f at one yard, a soft core of %.2f yards.",
                                  fm::Name(f), fam.kelvin, fam.intensity, fam.softRadius);
                else
                    std::snprintf(help, sizeof help, "The %s family's intensity, times this: its own hue, %.1f at one yard, a soft core of %.2f yards.",
                                  fm::Name(f), fam.intensity, fam.softRadius);
                ui::Slider(label, &fm::IntensityScale(f), 0.0f, 4.0f, help);
            }
        }
    }

    /// The GPU time of every span this extension draws, and of the shadow service it reads.
    void TimersLine()
    {
        namespace gpu = gl::gpu;
        if (!gpu::TimersSupported())
        {
            ui::Text("GPU timers: not available on this device (or not measured yet)");
        }
        else
        {
            ui::Textf("GPU: field %.3f ms, halo %.3f ms, surfaces %.3f ms, copy %.3f ms (compute %.3f ms)",
                      std::max(gpu::SpanMs(gpu::kSpanField), 0.0f), std::max(gpu::SpanMs(gpu::kSpanHalo), 0.0f),
                      std::max(gpu::SpanMs(gpu::kSpanSurface), 0.0f), std::max(gpu::SpanMs(gpu::kSpanCopy), 0.0f),
                      std::max(gpu::SpanMs(-1), 0.0f));
        }
        ui::Textf("GPU: composite %.3f ms, resolve %.3f ms", std::max(gl::hdr::CompositeMs(), 0.0f), std::max(gl::hdr::ResolveMs(), 0.0f));
        if (const WXL_GraphicsShadowApi* shadow = gl::Shadow()) ui::Textf("shadows: %s (%.3f ms)", shadow->Status(), std::max(shadow->GpuMs(), 0.0f));
        else ui::Text("shadows: wxl-graphics-shadow is not loaded; every lamp is unshadowed");
    }

    void DebugTab()
    {
        TimersLine();
        ui::Separator();
        gl::surface::DebugPanel();
        ui::Separator();
        ui::Text("Light markers");
        OverlayTab();
        ui::Separator();
        ui::Text("Isolates: each removes one ingredient, to find which one causes an artefact.");
        int noRooms = gl::Settings().rooms ? 0 : 1;
        if (ui::Check("No room gating", &noRooms,
                      "Lights inside buildings reach every room in their radius again, through floors and ceilings, on surfaces and in the fog."))
            gl::Settings().rooms = noRooms == 0;
        int noMerge = gl::Settings().merge ? 0 : 1;
        if (ui::Check("No fixture merge", &noMerge,
                      "Every source of light on a lamp is kept apart again: the curated light, the game's model light and the building's light each count, and which one shows changes with the camera."))
            gl::Settings().merge = noMerge == 0;
        ui::Separator();
        ui::Text("Missiles and emitters");
        EffectsTab();
    }

    void __cdecl Panel(void*)
    {
        Guarded("panel", [] {
            ui::Scope scope("graphics-lights");
            if (!ui::BeginTabs("graphics-lights")) return;
            if (ui::BeginTab("Light"))       { ui::Scope s("light");   LightTab();           ui::EndTab(); }
            if (ui::BeginTab("Surfaces"))    { ui::Scope s("surfaces"); gl::surface::Panel(); ui::EndTab(); }
            if (ui::BeginTab("Air"))         { ui::Scope s("air");     gl::field::Panel();   ui::EndTab(); }
            if (ui::BeginTab("Gather"))      { ui::Scope s("gather");  GatherTab();          ui::EndTab(); }
            if (ui::BeginTab("Cookies"))     { ui::Scope s("cookies"); gl::cookies::Panel(); ui::EndTab(); }
            if (ui::BeginTab("Shadow maps")) { ui::Scope s("omni");    gl::omni::Panel();    ui::EndTab(); }
            if (ui::BeginTab("Debug"))       { ui::Scope s("debug");   DebugTab();           ui::EndTab(); }
            ui::EndTabs();
        });
    }

    class Overlay final : public wxl::ext::EventScript
    {
    public:
        Overlay()
        {
            on<&Overlay::OnWorldSceneEnd>(ev::Event::OnWorldSceneEnd);
            on<&Overlay::OnWorldRenderEnd>(ev::Event::OnWorldRenderEnd);
        }

        void OnWorldRenderEnd(const ev::WorldRenderEndArgs& a)
        {
            Guarded("cookie atlas view", [&] { gl::cookies::DrawDebug(static_cast<IDirect3DDevice9*>(a.device)); });
        }

        void OnWorldSceneEnd(const ev::WorldSceneEndArgs& a)
        {
            Guarded("light markers", [&] { Draw(a); });
        }

    private:
        void Draw(const ev::WorldSceneEndArgs& a)
        {
            // The panel's histogram is live whenever the panel is open, even with the overlay off.
            if (!g_cfg.enabled && !g_logNext && !g_cfg.logStats && !g_cfg.missiles && !g_cfg.emitters &&
                !g_logEffects && !gl::g_api->UiIsOpen()) return;

            lt::Query q;
            cam::GetPosition(q.center);
            q.radius         = g_cfg.radius;
            q.m2             = true;
            q.wmo            = g_cfg.wmo != 0;
            q.maxStaleFrames = uint32_t(g_cfg.staleFrames);
            g_engineFound = lt::Collect(q, g_engine, kCap, &g_stats);
            g_tableFound  = g_cfg.table
                ? gl::table::Collect(q.center, g_cfg.radius, uint32_t(g_cfg.staleFrames), g_table, kCap, &g_tableStats) : 0;

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
                const gl::Light& l = g_table[i];
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

namespace wxl::gfx::lights::ui
{
    void Install()
    {
        g_cfg.enabled       = ConfigBool("WXL_GFX_LIGHTS_OVERLAY", false) ? 1 : 0;
        g_cfg.wmo           = ConfigBool("WXL_GFX_LIGHTS_OVERLAY_WMO", false) ? 1 : 0;
        g_cfg.table         = ConfigBool("WXL_GFX_LIGHTS_OVERLAY_TABLE", false) ? 1 : 0;
        g_cfg.through       = ConfigBool("WXL_GFX_LIGHTS_OVERLAY_THROUGH", true) ? 1 : 0;
        g_cfg.radius        = ConfigFloat("WXL_GFX_LIGHTS_OVERLAY_RADIUS", g_cfg.radius, 10.0f, 600.0f);
        g_cfg.logStats      = ConfigBool("WXL_GFX_LIGHTS_OVERLAY_LOG_STATS", false) ? 1 : 0;
        g_cfg.missiles      = ConfigBool("WXL_GFX_LIGHTS_OVERLAY_MISSILES", false) ? 1 : 0;
        g_cfg.emitters      = ConfigBool("WXL_GFX_LIGHTS_OVERLAY_EMITTERS", false) ? 1 : 0;
        g_cfg.emitterRadius = ConfigFloat("WXL_GFX_LIGHTS_OVERLAY_EMITTER_RADIUS", g_cfg.emitterRadius, 5.0f, 200.0f);

        static Overlay overlay;
        g_api->UiAddPanel("Graphics Lights", &Panel, nullptr);
        LIGHTS_LOG_INFO("lights: panel registered (overlay %d, wmo %d, table %d, radius %.0f)", g_cfg.enabled, g_cfg.wmo,
                        g_cfg.table, g_cfg.radius);
    }
}
