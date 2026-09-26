// wxl-graphics-shadow: reading the settings from Extensions\wxl-graphics-shadow\wxl-graphics-shadow.cfg
// (an environment variable of the same name wins).
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

namespace
{
    wxl::gfx::shadow::Settings g_settings;

    void Bool(const char* key, int& v) { v = wxl::gfx::shadow::ConfigBool(key, v != 0) ? 1 : 0; }
    void Int(const char* key, int& v, int lo, int hi) { v = wxl::gfx::shadow::ConfigInt(key, v, lo, hi); }
    void Float(const char* key, float& v, float lo, float hi) { v = wxl::gfx::shadow::ConfigFloat(key, v, lo, hi); }
}

namespace wxl::gfx::shadow
{
    Settings& Config() { return g_settings; }

    void LoadSettings()
    {
        Settings& s = g_settings;
        Bool("WXL_GFX_SHADOW", s.enabled);

        Bool("WXL_GFX_SHADOW_MAPS", s.maps);
        Int("WXL_GFX_SHADOW_MAPS_STILL", s.stillMaps, 0, 4);
        Int("WXL_GFX_SHADOW_MAPS_MOVING", s.movingMaps, 0, 2);
        Int("WXL_GFX_SHADOW_MAPS_STATIC_FACE", s.staticFace, 128, 1024);
        Int("WXL_GFX_SHADOW_MAPS_UNIT_FACE", s.unitFace, 64, 1024);
        Int("WXL_GFX_SHADOW_MAPS_MOVING_FACE", s.movingFace, 128, 1024);
        Int("WXL_GFX_SHADOW_MAPS_REFRESH", s.refresh, 1, 6);
        Int("WXL_GFX_SHADOW_MAPS_ATLAS_FACE", s.atlasFace, 128, 256);
        s.atlasFace = s.atlasFace >= 256 ? 256 : 128;
        Float("WXL_GFX_SHADOW_MAPS_HOUSING", s.housing, 0.0f, 2.0f);
        Float("WXL_GFX_SHADOW_MAPS_CARRIED_HOUSING", s.carriedHousing, 0.0f, 1.0f);
        Float("WXL_GFX_SHADOW_MAPS_CARRIER_MARGIN", s.carrierMargin, 0.0f, 1.0f);
        Float("WXL_GFX_SHADOW_EVSM_POSITIVE", s.evsmPositive, 5.0f, 42.0f);
        Float("WXL_GFX_SHADOW_EVSM_NEGATIVE", s.evsmNegative, 1.0f, 42.0f);
        Float("WXL_GFX_SHADOW_EVSM_BLEED", s.bleed, 0.0f, 0.9f);
        Float("WXL_GFX_SHADOW_EVSM_MIN_VARIANCE", s.minVariance, 0.0f, 0.01f);
        Float("WXL_GFX_SHADOW_MAPS_SOFTNESS", s.softness, 0.0f, 4.0f);
        Float("WXL_GFX_SHADOW_MAPS_LOD_BIAS", s.lodBias, -1.0f, 3.0f);
        Float("WXL_GFX_SHADOW_NORMAL_OFFSET", s.normalOffset, 0.0f, 4.0f);
        Float("WXL_GFX_SHADOW_SLOPE_BIAS", s.slopeBias, 0.0f, 4.0f);

        Float("WXL_GFX_SHADOW_SLOT_HOLD", s.slotHold, 0.0f, 5.0f);
        Float("WXL_GFX_SHADOW_SLOT_FADE", s.slotFade, 0.05f, 2.0f);
        Float("WXL_GFX_SHADOW_SLOT_MARGIN", s.slotMargin, 1.0f, 3.0f);

        Bool("WXL_GFX_SHADOW_CAPSULES", s.capsules);
        Float("WXL_GFX_SHADOW_CAPSULES_RANGE", s.capsuleRange, 10.0f, 120.0f);
        Float("WXL_GFX_SHADOW_CAPSULES_RADIUS", s.capsuleRadius, 0.1f, 0.4f);
        Float("WXL_GFX_SHADOW_CAPSULES_PENUMBRA", s.capsulePenumbra, 0.0f, 4.0f);

        Bool("WXL_GFX_SHADOW_SUN", s.sun);
        Bool("WXL_GFX_SHADOW_SUN_CASCADES", s.cascades);
        Bool("WXL_GFX_SHADOW_SUN_LOW", s.lowSun);
        Bool("WXL_GFX_SHADOW_MOON", s.moon);
        Float("WXL_GFX_SHADOW_SUN_LIFT", s.sunLift, 0.5f, 5.0f);
        Float("WXL_GFX_SHADOW_SUN_MIN_ELEVATION", s.sunMinElevation, 2.0f, 60.0f);
        Float("WXL_GFX_SHADOW_SUN_FILTER", s.cascadeFilter, 0.5f, 2.0f);
        Float("WXL_GFX_SHADOW_SUN_DUSK_SOFTNESS", s.duskSoftness, 0.0f, 2.0f);
        Float("WXL_GFX_SHADOW_SUN_BIAS", s.cascadeBias, 0.0f, 2.0f);

        Bool("WXL_GFX_SHADOW_TERRAIN_CASTER", s.terrainCaster);
        Int("WXL_GFX_SHADOW_TERRAIN_CASTER_CASCADES", s.casterCascades, 1, 4);
        Float("WXL_GFX_SHADOW_TERRAIN_CASTER_BIAS", s.casterBias, 0.0f, 4.0f);
        Float("WXL_GFX_SHADOW_TERRAIN_CASTER_DROP", s.casterDrop, 0.0f, 4.0f);
        Int("WXL_GFX_SHADOW_TERRAIN_CASTER_DETAIL", s.casterDetail, -1, 1);
        Bool("WXL_GFX_SHADOW_HORIZON", s.horizon);
        Float("WXL_GFX_SHADOW_HORIZON_STRENGTH", s.horizonStrength, 0.0f, 1.0f);
        Float("WXL_GFX_SHADOW_HORIZON_PENUMBRA", s.horizonPenumbra, 0.1f, 15.0f);
        Float("WXL_GFX_SHADOW_HORIZON_OCCLUDER", s.horizonOccluder, 10.0f, 400.0f);

        Bool("WXL_GFX_SHADOW_CONTACT", s.contact);
        Float("WXL_GFX_SHADOW_CONTACT_SUN", s.contactSun, 0.0f, 4.0f);
        Int("WXL_GFX_SHADOW_CONTACT_SLOTS", s.contactSlots, 0, 4);
        Float("WXL_GFX_SHADOW_CONTACT_LAMP", s.contactLamp, 0.0f, 3.0f);
        Float("WXL_GFX_SHADOW_CONTACT_THICKNESS", s.contactThickness, 0.1f, 2.0f);
        Float("WXL_GFX_SHADOW_CONTACT_STRENGTH", s.contactStrength, 0.0f, 1.0f);

        Bool("WXL_GFX_SHADOW_HALF_RES", s.halfRes);
        Int("WXL_GFX_SHADOW_VIEW", s.view, 0, kViewCount - 1);
        Bool("WXL_GFX_SHADOW_TIMERS", s.timers);

        SHADOW_LOG_INFO("settings: %s, maps %d (still %d, moving %d, faces %d/%d/%d, atlas %d), capsules %d, sun %d (cascades %d, "
                        "low %d, moon %d), terrain caster %d, horizon %d, contact %d, %s resolution",
                        s.enabled ? "on" : "off", s.maps, s.stillMaps, s.movingMaps, s.staticFace, s.unitFace, s.movingFace,
                        s.atlasFace, s.capsules, s.sun, s.cascades, s.lowSun, s.moon, s.terrainCaster, s.horizon, s.contact,
                        s.halfRes ? "half" : "full");
    }
}
