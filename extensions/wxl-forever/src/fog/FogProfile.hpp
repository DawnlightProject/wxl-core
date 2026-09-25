// wxl-forever fog: fog profiles, their config form, and the resolver that blends them over time.
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

#pragma once

#include <cstdint>

// A profile is everything that describes one kind of fog. The resolver keeps one live profile per
// slot (outdoor, indoor); a provider names which profile each slot should show, and a change of name
// blends in over time while an edit of the same profile applies at once. Today the provider is the
// two built-ins; an area-keyed table plugs in at ChooseProfile without touching the draw.

// Every numeric knob of a profile, once: member, config key suffix, outdoor default, indoor default,
// panel minimum, panel maximum, panel label, the panel tab it shows in, and its help text. The
// struct, the blend, the config and the panel all expand from this list.
#define WXL_FOG_PROFILE_FLOATS(X) \
    X(density, "DENSITY", 0.015f, 0.02f, 0.0f, 0.1f, "Density", kTabVolume, \
      "How thick the fog is. Raise it for a wall of fog, lower it for a light haze.") \
    X(heightFalloff, "HEIGHT_FALLOFF", 0.06f, 0.0f, 0.0f, 0.5f, "Height falloff", kTabVolume, \
      "How fast the fog thins as it rises. Raise it to keep the fog low on the ground, set it to 0 for fog of even thickness at every height.") \
    X(baseZ, "BASE_Z", 0.0f, 0.0f, -1000.0f, 2000.0f, "Base height (world Z)", kTabVolume, \
      "The height where the fog is thickest when it does not follow the ground. Use the button to put it at your feet.") \
    X(maxDistance, "MAX_DISTANCE", 300.0f, 120.0f, 20.0f, 1000.0f, "Max distance", kTabVolume, \
      "How far the fog volume reaches from the camera. Lower it for sharper detail nearby, raise it for fog on distant hills.") \
    X(noiseScale, "NOISE_SCALE", 0.035f, 0.08f, 0.001f, 0.2f, "Noise scale", kTabVolume, \
      "The size of the fog's billows. Lower values give large rolling banks, higher values give small tufts.") \
    X(noiseStrength, "NOISE_STRENGTH", 0.85f, 0.5f, 0.0f, 1.0f, "Noise strength", kTabVolume, \
      "How patchy the fog is. At 0 it is an even haze, at 1 it breaks into clumps and clear gaps.") \
    X(windSpeed, "WIND_SPEED", 2.0f, 0.3f, 0.0f, 20.0f, "Wind speed", kTabVolume, \
      "How fast the fog drifts, in yards per second. Keep it low for a still, heavy fog.") \
    X(coverage, "COVERAGE", 0.3f, 0.1f, 0.0f, 0.95f, "Coverage (gaps)", kTabVolume, \
      "Opens holes in the fog. Raise it for separate banks with clear air between them, lower it for a continuous blanket.") \
    X(erosion, "EROSION", 0.45f, 0.3f, 0.0f, 1.0f, "Erosion (detail)", kTabVolume, \
      "Eats small detail into the edges of each clump. Raise it for wispy, torn edges, lower it for soft round ones.") \
    X(billow, "BILLOW", 0.5f, 0.5f, 0.0f, 1.0f, "Billow", kTabVolume, \
      "The shape of the clumps. Higher values give rounded cauliflower lumps, lower values give stretched streaks.") \
    X(warp, "WARP", 0.35f, 0.2f, 0.0f, 1.0f, "Domain warp", kTabVolume, \
      "Twists the fog's pattern. Raise it for swirling, curling shapes, lower it for a calmer, regular look.") \
    X(detailScale, "DETAIL_SCALE", 0.12f, 0.2f, 0.01f, 1.0f, "Detail scale", kTabVolume, \
      "The size of the small detail that erosion carves. Higher values give finer grain.") \
    X(groundFollow, "GROUND_FOLLOW", 0.8f, 0.0f, 0.0f, 1.0f, "Follow the ground", kTabVolume, \
      "How much the fog hugs the terrain instead of a flat height. At 1 it flows over hills and fills hollows.") \
    X(valleyPooling, "VALLEY_POOLING", 0.6f, 0.0f, 0.0f, 2.0f, "Valley pooling", kTabVolume, \
      "Extra fog in dips of the ground. Raise it for fog that settles in valleys and ditches.") \
    X(flowLayer, "FLOW_LAYER", 0.5f, 0.3f, 0.0f, 1.0f, "Second flow layer", kTabVolume, \
      "Mixes in a second, slower layer of fog moving at an angle, so the fog rolls instead of sliding. Set 0 to turn it off.") \
    X(flowSpeed, "FLOW_SPEED", 0.45f, 0.5f, 0.0f, 2.0f, "Second layer speed", kTabVolume, \
      "The speed of the second layer as a share of the wind. Different speeds make the fog churn.") \
    X(roll, "ROLL", 0.25f, 0.1f, 0.0f, 2.0f, "Vertical roll", kTabVolume, \
      "A slow upward drift of the second layer, like fog rising off wet ground.") \
    X(macroStrength, "MACRO_STRENGTH", 0.7f, 0.0f, 0.0f, 1.0f, "Fog banks", kTabVolume,       "Breaks the fog into separate banks across the landscape with clear gaps between them, drifting with the wind. Set 0 for fog everywhere.")     X(macroScale, "MACRO_SCALE", 180.0f, 120.0f, 40.0f, 600.0f, "Bank size", kTabVolume,       "The size of a fog bank, in yards. Larger values give wide banks and long gaps.")     X(macroContrast, "MACRO_CONTRAST", 2.5f, 2.0f, 0.5f, 8.0f, "Bank contrast", kTabVolume,       "How sharply a bank ends. Higher values give banks with a clear boundary, lower values let them melt into the gaps.")     X(macroCoverage, "MACRO_COVERAGE", 0.6f, 0.8f, 0.05f, 1.0f, "Bank coverage", kTabVolume,       "How much of the land the banks cover. At 1 there are almost no gaps, at low values banks are rare islands.")     X(macroDrift, "MACRO_DRIFT", 0.5f, 0.2f, 0.0f, 3.0f, "Bank drift", kTabVolume,       "How fast the banks travel, as a share of the wind speed. Keep it low so banks move slower than the wisps inside them.")     X(edgeErosion, "BANK_EDGE_EROSION", 0.6f, 0.3f, 0.0f, 1.0f, "Bank edge erosion", kTabVolume,       "Tears the boundary of each bank into wisps. Raise it for ragged, defined edges, lower it for soft ones.")     X(hazeDensity, "HAZE_DENSITY", 0.002f, 0.0f, 0.0f, 0.02f, "High haze", kTabVolume,       "A second, thin layer of haze higher up, added to the ground mist. It softens hillsides and distant trees. Set 0 to turn it off.")     X(hazeHeight, "HAZE_HEIGHT", 40.0f, 10.0f, 0.0f, 300.0f, "High haze height", kTabVolume,       "How high above the ground the haze layer sits, in yards.")     X(hazeFalloff, "HAZE_FALLOFF", 0.04f, 0.1f, 0.0f, 0.5f, "High haze falloff", kTabVolume,       "How fast the haze thins above and below its height. Lower values give a deep layer, higher values a thin sheet.")     X(hazeSpeed, "HAZE_SPEED", 1.5f, 0.5f, 0.0f, 5.0f, "High haze speed", kTabVolume,       "How fast the haze drifts, as a share of the wind speed.")     X(hazeNoise, "HAZE_NOISE", 0.5f, 0.3f, 0.0f, 1.0f, "High haze patchiness", kTabVolume,       "How uneven the haze is. At 0 it is a smooth sheet, at 1 it breaks into large patches.")     X(ambient, "AMBIENT", 1.0f, 1.0f, 0.0f, 2.0f, "Ambient", kTabLighting, \
      "The fog's own brightness from the sky and surroundings. Lower it for dark, gloomy fog.") \
    X(ambientLow, "AMBIENT_LOW", 0.7f, 0.9f, 0.0f, 2.0f, "Ambient at the ground", kTabLighting, \
      "Brightness near the floor of the fog layer. Lower it to darken the fog close to the ground.") \
    X(ambientHigh, "AMBIENT_HIGH", 1.2f, 1.0f, 0.0f, 2.0f, "Ambient at the top", kTabLighting, \
      "Brightness at the top of the fog layer, where it catches more sky.") \
    X(skyAmbient, "SKY_AMBIENT", 0.5f, 0.0f, 0.0f, 1.0f, "Sky-driven ambient", kTabLighting, \
      "How much the fog takes its colour from the sky above and the horizon. Set 0 to use only the fog colour.") \
    X(heat, "HEAT", 0.6f, 0.6f, 0.0f, 2.0f, "Heat clears the fog", kTabLighting, \
      "Flames (torches, braziers, campfires) thin the fog right around them as their heat lifts it: a clear pocket a few times the flame's size. Set 0 for none.")     X(cloudLight, "CLOUD_LIGHT", 0.0f, 0.0f, 0.0f, 1.0f, "Cloud light", kTabLighting,       "Clouds drift between the sun or moon and the fog: its glow comes and goes in large patches that move with the upper wind, so moonlit mist brightens where the sky opens. Set 0 for an even glow.")     X(cloudSize, "CLOUD_SIZE", 220.0f, 220.0f, 40.0f, 800.0f, "Cloud size (yards)", kTabLighting,       "How wide the patches of light and shade the clouds throw on the fog are.") \
    X(sunScatter, "SUN_SCATTER", 0.6f, 0.0f, 0.0f, 4.0f, "Sun scatter", kTabLighting, \
      "How brightly the fog glows around the sun. Lower it if looking toward the sun washes everything out.") \
    X(moonScatter, "MOON_SCATTER", 0.5f, 0.0f, 0.0f, 4.0f, "Moon scatter", kTabLighting, \
      "How brightly the fog glows around the moon at night.") \
    X(lightScatter, "LIGHT_SCATTER", 1.0f, 1.0f, 0.0f, 4.0f, "Light scatter (torches)", kTabLighting, \
      "How brightly lamps, torches and braziers light the fog around them.") \
    X(anisotropy, "ANISOTROPY", 0.6f, 0.3f, 0.0f, 0.95f, "Forward lobe g", kTabLighting, \
      "How tightly the glow gathers around the sun or moon. Higher values give a small bright halo, lower values spread it wide.") \
    X(backAnisotropy, "BACK_ANISOTROPY", -0.3f, -0.3f, -0.95f, 0.0f, "Back lobe g", kTabLighting, \
      "The faint glow seen with the sun behind you. More negative values focus it.") \
    X(lobeBlend, "LOBE_BLEND", 0.25f, 0.25f, 0.0f, 1.0f, "Back lobe share", kTabLighting, \
      "How much of the light goes into that backward glow instead of the forward halo.") \
    X(shadowStrength, "SHADOW", 1.0f, 0.0f, 0.0f, 4.0f, "Self-shadow", kTabLighting, \
      "How much the fog shades itself from the sun or moon. Raise it for darker cores and more depth.") \
    X(shadowDistance, "SHADOW_DISTANCE", 12.0f, 6.0f, 1.0f, 60.0f, "Self-shadow reach", kTabLighting, \
      "How far toward the light the fog looks for its own shadow, in yards. Longer reach gives broader shading.") \
    X(worldShadow, "WORLD_SHADOW", 1.0f, 1.0f, 0.0f, 1.0f, "World shadows", kTabLighting,       "How much trees, buildings and hills shade the fog from the sun or moon. At 1 fog in their shadow gets no direct light, so light falls in columns between trunks.")     X(worldShadowSoftness, "WORLD_SHADOW_SOFTNESS", 0.35f, 0.35f, 0.0f, 1.0f, "World shadow softness", kTabLighting,       "How soft the edges of those shadows are. Lower values give crisp light columns, higher values blur them into broad shade.")     X(skyOcclusion, "SKY_OCCLUSION", 0.7f, 0.0f, 0.0f, 1.0f, "Sky occlusion", kTabLighting,       "Darkens the fog's sky light under trees, between walls and at the bottom of hollows, where less sky is seen. Set 0 for even ambient light.")     X(bankShading, "BANK_SHADING", 0.6f, 0.0f, 0.0f, 1.0f, "Bank shading", kTabLighting,       "Darkens fog that has more fog piled above it, so the tops of banks stay bright and their undersides go dark.")     X(bankReach, "BANK_SHADOW_REACH", 40.0f, 0.0f, 0.0f, 120.0f, "Bank shadow reach", kTabLighting,       "How far the fog looks for whole banks shading it, in yards, beyond the self-shadow reach. Set 0 to turn bank shading and bank shadows off.")     X(powder, "POWDER", 0.3f, 0.0f, 0.0f, 1.0f, "Powder (dark edges)", kTabLighting, \
      "Darkens thin fog seen against the light, which makes clumps look solid. Lower it for softer edges.") \
    X(msStrength, "MS_STRENGTH", 0.35f, 0.3f, 0.0f, 1.0f, "Multiple scattering", kTabLighting, \
      "Light bouncing inside the fog. Raise it for soft, luminous fog, set 0 for a harder, greyer look.") \
    X(msExtinction, "MS_EXTINCTION", 0.5f, 0.5f, 0.1f, 1.0f, "Multiple scattering reach", kTabLighting, \
      "How deep bounced light gets into thick fog. Lower values let it glow further inside.") \
    X(msPhase, "MS_PHASE", 0.5f, 0.5f, 0.0f, 1.0f, "Multiple scattering focus", kTabLighting, \
      "How directional the bounced light stays. Lower values spread it evenly.") \
    X(msOctaves, "MS_OCTAVES", 2.0f, 2.0f, 0.0f, 3.0f, "Multiple scattering octaves", kTabLighting, \
      "How many bounces are simulated. More bounces look softer and cost a little more.") \
    X(maxScatter, "MAX_SCATTER", 3.0f, 3.0f, 0.1f, 16.0f, "Max scatter", kTabLighting, \
      "A cap on how bright any bit of fog can get. Lower it if bright spots blow out.") \
    X(exposure, "EXPOSURE", 1.0f, 1.0f, 0.0f, 4.0f, "Exposure", kTabLighting, \
      "Overall brightness of the fog's light. Lower it if the fog looks too bright.") \
    X(tonemapWhite, "TONEMAP_WHITE", 2.0f, 2.0f, 0.0f, 8.0f, "Tonemap white (<= 1 off)", kTabLighting, \
      "Softens very bright fog so it never turns flat white. Lower values compress highlights more, 1 or less turns it off.") \
    X(chroma, "CHROMA", 0.3f, 0.0f, 0.0f, 1.0f, "Chromatic extinction", kTabLighting, \
      "Lets distant fog take a colour cast, bluer or warmer by channel. Set 0 for neutral grey fog.") \
    X(desaturation, "AERIAL_DESATURATION", 0.25f, 0.0f, 0.0f, 1.0f, "Aerial desaturation", kTabLighting,       "Drains colour from what lies behind the fog, the more fog the more grey, so distance reads as depth. Set 0 to keep full colour.")     X(chromaR, "CHROMA_R", 0.9f, 1.0f, 0.5f, 1.5f, "Extinction tint red", kTabLighting, \
      "How strongly the fog absorbs red. Below 1 lets red through, making distance warmer.") \
    X(chromaG, "CHROMA_G", 1.0f, 1.0f, 0.5f, 1.5f, "Extinction tint green", kTabLighting, \
      "How strongly the fog absorbs green.") \
    X(chromaB, "CHROMA_B", 1.12f, 1.0f, 0.5f, 1.5f, "Extinction tint blue", kTabLighting, \
      "How strongly the fog absorbs blue. Above 1 makes distant fog bluer and hazier.") \
    X(nearStrength, "NEAR_STRENGTH", 1.0f, 0.8f, 0.0f, 2.0f, "Near wisps", kTabEffects, \
      "Finer curls in the fog close to the camera, part of the volume itself: the fog gathers into curls and gaps without getting thicker overall. Raise it for more visible curls around you and past tree trunks.") \
    X(nearDistance, "NEAR_DISTANCE", 18.0f, 10.0f, 2.0f, 40.0f, "Near wisp distance", kTabEffects, \
      "How far from the camera the curls reach, in yards; they fade out over the last part of it.") \
    X(nearScale, "NEAR_SCALE", 0.35f, 0.5f, 0.05f, 2.0f, "Near wisp scale", kTabEffects, \
      "The size of the wisps. Higher values give smaller, finer curls.") \
    X(nearTurbulence, "NEAR_TURBULENCE", 1.5f, 0.8f, 0.0f, 6.0f, "Near turbulence", kTabEffects, \
      "How fast the wisps churn. Lower it for a slow, heavy drift.") \
    X(contact, "CONTACT", 0.6f, 0.3f, 0.0f, 4.0f, "Surface contact", kTabEffects, \
      "A thin layer of fog on every surface, so walls and ground sit in the fog. Raise it for a damp, soaked look.") \
    X(contactThickness, "CONTACT_THICKNESS", 1.2f, 0.6f, 0.1f, 6.0f, "Contact thickness", kTabEffects, \
      "How thick that surface layer is, in yards.") \
    X(wakeStrength, "WAKE_STRENGTH", 0.85f, 0.85f, 0.0f, 1.0f, "Wake strength", kTabEffects, \
      "How much fog a body pushes away as it moves. At 1 people and creatures leave clear holes.") \
    X(wakeRadius, "WAKE_RADIUS", 0.9f, 0.8f, 0.2f, 4.0f, "Wake radius", kTabEffects, \
      "How wide the cleared space around a body is, in yards.") \
    X(wakeSwirl, "WAKE_SWIRL", 0.6f, 0.6f, 0.0f, 2.0f, "Wake swirl", kTabEffects, \
      "How much the fog churns as it closes behind a moving body.") \
    X(projectileCarve, "PROJECTILE_CARVE", 0.9f, 0.9f, 0.0f, 1.0f, "Projectile holes", kTabEffects, \
      "How much fog a spell, arrow or bullet clears along its path. At 1 the hole is clean, set 0 to turn trails off.") \
    X(projectileRadius, "PROJECTILE_RADIUS", 1.5f, 1.5f, 0.2f, 6.0f, "Hole size", kTabEffects, \
      "The hole's width as a share of the missile's own size. Raise it for wide holes behind small arrows.") \
    X(projectileHold, "PROJECTILE_HOLD", 0.45f, 0.45f, 0.0f, 3.0f, "Hole hold (s)", kTabEffects, \
      "How long a hole stays fully open before the fog starts to flow back in.") \
    X(projectileRefill, "PROJECTILE_REFILL", 4.0f, 4.0f, 0.5f, 20.0f, "Refill time (s)", kTabEffects, \
      "How long the fog takes to fill a hole back in, closing from its edges inwards.") \
    X(projectileHeat, "PROJECTILE_HEAT", 0.35f, 0.35f, 0.0f, 2.0f, "Fire widens the hole", kTabEffects, \
      "A fire spell's hole slowly grows wider while it refills, as its heat thins the fog. Set 0 for none.") \
    X(projectileCrater, "PROJECTILE_CRATER", 1.3f, 1.3f, 0.0f, 4.0f, "Crater size", kTabEffects, \
      "The size of the hole left where a missile lands, as a share of its trail. Set 0 for no crater.") \
    X(trailBillow, "TRAIL_BILLOW", 0.8f, 0.6f, 0.0f, 3.0f, "Refill billows", kTabEffects, \
      "Thick fog rolling in at the edge of a hole as it refills, like smoke curling back. Set 0 for a plain refill.") \
    X(trailBillowSize, "TRAIL_BILLOW_SIZE", 1.5f, 1.2f, 0.3f, 6.0f, "Billow size", kTabEffects, \
      "The size of those rolling billows, in yards. Larger billows read better from afar.") \
    X(trailBillowRoll, "TRAIL_BILLOW_ROLL", 0.6f, 0.6f, 0.0f, 4.0f, "Billow roll speed", kTabEffects, \
      "How fast the billows roll around the trail, in turns of about a sixth per second at 1.") \
    X(trailSmoke, "TRAIL_SMOKE", 0.5f, 0.5f, 0.0f, 1.0f, "Fire trail smoke", kTabEffects, \
      "How much darker and warmer the billows of a fire spell's trail are than the fog.") \
    X(smoke, "SMOKE", 1.0f, 1.0f, 0.0f, 4.0f, "Smoke plumes", kTabEffects, \
      "How thick the smoke and steam of chimneys, campfires and spells is in the fog. Set 0 to turn plumes off.") \
    X(fireSmoke, "FIRE_SMOKE", 0.3f, 0.2f, 0.0f, 2.0f, "Smoke above fires", kTabEffects, \
      "A faint plume of heat and smoke above fires that carry no smoke of their own. Set 0 for none.") \
    X(smokeDarkness, "SMOKE_DARKNESS", 0.6f, 0.6f, 0.0f, 1.0f, "Smoke darkness", kTabEffects, \
      "How much darker and browner smoke is than the fog around it. Steam always stays pale.") \
    X(immersion, "IMMERSION", 0.7f, 0.7f, 0.0f, 1.0f, "Immersion", kTabEffects, \
      "When you stand inside dense fog or smoke, a veil of the fog's own light closes in from the screen edges. Set 0 to turn it off.") \
    X(immersionDensity, "IMMERSION_DENSITY", 0.08f, 0.08f, 0.005f, 0.5f, "Immersion density", kTabEffects, \
      "How dense the fog around you must be for the full effect. Lower it to feel even light fog.") \
    X(immersionBloom, "IMMERSION_BLOOM", 0.3f, 0.3f, 0.0f, 1.0f, "Softened lights", kTabEffects, \
      "Bright lights blur into a soft glow while you are inside the fog.") \
    X(horizonHaze, "HORIZON_HAZE", 0.5f, 0.0f, 0.0f, 1.0f, "Horizon continuation", kTabEffects, \
      "How much the fog carries on past the volume's far distance, with the density and colour it has at its edge, thinning upwards like the rest of the layer: the horizon haze. At 1 the horizon is fully fogged, at 0 the fog stops at the far distance.")

namespace wxl::forever::fog
{
    /// The panel tab a profile knob shows in.
    enum ProfileTab : int { kTabVolume = 0, kTabLighting = 1, kTabEffects = 2 };

    enum class ColorMode : int { Native = 0, Custom = 1, Blend = 2 };

    struct FogProfile
    {
        uint32_t id = 0;   // identity for the resolver; a different id means a transition
#define WXL_FOG_MEMBER(name, key, out, in, lo, hi, label, tab, help) float name = out;
        WXL_FOG_PROFILE_FLOATS(WXL_FOG_MEMBER)
#undef WXL_FOG_MEMBER
        int   colorMode  = int(ColorMode::Native);
        float color[4]   = { 0.55f, 0.58f, 0.60f, 1.0f }; // custom colour, rgb 0..1 (a unused)
        float colorBlend = 0.5f;                           // Blend mode: 0 native .. 1 custom
        float brightness = 1.0f;                           // multiplies the fog colour
    };

    /// A profile reduced to what the shader consumes, with its colour resolved against the native fog.
    struct ResolvedProfile
    {
#define WXL_FOG_MEMBER(name, key, out, in, lo, hi, label, tab, help) float name;
        WXL_FOG_PROFILE_FLOATS(WXL_FOG_MEMBER)
#undef WXL_FOG_MEMBER
        float rgb[3];
    };

    enum class Slot : int { Outdoor = 0, Indoor = 1, Count = 2 };

    /// Built-in profiles, editable from the panel.
    FogProfile& Builtin(Slot slot);

    /// Reads a profile's knobs from config; prefix is e.g. "WXL_FOG_" or "WXL_FOG_INDOOR_".
    /// Returns true when the prefix supplied a base height.
    bool LoadProfile(const char* prefix, FogProfile& p);

    /// Advances every slot's transition and resolves it against the native fog colour (0xAARRGGBB).
    void ResolveProfiles(float dt, float transitionSeconds, uint32_t nativeArgb, ResolvedProfile out[2]);

    /// a at t = 0, b at t = 1, every knob and the colour in between.
    ResolvedProfile MixProfiles(const ResolvedProfile& a, const ResolvedProfile& b, float t);
}
