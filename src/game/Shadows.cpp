// shadows: reads CShadowCache's maps and chains code after CShadowQuery's callbacks.
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

#include "game/Shadows.hpp"

#include "offsets/engine/Shadows.hpp"
#include "offsets/game/Lights.hpp"

#include <windows.h>

#include <cmath>
#include <cstring>

namespace wxl::game::shadows
{
    namespace
    {
        namespace off   = wxl::offsets::engine::shadows;
        namespace scene = wxl::offsets::game::lights;

        template <class T>
        inline T Read(uintptr_t address) { return *reinterpret_cast<const volatile T*>(address); }

        /// Depth bias CShadowQuery::GetMatrix (0x007BAC10) subtracts, by slot: main 0.1, bands
        /// 0.4 / 0.8 / 1.6, each plus 0.5 under hwPCF.
        float Bias(int slot, bool hwPcf)
        {
            static const float kBias[] = { 0.1f, 0.4f, 0.8f, 1.6f };
            return kBias[slot + 1] + (hwPcf ? 0.5f : 0.0f);
        }

        void* D3dTexture(uintptr_t handle)
        {
            if (!handle) return nullptr;
            void* gx = reinterpret_cast<off::TextureGetGxTexFn>(off::kTextureGetGxTex)(
                reinterpret_cast<void*>(handle), 1, nullptr);
            return gx ? Read<void*>(reinterpret_cast<uintptr_t>(gx) + off::kOffGxTexD3d) : nullptr;
        }

        void CopyRows(uintptr_t src, float rows[3][4])
        {
            std::memcpy(rows, reinterpret_cast<const void*>(src), sizeof(float) * 12);
        }

        uint32_t SceneFrame()
        {
            const uintptr_t sc = Read<uintptr_t>(scene::kWorldM2Scene);
            return sc ? Read<uint32_t>(sc + scene::kOffSceneFrame) : 0;
        }

        // Update detection: the engine bumps kUpdateCounter each time it renders the maps.
        uint8_t  g_lastCounter      = 0;
        uint32_t g_lastCounterFrame = 0xFFFFFFFF;
        bool     g_counterSeen      = false;

        // Recreation detection: a fingerprint of every handle and texture pointer.
        uint64_t g_fingerprint = 0;
        uint32_t g_generation  = 0;

        // --- callback chaining -------------------------------------------------------------------

        using Stock = int(__cdecl*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

        struct Subscriber { AfterFn fn; void* user; };
        struct Chain
        {
            Stock      prev = nullptr;
            Subscriber subs[8] = {};
            int        count = 0;
        };
        Chain g_chains[4];

        constexpr uintptr_t kSlotAddress[4] = {
            off::kMatrixCallback, off::kFrustumCallback, off::kQueryCallback, off::kRenderCallback,
        };

        // Five arguments are forwarded whatever the real count: the calls are __cdecl, so extra
        // stack words are read, never popped, and the stock callee ignores them.
        template <int Which>
        int __cdecl Trampoline(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e)
        {
            Chain& ch = g_chains[Which];
            const int result = ch.prev ? ch.prev(a, b, c, d, e) : 0;
            CallbackArgs args{ Callback(Which), { a, b, c, d, e }, result };
            for (int i = 0; i < ch.count; ++i) ch.subs[i].fn(args, ch.subs[i].user);
            return result;
        }

        constexpr uintptr_t kTextBegin = 0x00401000; // Wow.exe .text
        constexpr uintptr_t kTextEnd   = 0x009DE400;

        constexpr Stock kTrampolines[4] = { &Trampoline<0>, &Trampoline<1>, &Trampoline<2>, &Trampoline<3> };

        /// Puts the trampoline in the slot if the engine's pointer is there instead of ours.
        bool EnsureInstalled(int which)
        {
            Chain& ch = g_chains[which];
            if (!ch.count) return true;
            volatile uintptr_t* slot = reinterpret_cast<volatile uintptr_t*>(kSlotAddress[which]);
            const uintptr_t current = *slot;
            if (current == reinterpret_cast<uintptr_t>(kTrampolines[which])) return true;
            if (!current) return false; // CShadowQuery::Initialize has not run yet
            // Once installed, only re-wrap the engine's own function (it re-initialised): a pointer
            // outside Wow.exe's code is another binary's wrapper already chaining to us, and
            // wrapping it back would loop.
            if (ch.prev && (current < kTextBegin || current >= kTextEnd)) return true;
            ch.prev = reinterpret_cast<Stock>(current);
            *slot   = reinterpret_cast<uintptr_t>(kTrampolines[which]);
            return true;
        }
    }

    bool Get(Snapshot& out)
    {
        std::memset(&out, 0, sizeof out);
        for (int i = 0; i < 4; ++i) EnsureInstalled(i);

        const int  rawMode = Read<int>(off::kMode);
        const bool pending = Read<int>(off::kRecreatePending) != 0;
        out.mode  = rawMode;
        out.hwPcf = Read<int>(off::kHwPcf) != 0;
        out.size  = Read<uint32_t>(off::kMapSize);
        out.tier  = (rawMode >= 0 && rawMode <= 5) ? Read<int>(off::kTierByMode + (pending ? 0 : rawMode) * 4) : 0;

        for (int k = 0; k < 3; ++k)
        {
            out.lightDir[k]     = Read<float>(off::kLightDir + k * 4);
            out.lightDirView[k] = Read<float>(off::kLightDirView + k * 4);
            out.cameraPos[k]    = Read<float>(off::kCameraPos + k * 4);
        }
        const uintptr_t dev = Read<uintptr_t>(off::kGxDevicePtr);
        if (dev)
        {
            const int index = Read<int>(dev + off::kOffViewIndex);
            std::memcpy(out.view, reinterpret_cast<const void*>(dev + off::kOffViewStack + index * 0x40), sizeof out.view);
        }

        uint64_t fp = 1469598103934665603ull;
        auto mix = [&fp](uint64_t v) { fp = (fp ^ v) * 1099511628211ull; };
        mix(uint64_t(rawMode)); mix(out.size); mix(out.hwPcf);

        // Main and interior share the main centre and extent.
        const float mainExtent = Read<float>(off::kMainExtent);
        const uintptr_t mainHandle = Read<uintptr_t>(out.hwPcf ? off::kMainDepthTex : off::kMainTex);
        const uintptr_t intHandle  = Read<uintptr_t>(off::kInteriorTex);
        const uintptr_t handles[2] = { mainHandle, intHandle };
        for (int m = 0; m < 2; ++m)
        {
            Map& map = out.maps[m];
            map.present = handles[m] != 0;
            if (!map.present) continue;
            map.texture    = D3dTexture(handles[m]);
            map.halfExtent = mainExtent;
            map.depthBias  = Bias(-1, out.hwPcf);
            for (int k = 0; k < 3; ++k) map.centre[k] = Read<float>(off::kCentre + k * 4);
            CopyRows(off::kMainRows, map.rows);
            mix(handles[m]); mix(reinterpret_cast<uintptr_t>(map.texture));
        }

        for (size_t b = 0; b < off::kBandCount; ++b)
        {
            Map& map = out.maps[size_t(Slot::Band0) + b];
            const uintptr_t rec = off::kBands + b * off::kBandStride;
            const int current   = Read<int>(rec + off::kOffBandCurrent) ? 1 : 0;
            const uintptr_t handle = Read<uintptr_t>(rec + off::kOffBandTex + current * 4);
            map.present = rawMode >= 3 && handle != 0;
            if (!map.present) continue;
            map.texture    = D3dTexture(handle);
            map.halfExtent = Read<float>(rec + off::kOffBandExtent);
            map.depthBias  = Bias(int(b), out.hwPcf);
            for (int k = 0; k < 3; ++k) map.centre[k] = Read<float>(rec + off::kOffBandCentre + k * 4);
            CopyRows(off::kBandRows + b * off::kRowsStride, map.rows);
            mix(handle); mix(reinterpret_cast<uintptr_t>(map.texture));
        }

        if (fp != g_fingerprint) { g_fingerprint = fp; ++g_generation; }
        out.generation = g_generation;

        const uint8_t  counter = Read<uint8_t>(off::kUpdateCounter);
        const uint32_t frame   = SceneFrame();
        if (!g_counterSeen || counter != g_lastCounter)
        {
            g_counterSeen      = true;
            g_lastCounter      = counter;
            g_lastCounterFrame = frame;
        }
        const bool updated = g_lastCounterFrame == frame;

        out.valid = rawMode > 0 && !pending && updated && out.maps[0].texture != nullptr;
        return out.valid;
    }

    bool DescribeRender(const CallbackArgs& a, RenderPass& out)
    {
        std::memset(&out, 0, sizeof out);
        out.slot = Slot::Count;
        if (a.which != Callback::Render || !a.arg[0]) return false;
        const uintptr_t query = a.arg[0];
        const int slot = int(a.arg[1]);
        if (slot < 0 || slot > 2) return false;
        out.index = slot;
        out.hwPcf = Read<int>(off::kHwPcf) != 0;
        out.depthScale = 1.0f / off::kLightFar;

        // Which map: the colour and depth CGxTex the engine passed, against the cache's handles.
        auto gxOf = [](uintptr_t handle) -> uintptr_t {
            if (!handle) return 0;
            return reinterpret_cast<uintptr_t>(reinterpret_cast<off::TextureGetGxTexFn>(off::kTextureGetGxTex)(
                reinterpret_cast<void*>(handle), 1, nullptr));
        };
        const uintptr_t colour = a.arg[2], depth = a.arg[3];
        const uintptr_t interior = gxOf(Read<uintptr_t>(off::kInteriorTex));
        const uintptr_t mainCol  = gxOf(Read<uintptr_t>(off::kMainTex));
        const uintptr_t mainDep  = gxOf(Read<uintptr_t>(off::kMainDepthTex));
        const uintptr_t mapTex   = out.hwPcf ? depth : colour;
        if (mapTex && mapTex == interior) out.slot = Slot::Interior;
        else if (mapTex && mapTex == (out.hwPcf ? mainDep : mainCol)) out.slot = Slot::Main;
        else
            for (size_t b = 0; b < off::kBandCount && out.slot == Slot::Count; ++b)
            {
                const uintptr_t rec = off::kBands + b * off::kBandStride;
                for (int p = 0; p < 2; ++p)
                    if (mapTex && mapTex == gxOf(Read<uintptr_t>(rec + off::kOffBandTex + p * 4)))
                        out.slot = Slot(size_t(Slot::Band0) + b);
            }
        out.colour = colour ? Read<void*>(colour + off::kOffGxTexD3d) : nullptr;
        out.depth  = depth ? Read<void*>(depth + off::kOffGxTexD3d) : nullptr;

        for (int k = 0; k < 3; ++k)
        {
            out.centre[k]    = Read<float>(query + off::kOffQueryCentre + slot * 12 + k * 4);
            out.lightDir[k]  = Read<float>(off::kLightDir + k * 4);
            out.up[k]        = a.arg[4] ? Read<float>(a.arg[4] + k * 4) : (k == 0 ? 1.0f : 0.0f);
            out.cameraPos[k] = Read<float>(off::kCameraPos + k * 4);
        }
        out.halfExtent = Read<float>(query + off::kOffQueryExtent + slot * 4);
        for (int k = 0; k < 4; ++k) out.viewport[k] = Read<float>(query + off::kOffQueryViewport + slot * 16 + k * 4);
        const uint32_t* list = reinterpret_cast<const uint32_t*>(off::kDrawLists + slot * off::kDrawListStride);
        out.drewNothing = !list[0] && !list[4] && !list[7];

        // The look-at as GxuXformCreateLookAtXXX (0x006C0050) builds it: z = forward, x = up x z,
        // y = z x x, rows are the axes' components, last row -eye in that basis.
        float eye[3], zv[3], xv[3], yv[3];
        for (int k = 0; k < 3; ++k)
        {
            eye[k] = out.centre[k] - out.lightDir[k] * off::kLightDistance - out.cameraPos[k];
            zv[k]  = out.lightDir[k] * off::kLightDistance;
        }
        auto normalise = [](float v[3]) {
            const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (l > 1e-12f) for (int k = 0; k < 3; ++k) v[k] /= l;
        };
        auto cross = [](const float p[3], const float q[3], float o[3]) {
            o[0] = p[1] * q[2] - p[2] * q[1];
            o[1] = p[2] * q[0] - p[0] * q[2];
            o[2] = p[0] * q[1] - p[1] * q[0];
        };
        normalise(zv);
        cross(out.up, zv, xv); normalise(xv);
        cross(zv, xv, yv);     normalise(yv);
        float* v = out.view;
        for (int k = 0; k < 16; ++k) v[k] = 0.0f;
        v[0] = xv[0]; v[1] = yv[0]; v[2]  = zv[0];
        v[4] = xv[1]; v[5] = yv[1]; v[6]  = zv[1];
        v[8] = xv[2]; v[9] = yv[2]; v[10] = zv[2];
        v[12] = -(eye[0] * xv[0] + eye[1] * xv[1] + eye[2] * xv[2]);
        v[13] = -(eye[0] * yv[0] + eye[1] * yv[1] + eye[2] * yv[2]);
        v[14] = -(eye[0] * zv[0] + eye[1] * zv[1] + eye[2] * zv[2]);
        v[15] = 1.0f;

        // The pass's ortho as GxuXformCreateOrtho left it (z to -1..1), then the D3D device's own
        // conversion (CGxDeviceD3d::IXformSetProjection): z to 0..1 over the same near and far.
        std::memcpy(out.proj, reinterpret_cast<const void*>(query + off::kOffQueryProjection + slot * 64), sizeof out.proj);
        float* p = out.proj;
        if (std::fabs(p[10]) > 1e-12f)
        {
            const float zn = (-1.0f - p[14]) / p[10];
            const float zf = (1.0f - p[14]) / p[10];
            p[10] = 1.0f / (zf - zn);
            p[14] = zn / (zn - zf);
        }
        return out.slot != Slot::Count;
    }

    bool ChainAfter(Callback which, AfterFn fn, void* user)
    {
        const int w = int(which);
        if (w < 0 || w > 3 || !fn) return false;
        Chain& ch = g_chains[w];
        if (ch.count >= 8) return false;
        if (!reinterpret_cast<volatile uintptr_t*>(kSlotAddress[w])[0]) return false;
        ch.subs[ch.count++] = { fn, user };
        if (!EnsureInstalled(w)) { --ch.count; return false; }
        return true;
    }
}
