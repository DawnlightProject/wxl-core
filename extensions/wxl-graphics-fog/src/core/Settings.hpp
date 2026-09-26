// wxl-graphics-fog: every setting -- the two profiles (outdoor WXL_FOG_*, indoor WXL_FOG_INDOOR_*),
// the simulation, the render and the switches -- with the config keys, defaults, ranges, panel labels
// and help texts in one place.
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

namespace wxl::gfx::fog
{
    enum class ColorMode : int { Native = 0, Custom = 1, Blend = 2 };

    /// The panel tab a profile knob is shown on.
    enum ProfileTab : int { kTabFog = 0, kTabShape, kTabMotion, kTabLight, kTabEffects, kTabAir, kTabCascade };

    // X(field, KEY, default, min, max, label, tab, help). The key is read as WXL_FOG_<KEY> (outdoor).
#define WXL_FOG_OUTDOOR_FLOATS(X)                                                                                       \
    X(density,         "DENSITY",          0.14f,  0.0f, 0.8f,  "Layer density",        kTabFog,                       \
      "Extinction per yard inside the terrain layer: the rivers, the pools, the fog lying in hollows. The main thickness of the fog.") \
    X(groundDensity,   "GROUND_DENSITY",   0.018f, 0.0f, 0.3f,  "Ground mist density",  kTabFog,                       \
      "A thin mist on the ground everywhere, even where no river runs.")                                                  \
    X(groundHeight,    "GROUND_HEIGHT",    5.0f,   0.3f, 40.0f, "Ground mist height",   kTabFog,                       \
      "How high the ground mist reaches, in yards: it halves roughly every 0.7 times this height.")                         \
    X(climb,           "CLIMB",            4.0f,   0.0f, 60.0f, "Fog climb",            kTabFog,                       \
      "How high the fog rises above the lying layer where that layer is deep, in yards: soft, tall, billowing tops. Shallow fog climbs less. 0 keeps a flat slab.") \
    X(minDepth,        "MIN_DEPTH",        0.0f,   0.0f, 6.0f,  "Least lying depth",    kTabFog,                       \
      "Yards of fog lying on open ground at night at the least, even between the rivers; a share of it by day (the rivers' day formation). 0 leaves the ground to the rivers.") \
    X(hazeDensity,     "HAZE_DENSITY",     0.0012f,0.0f, 0.02f, "Haze density",         kTabFog,                       \
      "The haze that fills the air all the way to the horizon, per yard. It is what makes distant hills fade.")             \
    X(hazeHeight,      "HAZE_HEIGHT",      30.0f,  0.0f, 400.0f,"Haze height",          kTabFog,                       \
      "Yards above the ground under the camera where the haze starts to thin.")                                             \
    X(hazeFalloff,     "HAZE_FALLOFF",     0.012f, 0.0f, 0.2f,  "Haze falloff",         kTabFog,                       \
      "How fast the haze thins above its height, per yard. Low values keep hills hazy up to their tops.")                   \
    X(bankCoverage,    "BANK_COVERAGE",    0.72f,  0.0f, 1.0f,  "Bank coverage",        kTabFog,                       \
      "Share of the land under fog banks. Lower values open wide clear gaps between banks.")                                \
    X(bankStrength,    "BANK_STRENGTH",    0.5f,   0.0f, 1.0f,  "Bank strength",        kTabFog,                       \
      "How much the banks and gaps shape the fog. 0 spreads it evenly.")                                                    \
    X(bankScale,       "BANK_SCALE",       320.0f, 40.0f, 2000.0f,"Bank size",          kTabFog,                       \
      "Size of the drifting fog banks, in yards.")                                                                           \
    X(shapeScale,      "SHAPE_SCALE",      3.0f,   0.5f, 20.0f, "Billow size",          kTabShape,                     \
      "Size of the fog's billows and lumps, in yards. Larger ones cascade down to finer ones.")                             \
    X(shapeContrast,   "SHAPE_CONTRAST",   0.85f,  0.0f, 1.0f,  "Billow contrast",      kTabShape,                     \
      "How strongly the billows shape thin fog: holes, wisps and tendrils where it is thin. 0 gives smooth fog.")           \
    X(denseContrast,   "DENSE_CONTRAST",   0.35f,  0.0f, 1.0f,  "Core lumpiness",       kTabShape,                     \
      "How lumpy the fog stays where it is thick. Low values give full, even cores.")                                       \
    X(topSoftness,     "TOP_SOFTNESS",     1.2f,   0.1f, 8.0f,  "Top softness",         kTabShape,                     \
      "How soft the top of the lying fog is. Low values give a sharp surface, like a lake of fog.")                         \
    X(topBillow,       "TOP_BILLOW",       0.6f,   0.0f, 2.0f,  "Top billows",          kTabShape,                     \
      "How much the top of the lying fog heaves in rounded billows, as a share of its depth.")                              \
    X(detailScale,     "DETAIL_SCALE",     0.7f,   0.1f, 5.0f,  "Wisp size",            kTabShape,                     \
      "Size of the fine wisps that fray the fog's edges near you, in yards.")                                               \
    X(erosion,         "EROSION",          0.6f,   0.0f, 1.0f,  "Fraying",              kTabShape,                     \
      "How much the fine wisps eat the thin edges of the fog near you. Thick cores are left whole.")                        \
    X(curl,            "CURL",             0.35f,  0.0f, 2.0f,  "Wisp curl",            kTabShape,                     \
      "How much the wisps curl and twist.")                                                                                  \
    X(renewal,         "RENEWAL",          8.0f,   1.0f, 60.0f, "Renewal time",         kTabMotion,                    \
      "Seconds the fog takes to grow back where something carved it, and how long a billow lives before a new one replaces it. Longer keeps what the flow carries longer.") \
    X(windSpeed,       "WIND_SPEED",       1.0f,   0.0f, 15.0f, "Wind speed",           kTabMotion,                    \
      "Yards per second at 40 yards up. Near the ground and inside deep fog it is calmer.")                                 \
    X(windDirection,   "WIND_DIRECTION",   200.0f, 0.0f, 360.0f,"Wind heading",         kTabMotion,                    \
      "Where the wind blows towards, in degrees clockwise from north.")                                                     \
    X(turbulence,      "TURBULENCE",       0.45f,  0.0f, 4.0f,  "Turbulence",           kTabMotion,                    \
      "Swirling motion inside the fog, in yards per second, strongest at the top of the lying fog.")                        \
    X(layerFlow,       "LAYER_FLOW",       1.0f,   0.0f, 3.0f,  "River flow",           kTabMotion,                    \
      "How much of the terrain layer's flow moves the fog: 1 carries the billows down the rivers at their own speed.")      \
    X(ambient,         "AMBIENT",          1.0f,   0.0f, 4.0f,  "Sky light",            kTabLight,                     \
      "Strength of the light the sky gives the fog.")                                                                        \
    X(ambientFloor,    "AMBIENT_FLOOR",    0.12f,  0.0f, 1.0f,  "Sky light floor",      kTabLight,                     \
      "The least sky light any fog receives, however deep in a hollow or under thick fog.")                                 \
    X(valleyDarkening, "VALLEY_DARKENING", 1.5f,   0.0f, 4.0f,  "Valley darkening",     kTabLight,                     \
      "How much darker the fog is in hollows and valleys, where less sky is seen.")                                          \
    X(zoneTint,        "ZONE_TINT",        0.5f,   0.0f, 1.0f,  "Zone light tint",      kTabLight,                     \
      "How much the zone's own fog colour tints the sky light, following the time of day.")                                 \
    X(sunScatter,      "SUN_SCATTER",      1.0f,   0.0f, 4.0f,  "Sunlight",             kTabLight,                     \
      "Strength of the sunlight scattered by the fog.")                                                                      \
    X(moonScatter,     "MOON_SCATTER",     1.0f,   0.0f, 4.0f,  "Moonlight",            kTabLight,                     \
      "Strength of the moonlight scattered by the fog. Lower it for darker nights.")                                        \
    X(lampScatter,     "LAMP_SCATTER",     1.6f,   0.0f, 8.0f,  "Lamp glow",            kTabLight,                     \
      "Strength of the halos lamps, torches and fires make in the fog.")                                                     \
    X(bodyShadow,      "BODY_SHADOW",      0.85f,  0.0f, 1.0f,  "Body shadows",         kTabLight,                     \
      "How dark the bodies near you (you, zombies, passers-by) make the fog behind them, lit by a torch or a lamp that has no shadow map of its own. A carried torch throws its carrier's shadow into the fog.") \
    X(anisotropy,      "ANISOTROPY",       0.55f,  0.0f, 0.95f, "Forward glow",         kTabLight,                     \
      "How much brighter the fog is looking towards the sun, the moon or a lamp.")                                          \
    X(backAnisotropy,  "BACK_ANISOTROPY",  -0.2f,  -0.9f, 0.0f, "Back glow",            kTabLight,                     \
      "The softer glow the fog gives back towards the light.")                                                               \
    X(lobeBlend,       "LOBE_BLEND",       0.25f,  0.0f, 1.0f,  "Back glow share",      kTabLight,                     \
      "Share of the back glow against the forward glow.")                                                                    \
    X(powder,          "POWDER",           0.4f,   0.0f, 1.0f,  "Powder",               kTabLight,                     \
      "Thin fog seen with the light behind you is darker at its edges, as real cloud is. Gives the billows their shape.")   \
    X(msStrength,      "MS_STRENGTH",      0.8f,   0.0f, 2.0f,  "Multiple scattering",  kTabLight,                     \
      "Light bouncing many times inside thick fog: it glows through instead of going black.")                               \
    X(selfShadow,      "SELF_SHADOW",      1.0f,   0.0f, 4.0f,  "Self-shadow",          kTabLight,                     \
      "How much the fog shades itself from the sun and moon. Deep pools get dark bellies and bright tops.")                 \
    X(skyShadow,       "SKY_SHADOW",       0.5f,   0.0f, 4.0f,  "Sky shadow",           kTabLight,                     \
      "How much fog overhead dims the sky light below it. Deep pools grow dim inside, never black.")                                                                  \
    X(exposure,        "EXPOSURE",         1.0f,   0.1f, 4.0f,  "Fog brightness",       kTabLight,                     \
      "Scales all the light the fog scatters.")                                                                              \
    X(softWhite,       "SOFT_WHITE",       2.0f,   0.5f, 16.0f, "Glow ceiling",         kTabLight,                     \
      "How bright the fog's glow may get before it is softened. Low values keep bright glows from burning out.")            \
    X(brightness,      "BRIGHTNESS",       1.0f,   0.0f, 4.0f,  "Colour brightness",    kTabLight,                     \
      "Multiplies the fog's colour. Lower it for darker fog without changing its thickness.")                               \
    X(colorBlend,      "COLOR_BLEND",      0.5f,   0.0f, 1.0f,  "Colour blend",         kTabLight,                     \
      "In Blend mode: 0 takes the zone's colour, 1 yours.")                                                                  \
    X(smokeDensity,    "SMOKE",            1.0f,   0.0f, 5.0f,  "Smoke",                kTabEffects,                   \
      "Density of the smoke chimneys, campfires and spells put into the fog.")                                              \
    X(fireSmoke,       "FIRE_SMOKE",       0.4f,   0.0f, 3.0f,  "Fire smoke",           kTabEffects,                   \
      "Smoke a fire adds above itself when its model carries none.")                                                         \
    X(smokeDarkness,   "SMOKE_DARKNESS",   0.6f,   0.0f, 1.0f,  "Smoke darkness",       kTabEffects,                   \
      "How much darker and browner smoke is than fog.")                                                                      \
    X(heat,            "HEAT",             0.6f,   0.0f, 3.0f,  "Heat",                 kTabEffects,                   \
      "How much fires thin the fog around them.")                                                                            \
    X(wakeStrength,    "WAKE_STRENGTH",    0.9f,   0.0f, 1.0f,  "Wake strength",        kTabEffects,                   \
      "How much of the fog a body clears as it passes: the hole the flow then carries and fills back in.")             \
    X(wakeRadius,      "WAKE_RADIUS",      1.0f,   0.2f, 4.0f,  "Wake radius",          kTabEffects,                   \
      "The size of the space a body clears, as a share of its own size.")                                              \
    X(wakePush,        "WAKE_PUSH",        1.5f,   0.0f, 8.0f,  "Wake push",            kTabEffects,                   \
      "How strongly the air the bodies stir moves the whole fog around them, not only near the ground.")               \
    X(wakeVorticity,   "WAKE_VORTICITY",   0.6f,   0.0f, 3.0f,  "Wake eddies",          kTabEffects,                   \
      "How much the small eddies at a wake's edges keep curling. 0 leaves a smooth wake.")                             \
    X(wakeDamping,     "WAKE_DAMPING",     0.5f,   0.0f, 5.0f,  "Wake calming",         kTabEffects,                   \
      "How fast the air stirred by bodies comes to rest, per second. Lower keeps wakes swirling longer.")              \
    X(wakeRefill,      "WAKE_REFILL",      12.0f,  0.5f, 120.0f, "Wake settling",       kTabEffects,                   \
      "Seconds for a hole to close by itself where no flow brings the fog back. The flow usually closes it sooner.")   \
    X(wakeHeight,      "WAKE_HEIGHT",      0.8f,   0.0f, 6.0f,  "Wake height",          kTabEffects,                   \
      "Yards over a body's head its wake reaches.")                                                                    \
    X(shockStrength,   "SHOCK",            1.0f,   0.0f, 3.0f,  "Blast shock",          kTabEffects,                   \
      "How hard impacts and area spells blow the fog out in a ring that then flows back.")                             \
    X(fireEvaporation, "FIRE_EVAPORATION", 1.0f,   0.0f, 4.0f,  "Fire evaporation",     kTabEffects,                   \
      "How fast fire spells and flames burn the fog away around them.")                                                \
    X(frostFog,        "FROST_FOG",        0.6f,   0.0f, 3.0f,  "Frost fog",            kTabEffects,                   \
      "How much fog frost spells and icy effects leave behind them.")                                                  \
    X(torchClear,      "TORCH_CLEAR",      0.85f,  0.0f, 1.0f,  "Torch pocket",         kTabEffects,                   \
      "A carried torch or lantern pushes the fog back around its bearer and keeps it from forming there: how much. 0 turns it off.") \
    X(torchRadius,     "TORCH_RADIUS",     0.45f,  0.1f, 1.0f,  "Pocket size",          kTabEffects,                   \
      "The pocket's radius as a share of the light's reach.")                                                          \
    X(torchFloor,      "TORCH_FLOOR",      0.01f,  0.0f, 0.1f,  "Pocket mist",          kTabEffects,                   \
      "The thin mist left on the ground inside the pocket, per yard. 0 clears it entirely.")                           \
    X(torchRecovery,   "TORCH_RECOVERY",   8.0f,   1.0f, 60.0f, "Pocket recovery",      kTabEffects,                   \
      "Seconds the fog takes to settle back once the torch is put away (it also flows back in).")                      \
    X(projectileCarve, "PROJECTILE_CARVE", 0.95f,  0.0f, 1.0f,  "Projectile carve",     kTabEffects,                   \
      "How much of the fog a missile's tunnel clears.")                                                                      \
    X(projectileRadius,"PROJECTILE_RADIUS",1.0f,   0.2f, 4.0f,  "Projectile radius",    kTabEffects,                   \
      "Scale on the tunnel's width.")                                                                                        \
    X(projectileHold,  "PROJECTILE_HOLD",  0.5f,   0.0f, 5.0f,  "Tunnel hold",          kTabEffects,                   \
      "Seconds a tunnel stays open before the fog rolls back in.")                                                          \
    X(projectileRefill,"PROJECTILE_REFILL",5.0f,   0.5f, 30.0f, "Tunnel refill",        kTabEffects,                   \
      "Seconds the fog takes to roll back into a tunnel.")                                                                  \
    X(trailBillow,     "TRAIL_BILLOW",     1.0f,   0.0f, 3.0f,  "Refill billows",       kTabEffects,                   \
      "How thick and lumpy the fog rolling back into a tunnel is.")                                                          \
    X(immersion,       "IMMERSION",        0.6f,   0.0f, 1.0f,  "Immersion",            kTabEffects,                   \
      "Standing inside thick fog: its own colour closes in from the screen's edges.")                                       \
    X(immersionBlur,   "IMMERSION_BLUR",   0.5f,   0.0f, 1.0f,  "Immersion blur",       kTabEffects,                   \
      "Standing inside thick fog: the world softens a little.")                                                         \
    X(airDensity,      "AIR_DENSITY",      0.003f, 0.0f, 0.1f,  "Air density",          kTabAir,                       \
      "Extinction per yard of the atmosphere at its base: a smooth haze that fills the whole air column, above and beyond the lying fog. 0 turns it off.") \
    X(airHeight,       "AIR_HEIGHT",       40.0f,  2.0f, 500.0f, "Air height",          kTabAir,                       \
      "Yards over its base in which the atmosphere thins to a third. Larger fills more of the sky.")                    \
    X(airBase,         "AIR_BASE",         0.0f,   -20.0f, 200.0f, "Air base",          kTabAir,                       \
      "Yards over the ground up to which the atmosphere keeps its full density.")                                       \
    X(airFollow,       "AIR_FOLLOW",       0.5f,   0.0f, 1.0f,  "Air follows terrain",  kTabAir,                       \
      "0 measures its height from the ground under you, so hills rise out of it; 1 from the terrain everywhere, so it blankets hills and valleys alike.") \
    X(airNoise,        "AIR_NOISE",        0.35f,  0.0f, 1.0f,  "Air variation",        kTabAir,                       \
      "How much the atmosphere thickens and thins in large, slow patches that drift with the wind.")                    \
    X(airNoiseScale,   "AIR_NOISE_SCALE",  180.0f, 20.0f, 2000.0f, "Air patch size",    kTabAir,                       \
      "Size of those patches, in yards.")                                                                               \
    X(airColorShare,   "AIR_COLOR_SHARE",  0.5f,   0.0f, 1.0f,  "Air colour share",     kTabAir,                       \
      "How much the atmosphere takes the fog's colour. 0 leaves it neutral, coloured only by the sky and the lights.")  \
    X(airAnisotropy,   "AIR_ANISOTROPY",   0.35f,  0.0f, 0.95f, "Air forward glow",     kTabAir,                       \
      "How much brighter the atmosphere is looking towards the sun, the moon or a lamp.")                               \
    X(airSun,          "AIR_SUN",          1.0f,   0.0f, 4.0f,  "Air sunlight",         kTabAir,                       \
      "Sunlight the atmosphere scatters: shafts between trees and buildings, a glow towards the sun.")                  \
    X(airMoon,         "AIR_MOON",         1.0f,   0.0f, 4.0f,  "Air moonlight",        kTabAir,                       \
      "Moonlight the atmosphere scatters at night.")                                                                    \
    X(airSky,          "AIR_SKY",          1.0f,   0.0f, 4.0f,  "Air sky light",        kTabAir,                       \
      "The sky's light in the atmosphere: how bright the haze is in the distance.")                                     \
    X(airLamp,         "AIR_LAMP",         1.0f,   0.0f, 8.0f,  "Air lamp glow",        kTabAir,                       \
      "Halos lamps, torches and fires make in the atmosphere, even where no fog lies.")                                \
    X(cascadeStrength, "CASCADE_STRENGTH", 1.0f,   0.0f, 3.0f,  "Cascade banks",        kTabCascade,                   \
      "How much fog the banks behind crests hold, from the baked map of spill points. 0 turns the automatic cascades off.") \
    X(cascadeDepth,    "CASCADE_DEPTH",    12.0f,  1.0f, 60.0f, "Bank depth",           kTabCascade,                   \
      "Yards deep a full bank lies behind its crest: the deeper, the more pours over.")                                \
    X(cascadeThreshold, "CASCADE_THRESHOLD", 0.3f,   0.0f, 0.95f, "Spill threshold",    kTabCascade,                   \
      "How clear a spill point must be to hold a bank. Lower gives more cascades, on lesser crests too.")              \
    X(cascadeWind,     "CASCADE_WIND",     0.6f,   0.0f, 1.0f,  "Wind choice",          kTabCascade,                   \
      "How much the wind decides which crests pour: 1 only those it blows over, 0 all of them whatever the wind.")     \
    X(cascadeEvaporation, "CASCADE_EVAPORATION", 100.0f,  5.0f, 500.0f, "Evaporation drop", kTabCascade,                \
      "Yards of descent over which falling cascade fog warms and thins to a third. Lower dissolves it higher up the slope.") \
    X(cascadeStreaks,  "CASCADE_STREAKS",  1.5f,   0.0f, 6.0f,  "Falling strands",      kTabCascade,                   \
      "How much fog running down steep slopes stretches into long strands along its fall.")

    // The indoor profile's keys are read as WXL_FOG_INDOOR_<KEY>.
#define WXL_FOG_INDOOR_FLOATS(X)                                                                                        \
    X(density,         "DENSITY",          0.022f, 0.0f, 0.4f,  "Dust density",         kTabFog,                       \
      "Extinction per yard of the stale, dusty air inside buildings.")                                                       \
    X(floorHaze,       "FLOOR_HAZE",       1.0f,   0.0f, 6.0f,  "Floor haze",           kTabFog,                       \
      "Extra haze lying near the floor, as a multiple of the dust.")                                                         \
    X(floorHazeHeight, "FLOOR_HAZE_HEIGHT",1.5f,   0.1f, 10.0f, "Floor haze height",    kTabFog,                       \
      "How high the floor haze reaches, in yards.")                                                                          \
    X(noiseStrength,   "NOISE_STRENGTH",   0.5f,   0.0f, 1.0f,  "Dust variation",       kTabShape,                     \
      "How patchy the dust is.")                                                                                             \
    X(noiseScale,      "NOISE_SCALE",      4.0f,   0.5f, 30.0f, "Dust patch size",      kTabShape,                     \
      "Size of the dust's patches, in yards.")                                                                               \
    X(drift,           "DRIFT",            0.08f,  0.0f, 2.0f,  "Dust drift",           kTabMotion,                    \
      "How fast the still air drifts, in yards per second.")                                                                 \
    X(seep,            "SEEP",             0.75f,  0.05f, 4.0f, "Doorway seep",         kTabFog,                       \
      "How many yards the outdoor fog reaches inside a building through its doorways and walls.")                           \
    X(ambient,         "AMBIENT",          0.35f,  0.0f, 4.0f,  "Ambient light",        kTabLight,                     \
      "The dim light inside, from windows and doors.")                                                                       \
    X(lampScatter,     "LAMP_SCATTER",     2.0f,   0.0f, 8.0f,  "Lamp glow",            kTabLight,                     \
      "Strength of the halos lamps and candles make in the dust.")                                                           \
    X(anisotropy,      "ANISOTROPY",       0.45f,  0.0f, 0.95f, "Forward glow",         kTabLight,                     \
      "How much brighter the dust is looking towards a lamp.")                                                               \
    X(backAnisotropy,  "BACK_ANISOTROPY",  -0.15f, -0.9f, 0.0f, "Back glow",            kTabLight,                     \
      "The softer glow the dust gives back towards the lamp.")                                                               \
    X(lobeBlend,       "LOBE_BLEND",       0.2f,   0.0f, 1.0f,  "Back glow share",      kTabLight,                     \
      "Share of the back glow against the forward glow.")                                                                    \
    X(brightness,      "BRIGHTNESS",       1.0f,   0.0f, 4.0f,  "Colour brightness",    kTabLight,                     \
      "Multiplies the dust's colour.")                                                                                       \
    X(colorBlend,      "COLOR_BLEND",      0.5f,   0.0f, 1.0f,  "Colour blend",         kTabLight,                     \
      "In Blend mode: 0 takes the zone's colour, 1 yours.")

    struct OutdoorProfile
    {
#define WXL_FOG_DECLARE(f, key, def, lo, hi, label, tab, help) float f = def;
        WXL_FOG_OUTDOOR_FLOATS(WXL_FOG_DECLARE)
#undef WXL_FOG_DECLARE
        float color[4] = { 0.52f, 0.56f, 0.53f, 1.0f };
        int   colorMode = int(ColorMode::Blend);
    };

    struct IndoorProfile
    {
#define WXL_FOG_DECLARE(f, key, def, lo, hi, label, tab, help) float f = def;
        WXL_FOG_INDOOR_FLOATS(WXL_FOG_DECLARE)
#undef WXL_FOG_DECLARE
        float color[4] = { 0.60f, 0.54f, 0.47f, 1.0f };
        int   colorMode = int(ColorMode::Custom);
    };

    /// The transport layer (the fog rivers), WXL_FOG_SIM_*.
    struct Transport
    {
        int   enabled     = 1;
        float drainage    = 4.0f;    // yd/s of flow per unit slope, once it has run a while
        float friction    = 0.8f;    // per second: how fast the flow forgets its speed
        float windPush    = 0.02f;   // surface tilt the wind gives the layer, per yd/s
        float maxSpeed    = 10.0f;   // yd/s
        float formation   = 2.5f;    // yd of depth per minute on open ground at night
        float dayShare    = 0.3f;    // share of it while the sun is up
        float hollowBoost = 1.5f;    // extra in hollows, times the formation
        float waterBoost  = 2.0f;    // extra over water, times the formation
        float decay       = 0.6f;    // share per minute
        float sunDecay    = 2.0f;    // share per minute burnt off in full sun
        float windScour   = 0.4f;    // share per minute per yd/s of wind on ridges
        float poolDepth   = 24.0f;   // yd; deeper mixes away fast
        float timeScale   = 1.0f;    // simulated seconds per second
        float rate        = 10.0f;   // steps per second
        float warmup      = 180.0f;  // simulated seconds run quickly when tiles arrive
        float initialFill = 0.6f;    // share of a hollow's depth filled at the start
    };

    struct Settings
    {
        int   enabled         = 1;
        int   quality         = 3;       // 0 custom, 1 low, 2 medium, 3 high, 4 ultra

        // The march.
        int   nearSteps       = 28;
        int   farSteps        = 32;
        float nearRange       = 60.0f;   // yd: the half-resolution near march
        float farDistance     = 2500.0f; // yd: where the far march stops for the sky
        float startDistance   = 0.15f;   // yd from the lens
        float detailDistance  = 60.0f;   // yd: the fraying fades out by here (by the near range at most)
        float temporal        = 0.9f;    // share of the history kept
        float clipGamma       = 1.25f;   // neighbourhood clip, in standard deviations
        float rejectDepth     = 0.12f;   // relative scene distance change that drops the history
        int   dither          = 1;
        float intensity       = 1.0f;    // global density scale (the threat API fades it)

        // The clipmaps.
        float clipRate        = 30.0f;   // L0 steps per second; each coarser level half as often
        float restrict        = 0.35f;   // share of a finer level's state a coarse cell takes per step
        Transport transport;
        int   cascades        = 1;       // banks behind crests pour over them (the baked map and the API)

        // Light.
        float terrainShadow   = 1.0f;
        float penumbra        = 3.0f;    // degrees
        float worldShadow     = 0.6f;    // the engine's cascades
        int   worldShadows    = 1;
        int   msOctaves       = 2;

        // Lamps and indoor.
        int   lamps           = 1;
        int   gridX = 160, gridY = 90, gridZ = 64;
        float gridFar         = 250.0f;
        int   omniShadows     = 1;
        float omniBias        = 0.15f;
        int   indoorDetect    = 1;
        float indoorTransition = 5.0f;   // seconds for the camera's own medium
        float indoorLagRadius = 8.0f;    // yd around the camera that follow it
        float classifyTime    = 0.5f;    // seconds a room that streams in takes to fade

        // Effects.
        int   wakes           = 1;
        float wakeRange       = 100.0f;
        int   projectiles     = 1;
        int   plumes          = 1;
        int   heat            = 1;
        int   immersion       = 1;
        int   nativeFog       = 1;
        float nativeFogStart  = 0.85f;
        int   debugLantern    = 0;
        int   debugTorch      = 0;       // a pretend torch in the player's hand

        // Debug.
        int   view            = 0;
        int   debugLevel      = 0;
        float debugHeight     = 2.0f;
        float debugFar        = 300.0f;
        uint32_t isolate      = 0;       // FOG_ISO_* bits
    };

    Settings&       Config();
    OutdoorProfile& Outdoor();
    IndoorProfile&  Indoor();

    /// Reads every key once (WXL_Load).
    void LoadSettings();

    /// Low (1) to Ultra (4): steps, grid, simulation rate and detail together.
    void ApplyQuality(int quality);

    /// "RRGGBB" or "#RRGGBB" -> rgb 0..1; false when unreadable.
    bool ParseColor(const char* text, float rgb[3]);
}
