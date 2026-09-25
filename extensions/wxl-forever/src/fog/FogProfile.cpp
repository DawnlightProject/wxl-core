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

#include "../core/ExtensionApi.hpp"
#include "FogProfile.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    namespace fog = wxl::forever::fog;

    fog::FogProfile MakeOutdoor()
    {
        fog::FogProfile p;
        p.id = 1;
        return p;
    }

    fog::FogProfile MakeIndoor()
    {
        fog::FogProfile p;
        p.id = 2;
#define WXL_FOG_INDOOR(name, key, out, in, lo, hi, label, tab, help) p.name = in;
        WXL_FOG_PROFILE_FLOATS(WXL_FOG_INDOOR)
#undef WXL_FOG_INDOOR
        return p;
    }

    fog::FogProfile g_builtin[2] = { MakeOutdoor(), MakeIndoor() };

    struct SlotState
    {
        bool                 started = false;
        uint32_t             targetId = 0;
        fog::ResolvedProfile from{};
        fog::ResolvedProfile last{};
        float                t = 1.0f;
    };
    SlotState g_slots[2];

    /// The provider: which profile a slot shows now. An area-keyed table answers here later.
    const fog::FogProfile& ChooseProfile(fog::Slot slot)
    {
        return g_builtin[int(slot)];
    }

    fog::ResolvedProfile Resolve(const fog::FogProfile& p, uint32_t nativeArgb)
    {
        const float native[3] = { float((nativeArgb >> 16) & 0xFF) / 255.0f,
                                  float((nativeArgb >> 8) & 0xFF) / 255.0f,
                                  float(nativeArgb & 0xFF) / 255.0f };
        fog::ResolvedProfile r{};
#define WXL_FOG_COPY(name, key, out, in, lo, hi, label, tab, help) r.name = p.name;
        WXL_FOG_PROFILE_FLOATS(WXL_FOG_COPY)
#undef WXL_FOG_COPY
        for (int i = 0; i < 3; ++i)
        {
            float c = native[i];
            if (p.colorMode == int(fog::ColorMode::Custom))     c = p.color[i];
            else if (p.colorMode == int(fog::ColorMode::Blend)) c = native[i] + (p.color[i] - native[i]) * p.colorBlend;
            r.rgb[i] = c * p.brightness;
        }
        return r;
    }

    fog::ResolvedProfile Lerp(const fog::ResolvedProfile& a, const fog::ResolvedProfile& b, float s)
    {
        fog::ResolvedProfile r{};
#define WXL_FOG_MIX(name, key, out, in, lo, hi, label, tab, help) r.name = a.name + (b.name - a.name) * s;
        WXL_FOG_PROFILE_FLOATS(WXL_FOG_MIX)
#undef WXL_FOG_MIX
        for (int i = 0; i < 3; ++i) r.rgb[i] = a.rgb[i] + (b.rgb[i] - a.rgb[i]) * s;
        return r;
    }

    std::string Key(const char* prefix, const char* name) { return std::string(prefix) + name; }

    /// Accepts "RRGGBB" hex or "r,g,b" in 0..1.
    bool ParseColor(const char* raw, float out[3])
    {
        if (std::strchr(raw, ','))
            return std::sscanf(raw, "%f,%f,%f", &out[0], &out[1], &out[2]) == 3;
        const char* hex = raw[0] == '#' ? raw + 1 : raw;
        char* end = nullptr;
        const unsigned long v = std::strtoul(hex, &end, 16);
        if (end == hex) return false;
        out[0] = float((v >> 16) & 0xFF) / 255.0f;
        out[1] = float((v >> 8) & 0xFF) / 255.0f;
        out[2] = float(v & 0xFF) / 255.0f;
        return true;
    }
}

namespace wxl::forever::fog
{
    FogProfile& Builtin(Slot slot) { return g_builtin[int(slot)]; }

    bool LoadProfile(const char* prefix, FogProfile& p)
    {
        using wxl_forever::ConfigFloat;
        using wxl_forever::ConfigRaw;

        // Config ranges are wider than the panel's: a file may ask for what a slider cannot reach.
#define WXL_FOG_LOAD(name, key, out, in, lo, hi, label, tab, help) \
        p.name = ConfigFloat(Key(prefix, key).c_str(), p.name, (lo) < 0.0f ? (lo) * 4.0f : 0.0f, (hi) * 4.0f);
        WXL_FOG_PROFILE_FLOATS(WXL_FOG_LOAD)
#undef WXL_FOG_LOAD

        p.colorBlend = ConfigFloat(Key(prefix, "COLOR_BLEND").c_str(), p.colorBlend, 0.0f, 1.0f);
        p.brightness = ConfigFloat(Key(prefix, "BRIGHTNESS").c_str(), p.brightness, 0.0f, 10.0f);

        char raw[64];
        if (ConfigRaw(Key(prefix, "COLOR_MODE").c_str(), raw, sizeof raw))
        {
            if (raw[0] == 'n' || raw[0] == 'N' || raw[0] == '0')      p.colorMode = int(ColorMode::Native);
            else if (raw[0] == 'c' || raw[0] == 'C' || raw[0] == '1') p.colorMode = int(ColorMode::Custom);
            else if (raw[0] == 'b' || raw[0] == 'B' || raw[0] == '2') p.colorMode = int(ColorMode::Blend);
        }
        if (ConfigRaw(Key(prefix, "COLOR").c_str(), raw, sizeof raw))
            ParseColor(raw, p.color);

        return ConfigRaw(Key(prefix, "BASE_Z").c_str(), raw, sizeof raw);
    }

    ResolvedProfile MixProfiles(const ResolvedProfile& a, const ResolvedProfile& b, float t)
    {
        return Lerp(a, b, t);
    }

    void ResolveProfiles(float dt, float transitionSeconds, uint32_t nativeArgb, ResolvedProfile out[2])
    {
        for (int i = 0; i < int(Slot::Count); ++i)
        {
            SlotState& s = g_slots[i];
            const FogProfile& want = ChooseProfile(Slot(i));
            const ResolvedProfile target = Resolve(want, nativeArgb);

            if (!s.started || transitionSeconds <= 0.0f)
            {
                s.started = true;
                s.t = 1.0f;
            }
            else if (want.id != s.targetId)
            {
                // A new profile starts from whatever is on screen now, even mid-transition.
                s.from = s.last;
                s.t = 0.0f;
            }
            s.targetId = want.id;

            if (s.t < 1.0f)
            {
                s.t += dt / transitionSeconds;
                if (s.t > 1.0f) s.t = 1.0f;
            }
            const float smooth = s.t * s.t * (3.0f - 2.0f * s.t);
            s.last = s.t >= 1.0f ? target : Lerp(s.from, target, smooth);
            out[i] = s.last;
        }
    }
}
