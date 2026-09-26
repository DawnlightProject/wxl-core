// wxl-graphics-fog: reading the settings from Extensions\wxl-graphics-fog\wxl-graphics-fog.cfg (an
// environment variable of the same name wins), and the quality presets.
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

#include "Settings.hpp"
#include "Extension.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace
{
    using namespace wxl::gfx::fog;

    Settings       g_settings;
    OutdoorProfile g_outdoor;
    IndoorProfile  g_indoor;

    int ReadColorMode(const char* key, int fallback)
    {
        char buf[32];
        if (!ConfigRaw(key, buf, sizeof buf)) return fallback;
        const char c = char(std::tolower(uint8_t(buf[0])));
        if (c == 'n' || c == '0') return int(ColorMode::Native);
        if (c == 'c' || c == '1') return int(ColorMode::Custom);
        if (c == 'b' || c == '2') return int(ColorMode::Blend);
        return fallback;
    }

    void ReadColor(const char* key, float color[4])
    {
        char buf[32];
        if (!ConfigRaw(key, buf, sizeof buf)) return;
        float rgb[3];
        if (ParseColor(buf, rgb)) std::copy(rgb, rgb + 3, color);
    }

    void ReadQuality()
    {
        char buf[32];
        if (!ConfigRaw("WXL_FOG_QUALITY", buf, sizeof buf)) return;
        const char q = char(std::tolower(uint8_t(buf[0])));
        const int level = q == 'l' ? 1 : q == 'm' ? 2 : q == 'h' ? 3 : q == 'u' ? 4 : 0;
        g_settings.quality = level;
        ApplyQuality(level);
    }
}

namespace wxl::gfx::fog
{
    Settings& Config() { return g_settings; }
    OutdoorProfile& Outdoor() { return g_outdoor; }
    IndoorProfile& Indoor() { return g_indoor; }

    bool ParseColor(const char* text, float rgb[3])
    {
        if (!text) return false;
        while (*text == ' ' || *text == '#') ++text;
        unsigned value = 0;
        int digits = 0;
        for (; text[digits] && std::isxdigit(uint8_t(text[digits])) && digits < 8; ++digits)
        {
            const char c = char(std::tolower(uint8_t(text[digits])));
            value = value * 16u + unsigned(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        if (digits != 6 && digits != 8) return false;
        if (digits == 8) value &= 0xFFFFFFu;   // AARRGGBB: the alpha is ignored
        rgb[0] = float((value >> 16) & 0xFF) / 255.0f;
        rgb[1] = float((value >> 8) & 0xFF) / 255.0f;
        rgb[2] = float(value & 0xFF) / 255.0f;
        return true;
    }

    void ApplyQuality(int quality)
    {
        struct Level { int nearSteps, farSteps, gx, gy, gz; float clipRate, detail; int octaves; };
        static const Level kLevels[] = {
            { 16, 16,  96, 54, 32, 15.0f, 30.0f, 1 },
            { 24, 24, 128, 72, 48, 20.0f, 45.0f, 2 },
            { 28, 32, 160, 90, 64, 30.0f, 60.0f, 2 },
            { 48, 40, 192, 108, 96, 45.0f, 90.0f, 3 },
        };
        if (quality < 1 || quality > 4) return;
        const Level& l = kLevels[quality - 1];
        Settings& s = g_settings;
        s.nearSteps = l.nearSteps;
        s.farSteps = l.farSteps;
        s.gridX = l.gx;
        s.gridY = l.gy;
        s.gridZ = l.gz;
        s.clipRate = l.clipRate;
        s.detailDistance = l.detail;
        s.msOctaves = l.octaves;
    }

    void LoadSettings()
    {
        Settings& s = g_settings;
        ApplyQuality(s.quality);
        ReadQuality();
        s.enabled        = ConfigBool("WXL_FOG_ENABLED", true) ? 1 : 0;
        s.nearSteps      = ConfigInt("WXL_FOG_NEAR_STEPS", s.nearSteps, 4, 128);
        s.farSteps       = ConfigInt("WXL_FOG_FAR_STEPS", s.farSteps, 4, 128);
        s.nearRange      = ConfigFloat("WXL_FOG_NEAR_RANGE", s.nearRange, 8.0f, 200.0f);
        s.farDistance    = ConfigFloat("WXL_FOG_FAR_DISTANCE", s.farDistance, 300.0f, 8000.0f);
        s.startDistance  = ConfigFloat("WXL_FOG_START_DISTANCE", s.startDistance, 0.0f, 2.0f);
        s.detailDistance = ConfigFloat("WXL_FOG_DETAIL_DISTANCE", s.detailDistance, 0.0f, 200.0f);
        s.temporal       = ConfigFloat("WXL_FOG_TEMPORAL", s.temporal, 0.0f, 0.98f);
        s.clipGamma      = ConfigFloat("WXL_FOG_TEMPORAL_CLIP", s.clipGamma, 0.5f, 4.0f);
        s.rejectDepth    = ConfigFloat("WXL_FOG_TEMPORAL_REJECT", s.rejectDepth, 0.01f, 1.0f);
        s.dither         = ConfigBool("WXL_FOG_DITHER", true) ? 1 : 0;
        s.intensity      = ConfigFloat("WXL_FOG_INTENSITY", s.intensity, 0.0f, 4.0f);

        s.clipRate       = ConfigFloat("WXL_FOG_CLIP_RATE", s.clipRate, 5.0f, 120.0f);
        s.restrict       = ConfigFloat("WXL_FOG_CLIP_RESTRICT", s.restrict, 0.0f, 1.0f);

        Transport& t = s.transport;
        t.enabled     = ConfigBool("WXL_FOG_SIM", true) ? 1 : 0;
        t.drainage    = ConfigFloat("WXL_FOG_SIM_DRAINAGE", t.drainage, 0.0f, 40.0f);
        t.friction    = ConfigFloat("WXL_FOG_SIM_FRICTION", t.friction, 0.05f, 10.0f);
        t.windPush    = ConfigFloat("WXL_FOG_SIM_WIND_PUSH", t.windPush, 0.0f, 0.5f);
        t.maxSpeed    = ConfigFloat("WXL_FOG_SIM_MAX_SPEED", t.maxSpeed, 0.5f, 60.0f);
        t.formation   = ConfigFloat("WXL_FOG_SIM_SOURCE", t.formation, 0.0f, 60.0f);
        t.dayShare    = ConfigFloat("WXL_FOG_SIM_DAY_SOURCE", t.dayShare, 0.0f, 1.0f);
        t.hollowBoost = ConfigFloat("WXL_FOG_SIM_HOLLOW_SOURCE", t.hollowBoost, 0.0f, 20.0f);
        t.waterBoost  = ConfigFloat("WXL_FOG_SIM_WATER_SOURCE", t.waterBoost, 0.0f, 20.0f);
        t.decay       = ConfigFloat("WXL_FOG_SIM_DECAY", t.decay, 0.0f, 60.0f);
        t.sunDecay    = ConfigFloat("WXL_FOG_SIM_SUN_DECAY", t.sunDecay, 0.0f, 60.0f);
        t.windScour   = ConfigFloat("WXL_FOG_SIM_WIND_DECAY", t.windScour, 0.0f, 20.0f);
        t.poolDepth   = ConfigFloat("WXL_FOG_SIM_POOL_DEPTH", t.poolDepth, 1.0f, 200.0f);
        t.timeScale   = ConfigFloat("WXL_FOG_SIM_TIME_SCALE", t.timeScale, 0.0f, 20.0f);
        t.rate        = ConfigFloat("WXL_FOG_SIM_RATE", t.rate, 2.0f, 60.0f);
        t.warmup      = ConfigFloat("WXL_FOG_SIM_WARMUP", t.warmup, 0.0f, 1200.0f);
        t.initialFill = ConfigFloat("WXL_FOG_SIM_INITIAL_FILL", t.initialFill, 0.0f, 1.0f);

        s.terrainShadow  = ConfigFloat("WXL_FOG_TERRAIN_SHADOW", s.terrainShadow, 0.0f, 1.0f);
        s.penumbra       = ConfigFloat("WXL_FOG_TERRAIN_PENUMBRA", s.penumbra, 0.2f, 20.0f);
        s.worldShadows   = ConfigBool("WXL_FOG_WORLD_SHADOWS", true) ? 1 : 0;
        s.worldShadow    = ConfigFloat("WXL_FOG_WORLD_SHADOW", s.worldShadow, 0.0f, 1.0f);
        s.msOctaves      = ConfigInt("WXL_FOG_MS_OCTAVES", s.msOctaves, 0, 3);

        s.lamps          = ConfigBool("WXL_FOG_LAMPS", true) ? 1 : 0;
        s.gridX          = ConfigInt("WXL_FOG_LAMP_GRID_X", s.gridX, 32, 320);
        s.gridY          = ConfigInt("WXL_FOG_LAMP_GRID_Y", s.gridY, 18, 180);
        s.gridZ          = ConfigInt("WXL_FOG_LAMP_GRID_Z", s.gridZ, 16, 128);
        s.omniShadows    = ConfigBool("WXL_FOG_OMNI_SHADOWS", true) ? 1 : 0;
        s.omniBias       = ConfigFloat("WXL_FOG_OMNI_BIAS", s.omniBias, 0.0f, 2.0f);
        s.indoorDetect   = ConfigBool("WXL_FOG_INDOOR_DETECT", true) ? 1 : 0;
        s.indoorTransition = ConfigFloat("WXL_FOG_INDOOR_TRANSITION", s.indoorTransition, 0.0f, 30.0f);
        s.indoorLagRadius  = ConfigFloat("WXL_FOG_INDOOR_LAG_RADIUS", s.indoorLagRadius, 1.0f, 40.0f);

        s.wakes          = ConfigBool("WXL_FOG_WAKES", true) ? 1 : 0;
        s.wakeRange      = ConfigFloat("WXL_FOG_WAKE_RANGE", s.wakeRange, 5.0f, 150.0f);
        s.projectiles    = ConfigBool("WXL_FOG_PROJECTILES", true) ? 1 : 0;
        s.plumes         = ConfigBool("WXL_FOG_PLUMES", true) ? 1 : 0;
        s.heat           = ConfigBool("WXL_FOG_HEAT_ENABLED", true) ? 1 : 0;
        s.immersion      = ConfigBool("WXL_FOG_IMMERSION_ENABLED", true) ? 1 : 0;
        s.nativeFog      = ConfigBool("WXL_FOG_NATIVE_FOG", true) ? 1 : 0;
        s.nativeFogStart = ConfigFloat("WXL_FOG_NATIVE_FOG_START", s.nativeFogStart, 0.0f, 0.99f);
        s.debugLantern   = ConfigBool("WXL_FOG_DEBUG_LANTERN", false) ? 1 : 0;
        s.debugTorch     = ConfigBool("WXL_FOG_DEBUG_TORCH", false) ? 1 : 0;
        s.cascades       = ConfigBool("WXL_FOG_CASCADES", true) ? 1 : 0;

        char key[96];
#define WXL_FOG_READ(f, k, def, lo, hi, label, tab, help)                  \
        std::snprintf(key, sizeof key, "WXL_FOG_%s", k);                    \
        g_outdoor.f = ConfigFloat(key, g_outdoor.f, lo, hi);
        WXL_FOG_OUTDOOR_FLOATS(WXL_FOG_READ)
#undef WXL_FOG_READ
        ReadColor("WXL_FOG_COLOR", g_outdoor.color);
        g_outdoor.colorMode = ReadColorMode("WXL_FOG_COLOR_MODE", g_outdoor.colorMode);

#define WXL_FOG_READ(f, k, def, lo, hi, label, tab, help)                  \
        std::snprintf(key, sizeof key, "WXL_FOG_INDOOR_%s", k);             \
        g_indoor.f = ConfigFloat(key, g_indoor.f, lo, hi);
        WXL_FOG_INDOOR_FLOATS(WXL_FOG_READ)
#undef WXL_FOG_READ
        ReadColor("WXL_FOG_INDOOR_COLOR", g_indoor.color);
        g_indoor.colorMode = ReadColorMode("WXL_FOG_INDOOR_COLOR_MODE", g_indoor.colorMode);
    }
}
