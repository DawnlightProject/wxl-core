// Omni shadows: re-issues the engine's main shadow-map casters from chosen point lights into 6-face
// R32F atlases, the way CShadowQuery::Render (0x007BBC50) draws its own maps, optionally split into
// static and unit casters. "wxl.omnishadows" v3 (also published as v2).
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

#include "common/Config.hpp"
#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "engine/hook/Registry.hpp"
#include "game/Gx.hpp"
#include "game/Shadows.hpp"
#include "game/Unit.hpp"
#include "game/World.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/engine/Shadows.hpp"
#include "offsets/game/Lights.hpp"
#include "runtime/Extensions.hpp"
#include "wxl/OmniShadowsApi.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
    namespace off = wxl::offsets::engine::shadows;
    namespace sh  = wxl::game::shadows;

    // --- engine calls CShadowCache::Create (0x00875D30) makes for its own targets ------------------
    constexpr uintptr_t kTextureCreate  = 0x004B8C80; // __cdecl, 11 args, -> HTEXTURE
    using TextureCreateFn = void*(__cdecl*)(int, uint32_t, uint32_t, int, int, int, uint32_t, int, void*, const char*, int);
    constexpr uintptr_t kTexFlagsCtor   = 0x00681BE0; // CGxTexFlags::CGxTexFlags __thiscall, 10 args
    using TexFlagsCtorFn = void(__fastcall*)(uint32_t* self, void* edx, int, int, int, int, int, int, int, int, int, int);
    constexpr uintptr_t kTexCallback    = 0x005EEB70; // the no-op the shadow cache passes
    constexpr uintptr_t kTextureRelease = 0x0047BF30; // __cdecl(HTEXTURE): drops a handle reference
    using TextureReleaseFn = void(__cdecl*)(void* handle);
    constexpr uintptr_t kBindTarget     = 0x0057E4F0; // __cdecl(slot, CGxTex*, face): engine render-target bind
    using BindTargetFn = void(__cdecl*)(int slot, void* gxTex, int face);
    constexpr uintptr_t kReadTarget     = 0x00682D50; // __thiscall(device, slot, CGxTex** out)
    using ReadTargetFn = void(__fastcall*)(void* device, void* edx, int slot, void** out);
    constexpr uintptr_t kSetViewport    = 0x00681F60; // __cdecl(x0, x1, y0, y1, z0, z1), fractions of the target
    using SetViewportFn = void(__cdecl*)(float, float, float, float, float, float);
    constexpr uintptr_t kSceneClear     = 0x006813B0; // __cdecl(mask, argb)
    using SceneClearFn = void(__cdecl*)(int mask, uint32_t argb);
    constexpr uintptr_t kCameraPosition = 0x004F6650; // CGWorldFrame::GetCameraPosition __cdecl(float*)
    using CameraPositionFn = void(__cdecl*)(float* out);
    constexpr int kFmtR32F = 0xB, kFmtD24X8 = 0xC;
    // A power-of-two atlas: 4 x 2 cells, faces in the first six.
    constexpr uint32_t kCols = 4, kRows = 2;
    constexpr size_t   kOffWindowHeight = 0x17C; // CGxDevice current window (DeviceCurWindow = +0x174), floats
    constexpr size_t   kOffWindowWidth  = 0x180;

    constexpr uint32_t kMaxLights = WXL_OMNISHADOWS_MAX_V3;

    struct Light
    {
        WXL_OmniLight params{};
        uint32_t flags    = 0;         // WXL_OMNI_CASTERS_* | WXL_OMNI_REDRAW_OCCUPIED (0: every caster, as v2)
        uint32_t faceMask = 0x3F;      // faces rendered
        uint32_t wantSize = 0;         // face size asked for (0: the default)
        uint32_t occupied = 0;         // bit f: face f held a unit when last rendered
        uint32_t seen     = 0;         // bit f: face f sees a unit this frame
        void*    colour = nullptr;   // HTEXTURE
        uint32_t size   = 0;
        WXL_OmniShadow state{};
        WXL_OmniShadowState track{};
        float    anchor[3]{};          // position the atlas was last fully rendered from
        float    anchorRadius = 0.0f;
        uint32_t casters[6]  = {};     // per face: hash of the units that face sees
        bool     haveCasters = false;
        bool     needsClear  = true;   // atlas to 1 before any face (new texture, slot reassigned)
        bool     priority    = false;  // the faces in dirtyFaces this frame, outside the budget
        uint32_t dirtyFaces  = 0;      // bit f: face f changed (the light moved, or a unit it sees)
        void*    d3dSeen     = nullptr; // D3D texture the contents belong to
        float    coverage[6] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f }; // per face: Coverage() when last drawn (-1 never)
    };

    Light    g_lights[kMaxLights];

    // The engine's main exterior map this frame: the window its caster lists were culled to. It follows
    // the camera, so as the camera turns, casters around a lamp leave the lists and come back. A static
    // map redrawn from lists that hold less of its surroundings than when it was last drawn loses their
    // shadows: they popped out and in as the camera moved. See Coverage.
    struct Window { bool valid; float centre[3], x[3], y[3], half; };
    Window   g_window = {};
    uint32_t g_count      = 0;
    uint32_t g_budget     = 6;
    uint32_t g_faceSize   = 512;
    // Shared D24X8 atlases, one per face size in use (64, 128, 256, 512, 1024).
    void*    g_depths[5]  = {};
    const void* g_owner   = nullptr; // the claim's holder; null while free
    uint32_t g_facesLast  = 0;       // faces rendered on the last frame
    uint32_t g_staticHash = 0;       // the engine's static casters this frame, as a set
    uint32_t g_staticSeen = 0;       // the set static maps were last refreshed for
    uint32_t g_generation = 1;
    uint32_t g_cursor     = 0;       // round-robin over (light, face)
    uint32_t g_lastFrame  = 0xFFFFFFFF;
    bool     g_chained    = false;
    bool     g_diag       = false;
    uint32_t g_faces      = 0;       // faces rendered since the last log line
    unsigned long long g_nextLog = 0;
    unsigned long long g_lastSet = 0;  // when a consumer last called SetLights
    bool     g_enabled    = true;    // WXL_OMNI_SHADOWS
    constexpr unsigned long long kConsumerTimeoutMs = 1000;
    constexpr float    kMoveThreshold = 0.05f; // yards
    constexpr uint32_t kAllFaces      = 0x3F;
    bool     g_costLogged = false;

    inline void* Engine() { return *reinterpret_cast<void* const*>(off::kGxDevicePtr); }

    template <class Fn> inline Fn Vt(void* obj, size_t offset)
    {
        return *reinterpret_cast<Fn*>(*reinterpret_cast<uintptr_t*>(obj) + offset);
    }

    void* GxOf(void* handle)
    {
        return handle ? reinterpret_cast<off::TextureGetGxTexFn>(off::kTextureGetGxTex)(handle, 1, nullptr) : nullptr;
    }

    void* D3dOf(void* handle)
    {
        void* gx = GxOf(handle);
        return gx ? *reinterpret_cast<void**>(uintptr_t(gx) + off::kOffGxTexD3d) : nullptr;
    }

    void* CreateTarget(uint32_t w, uint32_t h, int format, bool depth)
    {
        uint32_t flags = 0;
        reinterpret_cast<TexFlagsCtorFn>(kTexFlagsCtor)(&flags, nullptr, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0);
        flags = (flags & 0xFFFFFFE0u) | 0x80;
        flags = depth ? ((flags & 0xFFFFFFF9u) | 1) : (flags & 0xFFFFFFF8u); // as CShadowCache::Create
        return reinterpret_cast<TextureCreateFn>(kTextureCreate)(0, w, h, 0, format, format, flags, 0,
                                                                 reinterpret_cast<void*>(kTexCallback), "OmniShadow", 0);
    }

    // --- 4x4, row vectors -------------------------------------------------------------------------
    void Mul(const float* a, const float* b, float* o)
    {
        float r[16];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
        std::memcpy(o, r, sizeof r);
    }

    bool Inverse(const float* m, float* o)
    {
        float inv[16];
        inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
        inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
        inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
        inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
        inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
        inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
        inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
        inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
        inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
        inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
        inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
        inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
        inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
        inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
        inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
        inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
        const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
        if (std::fabs(det) < 1e-12f) return false;
        for (int i = 0; i < 16; ++i) o[i] = inv[i] / det;
        return true;
    }

    // --- resources ----------------------------------------------------------------------------------
    // An atlas replaced (its face size changed) is released a few frames later: a consumer may have read
    // its texture pointer before the world pass that replaced it, and imports it after.
    struct Retired { void* handle; uint32_t frame; };
    Retired  g_retired[kMaxLights * 2] = {};
    constexpr uint32_t kRetireFrames = 4;

    void Retire(void* handle, uint32_t frame)
    {
        for (Retired& r : g_retired)
            if (!r.handle) { r = Retired{ handle, frame }; return; }
        reinterpret_cast<TextureReleaseFn>(kTextureRelease)(handle);   // full: release at once
    }

    void ReleaseRetired(uint32_t frame)
    {
        for (Retired& r : g_retired)
            if (r.handle && frame - r.frame >= kRetireFrames)
            {
                reinterpret_cast<TextureReleaseFn>(kTextureRelease)(r.handle);
                r = Retired{};
            }
    }

    uint32_t SizeOf(const Light& l) { return l.wantSize ? l.wantSize : g_faceSize; }

    int DepthIndex(uint32_t size)
    {
        int i = 0;
        for (uint32_t p = 64; p < size && i < 4; p *= 2) ++i;
        return i;
    }

    void* DepthFor(uint32_t size) { return g_depths[DepthIndex(size)]; }

    bool EnsureTargets(uint32_t frame)
    {
        ReleaseRetired(frame);
        bool changed = false;
        for (uint32_t i = 0; i < g_count; ++i)
        {
            Light& l = g_lights[i];
            const uint32_t size = SizeOf(l);
            void*& depth = g_depths[DepthIndex(size)];
            if (!depth)
            {
                depth = CreateTarget(size * kCols, size * kRows, kFmtD24X8, true);
                changed = true;
            }
            if (l.colour && l.size == size) continue;
            if (l.colour) Retire(l.colour, frame);
            l.colour = CreateTarget(size * kCols, size * kRows, kFmtR32F, false);
            l.size   = size;
            std::memset(l.state.faceFrame, 0, sizeof l.state.faceFrame);
            l.needsClear = true;
            l.track.staleFaces = kAllFaces;
            changed = true;
        }
        if (changed)
        {
            ++g_generation;
            WLOG_INFO("omni-shadows: atlases for %u light(s) (default face %u), generation %u", g_count, g_faceSize, g_generation);
        }
        for (uint32_t i = 0; i < g_count; ++i)
            if (!DepthFor(SizeOf(g_lights[i]))) return false;
        return true;
    }

    /// True when the device's render target 0 is this light's atlas, at the atlas size and format, and
    /// the engine's current window (what it converts fractional viewports against) is the atlas too.
    /// The engine falls back to its cached back buffer (device +0x3B3C) when a target texture has no
    /// D3D surface, so a failed bind would otherwise draw the faces over the world.
    bool AtlasBound(void* dev, Light& l)
    {
        auto* d = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
        void* gx = GxOf(l.colour);
        auto* tex = gx ? *reinterpret_cast<IDirect3DTexture9**>(uintptr_t(gx) + off::kOffGxTexD3d) : nullptr;
        IDirect3DSurface9* want = nullptr;
        IDirect3DSurface9* bound = nullptr;
        if (tex) tex->GetSurfaceLevel(0, &want);
        if (d) d->GetRenderTarget(0, &bound);
        D3DSURFACE_DESC desc{};
        if (bound) bound->GetDesc(&desc);
        const float winW = *reinterpret_cast<const float*>(uintptr_t(dev) + kOffWindowWidth);
        const float winH = *reinterpret_cast<const float*>(uintptr_t(dev) + kOffWindowHeight);
        const uint32_t w = l.size * kCols, h = l.size * kRows;
        const bool ok = want && bound == want && desc.Width == w && desc.Height == h && desc.Format == D3DFMT_R32F &&
                        uint32_t(winW) == w && uint32_t(winH) == h;
        if (!ok)
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                WLOG_ERROR("omni-shadows: atlas not bound, pass refused (gxTex %p d3dTex %p surface %p, bound %p %ux%u fmt %d, "
                           "engine window %.0fx%.0f, expected %ux%u R32F)", gx, (void*)tex, (void*)want, (void*)bound,
                           desc.Width, desc.Height, int(desc.Format), winW, winH, w, h);
            }
        }
        static bool announced = false;
        if (ok && !announced)
        {
            announced = true;
            WLOG_INFO("omni-shadows: atlas bound for the face passes (%ux%u R32F, engine window %.0fx%.0f)", w, h, winW, winH);
        }
        if (want) want->Release();
        if (bound) bound->Release();
        return ok;
    }

    // What the pass changes, captured before its first change and put back on every exit. RsPush /
    // RsPop cover render states only: the engine's fractional viewport (device +0xF70..+0xF84, what
    // GxXformSetViewport writes), its view and projection and its bound targets are restored here,
    // in the order CShadowQuery::Render (0x007BBC50) restores its own.
    constexpr size_t kOffViewport = 0xF70; // float x0, x1, y0, y1, z0, z1

    struct Saved
    {
        bool  active = false;   // something was changed and not yet put back
        bool  pushed = false;   // RsPush happened
        float viewport[6];
        float view[16];
        float proj[16];
        void* colour = nullptr;
        void* depth  = nullptr;
        float window[4];          // the engine's current window (+0x174), what viewports resolve against
        D3DVIEWPORT9 d3dViewport{};
        bool  haveD3dViewport = false;
        // The engine's process-wide shader-constant caches (vertex, pixel): the pass writes c0 and
        // c14..c16 itself and the engine's shadow draws write more, and no later draw is bound to
        // rewrite them, so every register the pass changed is put back on the way out.
        float vs[256 * 4];
        float ps[256 * 4];
    };
    Saved g_saved;

    void Capture(void* dev)
    {
        const uintptr_t base = uintptr_t(dev);
        std::memcpy(g_saved.viewport, reinterpret_cast<const void*>(base + kOffViewport), sizeof g_saved.viewport);
        std::memcpy(g_saved.proj, reinterpret_cast<const void*>(base + off::kOffProjection), sizeof g_saved.proj);
        const int viewIndex = *reinterpret_cast<const int*>(base + off::kOffViewIndex);
        std::memcpy(g_saved.view, reinterpret_cast<const void*>(base + off::kOffViewStack + viewIndex * 0x40), sizeof g_saved.view);
        g_saved.colour = nullptr;
        g_saved.depth  = nullptr;
        reinterpret_cast<ReadTargetFn>(kReadTarget)(dev, nullptr, 0, &g_saved.colour);
        reinterpret_cast<ReadTargetFn>(kReadTarget)(dev, nullptr, 1, &g_saved.depth);
        std::memcpy(g_saved.window, reinterpret_cast<const void*>(base + kOffWindowHeight - 8), sizeof g_saved.window);
        auto* d = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
        g_saved.haveD3dViewport = d && SUCCEEDED(d->GetViewport(&g_saved.d3dViewport));
        namespace gxo = wxl::offsets::engine::gx;
        std::memcpy(g_saved.vs, reinterpret_cast<const void*>(gxo::kVsConstCache), sizeof g_saved.vs);
        std::memcpy(g_saved.ps, reinterpret_cast<const void*>(gxo::kPsConstCache), sizeof g_saved.ps);
        g_saved.pushed = false;
        g_saved.active = true;
    }

    /// Writes back, through the engine's own setter (so its cache and dirty range stay true), every
    /// run of registers whose cached value differs from the one saved before the pass.
    void RestoreConstants(void* dev, int target, const float* saved, uintptr_t cache)
    {
        const float* now = reinterpret_cast<const float*>(cache);
        int reg = 0;
        while (reg < 256)
        {
            if (std::memcmp(now + reg * 4, saved + reg * 4, 16) == 0) { ++reg; continue; }
            int end = reg + 1;
            while (end < 256 && std::memcmp(now + end * 4, saved + end * 4, 16) != 0) ++end;
            Vt<off::SetConstantsFn>(dev, off::kVtConstants)(dev, nullptr, target, reg, saved + reg * 4, end - reg);
            reg = end;
        }
    }

    void SetEngineViewport(const float v[6])
    {
        reinterpret_cast<SetViewportFn>(kSetViewport)(v[0], v[1], v[2], v[3], v[4], v[5]);
    }

    void Restore(void* dev)
    {
        if (!g_saved.active) return;
        g_saved.active = false;
        const uintptr_t base = uintptr_t(dev);
        if (g_saved.pushed) reinterpret_cast<off::RsFn>(off::kRsPop)(dev, nullptr);
        g_saved.pushed = false;
        SetEngineViewport(g_saved.viewport);
        Vt<off::SetMatrixFn>(dev, off::kVtView)(dev, nullptr, g_saved.view);
        if (*reinterpret_cast<const int*>(base + off::kOffRsEnabled))
        {
            int* scissor = reinterpret_cast<int*>(*reinterpret_cast<const uintptr_t*>(base + off::kOffRsState) + 0x1E0);
            if (*scissor) { reinterpret_cast<off::RsDirtyFn>(off::kRsDirty)(dev, nullptr, 0x14); *scissor = 0; }
        }
        Vt<void(__fastcall*)(void*, void*, int, void*, int)>(dev, 0x5C)(dev, nullptr, 0, g_saved.colour, 0);
        reinterpret_cast<BindTargetFn>(kBindTarget)(1, g_saved.depth, 0);
        // The target binds re-apply the viewport fields against the restored window; set them again
        // so the engine's fractions are exactly the saved ones whatever the binds did.
        SetEngineViewport(g_saved.viewport);
        Vt<off::SetMatrixFn>(dev, off::kVtProjection)(dev, nullptr, g_saved.proj);
        reinterpret_cast<off::UpdateProjMatrixFn>(off::kUpdateProjMatrix)();
        // Last, after the matrix updates (which may write constants of their own).
        namespace gxo = wxl::offsets::engine::gx;
        RestoreConstants(dev, 0, g_saved.vs, gxo::kVsConstCache);
        RestoreConstants(dev, 4, g_saved.ps, gxo::kPsConstCache);
        // The D3D viewport at once too; the engine re-derives the same one from its fields at its next draw.
        if (g_saved.haveD3dViewport)
            if (auto* d = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice())) d->SetViewport(&g_saved.d3dViewport);
    }

    /// After a pass: the engine's viewport fractions are the saved ones, and the D3D viewport is the
    /// one the engine derives from them for the restored target (IStateSyncXforms, 0x006A6xxx).
    bool PostConditionHolds(void* dev)
    {
        const uintptr_t base = uintptr_t(dev);
        const float* now = reinterpret_cast<const float*>(base + kOffViewport);
        for (int i = 0; i < 6; ++i)
            if (std::fabs(now[i] - g_saved.viewport[i]) > 1e-6f) return false;

        // The window viewports resolve against is the one from before the pass, not the atlas.
        const float* win = reinterpret_cast<const float*>(base + kOffWindowHeight - 8);
        for (int i = 0; i < 4; ++i)
            if (win[i] != g_saved.window[i]) return false;
        auto* d = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
        D3DVIEWPORT9 vp{};
        if (g_saved.haveD3dViewport && d && SUCCEEDED(d->GetViewport(&vp)))
            return vp.X == g_saved.d3dViewport.X && vp.Y == g_saved.d3dViewport.Y &&
                   vp.Width == g_saved.d3dViewport.Width && vp.Height == g_saved.d3dViewport.Height;
        return true;
    }

    // --- caster classes -------------------------------------------------------------------------------
    // The engine's M2 batch lists hold 12-byte entries {CM2Model*, batch, group} read through list[0]
    // (array) and list[1] (count) by CM2Model::RenderModelBatchListShadowMap and RenderBatchShadowMap;
    // a copy with a group of 1 per entry takes their plain, non-instanced path. An entry is dynamic
    // when the root of its model's attachment chain is a unit's body model.
    struct Entry { uint32_t model, batch, group; };
    struct List  { uint32_t data, count, capacity; };

    std::vector<Entry> g_split[4];      // static opaque, static alpha, dynamic opaque, dynamic alpha
    List     g_lists[4] = {};
    uint32_t g_splitFrame = 0xFFFFFFFF;
    std::vector<uintptr_t> g_unitRoots; // sorted body models of this frame's units

    bool IsUnitModel(uintptr_t model)
    {
        for (int depth = 0; model && depth < 8; ++depth)
        {
            void* parent = wxl::game::unit::ModelParent(reinterpret_cast<void*>(model));
            if (!parent) break;
            model = reinterpret_cast<uintptr_t>(parent);
        }
        size_t lo = 0, hi = g_unitRoots.size();
        while (lo < hi)
        {
            const size_t mid = (lo + hi) / 2;
            if (g_unitRoots[mid] < model) lo = mid + 1; else hi = mid;
        }
        return lo < g_unitRoots.size() && g_unitRoots[lo] == model;
    }

    uint32_t Mix(uint32_t h);

    /// Splits this frame's main-pass M2 lists by class, once per frame, and hashes the static set.
    void SplitCasters(int slot, uint32_t frame)
    {
        if (g_splitFrame == frame) return;
        g_splitFrame = frame;
        namespace world = wxl::game::world;
        g_unitRoots.clear();
        world::ForEachObject(world::kTypeMaskUnit, [&](unsigned long long, void* unit) {
            if (void* m = wxl::game::unit::Model(unit)) g_unitRoots.push_back(reinterpret_cast<uintptr_t>(m));
            return true;
        });
        std::sort(g_unitRoots.begin(), g_unitRoots.end());
        const uint32_t* list = reinterpret_cast<const uint32_t*>(off::kDrawLists + slot * off::kDrawListStride);
        uint32_t hash = Mix(list[0] ^ Mix(list[1] ^ Mix(list[2])));
        for (int kind = 0; kind < 2; ++kind)   // 0 opaque (list + 3), 1 alpha (list + 6)
        {
            std::vector<Entry>& st = g_split[kind];
            std::vector<Entry>& dy = g_split[2 + kind];
            st.clear();
            dy.clear();
            const uint32_t* src = list + (kind ? 6 : 3);
            const Entry* entries = reinterpret_cast<const Entry*>(uintptr_t(src[0]));
            const uint32_t count = entries ? src[1] : 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                Entry e = entries[i];
                e.group = 1;
                if (IsUnitModel(e.model)) dy.push_back(e);
                else
                {
                    st.push_back(e);
                    hash += Mix(e.model ^ (e.batch << 16));
                }
            }
        }
        for (int k = 0; k < 4; ++k)
        {
            const uint32_t n = uint32_t(g_split[k].size());
            g_lists[k] = List{ n ? uint32_t(uintptr_t(g_split[k].data())) : 0u, n, n };
        }
        g_staticHash = hash;
    }

    // --- the pass ------------------------------------------------------------------------------------
    const float kAxes[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    const float kUps[6][3]  = { { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };

    /// How completely the engine's lists hold what face f of a light sees: 1 when the region the face
    /// covers (half its reach along the axis, that wide across) lies inside the main map's window, down
    /// to 0 as it leaves it; 1 when the window is unknown. A static face is redrawn only when this is at
    /// least what it was when the face was last drawn, so a redraw never holds fewer casters.
    float Coverage(const Light& l, int f)
    {
        if (!g_window.valid) return 1.0f;
        const float r = std::fmax(l.params.radius, 1.0f);
        float d[3];
        for (int k = 0; k < 3; ++k) d[k] = l.params.position[k] + kAxes[f][k] * 0.5f * r - g_window.centre[k];
        const float ax = std::fabs(d[0] * g_window.x[0] + d[1] * g_window.x[1] + d[2] * g_window.x[2]);
        const float ay = std::fabs(d[0] * g_window.y[0] + d[1] * g_window.y[1] + d[2] * g_window.y[2]);
        return std::clamp((g_window.half - std::fmax(ax, ay)) / (0.5f * r), 0.0f, 1.0f);
    }

    void RenderFaces(void* query, int slot, uint32_t frame)
    {
        void* dev = Engine();
        const uint32_t* list = reinterpret_cast<const uint32_t*>(off::kDrawLists + slot * off::kDrawListStride);
        const bool anything = list[0] || list[4] || list[7];

        const uintptr_t base = uintptr_t(dev);
        float camView[16], invCamView[16];
        const int viewIndex = *reinterpret_cast<const int*>(base + off::kOffViewIndex);
        std::memcpy(camView, reinterpret_cast<const void*>(base + off::kOffViewStack + viewIndex * 0x40), sizeof camView);
        if (!Inverse(camView, invCamView)) return;
        Capture(dev);

        float cam[3];
        reinterpret_cast<CameraPositionFn>(kCameraPosition)(cam);
        const float toCamRel[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -cam[0], -cam[1], -cam[2], 1 };

        reinterpret_cast<off::RsFn>(off::kRsPush)(dev, nullptr);
        g_saved.pushed = true;
        const uintptr_t rs = *reinterpret_cast<const uintptr_t*>(base + off::kOffRsState);
        auto clearState = [&](size_t field, int state)
        {
            if (!*reinterpret_cast<const int*>(base + off::kOffRsEnabled)) return;
            int* p = reinterpret_cast<int*>(rs + field);
            if (*p) { reinterpret_cast<off::RsDirtyFn>(off::kRsDirty)(dev, nullptr, state); *p = 0; }
        };
        clearState(0x198, 0x11);
        reinterpret_cast<off::SetTexMtxIdentityFn>(off::kSetTexMtxIdentity)(0);
        clearState(0x120, 0xC);
        void* effect = reinterpret_cast<off::GetEffectFn>(off::kGetEffect)(reinterpret_cast<const char*>(off::kEffectName));
        if (effect) reinterpret_cast<off::EffectSetCurrentFn>(off::kEffectSetCurrent)(effect);

        const float cells[6][2] = { { 0, 0 }, { 1, 0 }, { 2, 0 }, { 3, 0 }, { 0, 1 }, { 1, 1 } };
        auto bindAtlas = [&](Light& l)
        {
            clearState(0x1E0, 0x14);
            reinterpret_cast<BindTargetFn>(kBindTarget)(1, GxOf(DepthFor(l.size)), 0);
            Vt<void(__fastcall*)(void*, void*, int, void*, int)>(dev, 0x5C)(dev, nullptr, 0, GxOf(l.colour), 0);
        };

        // Whole atlas to 1 (lit): new texture, reassigned slot, or D3D contents replaced (device reset).
        auto clearAtlas = [&](Light& l) -> bool
        {
            bindAtlas(l);
            reinterpret_cast<SetViewportFn>(kSetViewport)(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
            if (!AtlasBound(dev, l)) return false;
            reinterpret_cast<SceneClearFn>(kSceneClear)(3, 0xFFFFFFFF);
            std::memset(l.state.faceFrame, 0, sizeof l.state.faceFrame);
            for (float& c : l.coverage) c = -1.0f;
            l.needsClear = false;
            l.track.staleFaces = kAllFaces;
            l.track.lastClearFrame = frame;
            return true;
        };

        auto renderFace = [&](Light& l, int f) -> bool
        {
            const float n = 0.05f, r = std::fmax(l.params.radius, 1.0f);
            const float proj[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, (r + n) / (r - n), 1, 0, 0, -2.0f * r * n / (r - n), 0 };
            const float scale[4] = { 0, 0, 0, 1.0f / r };
            Vt<off::SetConstantsFn>(dev, off::kVtConstants)(dev, nullptr, 4, 0, scale, 1);
            Vt<off::SetMatrixFn>(dev, off::kVtProjection)(dev, nullptr, proj);
            reinterpret_cast<off::UpdateProjMatrixFn>(off::kUpdateProjMatrix)();

            bindAtlas(l);
            const float x0 = cells[f][0] / kCols, y0 = cells[f][1] / kRows;
            reinterpret_cast<SetViewportFn>(kSetViewport)(x0, x0 + 1.0f / kCols, y0, y0 + 1.0f / kRows, 0.0f, 1.0f);
            if (!AtlasBound(dev, l)) return false;
            reinterpret_cast<SceneClearFn>(kSceneClear)(3, 0xFFFFFFFF);

            const float eye[3]    = { l.params.position[0] - cam[0], l.params.position[1] - cam[1], l.params.position[2] - cam[2] };
            const float target[3] = { eye[0] + kAxes[f][0], eye[1] + kAxes[f][1], eye[2] + kAxes[f][2] };
            float view[16];
            reinterpret_cast<off::LookAtFn>(off::kLookAt)(eye, target, kUps[f], view);
            Vt<off::SetMatrixFn>(dev, off::kVtView)(dev, nullptr, view);

            // c14..c16: the camera view the skinned palettes carry, re-based onto this face's view.
            float rebase[16], t[12];
            Mul(invCamView, view, rebase);
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 4; ++col) t[row * 4 + col] = rebase[col * 4 + row];
            Vt<off::SetConstantsFn>(dev, off::kVtConstants)(dev, nullptr, 0, 14, t, 3);

            const uint32_t classes = l.flags & (WXL_OMNI_CASTERS_STATIC | WXL_OMNI_CASTERS_DYNAMIC);
            if (anything && classes && classes != (WXL_OMNI_CASTERS_STATIC | WXL_OMNI_CASTERS_DYNAMIC))
            {
                const bool wantStatic = classes == WXL_OMNI_CASTERS_STATIC;
                if (wantStatic && list[0])
                    reinterpret_cast<off::RenderWmoShadowFn>(off::kRenderWmoShadow)(
                        list[1], list[0], list[2], toCamRel,
                        reinterpret_cast<uint8_t*>(query) + off::kOffQueryFrustum + slot * off::kQueryFrustumStride);
                List& opaque = g_lists[wantStatic ? 0 : 2];
                List& alpha = g_lists[wantStatic ? 1 : 3];
                if (opaque.count || alpha.count)
                {
                    uint32_t& maxO = *reinterpret_cast<uint32_t*>(off::kMaxOpaqueBatches);
                    uint32_t& maxA = *reinterpret_cast<uint32_t*>(off::kMaxAlphaBatches);
                    if (maxO < opaque.count) maxO = opaque.count;
                    if (maxA < alpha.count) maxA = alpha.count;
                    reinterpret_cast<off::RenderM2ShadowFn>(off::kRenderM2Shadow)(&opaque, &alpha);
                }
            }
            else if (anything)
            {
                if (list[0])
                    reinterpret_cast<off::RenderWmoShadowFn>(off::kRenderWmoShadow)(
                        list[1], list[0], list[2], toCamRel,
                        reinterpret_cast<uint8_t*>(query) + off::kOffQueryFrustum + slot * off::kQueryFrustumStride);
                if (list[4] || list[7])
                {
                    const uintptr_t cvar = *reinterpret_cast<const uintptr_t*>(off::kInstancingCVar);
                    if (cvar && *reinterpret_cast<const int*>(cvar + 0x30))
                    {
                        reinterpret_cast<off::InstancingPrepareFn>(off::kInstancingPrepare)(const_cast<uint32_t*>(list + 3), nullptr);
                        reinterpret_cast<off::InstancingPrepareFn>(off::kInstancingPrepare)(const_cast<uint32_t*>(list + 6), nullptr);
                    }
                    uint32_t& maxO = *reinterpret_cast<uint32_t*>(off::kMaxOpaqueBatches);
                    uint32_t& maxA = *reinterpret_cast<uint32_t*>(off::kMaxAlphaBatches);
                    if (maxO < list[4]) maxO = list[4];
                    if (maxA < list[7]) maxA = list[7];
                    reinterpret_cast<off::RenderM2ShadowFn>(off::kRenderM2Shadow)(const_cast<uint32_t*>(list + 3),
                                                                                  const_cast<uint32_t*>(list + 6));
                }
            }

            // Lookup rows: world -> camera-relative -> face view -> projection -> atlas.
            const float atlas[16] = { 0.5f / kCols, 0, 0, 0, 0, -0.5f / kRows, 0, 0, 0, 0, 1, 0,
                                      (0.5f + cells[f][0]) / kCols, (0.5f + cells[f][1]) / kRows, 0, 1 };
            float m[16];
            Mul(toCamRel, view, m);
            Mul(m, proj, m);
            Mul(m, atlas, m);
            for (int k = 0; k < 4; ++k)
                for (int i = 0; i < 4; ++i) l.state.faceRows[f][k][i] = m[i * 4 + k];
            l.state.faceFrame[f] = frame;
            l.coverage[f] = Coverage(l, f);
            std::memcpy(l.state.position, l.params.position, sizeof l.state.position);
            l.state.radius = r;
            l.track.staleFaces &= ~(1u << f);
            l.occupied = (l.occupied & ~(1u << f)) | (l.seen & (1u << f));
            ++g_faces;
            ++g_facesLast;
            return true;
        };

        bool refused = false;
        uint32_t priorityFaces = 0, robinFaces = 0, moving = 0;
        for (uint32_t i = 0; i < g_count && !refused; ++i)
        {
            Light& l = g_lights[i];
            if (!l.colour) continue;
            if (D3dOf(l.colour) != l.d3dSeen) { l.needsClear = true; l.priority = true; l.dirtyFaces = kAllFaces; }
            if (l.needsClear && !clearAtlas(l)) { refused = true; break; }
            l.d3dSeen = D3dOf(l.colour);
        }

        // Moved lights (all six faces) and faces whose casters moved first, outside the budget. Faces a
        // light leaves out are never drawn: they keep the clear value, lit.
        for (uint32_t i = 0; i < g_count && !refused; ++i)
        {
            Light& l = g_lights[i];
            if (!l.colour || !l.priority) continue;
            ++moving;
            for (int f = 0; f < 6; ++f)
            {
                if (!(l.dirtyFaces & l.faceMask & (1u << f))) continue;
                if (!renderFace(l, f)) { refused = true; break; }
                ++priorityFaces;
            }
            if (refused) break;
            if (l.dirtyFaces == kAllFaces)
            {
                std::memcpy(l.anchor, l.params.position, sizeof l.anchor);
                l.anchorRadius = l.params.radius;
            }
        }

        // Still lights keep the round-robin with what the budget has left (at least one face). A light
        // of static casters only takes part while its faces are stale (the static set changed); one
        // of units only never does: its faces are redrawn when they see a unit.
        uint32_t budget = g_budget > priorityFaces ? g_budget - priorityFaces : 1;
        for (uint32_t tries = 0; !refused && budget && tries < g_count * 6;
             ++tries, g_cursor = (g_cursor + 1) % (g_count * 6))
        {
            Light& l = g_lights[g_cursor / 6];
            const int f = int(g_cursor % 6);
            if (!l.colour || l.priority || !(l.faceMask & (1u << f))) continue;
            const uint32_t classes = l.flags & (WXL_OMNI_CASTERS_STATIC | WXL_OMNI_CASTERS_DYNAMIC);
            if (classes == WXL_OMNI_CASTERS_DYNAMIC) continue;
            if (classes == WXL_OMNI_CASTERS_STATIC && !(l.track.staleFaces & (1u << f))) continue;
            --budget;
            if (!renderFace(l, f)) { refused = true; break; }
            ++robinFaces;
        }
        for (uint32_t i = 0; i < g_count; ++i) { g_lights[i].priority = false; g_lights[i].dirtyFaces = 0; }

        if (moving && !g_costLogged)
        {
            g_costLogged = true;
            WLOG_INFO("omni-shadows: moving light present: %u faces this frame (%u for %u moving light(s), %u round-robin, "
                      "budget %u, %u light(s))", priorityFaces + robinFaces, priorityFaces, moving, robinFaces, g_budget, g_count);
        }

        if (refused) g_count = 0; // nothing more until a consumer sets lights again
        Restore(dev);
        if (!PostConditionHolds(dev))
        {
            const float* v = reinterpret_cast<const float*>(base + kOffViewport);
            WLOG_ERROR("omni-shadows: engine state not restored after the pass (viewport %.3f %.3f %.3f %.3f, saved "
                       "%.3f %.3f %.3f %.3f, window %.0fx%.0f), omni shadows off for this session",
                       v[0], v[1], v[2], v[3], g_saved.viewport[0], g_saved.viewport[1], g_saved.viewport[2],
                       g_saved.viewport[3], *reinterpret_cast<const float*>(base + kOffWindowWidth),
                       *reinterpret_cast<const float*>(base + kOffWindowHeight));
            g_count   = 0;
            g_enabled = false;
        }
    }

    /// Puts the engine back after a fault in the pass, without letting a second fault escape.
    void RestoreGuarded()
    {
        __try { Restore(Engine()); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_saved.active = false; }
    }

    uint32_t Mix(uint32_t h)
    {
        h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
        return h;
    }

    /// The faces of a cube map whose 90-degree frustum a sphere of radius reach at v (relative to
    /// the light) may touch: along a face's axis v must not lie behind it by more than reach, and
    /// sideways it must stay within the frustum widened by reach.
    uint32_t FacesTouched(const float v[3], float reach)
    {
        uint32_t faces = 0;
        for (int f = 0; f < 6; ++f)
        {
            const int a = f / 2;
            const float along = (f % 2 == 0 ? v[a] : -v[a]) + reach;
            const float side = std::fmax(std::fabs(v[(a + 1) % 3]), std::fabs(v[(a + 2) % 3])) - reach;
            if (along >= 0.0f && side <= along) faces |= 1u << f;
        }
        return faces;
    }

    /// Marks each light's faces for a refresh: all six when it moved past the threshold, else those
    /// whose units (as a face sees them) moved or turned. By class: static casters only follow the
    /// engine's static set (stale faces, refreshed in turn); units only redraw the faces that see a unit
    /// (or saw one when last drawn) when asked to, every frame.
    void UpdateTracking(uint32_t frame)
    {
        namespace world = wxl::game::world;
        constexpr float kBodyReach = 1.5f;  // a unit's extent around its centre, a yard above its feet
        uint32_t hashes[kMaxLights][6] = {};
        uint32_t seen[kMaxLights] = {};
        // One walk over the units; each hashes its position (0.05 yd grid) and facing into every
        // face of every light that sees it. Summed, so the walk order does not matter.
        world::ForEachObject(world::kTypeMaskUnit, [&](unsigned long long guid, void* unit) {
            float p[3];
            world::UnitPosition(unit, p);
            const float facing = world::Facing(unit);
            const uint32_t h = Mix(uint32_t(guid) ^ Mix(uint32_t(int(std::floor(p[0] / kMoveThreshold))) ^
                                   Mix(uint32_t(int(std::floor(p[1] / kMoveThreshold))) ^
                                   Mix(uint32_t(int(std::floor(p[2] / kMoveThreshold))) ^
                                   Mix(uint32_t(int(std::floor(facing * 20.0f))))))));
            for (uint32_t i = 0; i < g_count; ++i)
            {
                const Light& l = g_lights[i];
                const float v[3] = { p[0] - l.params.position[0], p[1] - l.params.position[1],
                                     p[2] + 1.0f - l.params.position[2] };
                const float r = l.params.radius + 2.0f; // + a body's reach
                if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > r * r) continue;
                const uint32_t faces = FacesTouched(v, kBodyReach);
                seen[i] |= faces;
                for (int f = 0; f < 6; ++f)
                    if (faces & (1u << f)) hashes[i][f] += h;
            }
            return true;
        });

        const bool staticChanged = g_staticHash != g_staticSeen;
        g_staticSeen = g_staticHash;
        for (uint32_t i = 0; i < g_count; ++i)
        {
            Light& l = g_lights[i];
            l.seen = seen[i];
            const float dx = l.params.position[0] - l.anchor[0], dy = l.params.position[1] - l.anchor[1],
                        dz = l.params.position[2] - l.anchor[2];
            const bool moved = dx * dx + dy * dy + dz * dz > kMoveThreshold * kMoveThreshold ||
                               std::fabs(l.params.radius - l.anchorRadius) > kMoveThreshold;
            uint32_t dirty = moved || l.needsClear ? kAllFaces : 0;
            const uint32_t classes = l.flags & (WXL_OMNI_CASTERS_STATIC | WXL_OMNI_CASTERS_DYNAMIC);
            if (classes == WXL_OMNI_CASTERS_STATIC)
            {
                // Only faces the engine's lists now cover at least as well as when they were drawn: a
                // caster missing from the lists was culled by the camera-bound window, not removed.
                if (staticChanged)
                    for (int f = 0; f < 6; ++f)
                        if ((l.faceMask & (1u << f)) && Coverage(l, f) >= l.coverage[f] - 0.02f) l.track.staleFaces |= 1u << f;
            }
            else if (classes == WXL_OMNI_CASTERS_DYNAMIC && (l.flags & WXL_OMNI_REDRAW_OCCUPIED))
                dirty |= seen[i] | l.occupied;
            else
                for (int f = 0; f < 6; ++f)
                    if (l.haveCasters && hashes[i][f] != l.casters[f]) dirty |= 1u << f;
            std::memcpy(l.casters, hashes[i], sizeof l.casters);
            l.haveCasters = true;
            dirty &= l.faceMask;
            if (dirty)
            {
                l.priority = true;
                l.dirtyFaces |= dirty;
                l.track.staleFaces |= dirty;
                l.track.lastMovedFrame = frame;
            }
        }
    }

    bool AnyClassed()
    {
        for (uint32_t i = 0; i < g_count; ++i)
        {
            const uint32_t c = g_lights[i].flags & (WXL_OMNI_CASTERS_STATIC | WXL_OMNI_CASTERS_DYNAMIC);
            if (c && c != (WXL_OMNI_CASTERS_STATIC | WXL_OMNI_CASTERS_DYNAMIC)) return true;
        }
        return false;
    }

    /// Runs after each stock shadow render; acts once per frame, on the main exterior pass.
    void __cdecl AfterRender(const sh::CallbackArgs& a, void*)
    {
        if (!g_count) return;
        if (GetTickCount64() - g_lastSet > kConsumerTimeoutMs) { g_count = 0; return; } // consumer gone
        const uintptr_t mainColour = uintptr_t(GxOf(*reinterpret_cast<void* const*>(off::kMainTex)));
        const uintptr_t interior   = uintptr_t(GxOf(*reinterpret_cast<void* const*>(off::kInteriorTex)));
        const bool hw = *reinterpret_cast<const int*>(off::kHwPcf) != 0;
        const bool isMain = a.arg[2] == mainColour &&
                            (hw ? a.arg[3] == uintptr_t(GxOf(*reinterpret_cast<void* const*>(off::kMainDepthTex))) : a.arg[3] != interior);
        if (!isMain) return;

        const uintptr_t sc = *reinterpret_cast<const uintptr_t*>(wxl::offsets::game::lights::kWorldM2Scene);
        const uint32_t frame = sc ? *reinterpret_cast<const uint32_t*>(sc + wxl::offsets::game::lights::kOffSceneFrame) : 0;
        if (frame == g_lastFrame) return;
        g_lastFrame = frame;
        g_facesLast = 0;

        // The window the engine's lists were culled to (its main map's rectangle across the light).
        sh::RenderPass pass{};
        g_window.valid = sh::DescribeRender(a, pass) && pass.slot == sh::Slot::Main && pass.halfExtent > 1.0f;
        if (g_window.valid)
        {
            std::memcpy(g_window.centre, pass.centre, sizeof g_window.centre);
            for (int k = 0; k < 3; ++k) { g_window.x[k] = pass.view[k * 4]; g_window.y[k] = pass.view[k * 4 + 1]; }
            g_window.half = pass.halfExtent;
        }

        __try
        {
            if (EnsureTargets(frame))
            {
                if (AnyClassed()) SplitCasters(int(a.arg[1]), frame);
                UpdateTracking(frame);
                RenderFaces(reinterpret_cast<void*>(a.arg[0]), int(a.arg[1]), frame);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            static bool logged = false;
            if (!logged) { logged = true; WLOG_ERROR("omni-shadows: fault in the pass, disabled for this session"); }
            g_count   = 0;
            g_enabled = false;
            RestoreGuarded();
        }
    }

    // --- interface -----------------------------------------------------------------------------------
    /// Resets a slot for a different light: its atlas (kept, not recreated) is cleared before use.
    void Reassign(Light& l, const WXL_OmniLightV3& in)
    {
        std::memset(l.state.faceFrame, 0, sizeof l.state.faceFrame);
        l.track = WXL_OmniShadowState{};
        l.track.staleFaces = kAllFaces;
        l.track.id = in.id;
        std::memcpy(l.anchor, in.position, sizeof l.anchor);
        l.anchorRadius = in.radius;
        l.haveCasters = false;
        l.needsClear  = true;
        l.occupied    = 0;
        for (float& c : l.coverage) c = -1.0f;
    }

    uint32_t PowerOfTwoFace(uint32_t size)
    {
        uint32_t p = 64;
        while (p * 2 <= size && p < 1024) p *= 2; // power of two, so the atlas is one too
        return p;
    }

    void SetLights(const WXL_OmniLightV3* lights, uint32_t count, uint32_t max)
    {
        if (!g_enabled) return;
        g_lastSet = GetTickCount64();
        if (!lights) count = 0;
        if (count > max) count = max;

        // Each incoming light keeps its slot's state (atlas included) when it is the same light:
        // same nonzero id, or with id 0, same index and within its radius of where it was.
        Light next[kMaxLights];
        bool taken[kMaxLights] = {};
        int  from[kMaxLights];
        for (uint32_t i = 0; i < count; ++i)
        {
            from[i] = -1;
            const WXL_OmniLightV3& in = lights[i];
            for (uint32_t j = 0; j < g_count && from[i] < 0; ++j)
            {
                const Light& o = g_lights[j];
                if (taken[j]) continue;
                if (in.id ? o.track.id == in.id : (j == i && !o.track.id))
                {
                    const float dx = in.position[0] - o.params.position[0], dy = in.position[1] - o.params.position[1],
                                dz = in.position[2] - o.params.position[2], r = std::fmax(o.params.radius, 1.0f);
                    if (in.id || dx * dx + dy * dy + dz * dz <= r * r) from[i] = int(j);
                }
            }
            if (from[i] >= 0) taken[from[i]] = true;
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            if (from[i] >= 0) { next[i] = g_lights[from[i]]; continue; }
            // A slot nobody keeps: reuse its atlas, one of the asked size first (no reallocation).
            const uint32_t want = lights[i].faceSize ? PowerOfTwoFace(lights[i].faceSize) : g_faceSize;
            uint32_t spare = kMaxLights;
            for (uint32_t j = 0; j < kMaxLights && spare == kMaxLights; ++j)
                if (!taken[j] && g_lights[j].colour && g_lights[j].size == want) spare = j;
            for (uint32_t j = 0; j < kMaxLights && spare == kMaxLights; ++j)
                if (!taken[j]) spare = j;
            taken[spare] = true;
            next[i] = g_lights[spare];
            Reassign(next[i], lights[i]);
        }
        for (uint32_t k = count, s = 0; k < kMaxLights; ++k) // unused slots keep their atlases
        {
            while (s < kMaxLights && taken[s]) ++s;
            if (s < kMaxLights) { next[k] = g_lights[s]; taken[s] = true; next[k].needsClear = true; }
        }
        for (uint32_t i = 0; i < kMaxLights; ++i) g_lights[i] = next[i];
        for (uint32_t i = 0; i < count; ++i)
        {
            Light& l = g_lights[i];
            const WXL_OmniLightV3& in = lights[i];
            const uint32_t classes = in.flags & (WXL_OMNI_CASTERS_STATIC | WXL_OMNI_CASTERS_DYNAMIC);
            const uint32_t mask = (in.faceMask & kAllFaces) ? (in.faceMask & kAllFaces) : kAllFaces;
            // A light whose classes or faces change is a different map: drawn again from clear.
            if ((l.flags & (WXL_OMNI_CASTERS_STATIC | WXL_OMNI_CASTERS_DYNAMIC)) != classes || l.faceMask != mask)
            {
                l.needsClear = true;
                l.occupied = 0;
            }
            std::memcpy(l.params.position, in.position, sizeof l.params.position);
            l.params.radius = in.radius;
            l.track.id = in.id;
            l.flags = in.flags;
            l.faceMask = mask;
            l.wantSize = in.faceSize ? PowerOfTwoFace(in.faceSize) : 0;
        }

        g_count = count;
        if (g_cursor >= g_count * 6) g_cursor = 0;
        if (!g_chained && g_count) g_chained = sh::ChainAfter(sh::Callback::Render, &AfterRender, nullptr);
    }

    void __cdecl ApiSetLightsEx(const WXL_OmniLightEx* lights, uint32_t count)
    {
        if (g_owner) return;   // claimed: accepted, ignored
        WXL_OmniLightV3 v3[WXL_OMNISHADOWS_MAX] = {};
        if (!lights) count = 0;
        if (count > WXL_OMNISHADOWS_MAX) count = WXL_OMNISHADOWS_MAX;
        for (uint32_t i = 0; i < count; ++i)
        {
            std::memcpy(v3[i].position, lights[i].position, sizeof v3[i].position);
            v3[i].radius = lights[i].radius;
            v3[i].id = lights[i].id;
        }
        SetLights(lights ? v3 : nullptr, count, WXL_OMNISHADOWS_MAX);
    }

    void __cdecl ApiSetLights(const WXL_OmniLight* lights, uint32_t count)
    {
        WXL_OmniLightEx ex[WXL_OMNISHADOWS_MAX] = {};
        if (!lights) count = 0;
        if (count > WXL_OMNISHADOWS_MAX) count = WXL_OMNISHADOWS_MAX;
        for (uint32_t i = 0; i < count; ++i)
        {
            std::memcpy(ex[i].position, lights[i].position, sizeof ex[i].position);
            ex[i].radius = lights[i].radius;
        }
        ApiSetLightsEx(lights ? ex : nullptr, count);
    }

    int __cdecl ApiGetState(uint32_t index, WXL_OmniShadowState* out)
    {
        if (index >= g_count || !out) return 0;
        *out = g_lights[index].track;
        return 1;
    }

    void __cdecl ApiSetBudget(uint32_t faces)
    {
        if (g_owner) return;
        g_budget = faces ? faces : 1;
    }

    void __cdecl ApiSetFaceSize(uint32_t size)
    {
        if (g_owner) return;
        g_faceSize = PowerOfTwoFace(size);
    }

    int __cdecl ApiGet(uint32_t index, WXL_OmniShadow* out)
    {
        if (index >= g_count || !out) return 0;
        Light& l = g_lights[index];
        *out = l.state;
        out->faceSize = l.size;
        void* gx = GxOf(l.colour);
        out->texture = gx ? *reinterpret_cast<void**>(uintptr_t(gx) + off::kOffGxTexD3d) : nullptr;
        return 1;
    }

    uint32_t __cdecl ApiCount() { return g_count; }
    uint32_t __cdecl ApiGeneration() { return g_generation; }

    int __cdecl ApiClaim(const void* owner)
    {
        if (!owner) return 0;
        if (g_owner && g_owner != owner) return 0;
        if (!g_owner)
        {
            g_owner = owner;
            g_count = 0;   // the previous driver's set goes; the owner sets its own
            WLOG_INFO("omni-shadows: claimed; other drivers are ignored until it is released");
        }
        return 1;
    }

    void __cdecl ApiRelease(const void* owner)
    {
        if (!owner || g_owner != owner) return;
        g_owner = nullptr;
        g_count = 0;
        WLOG_INFO("omni-shadows: released");
    }

    void __cdecl ApiSetLightsV3(const void* owner, const WXL_OmniLightV3* lights, uint32_t count)
    {
        if (g_owner && g_owner != owner) return;
        SetLights(lights, count, kMaxLights);
    }

    void __cdecl ApiSetBudgetV3(const void* owner, uint32_t faces)
    {
        if (g_owner && g_owner != owner) return;
        g_budget = faces ? faces : 1;
    }

    uint32_t __cdecl ApiFacesLastFrame() { return g_facesLast; }

    const WXL_OmniShadowsApi g_api = {
        sizeof(WXL_OmniShadowsApi), WXL_OMNISHADOWS_API_VERSION,
        &ApiSetLights, &ApiSetBudget, &ApiSetFaceSize, &ApiGet, &ApiCount, &ApiGeneration,
        &ApiSetLightsEx, &ApiGetState,
        &ApiClaim, &ApiRelease, &ApiSetLightsV3, &ApiSetBudgetV3, &ApiFacesLastFrame,
    };

    // --- diagnostic: the scene-lights orb casts ------------------------------------------------------
    void __cdecl OnUpdateDiag(void*, const void*)
    {
        namespace world = wxl::game::world;
        const unsigned long long guid = world::ActivePlayerGuid();
        void* unit = guid ? world::ResolveObject(guid, world::kTypeMaskPlayer) : nullptr;
        if (!unit) return;
        WXL_OmniLight orb{};
        world::UnitPosition(unit, orb.position);
        orb.position[2] += 2.5f;
        orb.radius = 12.0f;
        ApiSetLights(&orb, 1);

        const unsigned long long now = GetTickCount64();
        if (now >= g_nextLog)
        {
            if (g_nextLog)
            {
                WXL_OmniShadow s{};
                WXL_OmniShadowState t{};
                ApiGet(0, &s);
                ApiGetState(0, &t);
                WLOG_INFO("omni-shadows: %u faces in 10 s, atlas %p, chained %d, generation %u, stale 0x%02X, moved at %u, "
                          "cleared at %u", g_faces, s.texture, int(g_chained), g_generation, t.staleFaces, t.lastMovedFrame,
                          t.lastClearFrame);
            }
            g_faces   = 0;
            g_nextLog = now + 10000;
        }
    }

    bool InstallOmniShadows()
    {
        g_enabled = wxl::config::Env("WXL_OMNI_SHADOWS", true);
        if (!g_enabled) WLOG_INFO("omni-shadows: disabled (WXL_OMNI_SHADOWS=0)");
        // The same table under v2 as well: a v2 caller reads only the fields it knows.
        wxl::runtime::extensions::PublishInterface("wxl.omnishadows", WXL_OMNISHADOWS_API_VERSION,
                                                   const_cast<WXL_OmniShadowsApi*>(&g_api));
        wxl::runtime::extensions::PublishInterface("wxl.omnishadows", 2, const_cast<WXL_OmniShadowsApi*>(&g_api));
        g_diag = wxl::config::Env("WXL_DIAG_LIGHTS", false);
        if (g_diag) wxl::events::Subscribe(wxl::events::Event::OnUpdate, &OnUpdateDiag, nullptr);
        return true;
    }
}

WXL_REGISTER_FEATURE("omni-shadows", true, InstallOmniShadows)
