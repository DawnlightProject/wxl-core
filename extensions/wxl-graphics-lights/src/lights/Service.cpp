// wxl-graphics-lights: the frame's light list, gathered and published once for every consumer.
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
#include "Lights.hpp"
#include "Cookies.hpp"
#include "Families.hpp"
#include "Omni.hpp"
#include "Rooms.hpp"
#include "Sky.hpp"

#include "wxl/gfx/Matrix.hpp"
#include "game/Camera.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    namespace gl  = wxl::gfx::lights;
    namespace fm  = wxl::gfx::lights::families;
    namespace cam = wxl::game::camera;

    gl::Options g_options;
    bool        g_interiorGate = false;

    // Given lights, one set per caller key, concatenated in the order the keys first arrived.
    constexpr int kGivenSets = 8;
    struct GivenSet
    {
        const void* key = nullptr;
        int         count = 0;
        gl::Light   lights[gl::kMaxLights];
    };
    GivenSet  g_givenSets[kGivenSets];
    gl::Light g_given[gl::kMaxLights];
    int       g_givenCount = 0;
    bool      g_givenWarned = false;

    gl::Light          g_list[gl::kMaxLights];
    int                g_count = 0;
    WXL_GfxLight       g_abi[gl::kMaxLights];
    WXL_GfxLightSource g_sources[gl::kMaxLights];
    int                g_abiCount = 0;
    uint32_t           g_abiFrame = 0;
    float              g_eye[3] = {};

    // Where each light was last frame, before the flicker, and the frame it last moved.
    struct Seen { const void* owner; uint32_t index; float position[3]; uint32_t moved; };
    std::vector<Seen> g_seen, g_seenNext;
    struct MovingEntry { float key; gl::Moving m; uint8_t carriedOnly; };
    std::vector<MovingEntry> g_movingEntries;
    std::vector<gl::Moving>  g_moving;
    std::vector<uint8_t>     g_movingCarriedOnly;
    constexpr uint32_t kMoveHold = 12;       // frames a light still counts as moving once it stops
    constexpr float    kMoveStep = 0.03f;    // yards per frame that count as a move

    void TrackMoves(const float eye[3], uint32_t frame)
    {
        std::vector<Seen>& seen = g_seenNext;
        seen.clear();
        std::vector<MovingEntry>& moving = g_movingEntries;
        moving.clear();
        for (int i = 0; i < g_count; ++i)
        {
            const gl::Light& l = g_list[i];
            Seen now{ l.owner, l.owner ? l.index : uint32_t(i), { l.position[0], l.position[1], l.position[2] }, 0 };
            for (const Seen& s : g_seen)
            {
                if (s.owner != now.owner || s.index != now.index) continue;
                const float dx = l.position[0] - s.position[0], dy = l.position[1] - s.position[1], dz = l.position[2] - s.position[2];
                now.moved = dx * dx + dy * dy + dz * dz > kMoveStep * kMoveStep ? frame : s.moved;
                break;
            }
            seen.push_back(now);
            const bool moved = now.moved && frame - now.moved <= kMoveHold;
            if (moved || l.carried)
            {
                const float dx = l.position[0] - eye[0], dy = l.position[1] - eye[1], dz = l.position[2] - eye[2];
                moving.push_back({ std::sqrt(dx * dx + dy * dy + dz * dz) - l.radius,
                                   gl::Moving{ { l.position[0], l.position[1], l.position[2] }, l.radius },
                                   uint8_t(moved ? 0 : 1) });
            }
        }
        g_seen.swap(seen);
        std::sort(moving.begin(), moving.end(), [](const MovingEntry& a, const MovingEntry& b) { return a.key < b.key; });
        g_moving.clear();
        g_movingCarriedOnly.clear();
        for (const MovingEntry& e : moving)
        {
            g_moving.push_back(e.m);
            g_movingCarriedOnly.push_back(e.carriedOnly);
        }
    }

    uint32_t    g_frame = 0;
    bool        g_ran = false;
    bool        g_published = false;
    LONGLONG    g_lastCount = 0;
    LONGLONG    g_startCount = 0;

    uint32_t g_polls = 0;
    uint32_t g_wantedAt = 0;
    bool     g_wantedEver = false;

    float Hash(uint32_t x)
    {
        x ^= x >> 16; x *= 0x7FEB352Du;
        x ^= x >> 15; x *= 0x846CA68Bu;
        x ^= x >> 16;
        return float(x & 0xFFFFFF) / 16777216.0f;
    }

    /// Smooth value noise in -1..1 along time, its own sequence per seed.
    float Wave(float t, uint32_t seed)
    {
        const float i = std::floor(t);
        const float f = t - i;
        const uint32_t k = uint32_t(int64_t(i)) * 0x9E3779B1u + seed * 0x85EBCA77u;
        const float a = Hash(k), b = Hash(k + 0x9E3779B1u);
        const float s = f * f * (3.0f - 2.0f * f);
        return (a + (b - a) * s) * 2.0f - 1.0f;
    }

    /// A light's flicker at time t: its intensity factor, and for a fire a jitter of a few centimetres
    /// of its position. Seeded by the light's identity, so every consumer sees the same flicker.
    float Animate(gl::Light& l, float t, float strength)
    {
        if (l.flicker == gl::Flicker::None || strength <= 0.0f) return 1.0f;
        const uint32_t seed = uint32_t(reinterpret_cast<uintptr_t>(l.owner)) * 2654435761u + l.index * 40503u;
        float v = 0.0f;
        switch (l.flicker)
        {
        case gl::Flicker::Fire:
            // Layered breathing, and now and then a dip as the flame gutters.
            v = 0.14f * Wave(t * 1.3f, seed) + 0.09f * Wave(t * 3.7f, seed + 1) + 0.05f * Wave(t * 9.1f, seed + 2);
            v -= 0.5f * std::max(Wave(t * 0.7f, seed + 3) - 0.6f, 0.0f);
            for (int k = 0; k < 3; ++k) l.position[k] += 0.03f * strength * Wave(t * 2.3f, seed + 4 + k);
            break;
        case gl::Flicker::Candle:
            v = 0.05f * Wave(t * 7.9f, seed) + 0.03f * Wave(t * 15.3f, seed + 1);
            break;
        default:
            v = 0.02f * Wave(t * 0.9f, seed) + 0.01f * Wave(t * 3.1f, seed + 1);
            break;
        }
        const float gain = std::max(1.0f + v * strength, 0.3f);
        l.intensity *= gain;
        return gain;
    }

    // --- room gates that change smoothly -------------------------------------------------------------

    /// A light's room by identity, and how much its gate applies. Changing room, the gate opens (eases
    /// to 0) over half the time, the room switches, and it closes (eases to 1) over the other half.
    struct Gate
    {
        const void* owner = nullptr;   // the room's placement; null outside every room
        uint32_t    group = 0;
        float       weight = 1.0f;
        uint32_t    seen = 0;
    };
    std::unordered_map<uint32_t, Gate> g_gates;
    constexpr float    kGateHalf = 0.15f;    // seconds, each half of a change
    constexpr uint32_t kGateKeep = 300;      // frames a light's gate is kept once gone

    void UpdateGate(gl::Light& l, const float eye[3], float dt, uint32_t frame)
    {
        namespace rooms = gl::rooms;
        const float r[3] = { l.rest[0] - eye[0], l.rest[1] - eye[1], l.rest[2] - eye[2] };
        float lo = 0.0f, hi = 0.0f;
        // Within the group's own bounds, not its padding: a lantern standing just outside a wall
        // is an outdoor light, not a light of the room behind the wall.
        const int now = rooms::RoomOf(r, lo, hi, rooms::kPad);
        const void* owner = nullptr;
        uint32_t group = 0;
        if (now >= 0) rooms::Identity(now, owner, group);

        int room = now;
        float weight = 1.0f;
        if (l.id)
        {
            auto it = g_gates.find(l.id);
            if (it == g_gates.end()) it = g_gates.emplace(l.id, Gate{ owner, group, 1.0f, frame }).first;
            Gate& g = it->second;
            g.seen = frame;
            const float step = dt / kGateHalf;
            if (g.owner == owner && g.group == group) g.weight = std::min(g.weight + step, 1.0f);
            else
            {
                g.weight -= step;
                if (g.weight <= 0.0f)
                {
                    g.owner = owner;
                    g.group = group;
                    g.weight = 0.0f;
                }
            }
            room = g.owner ? rooms::IndexOf(g.owner, g.group) : -1;
            weight = g.weight;
        }
        l.room = room;
        // The gate carries its room's own fade, so a room joining or leaving eases the light's gate too.
        l.roomGate = room >= 0 ? weight * rooms::Weights()[room] : 0.0f;
        if (room >= 0 && rooms::Height(room, r, lo, hi))
        {
            l.roomFloor = lo + eye[2];
            l.roomCeiling = hi + eye[2];
        }
        else l.roomFloor = l.roomCeiling = 0.0f;
        l.interior = g_interiorGate && l.room >= 0 ? 1 : 0;
    }

    void ForgetGates(uint32_t frame)
    {
        if (g_gates.size() < 256) return;
        for (auto it = g_gates.begin(); it != g_gates.end();)
            it = frame - it->second.seen > kGateKeep ? g_gates.erase(it) : std::next(it);
    }

    // --- the shadow service ---------------------------------------------------------------------------

    /// Hands this frame's lights to wxl-graphics-shadow (it keys its slots on their ids), ranked by
    /// brightness over distance. Lights without an id (given ones) cast nothing.
    void GiveShadowLights(const float eye[3])
    {
        const WXL_GraphicsShadowApi* shadow = gl::Shadow();
        if (!shadow || !shadow->SetLights) return;
        static WXL_GfxShadowLight out[gl::kMaxLights];
        int n = 0;
        for (int i = 0; i < g_count; ++i)
        {
            const gl::Light& l = g_list[i];
            if (!l.id) continue;
            WXL_GfxShadowLight& s = out[n++];
            s = WXL_GfxShadowLight{};
            s.id = l.id;
            const bool tube = l.extent[0] != 0.0f || l.extent[1] != 0.0f || l.extent[2] != 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                s.position[k] = l.rest[k];
                s.direction[k] = l.direction[k];
            }
            s.radius = std::max(l.reach, l.radius);
            s.cosCone = tube ? -2.0f : l.cosCone;
            s.sourceSize = std::max(l.size, 0.01f);
            float rgb[3];
            gl::SourceIntensity(l, rgb);
            const float dx = l.rest[0] - eye[0], dy = l.rest[1] - eye[1], dz = l.rest[2] - eye[2];
            s.importance = fm::Luma(rgb) / (1.0f + (dx * dx + dy * dy + dz * dz) / std::max(s.radius * s.radius, 1.0f));
            s.flags = (l.carried ? WXL_GFX_SHADOW_LIGHT_CARRIED : 0u) | (tube ? WXL_GFX_SHADOW_LIGHT_NO_MAP : 0u);
            s.listIndex = i;
        }
        shadow->SetLights(out, n);
        for (int i = 0; i < g_count; ++i)
            g_list[i].shadowSlot = g_list[i].id && shadow->SlotOf ? shadow->SlotOf(g_list[i].id) : -1;
    }

    void ConcatGiven()
    {
        g_givenCount = 0;
        for (const GivenSet& s : g_givenSets)
        {
            if (!s.key) continue;
            const int n = std::min(s.count, gl::kMaxLights - g_givenCount);
            std::copy(s.lights, s.lights + n, g_given + g_givenCount);
            g_givenCount += n;
        }
    }

    template <class T> uint8_t Byte(T v) { return uint8_t(v); }

    /// WXL_GFX_LIGHTS_INTENSITY_<FAMILY> scales one family (spaces in its name become underscores).
    void ReadFamilyScales()
    {
        for (uint32_t f = 0; f < fm::kCount; ++f)
        {
            char key[64] = "WXL_GFX_LIGHTS_INTENSITY_";
            size_t n = std::strlen(key);
            for (const char* c = fm::Name(f); *c && n + 1 < sizeof key; ++c)
                key[n++] = *c == ' ' ? '_' : char(std::toupper(static_cast<unsigned char>(*c)));
            key[n] = '\0';
            fm::IntensityScale(f) = gl::ConfigFloat(key, 1.0f, 0.0f, 10.0f);
        }
    }
}

namespace wxl::gfx::lights
{
    Options& Settings() { return g_options; }

    void Install()
    {
        Options& o = g_options;
        o.engine = ConfigBool("WXL_GFX_LIGHTS_GATHER_ENGINE", true);
        o.wmo    = ConfigBool("WXL_GFX_LIGHTS_GATHER_WMO", true);
        o.table  = ConfigBool("WXL_GFX_LIGHTS_GATHER_TABLE", true);
        o.radius = ConfigFloat("WXL_GFX_LIGHTS_GATHER_RADIUS", o.radius, 5.0f, 300.0f);
        o.flicker = ConfigFloat("WXL_GFX_LIGHTS_FLICKER", o.flicker, 0.0f, 4.0f);
        o.carriedCore = ConfigFloat("WXL_GFX_LIGHTS_CARRIED_CORE", o.carriedCore, 0.0f, 3.0f);
        o.nearCap = ConfigFloat("WXL_GFX_LIGHTS_NEAR_CAP", o.nearCap, 0.1f, 10.0f);
        o.hotCore = ConfigFloat("WXL_GFX_LIGHTS_HOT_CORE", o.hotCore, 0.0f, 1.0f);
        o.maxPeak = ConfigFloat("WXL_GFX_LIGHTS_MAX_PEAK", o.maxPeak, 0.5f, 20.0f);
        o.maxRadius = ConfigFloat("WXL_GFX_LIGHTS_MAX_RADIUS", o.maxRadius, 5.0f, 200.0f);
        o.rooms = ConfigBool("WXL_GFX_LIGHTS_ROOMS", true);
        o.roomLeak = ConfigFloat("WXL_GFX_LIGHTS_ROOM_LEAK", o.roomLeak, 0.0f, 1.0f);
        o.merge = ConfigBool("WXL_GFX_LIGHTS_MERGE", true);
        o.mergeReach = ConfigBool("WXL_GFX_LIGHTS_MERGE_REACH", true);
        o.gain = ConfigFloat("WXL_GFX_LIGHTS_GAIN", o.gain, 0.0f, 10.0f);
        o.adaptation = ConfigFloat("WXL_GFX_LIGHTS_ADAPTATION", o.adaptation, 0.0f, 1.0f);
        o.cutoff = ConfigFloat("WXL_GFX_LIGHTS_CUTOFF", o.cutoff, 0.001f, 0.1f);
        o.legacyChroma = ConfigBool("WXL_GFX_LIGHTS_LEGACY_CHROMA", true);
        ReadFamilyScales();
        LIGHTS_LOG_INFO("lights: service installed (engine %d, wmo %d, table %d, radius %.0f, flicker %.2f, merge %d, "
                        "gain %.2f, adaptation %.2f, cutoff %.3f)",
                        o.engine, o.wmo, o.table, o.radius, o.flicker, o.merge, o.gain, o.adaptation, o.cutoff);
    }

    void SetGiven(const void* key, const WXL_GfxLight* lights, int count)
    {
        if (!key) return;
        if (!lights) count = 0;
        GivenSet* set = nullptr;
        GivenSet* free = nullptr;
        for (GivenSet& s : g_givenSets)
        {
            if (s.key == key) { set = &s; break; }
            if (!s.key && !free) free = &s;
        }
        if (!set)
        {
            if (count <= 0) return;
            if (!free)
            {
                if (!g_givenWarned)
                {
                    g_givenWarned = true;
                    LIGHTS_LOG_WARN("lights: more than %d consumers give lights; the set of %p is dropped", kGivenSets, key);
                }
                return;
            }
            set = free;
            set->key = key;
        }
        if (count <= 0)
        {
            set->key = nullptr;
            set->count = 0;
            return;
        }
        if (count > kMaxLights)
        {
            static bool cut = false;
            if (!cut)
            {
                cut = true;
                LIGHTS_LOG_WARN("lights: %p gives %d lights; the list holds %d", key, count, kMaxLights);
            }
            count = kMaxLights;
        }
        set->count = count;
        for (int i = 0; i < count; ++i) set->lights[i] = FromGiven(lights[i]);
    }

    void SetInteriorGate(bool on) { g_interiorGate = on; }

    void Want()
    {
        g_wantedAt = g_polls;
        g_wantedEver = true;
    }

    bool PollWanted()
    {
        ++g_polls;
        return g_wantedEver && g_polls - g_wantedAt <= 1;
    }

    bool Frame(IDirect3DDevice9* dev, uint32_t index)
    {
        if (g_ran && index == g_frame) return g_published;
        g_ran = true;
        g_frame = index;

        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const float dtRaw = g_lastCount ? float(double(now.QuadPart - g_lastCount) / double(frequency.QuadPart)) : 0.0f;
        const float dt = std::min(dtRaw, 0.25f);
        g_lastCount = now.QuadPart;

        float eye[3];
        cam::GetPosition(eye);
        std::memcpy(g_eye, eye, sizeof g_eye);
        ConcatGiven();
        g_count = Gather(eye, g_given, g_givenCount, dt, g_options, g_list);
        // One identity and one order for every consumer: the list sorted by id (given lights, id 0,
        // first in the order they were given), so the same lights keep the same indices.
        for (int i = 0; i < g_count; ++i) g_list[i].id = LightId(g_list[i]);
        std::stable_sort(g_list, g_list + g_count, [](const Light& a, const Light& b) {
            if (a.id != b.id) return a.id < b.id;
            if (a.kind != b.kind) return a.kind < b.kind;
            if (a.owner != b.owner) return std::less<const void*>()(a.owner, b.owner);
            return a.index < b.index;
        });
        TrackMoves(eye, index);
        if (!g_startCount) g_startCount = now.QuadPart;
        const float t = float(double(now.QuadPart - g_startCount) / double(frequency.QuadPart));
        for (int i = 0; i < g_count; ++i)
        {
            Light& l = g_list[i];
            for (int k = 0; k < 3; ++k) l.rest[k] = l.position[k];
            ApplyFamily(l, g_options);
            l.flickerGain = Animate(l, t, g_options.flicker);
        }
        sky::Update(index, dt);
        // Rooms from the placed map objects around the camera; each light's gate eases as it changes room.
        rooms::Build(eye);
        for (int i = 0; i < g_count; ++i) UpdateGate(g_list[i], eye, dt, index);
        ForgetGates(index);
        cookies::Frame(dev, g_list, g_count, eye, index);

        // The cluster space: the camera's own GL-form projection, camera-relative (the legacy layout).
        float viewProjRel[16];
        wxl::gfx::matrix::CameraRelativeViewProj(cam::GetView(), cam::GetProjection(), eye, viewProjRel);
        ClusterSpace space{ viewProjRel, { eye[0], eye[1], eye[2] }, kClusterNear, std::log(kClusterFar / kClusterNear) };
        GiveShadowLights(eye);
        g_published = Publish(dev, g_list, g_count, space);

        // The legacy omni table follows the list: the core's maps matched to it by id.
        omni::Frame(dev, g_list, g_count, eye, index);

        for (int i = 0; i < g_count; ++i)
        {
            ToAbi(g_list[i], g_abi[i]);
            ToSource(g_list[i], i, g_sources[i]);
        }
        g_abiCount = g_count;
        g_abiFrame = index;
        return g_published;
    }

    const Light* Current(int& count)
    {
        count = g_count;
        return g_list;
    }

    const WXL_GfxLight* CurrentAbi(int& count, uint32_t& frame)
    {
        count = g_abiCount;
        frame = g_abiFrame;
        return g_abiCount ? g_abi : nullptr;
    }

    const WXL_GfxLightSource* CurrentSources(int& count, uint32_t& frame)
    {
        count = g_abiCount;
        frame = g_abiFrame;
        return g_abiCount ? g_sources : nullptr;
    }

    const float* PublishedEye() { return g_eye; }

    int MovingLights(Moving* out, int max, bool withCarried)
    {
        int n = 0;
        for (size_t i = 0; i < g_moving.size() && n < max; ++i)
            if (withCarried || !g_movingCarriedOnly[i]) out[n++] = g_moving[i];
        return n;
    }

    void CarriedConstants(float out[4])
    {
        out[0] = std::clamp(g_options.carriedCore, 0.0f, 3.0f);
        out[1] = std::max(g_options.nearCap, 0.1f);
        out[2] = std::clamp(g_options.hotCore, 0.0f, 1.0f);
        out[3] = 0.0f;
    }

    float RoomCross()
    {
        return g_options.rooms ? std::clamp(g_options.roomLeak, 0.0f, 1.0f) : 1.0f;
    }

    void ClusterConstants(float clusterC[4], float clusterD[4])
    {
        clusterC[0] = float(kClustersX);
        clusterC[1] = float(kClustersY);
        clusterC[2] = float(kClustersZ);
        clusterC[3] = float(kClusterTexW);
        clusterD[0] = 1.0f / float(ClusterTextureWidth());
        clusterD[1] = 1.0f / float(ClusterTextureHeight());
        clusterD[2] = kClusterNear;
        clusterD[3] = 1.0f / std::log(kClusterFar / kClusterNear);
    }

    void SourceIntensity(const Light& l, float rgb[3])
    {
        const float s = std::max(l.power, 0.0f) * std::max(g_options.gain, 0.0f) * std::clamp(l.fade, 0.0f, 1.0f)
                      * std::max(l.flickerGain, 0.0f);
        for (int k = 0; k < 3; ++k) rgb[k] = l.chroma[k] * s;
    }

    float SourceEmissive(const Light& l)
    {
        float rgb[3];
        SourceIntensity(l, rgb);
        const float r0 = std::max(l.emissiveRadius * 0.5f, 0.02f);
        return std::min(fm::Luma(rgb) / (4.0f * r0 * r0), 400.0f);
    }

    void ToAbi(const Light& l, WXL_GfxLight& o)
    {
        std::memcpy(o.position, l.position, sizeof o.position);
        o.radius = l.radius;
        std::memcpy(o.color, l.color, sizeof o.color);
        o.intensity = l.intensity;
        std::memcpy(o.direction, l.direction, sizeof o.direction);
        o.cosCone = l.cosCone;
        o.innerRadius = l.innerRadius;
        o.interior = l.interior;
        o.kind = Byte(l.kind);
        o.flicker = Byte(l.flicker);
        o.profile = Byte(l.profile);
        o.carried = l.carried;
        o.size = l.size;
        std::memcpy(o.extent, l.extent, sizeof o.extent);
        o.owner = l.owner;
        o.index = l.index;
        o.id = l.id;
        std::memcpy(o.rest, l.rest, sizeof o.rest);
        o.cookieSourceLo = uint32_t(l.cookieSource);
        o.cookieSourceHi = uint32_t(l.cookieSource >> 32);
        std::memcpy(o.cookieRotation, l.cookieRotation, sizeof o.cookieRotation);
        o.cookieCell = l.cookieCell;
        o.cookieOpen = l.cookieOpen;
        std::memcpy(o.cookieTint, l.cookieTint, sizeof o.cookieTint);
        o.room = l.room;
        o.roomFloor = l.roomFloor;
        o.roomCeiling = l.roomCeiling;
        o.haloRadius = l.haloRadius;
        o.haloInner = l.haloInner;
    }

    void ToSource(const Light& l, int index, WXL_GfxLightSource& o)
    {
        o = WXL_GfxLightSource{};
        o.id = l.id;
        o.index = index;
        o.family = l.family;
        o.flicker = Byte(l.flicker);
        o.profile = Byte(l.profile);
        const bool tube = l.extent[0] != 0.0f || l.extent[1] != 0.0f || l.extent[2] != 0.0f;
        o.flags = uint8_t((l.carried ? WXL_GFX_LIGHT_SOURCE_CARRIED : 0u) | (tube ? WXL_GFX_LIGHT_SOURCE_TUBE : 0u)
                          | (l.cookieOpen >= 0.0f ? WXL_GFX_LIGHT_SOURCE_COOKIE : 0u) | (l.room >= 0 ? WXL_GFX_LIGHT_SOURCE_ROOM : 0u)
                          | (l.kind == Kind::M2 ? WXL_GFX_LIGHT_SOURCE_ENGINE : 0u));
        SourceIntensity(l, o.intensity);
        const bool spot = !tube && l.cosCone > -1.0f;
        for (int k = 0; k < 3; ++k)
        {
            o.position[k] = l.position[k];
            o.rest[k] = l.rest[k];
            o.direction[k] = spot ? l.direction[k] : 0.0f;
            o.extent[k] = tube ? l.extent[k] : 0.0f;
            o.flickerOffset[k] = l.position[k] - l.rest[k];
        }
        o.kelvin = l.kelvin;
        o.reach = l.reach;
        o.softRadius = l.softRadius;
        o.cosOuter = spot ? l.cosCone : -2.0f;
        o.cosInner = spot ? l.cosCone + (1.0f - l.cosCone) * 0.25f : -2.0f;
        o.emissive = SourceEmissive(l);
        o.emissiveRadius = l.emissiveRadius;
        o.flickerGain = l.flickerGain;
        o.fade = l.fade;
        o.room = l.room;
        o.roomGate = l.roomGate;
        o.shadowSlot = l.shadowSlot;
    }

    Light FromGiven(const WXL_GfxLight& in)
    {
        Light l{};
        std::memcpy(l.position, in.position, sizeof l.position);
        l.radius = in.radius;
        std::memcpy(l.color, in.color, sizeof l.color);
        l.intensity = in.intensity;
        std::memcpy(l.direction, in.direction, sizeof l.direction);
        l.cosCone = in.cosCone;
        l.innerRadius = in.innerRadius;
        l.interior = in.interior;
        l.kind = Kind::Given;
        l.flicker = in.flicker <= WXL_GFX_LIGHT_FLICKER_LANTERN ? Flicker(in.flicker) : Flicker::None;
        l.profile = in.profile <= WXL_GFX_LIGHT_PROFILE_GRILLE ? Profile(in.profile) : Profile::None;
        l.carried = in.carried ? 1 : 0;
        l.size = std::max(in.size, 0.0f);
        std::memcpy(l.extent, in.extent, sizeof l.extent);
        l.haloRadius = std::max(in.haloRadius, 0.0f);
        l.haloInner = std::max(in.haloInner, 0.0f);
        l.family = WXL_GFX_LIGHT_FAMILY_GIVEN;
        return l;
    }

    void OnDeviceLost()
    {
        ReleaseTextures();
        cookies::ReleaseTextures();
        g_published = false;
    }
}
