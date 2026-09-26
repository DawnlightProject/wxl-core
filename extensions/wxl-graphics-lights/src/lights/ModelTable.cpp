// wxl-graphics-lights: the model table, light for models that carry none of their own.
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
#include "ModelTable.hpp"
#include "Families.hpp"

#include "game/Lights.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    namespace lt = wxl::game::lights;
    namespace tb = wxl::gfx::lights::table;

    struct Entry
    {
        float rgb[3];
        float intensity;
        float attenStart, attenEnd;
        float offset[3];
        float extent[3];   // half the length of a tube light, model space; zero for a point
        uint32_t family;   // index into g_families
        uint8_t  familyId; // WXL_GFX_LIGHT_FAMILY_*
    };

    bool                     g_loaded = false;
    std::vector<Entry>       g_rows;
    std::vector<std::string> g_families;
    std::vector<wxl::gfx::lights::Flicker> g_familyFlicker;
    std::vector<wxl::gfx::lights::Profile> g_familyProfile;
    std::vector<float> g_familySize;

    /// A family's angular profile and the radius of its glowing source, yards: the flame, not the
    /// fixture. It is how soft the fixture's own shadows are: a cage bar a tenth of a yard from a
    /// flame this wide throws a penumbra of 2 x size / 0.1 radians, on shadow maps and on the baked
    /// cookie alike (Cookies.cpp softens the cookie by it), so a size too small prints the lantern's
    /// frame on the wall beside it as a hard, magnified silhouette.
    void ShapeOf(const std::string& family, wxl::gfx::lights::Profile& profile, float& size)
    {
        using P = wxl::gfx::lights::Profile;
        struct Shape { const char* family; P profile; float size; };
        static const Shape kShapes[] = {
            { "candle", P::Flame, 0.012f },     { "chandelier", P::None, 0.25f }, { "torch", P::Flame, 0.12f },
            { "brazier", P::Flame, 0.35f },     { "campfire", P::Flame, 0.5f },   { "hearth", P::Flame, 0.3f },
            { "fire", P::Flame, 0.3f },         { "lantern", P::Cage, 0.04f },    { "streetlamp", P::Downlight, 0.04f },
            { "greenlamp", P::Cage, 0.03f },    { "walllight", P::Grille, 0.05f },
        };
        profile = P::None;
        size = 0.1f;
        for (const Shape& s : kShapes)
            if (family == s.family) { profile = s.profile; size = s.size; return; }
    }

    /// A family's flicker, by its name in the table.
    wxl::gfx::lights::Flicker FlickerOf(const std::string& family)
    {
        using F = wxl::gfx::lights::Flicker;
        for (const char* fire : { "campfire", "brazier", "torch", "hearth", "fire" })
            if (family == fire) return F::Fire;
        for (const char* candle : { "candle", "chandelier" })
            if (family == candle) return F::Candle;
        for (const char* lantern : { "lantern", "streetlamp", "greenlamp", "walllight" })
            if (family == lantern) return F::Lantern;
        return F::None;
    }
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> g_byStem;   // first row, count

    void Load()
    {
        g_loaded = true;
        FILE* f = nullptr;
        if (fopen_s(&f, tb::kFile, "rb") != 0 || !f)
        {
            LIGHTS_LOG_WARN("lights: model table %s missing; models without lights stay dark", tb::kFile);
            return;
        }
        char line[512];
        std::vector<std::pair<uint64_t, Entry>> rows;
        while (std::fgets(line, sizeof line, f))
        {
            tb::Row row;
            if (!tb::ParseLine(line, row)) continue;
            const std::string family(row.family);
            uint32_t familyIndex = 0;
            for (; familyIndex < g_families.size() && g_families[familyIndex] != family; ++familyIndex) {}
            if (familyIndex == g_families.size())
            {
                g_families.push_back(family);
                g_familyFlicker.push_back(FlickerOf(family));
                wxl::gfx::lights::Profile profile;
                float size;
                ShapeOf(family, profile, size);
                g_familyProfile.push_back(profile);
                g_familySize.push_back(size);
            }
            Entry e{};
            std::memcpy(e.rgb, row.rgb, sizeof e.rgb);
            e.intensity  = row.intensity;
            e.attenStart = row.attenStart;
            e.attenEnd   = row.attenEnd;
            std::memcpy(e.offset, row.offset, sizeof e.offset);
            std::memcpy(e.extent, row.extent, sizeof e.extent);
            e.family = familyIndex;
            e.familyId = uint8_t(wxl::gfx::lights::families::ByName(family.c_str()));
            rows.push_back({ row.key, e });
        }
        std::fclose(f);

        // Rows of one model are adjacent in the file; a model split across it keeps its first run.
        g_rows.reserve(rows.size());
        for (const auto& r : rows)
        {
            auto it = g_byStem.find(r.first);
            if (it == g_byStem.end())
                g_byStem.emplace(r.first, std::make_pair(uint32_t(g_rows.size()), 1u));
            else if (it->second.first + it->second.second == g_rows.size())
                ++it->second.second;
            else
                continue;
            g_rows.push_back(r.second);
        }
        LIGHTS_LOG_INFO("lights: model table %s, %u rows for %u models, %u families", tb::kFile, unsigned(g_rows.size()),
                        unsigned(g_byStem.size()), unsigned(g_families.size()));
    }

    constexpr size_t kInstanceCap = 4096;

    /// The instance's rotation as a quaternion taking world directions into the model's frame:
    /// R[k][j] = m[k * 4 + j] / scale, the rows of the placement (world = local * M, row vectors),
    /// so local = R * world. Identity, and false, for a mirrored placement.
    bool RotationQuatScaled(const float* m, float scale, float q[4])
    {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
        if (scale <= 1e-6f) return false;
        const float s = 1.0f / scale;
        float R[3][3];
        for (int k = 0; k < 3; ++k)
            for (int j = 0; j < 3; ++j) R[k][j] = m[k * 4 + j] * s;
        const float det = R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) - R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0])
                        + R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]);
        if (det < 0.0f) return false;
        const float trace = R[0][0] + R[1][1] + R[2][2];
        float w, x, y, z;
        if (trace > 0.0f)
        {
            const float t = std::sqrt(trace + 1.0f) * 2.0f;
            w = 0.25f * t; x = (R[2][1] - R[1][2]) / t; y = (R[0][2] - R[2][0]) / t; z = (R[1][0] - R[0][1]) / t;
        }
        else if (R[0][0] > R[1][1] && R[0][0] > R[2][2])
        {
            const float t = std::sqrt(1.0f + R[0][0] - R[1][1] - R[2][2]) * 2.0f;
            w = (R[2][1] - R[1][2]) / t; x = 0.25f * t; y = (R[0][1] + R[1][0]) / t; z = (R[0][2] + R[2][0]) / t;
        }
        else if (R[1][1] > R[2][2])
        {
            const float t = std::sqrt(1.0f + R[1][1] - R[0][0] - R[2][2]) * 2.0f;
            w = (R[0][2] - R[2][0]) / t; x = (R[0][1] + R[1][0]) / t; y = 0.25f * t; z = (R[1][2] + R[2][1]) / t;
        }
        else
        {
            const float t = std::sqrt(1.0f + R[2][2] - R[0][0] - R[1][1]) * 2.0f;
            w = (R[1][0] - R[0][1]) / t; x = (R[0][2] + R[2][0]) / t; y = (R[1][2] + R[2][1]) / t; z = 0.25f * t;
        }
        const float n = std::sqrt(x * x + y * y + z * z + w * w);
        if (n <= 1e-6f) return false;
        q[0] = x / n; q[1] = y / n; q[2] = z / n; q[3] = w / n;
        return true;
    }
}

namespace wxl::gfx::lights::table
{
    bool ParseLine(const char* line, Row& out)
    {
        if (!line || line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == '\0') return false;
        const char* comma = std::strchr(line, ',');
        if (!comma) return false;
        const char* p = std::strchr(comma + 1, ',');
        if (!p) return false;
        const size_t familyLen = std::min(size_t(p - (comma + 1)), sizeof out.family - 1);

        float v[12] = {};
        int got = 0;
        for (; got < 12 && p && *p == ','; ++got)
        {
            char* end = nullptr;
            v[got] = std::strtof(p + 1, &end);
            if (end == p + 1) break;
            p = end;
        }
        if (got != 9 && got != 12) return false;   // the header row, or a malformed one

        out.key = StemHash(line, size_t(comma - line));
        std::memcpy(out.family, comma + 1, familyLen);
        out.family[familyLen] = '\0';
        out.rgb[0] = v[0]; out.rgb[1] = v[1]; out.rgb[2] = v[2];
        out.intensity = v[3];
        out.attenStart = v[4];
        out.attenEnd = v[5];
        out.offset[0] = v[6]; out.offset[1] = v[7]; out.offset[2] = v[8];
        out.extent[0] = v[9]; out.extent[1] = v[10]; out.extent[2] = v[11];
        return true;
    }

    uint32_t Rows() { return uint32_t(g_rows.size()); }

    bool SourceShape(uint64_t stem, uint32_t row, float& size, uint32_t& family)
    {
        if (!g_loaded) Load();
        const auto it = g_byStem.find(stem);
        if (it == g_byStem.end() || row >= it->second.second) return false;
        const Entry& e = g_rows[it->second.first + row];
        size = g_familySize[e.family];
        family = e.familyId;
        return true;
    }

    size_t Collect(const float center[3], float radius, uint32_t maxStale, Light* out, size_t cap, Stats* stats)
    {
        if (!g_loaded) Load();
        if (stats) *stats = Stats{ uint32_t(g_rows.size()), 0, 0, 0, 0 };
        if (g_rows.empty()) return 0;

        static lt::ModelInstance instances[kInstanceCap];
        lt::ModelQuery q;
        for (int k = 0; k < 3; ++k) q.center[k] = center[k];
        q.radius = radius + 8.0f;   // a table light sits a few yards from its model's origin
        q.maxStaleFrames = maxStale;
        const size_t n = std::min(lt::CollectModels(q, instances, kInstanceCap), kInstanceCap);

        size_t matched = 0;
        const float radiusSq = radius * radius;
        for (size_t i = 0; i < n; ++i)
        {
            const lt::ModelInstance& inst = instances[i];
            if (stats) ++stats->instances;
            const auto it = g_byStem.find(StemHash(inst.stem, sizeof inst.stem));
            if (it == g_byStem.end()) continue;
            if (stats) ++stats->matched;

            const float* m = inst.toWorld;
            const float scale = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
            for (uint32_t r = 0; r < it->second.second; ++r)
            {
                const Entry& row = g_rows[it->second.first + r];
                const float* p = row.offset;
                const float world[3] = {
                    p[0] * m[0] + p[1] * m[4] + p[2] * m[8]  + m[12],
                    p[0] * m[1] + p[1] * m[5] + p[2] * m[9]  + m[13],
                    p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14],
                };
                const float dx = world[0] - center[0], dy = world[1] - center[1], dz = world[2] - center[2];
                if (dx * dx + dy * dy + dz * dz > radiusSq) { if (stats) ++stats->tooFar; continue; }
                if (stats) ++stats->accepted;

                Light* o = matched < cap ? &out[matched] : nullptr;
                ++matched;
                if (!o) continue;
                *o = Light{};
                for (int k = 0; k < 3; ++k)
                {
                    o->position[k] = world[k];
                    o->color[k] = row.rgb[k] * row.intensity;
                }
                o->intensity   = 1.0f;
                o->radius      = row.attenEnd * scale;
                o->innerRadius = row.attenStart * scale;
                o->cosCone     = -2.0f;
                o->kind  = Kind::Table;
                o->family = row.familyId;
                o->flicker = g_familyFlicker[row.family];
                o->profile = g_familyProfile[row.family];
                o->size    = g_familySize[row.family] * scale;
                const float* e = row.extent;
                for (int k = 0; k < 3; ++k) o->extent[k] = e[0] * m[k] + e[1] * m[4 + k] + e[2] * m[8 + k];
                o->owner = inst.owner;
                o->index = r;
                // For the cookie service: which file, and the instance's rotation.
                const bool upright = RotationQuatScaled(m, scale, o->cookieRotation);
                o->cookieSource = upright ? it->first : 0;
            }
        }
        return matched;
    }

    uint64_t StemHash(const char* s, size_t maxLen)
    {
        size_t n = 0;
        while (n < maxLen && s[n]) ++n;
        for (const char* ext : { ".m2", ".mdx", ".wmo" })
        {
            const size_t e = std::strlen(ext);
            if (n > e && _strnicmp(s + n - e, ext, e) == 0) { n -= e; break; }
        }
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < n; ++i)
        {
            char c = s[i];
            if (c == '/') c = '\\';
            else if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            h = (h ^ uint8_t(c)) * 1099511628211ull;
        }
        return h;
    }

    bool RotationQuat(const float* m, float q[4])
    {
        return RotationQuatScaled(m, std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), q);
    }
}
