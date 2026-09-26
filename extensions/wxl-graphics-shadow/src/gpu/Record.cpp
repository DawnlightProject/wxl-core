// wxl-graphics-shadow: one frame's compute work -- the horizon uploads, each map's conversion from the
// core's omni atlases and its mips, the public block, the surface masks, their upsample and the debug view.
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

#include "Record.hpp"
#include "Gpu.hpp"
#include "../Shadow.hpp"
#include "../core/Extension.hpp"
#include "../core/Settings.hpp"
#include "../policy/Bodies.hpp"
#include "../policy/Slots.hpp"
#include "../sun/Sun.hpp"
#include "../terrain/Horizon.hpp"

#include "wxl/OmniShadowsApi.h"
#include "wxl/gfx/Matrix.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    using namespace wxl::gfx::shadow;
    using namespace wxl::gfx::shadow::gpu;
    namespace mx = wxl::gfx::matrix;
    namespace sl = wxl::gfx::shadow::slots;
    namespace bd = wxl::gfx::shadow::bodies;
    namespace su = wxl::gfx::shadow::sun;
    namespace hz = wxl::gfx::shadow::horizon;

    struct Block { float r[WXL_SHADOW_ROWS][4]; };
    struct Pass  { float r[SH_PASS_ROWS > SH_CONVERT_ROWS ? SH_PASS_ROWS : SH_CONVERT_ROWS][4]; };

    uint32_t g_lastW = 0, g_lastH = 0;
    float    g_exponents[2] = { 0.0f, 0.0f };
    uint32_t g_coreGeneration = 0;

    void Set(float row[4], float x, float y, float z, float w)
    {
        row[0] = x;
        row[1] = y;
        row[2] = z;
        row[3] = w;
    }

    float Bits(uint32_t v)
    {
        float f;
        std::memcpy(&f, &v, sizeof f);
        return f;
    }

    uint32_t Groups(uint32_t n, uint32_t g) { return (n + g - 1) / g; }

    bool Upload(const WXL_GfxVulkanApi* api, const void* data, size_t bytes, WXL_GfxVkAlloc& out)
    {
        if (!api->AllocUniform(bytes, &out) || !out.mapped) return false;
        std::memcpy(out.mapped, data, bytes);
        return true;
    }

    /// The core's face rows for camera-relative points (the eye folded into the constant term), three
    /// per face: (u w, v w, w); zero for a face never drawn.
    void CoreRows(const WXL_OmniShadow& s, const float eye[3], float out[18][4])
    {
        for (int f = 0; f < 6; ++f)
            for (int j = 0; j < 3; ++j)
            {
                float* row = out[f * 3 + j];
                if (!s.faceFrame[f]) { Set(row, 0.0f, 0.0f, 0.0f, 0.0f); continue; }
                const float* r = s.faceRows[f][j == 2 ? 3 : j];
                row[0] = r[0];
                row[1] = r[1];
                row[2] = r[2];
                row[3] = float(double(r[3]) + double(r[0]) * eye[0] + double(r[1]) * eye[1] + double(r[2]) * eye[2]);
            }
    }

    /// Converts each map's changed faces, then builds their mips. Returns the maps holding something.
    uint32_t ConvertMaps(const WXL_GfxVulkanApi* api, VkCommandBuffer cmd, const float eye[3], uint64_t& jobs)
    {
        jobs = 0;
        const Settings& s = Config();
        const Images& m = Img();
        const WXL_OmniShadowsApi* core = sl::Core();
        if (!core || m.atlas.image == VK_NULL_HANDLE || !m.atlasLevel[0]) return 0;
        const uint32_t generation = core->Generation();
        if (generation != g_coreGeneration)
        {
            g_coreGeneration = generation;
            ResetMaps();
        }
        const uint32_t F = m.atlasFace;
        uint32_t live = 0;
        sl::Map* maps = sl::Maps();
        for (int i = 0; i < sl::kMaps; ++i)
        {
            sl::Map& map = maps[i];
            if (!map.lightId || map.coreMain < 0) continue;
            WXL_OmniShadow main{}, units{};
            if (!core->Get(uint32_t(map.coreMain), &main) || !main.texture || main.radius <= 0.0f) continue;
            const bool haveUnits = map.coreUnits >= 0 && core->Get(uint32_t(map.coreUnits), &units) && units.texture;
            WXL_GfxVkImage mainImg{}, unitImg{};
            if (!api->ImportTexture(main.texture, &mainImg) || mainImg.image == VK_NULL_HANDLE) continue;
            if (haveUnits && (!api->ImportTexture(units.texture, &unitImg) || unitImg.image == VK_NULL_HANDLE)) continue;

            uint32_t faces = 0;
            bool complete = true;
            for (int f = 0; f < 6; ++f)
            {
                if (!(map.faceMask & (1u << f))) continue;
                const bool changed = map.reset || main.faceFrame[f] != map.converted[0][f]
                                  || (haveUnits && units.faceFrame[f] != map.converted[1][f]);
                if (changed) faces |= 1u << f;
                if (!main.faceFrame[f] || (haveUnits && !units.faceFrame[f])) complete = false;
            }
            ++live;
            if (!faces)
            {
                map.ready = map.ready || complete;
                continue;
            }

            Pass pass{};
            Set(pass.r[SH_CROW_LIGHT], main.position[0] - eye[0], main.position[1] - eye[1], main.position[2] - eye[2], main.radius);
            Set(pass.r[SH_CROW_INFO], map.housing / main.radius, haveUnits ? 1.0f : 0.0f, float(F), 0.0f);
            Set(pass.r[SH_CROW_ORIGIN], float((i % WXL_SHADOW_GRID_COLS) * WXL_SHADOW_FACE_COLS * F),
                float((i / WXL_SHADOW_GRID_COLS) * WXL_SHADOW_FACE_ROWS * F), 0.0f, 0.0f);
            Set(pass.r[SH_CROW_SIZES], float(main.faceSize * 4), float(main.faceSize * 2), float(units.faceSize * 4),
                float(units.faceSize * 2));
            CoreRows(main, eye, reinterpret_cast<float(*)[4]>(pass.r[SH_CROW_MAIN]));
            if (haveUnits) CoreRows(units, eye, reinterpret_cast<float(*)[4]>(pass.r[SH_CROW_UNITS]));

            Bind bind{};
            if (!Upload(api, &pass, sizeof pass, bind.pass)) continue;
            bind.src0 = &mainImg;
            bind.src1 = haveUnits ? &unitImg : nullptr;
            bind.out0 = m.atlasLevel[0];
            Push push{};
            push.a[0] = faces;
            if (!Dispatch(api, cmd, SH_PIPE_CONVERT, bind, push, Groups(F, 8), Groups(F, 8), 6)) continue;

            for (int f = 0; f < 6; ++f)
            {
                map.converted[0][f] = main.faceFrame[f];
                map.converted[1][f] = haveUnits ? units.faceFrame[f] : 0u;
                if (faces & (1u << f)) jobs |= 1ull << (i * 6 + f);
            }
            map.reset = false;
            map.ready = map.ready || complete;
        }
        (void)s;
        return live;
    }

    void BuildMips(const WXL_GfxVulkanApi* api, VkCommandBuffer cmd, uint64_t jobs)
    {
        const Images& m = Img();
        if (!jobs) return;
        for (uint32_t level = 1; level < WXL_SHADOW_MIPS; ++level)
        {
            const uint32_t F = m.atlasFace >> level;
            if (!F || !m.atlasLevel[level] || !m.atlasLevel[level - 1]) break;
            Bind bind{};
            bind.out0 = m.atlasLevel[level];
            bind.out1 = m.atlasLevel[level - 1];
            Push push{};
            push.a[0] = uint32_t(jobs & 0xFFFFFFFFull);
            push.a[1] = uint32_t(jobs >> 32);
            push.b[0] = float(F);
            Dispatch(api, cmd, SH_PIPE_MIPS, bind, push, Groups(F, 8), Groups(F, 8), 48);
        }
    }

    /// The public block: everything shadow.hlsli reads, camera-relative to this frame's eye.
    void BuildBlock(Block& b, const float eye[3], const su::Cascades& cascades, bool haveCascades, uint32_t liveMaps)
    {
        std::memset(&b, 0, sizeof b);
        const Settings& s = Config();
        const su::Bodies& bodies = su::Current();
        Set(b.r[WXL_SHADOW_ROW_FRAME], eye[0], eye[1], eye[2], 1.0f);
        const bool sunOn = s.sun != 0;
        Set(b.r[WXL_SHADOW_ROW_SUN], bodies.toSun[0], bodies.toSun[1], bodies.toSun[2], sunOn ? bodies.sunWeight : 0.0f);
        Set(b.r[WXL_SHADOW_ROW_MOON], bodies.toMoon[0], bodies.toMoon[1], bodies.toMoon[2], sunOn && s.moon ? bodies.moonWeight : 0.0f);

        // The cascades: their filter widens as the body they follow nears the horizon (long, soft dusk).
        const bool useCascades = sunOn && s.cascades && haveCascades && !Isolated(kIsoNoCascades);
        const int count = useCascades ? cascades.count : 0;
        float dusk = 0.0f;
        if (useCascades)
        {
            const float elevation = std::asin(std::clamp(cascades.towardsLight[2], -1.0f, 1.0f)) * 57.2957795f;
            dusk = 1.0f - std::clamp(elevation / 35.0f, 0.0f, 1.0f);
        }
        const float spacing = std::clamp(s.cascadeFilter * (1.0f + s.duskSoftness * dusk), 0.5f, 2.0f);
        Set(b.r[WXL_SHADOW_ROW_CASCADE], float(count), float(useCascades ? cascades.body : -1),
            cascades.size ? 1.0f / float(cascades.size) : 0.0f, spacing);
        Set(b.r[WXL_SHADOW_ROW_CASCADEDIR], cascades.towardsLight[0], cascades.towardsLight[1], cascades.towardsLight[2], cascades.depthScale);
        for (int m = 0; m < count; ++m)
        {
            for (int k = 0; k < 3; ++k) std::memcpy(b.r[WXL_SHADOW_ROW_CASCADEROWS + m * 3 + k], cascades.rows[m][k], sizeof(float) * 4);
            const float yardsPerTexel = 2.0f * cascades.extent[m] / float(std::max(cascades.size, 1u));
            Set(b.r[WXL_SHADOW_ROW_CASCADEPAR + m], cascades.extent[m], yardsPerTexel, s.cascadeBias, 0.0f);
        }

        const bool horizonOn = s.horizon && !Isolated(kIsoNoTerrain) && hz::Ready();
        Set(b.r[WXL_SHADOW_ROW_HORIZON], float(hz::OriginX()), float(hz::OriginY()), horizonOn ? s.horizonStrength : 0.0f,
            s.horizonPenumbra * 3.14159265f / 180.0f);
        Set(b.r[WXL_SHADOW_ROW_HORIZON2], s.horizonOccluder, 1.0f, float(hz::kSlices), 0.0f);

        Set(b.r[WXL_SHADOW_ROW_BIAS], s.normalOffset, s.slopeBias, s.minVariance, s.bleed);
        const bool capsulesOn = s.capsules && !Isolated(kIsoNoCapsules);
        Set(b.r[WXL_SHADOW_ROW_CAPSULE], capsulesOn ? float(bd::Count()) : 0.0f, s.capsulePenumbra, s.carrierMargin, 0.0f);
        Set(b.r[WXL_SHADOW_ROW_EVSM], s.evsmPositive, s.evsmNegative, s.lodBias, s.softness);
        const uint32_t F = Img().atlasFace;
        Set(b.r[WXL_SHADOW_ROW_ATLAS], F ? 1.0f / float(F * WXL_SHADOW_ATLAS_FACES_X) : 0.0f,
            F ? 1.0f / float(F * WXL_SHADOW_ATLAS_FACES_Y) : 0.0f, float(F), float(WXL_SHADOW_MIPS));

        const bd::Capsule* caps = bd::List();
        for (int k = 0; k < bd::Count(); ++k)
        {
            const bd::Capsule& c = caps[k];
            Set(b.r[WXL_SHADOW_ROW_CAPSULES + k * 2], c.a[0] - eye[0], c.a[1] - eye[1], c.a[2] - eye[2], c.radius);
            Set(b.r[WXL_SHADOW_ROW_CAPSULES + k * 2 + 1], c.b[0] - eye[0], c.b[1] - eye[1], c.b[2] - eye[2], 0.0f);
        }

        // The maps: where the core drew them from.
        const WXL_OmniShadowsApi* core = sl::Core();
        const sl::Map* maps = sl::Maps();
        bool mapValid[sl::kMaps] = {};
        for (int i = 0; i < sl::kMaps && core && liveMaps; ++i)
        {
            const sl::Map& map = maps[i];
            WXL_OmniShadow main{};
            if (!map.lightId || map.coreMain < 0 || !core->Get(uint32_t(map.coreMain), &main) || main.radius <= 0.0f) continue;
            const int row = WXL_SHADOW_ROW_MAPS + i * WXL_SHADOW_MAP_ROWS_EACH;
            Set(b.r[row], main.position[0] - eye[0], main.position[1] - eye[1], main.position[2] - eye[2], main.radius);
            Set(b.r[row + 1], float((i % WXL_SHADOW_GRID_COLS) * WXL_SHADOW_FACE_COLS) / float(WXL_SHADOW_ATLAS_FACES_X),
                float((i / WXL_SHADOW_GRID_COLS) * WXL_SHADOW_FACE_ROWS) / float(WXL_SHADOW_ATLAS_FACES_Y), map.ready ? 1.0f : 0.0f, 0.0f);
            mapValid[i] = map.ready;
        }

        // The slots.
        const sl::Slot* slots = sl::List();
        const bool mapsOn = !Isolated(kIsoNoMaps);
        for (int i = 0; i < sl::kSlots; ++i)
        {
            const sl::Slot& slot = slots[i];
            const int row = WXL_SHADOW_ROW_SLOTS + i * WXL_SHADOW_SLOT_ROWS;
            if (!slot.id || slot.weight <= 0.0f) continue;
            const WXL_GfxShadowLight& l = slot.light;
            Set(b.r[row], l.position[0] - eye[0], l.position[1] - eye[1], l.position[2] - eye[2], l.radius);
            Set(b.r[row + 1], l.direction[0], l.direction[1], l.direction[2], l.cosCone);
            const bool mapped = mapsOn && slot.map >= 0 && mapValid[slot.map];
            Set(b.r[row + 2], std::clamp(slot.weight, 0.0f, 1.0f), mapped ? std::clamp(slot.mapWeight, 0.0f, 1.0f) : 0.0f,
                std::max(l.sourceSize, 0.02f), mapped ? float(slot.map) : -1.0f);
            Set(b.r[row + 3], Bits(slot.capsuleMask), float(slot.carrier), s.carrierMargin, 0.0f);
        }

        // Light list index -> slot + 1.
        uint32_t* index = reinterpret_cast<uint32_t*>(b.r[WXL_SHADOW_ROW_INDEX]);
        for (int i = 0; i < sl::kSlots; ++i)
        {
            const sl::Slot& slot = slots[i];
            const int li = slot.light.listIndex;
            if (slot.id && slot.weight > 0.0f && li >= 0 && li < WXL_SHADOW_MAX_INDEX) index[li] = uint32_t(i + 1);
        }
    }

    /// The mask, upsample and debug passes' rows.
    void BuildPass(Pass& p, const WXL_GfxFrame& f, uint32_t traceW, uint32_t traceH, uint32_t scale, bool normals)
    {
        std::memset(&p, 0, sizeof p);
        const Settings& s = Config();
        const WXL_GfxView& v = f.view;
        mx::Columns(v.invViewProjRel, reinterpret_cast<float(*)[4]>(p.r[SH_ROW_INVVP]));
        mx::Columns(v.viewProjRel, reinterpret_cast<float(*)[4]>(p.r[SH_ROW_VP]));
        for (int k = 0; k < 3; ++k) Set(p.r[SH_ROW_VIEWROT + k], v.view[k * 4 + 0], v.view[k * 4 + 1], v.view[k * 4 + 2], 0.0f);
        Set(p.r[SH_ROW_DEPTH], f.depthRange[0], f.depthRange[1], normals ? 1.0f : 0.0f, 0.0f);
        std::memcpy(p.r[SH_ROW_LINEAR], v.depthLinearize, sizeof(float) * 4);
        Set(p.r[SH_ROW_SCREEN], float(f.width), float(f.height), 1.0f / float(f.width), 1.0f / float(f.height));
        Set(p.r[SH_ROW_TRACE], float(traceW), float(traceH), float(scale), 0.0f);
        const bool contact = s.contact && !Isolated(kIsoNoContact);
        Set(p.r[SH_ROW_CONTACT], s.contactSun, s.contactLamp, s.contactThickness, contact ? s.contactStrength : 0.0f);
        const su::Bodies& bodies = su::Current();
        const int body = !s.sun ? -1 : (bodies.night ? (s.moon && bodies.moonWeight > 0.01f ? 1 : -1) : (bodies.sunWeight > 0.01f ? 0 : -1));
        Set(p.r[SH_ROW_CONTACT2], 32.0f, 16.0f, float(body), 60.0f);   // most steps: the march takes one every 1.5 pixels
        float cs[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
        int n = 0;
        const sl::Slot* slots = sl::List();
        for (int i = 0; i < sl::kSlots && n < 4; ++i)
            if (slots[i].id && slots[i].contact) cs[n++] = float(i);
        Set(p.r[SH_ROW_CSLOTS], cs[0], cs[1], cs[2], cs[3]);
        Set(p.r[SH_ROW_DEBUG], float(s.view), float(std::clamp(s.viewSlot, 0, WXL_SHADOW_MAX_SLOTS - 1)), 0.0f, 0.0f);
    }
}

namespace wxl::gfx::shadow::gpu
{
    void ResetMaps()
    {
        sl::Map* maps = sl::Maps();
        for (int i = 0; i < sl::kMaps; ++i)
        {
            maps[i].reset = true;
            maps[i].ready = false;
        }
        Img().atlasFresh = true;
    }

    bool Prepare(const WXL_GfxVulkanApi* api, bool masks, bool debug)
    {
        if (!FollowDevice(api) || !EnsurePipelines(api) || !EnsureNeutral(api)) return false;
        hz::Prepare(api);
        const Settings& s = Config();
        if (s.maps) EnsureAtlas(api, uint32_t(s.atlasFace));
        if ((masks || debug) && g_lastW && g_lastH) EnsureScreen(api, g_lastW, g_lastH, s.halfRes != 0, debug);
        return true;
    }

    bool Record(const WXL_GfxVulkanApi* api, const WXL_GfxVkFrame& vk, bool masks, void* debugTexture)
    {
        const WXL_GfxFrame* frame = vk.frame;
        if (!frame || !vk.cmd || !Img().neutralMade) return false;
        VkCommandBuffer cmd = vk.cmd;
        const Settings& s = Config();
        const float eye[3] = { frame->view.eye[0], frame->view.eye[1], frame->view.eye[2] };
        g_lastW = frame->width;
        g_lastH = frame->height;

        // Moments of another exponent are meaningless: everything converts again.
        if (g_exponents[0] != s.evsmPositive || g_exponents[1] != s.evsmNegative)
        {
            g_exponents[0] = s.evsmPositive;
            g_exponents[1] = s.evsmNegative;
            ResetMaps();
        }

        const bool timed = s.timers && BeginTimers(cmd, vk.slot);
        Prime(api, cmd);
        hz::Update(api, cmd, eye);
        if (timed) MarkTimer(cmd, kSpanUpload);

        // The public block first: the conversion reads its exponents.
        FrameSet& set = Current();
        set = FrameSet{};
        su::Cascades cascades;
        const bool haveCascades = su::ReadCascades(eye, cascades);
        for (int c = 0; c < cascades.count && haveCascades; ++c)
            if (!api->ImportTexture(cascades.textures[c], &set.cascades[c]) || set.cascades[c].image == VK_NULL_HANDLE)
            {
                cascades.count = c;
                break;
            }
        uint64_t jobs = 0;
        static Block block;
        BuildBlock(block, eye, cascades, haveCascades, 1);
        if (!Upload(api, &block, sizeof block, set.block)) return false;
        if (hz::Volume()) set.horizon = *hz::Volume();
        if (hz::Heights()) set.heights = *hz::Heights();
        set.mapsBound = Img().atlas.image != VK_NULL_HANDLE;
        set.valid = true;

        const uint32_t live = s.maps ? ConvertMaps(api, cmd, eye, jobs) : 0;
        if (timed) MarkTimer(cmd, kSpanConvert);
        BuildMips(api, cmd, jobs);
        if (timed) MarkTimer(cmd, kSpanMips);
        // Maps that became ready this frame: the block says so from the next frame (a frame of capsules).
        (void)live;

        // The masks.
        Images& m = Img();
        bool wroteMasks = false;
        const bool debug = s.view != kViewNone && debugTexture;
        if (masks || debug)
        {
            const bool half = s.halfRes != 0;
            const bool sized = m.masks.image != VK_NULL_HANDLE && m.width == frame->width && m.height == frame->height
                            && (!half || m.halfMasks.image != VK_NULL_HANDLE);
            if (!sized)
                EnsureScreen(api, frame->width, frame->height, half, debug);   // usable from the next block
            else if (vk.depth.image != VK_NULL_HANDLE)
            {
                const bool normals = vk.normals.image != VK_NULL_HANDLE;
                const uint32_t scale = half ? 2u : 1u;
                const uint32_t tw = half ? m.halfW : m.width, th = half ? m.halfH : m.height;
                Pass pass;
                BuildPass(pass, *frame, tw, th, scale, normals);
                Bind bind{};
                if (Upload(api, &pass, sizeof pass, bind.pass))
                {
                    bind.depth = &vk.depth;
                    bind.normals = normals ? &vk.normals : nullptr;
                    bind.out0 = half ? m.halfMasks.view : m.masks.view;
                    bind.out1 = half ? m.halfDepth.view : m.oneR32.view;
                    Push push{};
                    wroteMasks = Dispatch(api, cmd, SH_PIPE_MASK, bind, push, Groups(tw, 8), Groups(th, 8), 1);
                    if (timed) MarkTimer(cmd, kSpanMask);
                    if (wroteMasks && half)
                    {
                        Bind up{};
                        up.pass = bind.pass;
                        up.depth = &vk.depth;
                        up.in0 = &m.halfMasks;
                        up.in1 = &m.halfDepth;
                        up.out0 = m.masks.view;
                        wroteMasks = Dispatch(api, cmd, SH_PIPE_UPSAMPLE, up, push, Groups(m.width, 8), Groups(m.height, 8), 1);
                    }
                    if (timed) MarkTimer(cmd, kSpanUpsample);
                    if (wroteMasks && debug && m.debug.image != VK_NULL_HANDLE)
                    {
                        Bind dbg{};
                        dbg.pass = bind.pass;
                        dbg.depth = &vk.depth;
                        dbg.normals = bind.normals;
                        dbg.in0 = &m.masks;
                        dbg.out0 = m.debug.view;
                        if (Dispatch(api, cmd, SH_PIPE_DEBUG, dbg, push, Groups(m.width, 8), Groups(m.height, 8), 1))
                            api->CopyToTexture(&m.debug, debugTexture);
                    }
                    if (timed) MarkTimer(cmd, kSpanDebug);
                }
            }
        }
        if (timed) EndTimers();

        // What consumers read this frame.
        Published& pub = Pub();
        pub.ran = true;
        pub.frameIndex = frame->frameIndex;
        pub.flags = WXL_GFX_SHADOW_FRAME_POINTS | (wroteMasks ? WXL_GFX_SHADOW_FRAME_MASKS : 0u)
                  | (s.halfRes && wroteMasks ? WXL_GFX_SHADOW_FRAME_HALFRES : 0u)
                  | (s.sun && (su::Current().sunWeight > 0.01f || su::Current().moonWeight > 0.01f) ? WXL_GFX_SHADOW_FRAME_SUN : 0u);
        pub.width = frame->width;
        pub.height = frame->height;
        pub.masks = wroteMasks ? m.masks : WXL_GfxVkImage{};
        return true;
    }
}
