// wxl-graphics-shadow: the panel -- status, every setting with its "(?)", the isolates, the debug views
// and the GPU time of each pass.
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

#include "../Shadow.hpp"
#include "../core/Extension.hpp"
#include "../core/Settings.hpp"
#include "../gpu/Gpu.hpp"
#include "../gpu/Record.hpp"
#include "../policy/Bodies.hpp"
#include "../policy/Slots.hpp"
#include "../sun/Sun.hpp"
#include "../terrain/Caster.hpp"
#include "../terrain/Horizon.hpp"

#include "wxl/gfx/Ui.hpp"

#include <cstdio>

namespace
{
    namespace ui = wxl::gfx::ui;
    namespace sh = wxl::gfx::shadow;
    namespace sl = wxl::gfx::shadow::slots;

    void Isolate(const char* label, int bit, const char* help)
    {
        int on = sh::Isolated(bit) ? 1 : 0;
        if (ui::Check(label, &on, help))
        {
            if (on) sh::Config().isolate |= bit;
            else sh::Config().isolate &= ~bit;
        }
    }

    void StatusTab()
    {
        ui::Text(sh::StatusLine());
        ui::Text(sl::Status());
        ui::Text(sh::sun::Status());
        ui::Text(sh::horizon::Status());
        ui::Text(sh::caster::Status());
        ui::Textf("bodies: %d capsules near the player", sh::bodies::Count());
        ui::Separator();
        const sl::Slot* slots = sl::List();
        for (int i = 0; i < sl::kSlots; ++i)
        {
            const sl::Slot& s = slots[i];
            if (!s.id) continue;
            ui::Textf("slot %2d: light %08X%s, weight %.2f, %s%s, importance %.3f, %s", i, s.id,
                      (s.light.flags & WXL_GFX_SHADOW_LIGHT_CARRIED) ? " (carried)" : "", s.weight,
                      s.map >= 0 ? "map " : "capsules", s.map >= 0 ? (s.moving ? "moving" : "still") : "", s.light.importance,
                      s.leaving ? "leaving" : (s.contact ? "contact" : ""));
        }
        ui::Separator();
        const float total = sh::gpu::TotalMs();
        if (total < 0.0f) ui::Text("GPU time: not measured yet");
        else
        {
            ui::Textf("GPU time: %.3f ms in all", total);
            for (int i = 0; i < sh::gpu::kSpanCount; ++i)
                ui::Textf("  %-9s %.3f ms", sh::gpu::kSpanNames[i], sh::gpu::SpanMs(i) < 0.0f ? 0.0f : sh::gpu::SpanMs(i));
        }
    }

    void LampsTab()
    {
        sh::Settings& s = sh::Config();
        ui::Check("Shadow maps", &s.maps,
                  "Cube shadow maps for the most important lights: their post, fences, walls and every passer-by cast soft shadows on surfaces and in the fog. Off: every lamp shadows with body capsules only.");
        ui::Slider("Still lamps with maps", &s.stillMaps, 0, 4,
                   "How many still lamps hold a map. Their still surroundings are drawn once and kept; only moving bodies are drawn again, on the faces that see one.");
        ui::Slider("Moving lights with maps", &s.movingMaps, 0, 2,
                   "How many moving or carried lights hold a map (the player's own torch first). Each redraws all six faces with every caster each frame, the costliest kind.");
        ui::Slider("Own housing (yards)", &s.housing, 0.0f, 1.5f,
                   "Casters this near a lamp (its own cage, post and bracket) cast nothing, so a lamp does not throw its own fixture across the street. Its contact shadow skips them too, so a wall just behind a lantern shows no copy of the lantern's frame.");
        ui::Slider("Carried item housing (yards)", &s.carriedHousing, 0.0f, 1.0f,
                   "The same for a carried light: the torch itself and the hand holding it cast nothing, in its map and in its contact shadow.");
        ui::Slider("Carrier margin (yards)", &s.carrierMargin, 0.0f, 0.5f,
                   "A carried light never shadows its own carrier: surfaces inside the carrier's body capsule, widened by this, are lit by it. The carrier's shadow still falls on everything else.");
        ui::Slider("Softness", &s.softness, 0.0f, 4.0f,
                   "How much the penumbra widens per yard of the light's source size, as the receiver moves away from the light.");
        ui::Slider("Blur (mip levels)", &s.lodBias, -1.0f, 3.0f, "Extra blur on every map lookup, in mip levels. Negative sharpens.");
        ui::Slider("Light bleeding reduction", &s.bleed, 0.0f, 0.9f,
                   "Cuts the faint light that leaks through where several casters overlap in depth (an EVSM trait). Too high darkens penumbrae.");
        ui::Slider("Minimum variance", &s.minVariance, 0.0f, 0.005f,
                   "Smooths the filtered maps near their casters. Raise it if a surface shows faint banding in its own shadow.");
        ui::Slider("Positive exponent", &s.evsmPositive, 5.0f, 42.0f,
                   "The EVSM warp's steepness: higher darkens contact shadows and reduces leaks, too high overflows. Changing it converts every map again.");
        ui::Slider("Negative exponent", &s.evsmNegative, 1.0f, 42.0f, "The second warp, which removes most leaks behind thin casters. Changing it converts every map again.");
        ui::Slider("Normal offset (texels)", &s.normalOffset, 0.0f, 4.0f,
                   "Receivers are moved off their surface by this many texels of the map at their distance, more at grazing light. Removes acne without moving the shadow away from its caster.");
        ui::Slider("Slope bias (texels)", &s.slopeBias, 0.0f, 4.0f, "A depth bias in texels of the map, growing on slopes away from the light.");
        ui::Slider("Static refresh (faces per frame)", &s.refresh, 1, 6,
                   "When the engine's still casters change around the player, the still lamps' faces are redrawn this many a frame, in turn.");
        ui::Separator();
        ui::Check("Body capsules", &s.capsules,
                  "Bodies (the player, NPCs, creatures) cut the beam of lamps without a map, tested along the whole segment from each surface to the light, with a penumbra that widens along it.");
        ui::Slider("Capsule range (yards)", &s.capsuleRange, 10.0f, 120.0f,
                   "Bodies this near the player (the camera without one) take part: the 32 nearest the player, so turning the camera changes nothing. A listed body stays listed unless a newcomer is clearly nearer, and each body's shadow fades in and out over 0.3 s.");
        ui::Slider("Capsule radius (per yard of height)", &s.capsuleRadius, 0.1f, 0.4f, "How thick a body's capsule is for its height.");
        ui::Slider("Capsule penumbra", &s.capsulePenumbra, 0.0f, 4.0f, "How soft a capsule's shadow grows with the light's source size.");
        ui::Separator();
        ui::Slider("Slot hold (seconds)", &s.slotHold, 0.0f, 5.0f,
                   "A light keeps its shadow slot at least this long, so slots never flap. Lights are ranked by their brightness and their distance to the player, not to the camera, so turning the camera hands nothing over.");
        ui::Slider("Map hold (seconds)", &s.mapHold, 0.0f, 5.0f,
                   "A light keeps its shadow map at least this long once it has one, so maps never flap between lamps of like importance. A lamp that starts or stops moving still changes its kind of map.");
        ui::Slider("Fade (seconds)", &s.slotFade, 0.05f, 2.0f, "How long a light's shadow, its map or its contact shadow takes to fade in or out.");
        ui::Slider("Slot margin", &s.slotMargin, 1.0f, 3.0f,
                   "How much more important a light must be than a holder to take its slot, its map or its contact shadow.");
    }

    void SunTab()
    {
        sh::Settings& s = sh::Config();
        ui::Check("Sun and moon", &s.sun, "Shadows from the sun by day and the moon by night.");
        ui::Check("Read the engine's cascades", &s.cascades,
                  "The engine's own sun shadow maps, read with a tent of bilinear comparisons whose width in yards is the same in every cascade, so nothing crawls as the camera moves.");
        ui::Check("Lower, truer sun", &s.lowSun,
                  "The cascades are rendered along the sun (or the moon) lifted by the factor below instead of the engine's five times, so dusk shadows stretch.");
        ui::Check("Moon shadows", &s.moon, "At night the cascades follow the moon, and surfaces get moon shadows.");
        ui::Slider("Height factor", &s.sunLift, 0.5f, 5.0f, "The body's height is multiplied by this before the cascades render along it. The engine uses 5.");
        ui::Slider("Minimum elevation (degrees)", &s.sunMinElevation, 2.0f, 60.0f,
                   "The lowest the shadow light goes. Lower gives longer dusk shadows; the engine's own floor is 50 degrees.");
        ui::Slider("Filter width (texels)", &s.cascadeFilter, 0.5f, 2.0f, "The tent's spacing in texels of the finest cascade: wider is softer.");
        ui::Slider("Dusk softness", &s.duskSoftness, 0.0f, 2.0f, "How much the filter widens as the body nears the horizon.");
        ui::Slider("Extra bias (yards)", &s.cascadeBias, 0.0f, 2.0f, "Extra depth bias on the cascades, on top of the engine's own.");
        ui::Separator();
        ui::Check("Terrain in the cascades", &s.terrainCaster,
                  "Draws the land into the engine's cascades, so hills shadow objects, the ground and the fog's shafts near the player.");
        ui::Slider("Terrain cascades", &s.casterCascades, 1, 4, "How many cascades receive the terrain: 1 the finest only, 4 every one.");
        ui::Slider("Terrain bias (yards)", &s.casterBias, 0.0f, 4.0f, "The terrain is pushed along the light by this, against self-shadowing on its own slopes.");
        ui::Slider("Terrain drop (yards)", &s.casterDrop, 0.0f, 4.0f,
                   "The terrain mesh is lowered by this per 4.17 yd of its cells (twice as much in a cascade drawn at twice the step). Its cells only follow the ground's corners, so where the ground dips between them the mesh sat above it and shadowed it in squares; under a low sun or moon, the bias along the light hardly lowers it at all.");
        ui::Slider("Terrain detail", &s.casterDetail, -1, 1, "-1 finer mesh, 0 the cascade's own step, 1 coarser.");
        ui::Check("Horizon maps", &s.horizon, "The baked terrain horizon: the land's shadow at any distance and sun angle, beyond the cascades too.");
        ui::Slider("Horizon strength", &s.horizonStrength, 0.0f, 1.0f, "How dark the terrain's own shadow is.");
        ui::Slider("Horizon penumbra (degrees)", &s.horizonPenumbra, 0.1f, 15.0f, "How far above and below the horizon the terrain's shadow edge ramps.");
    }

    void ContactTab()
    {
        sh::Settings& s = sh::Config();
        ui::Check("Contact shadows", &s.contact,
                  "Short shadows from the depth buffer that ground feet, stones and grass. Fixed steps, no noise, and a surface never shadows itself.");
        ui::Slider("Towards the sun (yards)", &s.contactSun, 0.0f, 4.0f, "How far the march goes towards the sun or the moon.");
        ui::Slider("Lamps with one", &s.contactSlots, 0, 4,
                   "The most important lamps that also get contact shadows. A lamp keeps its contact shadow unless another beats it by the slot margin, and it fades in and out with the slot fade.");
        ui::Slider("Towards a lamp (yards)", &s.contactLamp, 0.0f, 3.0f,
                   "How far the march goes towards a lamp at most (never more than half the way). The lamp's own fixture, within its housing, blocks nothing.");
        ui::Slider("Thickness (yards)", &s.contactThickness, 0.1f, 2.0f,
                   "How thick an object seen in the depth buffer is assumed to be: thinner lets light pass behind poles and legs.");
        ui::Slider("Strength", &s.contactStrength, 0.0f, 1.0f, "How dark contact shadows get.");
        ui::Check("Half resolution", &s.halfRes,
                  "Traces a quarter of the pixels and brings them up with a depth-aware filter. Cheaper, slightly softer at silhouettes.");
    }

    void DebugTab()
    {
        sh::Settings& s = sh::Config();
        static const char* const kViews[] = { "None", "Sun mask", "Moon mask", "One slot's mask", "Every slot", "Contact",
                                              "Terrain horizon", "Cascades", "Body capsules", "Map atlas", "Normals" };
        ui::Combo("Debug view", &s.view, kViews, sh::kViewCount,
                  "Draws one ingredient over the finished frame: masks in grey (white lit), every slot in its colour, which cascade each pixel reads, the capsules in red, the filtered maps, the normals the pass uses.");
        ui::Slider("Slot to show", &s.viewSlot, 0, WXL_GFX_SHADOW_SLOTS - 1, "The slot the One slot's mask view shows (the Status tab lists them).");
        ui::Separator();
        Isolate("No maps", sh::kIsoNoMaps, "Every lamp shadows with capsules only.");
        Isolate("No capsules", sh::kIsoNoCapsules, "Lamps without a map cast no body shadow.");
        Isolate("No contact", sh::kIsoNoContact, "No contact shadows.");
        Isolate("No cascades", sh::kIsoNoCascades, "The sun and the moon ignore the engine's cascades.");
        Isolate("No terrain horizon", sh::kIsoNoTerrain, "The sun and the moon ignore the baked horizon.");
        Isolate("No terrain caster", sh::kIsoNoCaster, "The land is not drawn into the cascades.");
        Isolate("No fades", sh::kIsoNoFade, "Slots and maps switch at once (to see what the fades hide).");
        ui::Separator();
        ui::Check("GPU timers", &s.timers, "Measures each pass with Vulkan timestamps (Status tab).");
        if (ui::Button("Convert every map again", "Throws away the filtered maps and builds them again from the core's maps."))
            sh::gpu::ResetMaps();
    }
}

namespace wxl::gfx::shadow
{
    void Panel()
    {
        Settings& s = Config();
        ui::Scope scope("shadow");
        ui::Check("Enabled", &s.enabled, "Every shadow of this extension. Off, surfaces and the fog are unshadowed by it and the engine keeps its own.");
        if (!ui::BeginTabs("shadowTabs")) return;
        if (ui::BeginTab("Status")) { StatusTab(); ui::EndTab(); }
        if (ui::BeginTab("Lamps")) { LampsTab(); ui::EndTab(); }
        if (ui::BeginTab("Sun and moon")) { SunTab(); ui::EndTab(); }
        if (ui::BeginTab("Contact")) { ContactTab(); ui::EndTab(); }
        if (ui::BeginTab("Debug")) { DebugTab(); ui::EndTab(); }
        ui::EndTabs();
    }
}
