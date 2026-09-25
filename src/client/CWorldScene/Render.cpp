// Native D3D9 render feature: device-vtable + engine detours that publish the render events.
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

#include "config.hpp"
#include "common/Mem.hpp"
#include "common/Log.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/events/Event.hpp"
#include "game/Gx.hpp"
#include "offsets/engine/Gx.hpp"
#include "client/CWorldScene/Mrt.hpp"
#include "game/GBuffer.hpp"

#include <windows.h>
#include <d3d9.h>

// Uses device-vtable pointer swaps (EndScene, Present, Reset) plus function-entry hooks on the
// liquid/world-boundary paths; no mid-function inline patch, so the native render pass stays intact.
// Each detour only publishes a render event -- the client owns its native D3D9 rendering.
//
// DrawIndexedPrimitive is NOT swapped here: that vtable slot, and the one-shot draw interceptor built
// on it, belong to wxl-m2 (its per-batch OnM2BatchDraw/OnRibbonDraw need the same slot; a second core
// swap on top would just fight it for the same vtable entry). wxl-m2 re-applies its own swap on the
// same per-device-recreate cadence this file uses for its three, and publishes "wxl.m2draw" for any
// other extension (wxl-wmo's four-layer material) that needs to bracket one native draw -- see
// include/wxl/M2DrawApi.h.
namespace
{
    namespace off = wxl::offsets::engine::gx;
    namespace ev  = wxl::events;
    namespace gx  = wxl::game::gx;

    using EndSceneFn = long (__stdcall*)(void*);
    using PresentFn  = long (__stdcall*)(void*, const void*, const void*, void*, const void*);
    using ResetFn    = long (__stdcall*)(void*, D3DPRESENT_PARAMETERS*);

    EndSceneFn g_origEndScene = nullptr;
    PresentFn  g_origPresent  = nullptr;
    ResetFn    g_origReset    = nullptr;
    off::WorldRenderFinalizeFn g_origWorldFinalize = nullptr;
    off::WorldOnRenderFn       g_origWorldScene    = nullptr;
    off::LiquidRenderPassFn    g_origLiquidRender  = nullptr;
    void*      g_hookedDevice  = nullptr;   // device whose vtable currently carries the render hooks

    void EnsureDeviceHooks(IDirect3DDevice9* dev);   // defined after SwapVtbl

    /**
     * @brief Detours EndScene, emitting OnEndScene before the native call.
     * @param dev  D3D9 device.
     * @return the EndScene result.
     */
    long __stdcall hkEndScene(void* dev)
    {
        ev::EndSceneArgs a{ dev };
        ev::Emit(ev::Event::OnEndScene, &a);
        return g_origEndScene(dev);
    }

    /**
     * @brief Detours Present, emitting OnFrame just before the buffers swap.
     * @param dev    D3D9 device.
     * @param src    source rect.
     * @param dst    destination rect.
     * @param wnd    target window override.
     * @param dirty  dirty region.
     * @return the Present result.
     */
    long __stdcall hkPresent(void* dev, const void* src, const void* dst, void* wnd, const void* dirty)
    {
        ev::FrameArgs a{ dev };
        ev::Emit(ev::Event::OnFrame, &a);
        return g_origPresent(dev, src, dst, wnd, dirty);
    }

    /**
     * @brief Detours the per-frame liquid render pass, emitting OnLiquidRender before the native draw.
     *
     * Fires once per pass (passType 0 main, 1 secondary). The liquid textures are already bound and the
     * wave animation already applied at this point.
     * @param bank       liquid material-settings bank (this-in-ECX), indexed by passType.
     * @param edx        unused register slot for the thiscall convention.
     * @param transform  shared liquid transform forwarded to every instance draw.
     * @param passType   liquid pass index (0 main, 1 secondary).
     */
    void __fastcall hkLiquidRender(void* bank, void* edx, void* transform, int passType)
    {
        const off::LiquidPassEntry& entry = static_cast<off::LiquidPassEntry*>(bank)[passType];

        ev::LiquidRenderArgs a{ bank, transform, passType, entry.count };
        ev::Emit(ev::Event::OnLiquidRender, &a);

        g_origLiquidRender(bank, edx, transform, passType);
    }

    /**
     * @brief Redirects the world pass's depth to a subscriber's surface.
     *
     * Every render-target bind the engine makes binds the depth surface cached on its device object,
     * so the cached pointer is swapped as well as the bound one; binding alone would be undone by the
     * first post-process target inside the pass.
     * @param d      the D3D9 device.
     * @param depth  the surface the pass draws its depth into.
     * @return the engine's cached depth surface to put back, or null when the redirect is declined.
     */
    void* BeginDepthOverride(IDirect3DDevice9* d, IDirect3DSurface9* depth)
    {
        auto* g = static_cast<off::GxDevice*>(gx::RawGraphicsDevice());
        if (!g || !g->depthSurface) return nullptr;

        // An offscreen-target override makes the engine bind another depth field; ours would not hold.
        const uintptr_t base = uintptr_t(g);
        if (*reinterpret_cast<const uint32_t*>(base + off::kRtOverrideField)
            || *reinterpret_cast<const uint32_t*>(base + off::kRtDepthOverrideField))
        {
            static bool logged = false;
            if (!logged) { logged = true; WLOG_WARN("render: depth override declined, engine target override active"); }
            return nullptr;
        }

        void* engineDepth = g->depthSurface;
        g->depthSurface = depth;
        d->SetDepthStencilSurface(depth);

        // Clear covers the viewport only, so it is widened to the bound target for the clear.
        // A pure device refuses GetViewport; the viewport is then left as found.
        D3DVIEWPORT9 saved{};
        const bool widen = SUCCEEDED(d->GetViewport(&saved));
        IDirect3DSurface9* rt = nullptr;
        D3DSURFACE_DESC rtDesc{};
        if (SUCCEEDED(d->GetRenderTarget(0, &rt)) && rt) { rt->GetDesc(&rtDesc); rt->Release(); }
        if (widen && rtDesc.Width && rtDesc.Height)
        {
            const D3DVIEWPORT9 full{ 0, 0, rtDesc.Width, rtDesc.Height, 0.0f, 1.0f };
            d->SetViewport(&full);
        }
        // The engine's own scene clear writes Z 1.0; stencil 0 is its clear value too.
        if (FAILED(d->Clear(0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0)))
            d->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
        if (widen) d->SetViewport(&saved);

        static bool announced = false;
        if (!announced) { announced = true; WLOG_INFO("render: world pass depth redirected (%p -> %p)", engineDepth, (void*)depth); }
        return engineDepth;
    }

    /**
     * @brief Puts the engine's depth surface back after a redirected pass.
     * @param d            the D3D9 device.
     * @param depth        the override the pass drew into.
     * @param engineDepth  the engine's cached surface returned by BeginDepthOverride.
     */
    void EndDepthOverride(IDirect3DDevice9* d, IDirect3DSurface9* depth, void* engineDepth)
    {
        auto* g = static_cast<off::GxDevice*>(gx::RawGraphicsDevice());
        if (g && g->depthSurface == depth) g->depthSurface = engineDepth;

        IDirect3DSurface9* bound = nullptr;
        d->GetDepthStencilSurface(&bound);
        if (bound == depth) d->SetDepthStencilSurface(static_cast<IDirect3DSurface9*>(engineDepth));
        if (bound) bound->Release();
    }

    /**
     * @brief Redirects the world pass's colour target. Every render-target bind the engine makes
     *        binds its cached back buffer (device +0x3B3C) unless an override is active, so the cache
     *        is swapped along with the bound target.
     * @return the engine's back-buffer surface to put back, or null when declined.
     */
    void* BeginColorOverride(IDirect3DDevice9* d, IDirect3DSurface9* color)
    {
        auto* g = static_cast<off::GxDevice*>(gx::RawGraphicsDevice());
        if (!g || !g->backBuffer || !color) return nullptr;
        const uintptr_t base = uintptr_t(g);
        if (*reinterpret_cast<const uint32_t*>(base + off::kRtOverrideField)) return nullptr;

        IDirect3DSurface9* rt0 = nullptr;
        if (FAILED(d->GetRenderTarget(0, &rt0)) || !rt0) return nullptr;
        D3DSURFACE_DESC a{}, b{};
        rt0->GetDesc(&a);
        color->GetDesc(&b);
        const bool isEngine = rt0 == g->backBuffer;
        rt0->Release();
        if (!isEngine || a.Width != b.Width || a.Height != b.Height || b.MultiSampleType != a.MultiSampleType)
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                WLOG_WARN("render: colour override declined (%ux%u fmt %d vs %ux%u fmt %d, engine target %d)",
                          b.Width, b.Height, int(b.Format), a.Width, a.Height, int(a.Format), int(isEngine));
            }
            return nullptr;
        }

        static bool capsLogged = false;
        if (!capsLogged)
        {
            capsLogged = true;
            IDirect3D9* d3d = nullptr;
            D3DDEVICE_CREATION_PARAMETERS cp{};
            D3DDISPLAYMODE mode{};
            if (SUCCEEDED(d->GetDirect3D(&d3d)) && d3d && SUCCEEDED(d->GetCreationParameters(&cp)) &&
                SUCCEEDED(d->GetDisplayMode(0, &mode)))
            {
                const HRESULT rt    = d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                                             D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, b.Format);
                const HRESULT blend = d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                                             D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING,
                                                             D3DRTYPE_TEXTURE, b.Format);
                const HRESULT copy  = d3d->CheckDeviceFormatConversion(cp.AdapterOrdinal, cp.DeviceType, b.Format, a.Format);
                WLOG_INFO("render: colour override format %d: render target %s, blending %s, StretchRect to %d %s",
                          int(b.Format), SUCCEEDED(rt) ? "yes" : "NO", SUCCEEDED(blend) ? "yes" : "NO", int(a.Format),
                          SUCCEEDED(copy) ? "yes" : "NO");
            }
            if (d3d) d3d->Release();
        }

        void* engine = g->backBuffer;
        g->backBuffer = color;
        d->SetRenderTarget(0, color);
        static bool announced = false;
        if (!announced) { announced = true; WLOG_INFO("render: world pass colour redirected (%p -> %p)", engine, (void*)color); }
        return engine;
    }

    /// Puts the engine's back buffer back in its cache and on the device.
    void EndColorOverride(IDirect3DDevice9* d, IDirect3DSurface9* color, void* engine)
    {
        auto* g = static_cast<off::GxDevice*>(gx::RawGraphicsDevice());
        if (g && g->backBuffer == color) g->backBuffer = engine;
        IDirect3DSurface9* bound = nullptr;
        d->GetRenderTarget(0, &bound);
        if (bound == color) d->SetRenderTarget(0, static_cast<IDirect3DSurface9*>(engine));
        if (bound) bound->Release();
    }

    /**
     * @brief Detours the world scene pass, emitting OnWorldSceneBegin before it and OnWorldSceneEnd once
     *        it has drawn.
     *
     * A Begin subscriber may redirect the pass's depth to a surface of its own; see
     * WorldSceneBeginArgs.
     *
     * Its caller runs this, then the world text batch, then puts back the projection and view it saved
     * before the pass. Emitting on the way out of the pass therefore lands in the one window where the
     * world is complete and the matrices that drew it are still the ones on the device -- which is what
     * a subscriber placing geometry by world coordinate needs, and what the later world -> UI boundary
     * no longer offers.
     * @param worldFrame  world frame being rendered.
     * @param edx         unused; the register the native convention passes nothing meaningful in.
     */
    void __fastcall hkWorldScene(void* worldFrame, void* edx)
    {
        // Taken before the pass, not after: the post-process passes run inside it and can leave a
        // surface of their own bound. A subscriber that depth-tests against whatever it finds bound
        // afterwards is testing against a surface the world never wrote to, which rejects all of its
        // geometry and reports nothing.
        IDirect3DDevice9*  d          = static_cast<IDirect3DDevice9*>(gx::RawDevice());
        IDirect3DSurface9* sceneDepth = nullptr;
        if (d) d->GetDepthStencilSurface(&sceneDepth);

        // The rewritten engine shaders add the light buffer only when a provider sets c30.x this pass.
        static const float kNoLightBuffer[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        wxl::game::gbuffer::SetEnginePixelConstants(wxl::game::gbuffer::kLightBufferParams, kNoLightBuffer, 1);

        IDirect3DSurface9* depthOverride = nullptr;
        IDirect3DSurface9* normalTarget  = nullptr;
        IDirect3DSurface9* colorOverride = nullptr;
        if (d && sceneDepth && ev::Any(ev::Event::OnWorldSceneBegin))
        {
            void* requested = nullptr;
            void* normals   = nullptr;
            void* color     = nullptr;
            ev::WorldSceneBeginArgs b{ d, sceneDepth, &requested, &normals, &color };
            ev::Emit(ev::Event::OnWorldSceneBegin, &b);
            depthOverride = static_cast<IDirect3DSurface9*>(requested);
            normalTarget  = static_cast<IDirect3DSurface9*>(normals);
            colorOverride = static_cast<IDirect3DSurface9*>(color);
        }
        if (d && !normalTarget) normalTarget = static_cast<IDirect3DSurface9*>(wxl::runtime::mrt::DiagTarget(d));

        void* engineDepth = depthOverride ? BeginDepthOverride(d, depthOverride) : nullptr;
        if (!engineDepth) depthOverride = nullptr;

        void* engineColor = colorOverride ? BeginColorOverride(d, colorOverride) : nullptr;
        if (!engineColor) colorOverride = nullptr;

        const bool mrt = normalTarget && wxl::runtime::mrt::Begin(d, normalTarget);

        g_origWorldScene(worldFrame, edx);

        if (mrt) wxl::runtime::mrt::End(d);
        if (colorOverride) EndColorOverride(d, colorOverride, engineColor);
        if (depthOverride) EndDepthOverride(d, depthOverride, engineDepth);

        int resolved = 0;
        if (ev::Any(ev::Event::OnWorldSceneEnd))
        {
            ev::WorldSceneEndArgs a{ gx::RawDevice(), depthOverride ? depthOverride : sceneDepth,
                                     mrt ? normalTarget : nullptr, colorOverride, &resolved };
            ev::Emit(ev::Event::OnWorldSceneEnd, &a);
        }
        if (colorOverride && !resolved)
        {
            // Nobody tonemapped: keep the frame visible rather than showing a stale back buffer.
            d->StretchRect(colorOverride, nullptr, static_cast<IDirect3DSurface9*>(engineColor), nullptr, D3DTEXF_POINT);
            static bool warned = false;
            if (!warned) { warned = true; WLOG_WARN("render: colour override not resolved by any subscriber, copied as is"); }
        }
        if (mrt) wxl::runtime::mrt::DiagShow(d, normalTarget);

        if (sceneDepth) sceneDepth->Release();
    }

    /**
     * @brief Detours world-frame finalize: the world -> UI boundary. Re-applies the device vtable hooks on
     *        the live device (they are lost on a device recreate, and this function-entry hook survives it),
     *        runs the native finalize, then emits OnWorldRenderEnd for post-world subscribers.
     * @param worldFrame  world frame being finalized.
     */
    void __cdecl hkWorldFinalize(void* worldFrame)
    {
        if (IDirect3DDevice9* d = static_cast<IDirect3DDevice9*>(gx::RawDevice()))
            EnsureDeviceHooks(d);

        g_origWorldFinalize(worldFrame);

        ev::WorldRenderEndArgs a{ gx::RawDevice() };
        ev::Emit(ev::Event::OnWorldRenderEnd, &a);
    }

    /**
     * @brief Replaces one vtable entry with a hook, returning the original through origOut.
     * @param vtbl     vtable base.
     * @param idx      entry index to swap.
     * @param hook     replacement function pointer.
     * @param origOut  receives the original entry.
     */
    template <class Fn>
    void SwapVtbl(void** vtbl, unsigned idx, Fn* hook, Fn** origOut)
    {
        wxl::mem::SwapPointer(&vtbl[idx], reinterpret_cast<void*>(hook),
                              reinterpret_cast<void**>(origOut));
    }

    /**
     * @brief Detours IDirect3DDevice9::Reset (a resolution / window resize). D3D9 requires every
     *        D3DPOOL_DEFAULT resource released before a Reset or it fails (D3DERR_INVALIDCALL) and the device
     *        is lost -- the resize "crash". Fires OnDeviceLost so subscribers retire their GPU work and free
     *        DEFAULT-pool resources, releases the engine's own tracked render targets, runs the native Reset,
     *        then fires OnDeviceReset on success so subscribers recreate.
     * @param dev     the D3D9 device being reset.
     * @param params  the present parameters the device resets with (new size / window mode).
     * @return the native Reset result.
     */
    long __stdcall hkReset(void* dev, D3DPRESENT_PARAMETERS* params)
    {
        static unsigned resetLog = 0;
        const bool logThis = resetLog < 16;
        if (logThis)
        {
            ++resetLog;
            WLOG_INFO("render: Reset begin %ux%u windowed=%u",
                      params ? params->BackBufferWidth : 0,
                      params ? params->BackBufferHeight : 0,
                      params ? static_cast<unsigned>(params->Windowed) : 0);
        }

        ev::DeviceResetArgs a{ dev, params };
        // Subscribers using engine-owned color/depth surfaces must retire their GPU work and release their
        // DEFAULT-pool resources before the native Reset, or the Reset fails and the device is lost.
        ev::Emit(ev::Event::OnDeviceLost, &a);
        gx::ReleaseResetResources();  // free any tracked engine render targets (DEFAULT pool)
        wxl::runtime::mrt::OnDeviceLost();

        const long r = g_origReset(dev, params);
        if (SUCCEEDED(r))
        {
            ev::Emit(ev::Event::OnDeviceReset, &a);
            if (logThis) WLOG_INFO("render: Reset ok");
        }
        else
        {
            static unsigned logged = 0;
            if (logged < 8)
            {
                ++logged;
                WLOG_WARN("render: Reset failed hr=0x%08lX", static_cast<unsigned long>(r));
            }
        }
        return r;
    }

    /**
     * @brief Installs the render vtable hooks on the live device. Each device instance may carry its own
     *        vtable, so on a device recreate the swaps are gone; this re-applies them on the current device.
     *        The surviving world-render function-entry hook calls this each frame, so OnEndScene / OnFrame /
     *        OnM2BatchDraw keep firing after a graphics restart. Guarded against a shared vtable: if it already
     *        carries our hooks, re-swapping would capture our own hook as the "original" and recurse, so only
     *        the device pointer is updated.
     * @param dev  the live D3D9 device.
     */
    void EnsureDeviceHooks(IDirect3DDevice9* dev)
    {
        if (!dev || g_hookedDevice == dev) return;
        void** vtbl = *reinterpret_cast<void***>(dev);
        if (vtbl[off::vt::kPresent] != reinterpret_cast<void*>(&hkPresent))
        {
            SwapVtbl(vtbl, off::vt::kEndScene,             &hkEndScene, &g_origEndScene);
            SwapVtbl(vtbl, off::vt::kPresent,              &hkPresent, &g_origPresent);
            SwapVtbl(vtbl, off::vt::kReset,                &hkReset, &g_origReset);
            WLOG_INFO("render: device hooks installed (dev=%p)", (void*)dev);
        }
        g_hookedDevice = dev;
    }

    /**
     * @brief Installs the render detours: device vtable swaps plus the world-boundary / liquid
     *        function-entry hooks. Detours are enabled by the caller's batch EnableAll() afterwards.
     * @return true; a missing device only defers the vtable swaps to the first world finalize.
     */
    bool Install()
    {
        if (void* dev = gx::RawDevice())
            EnsureDeviceHooks(static_cast<IDirect3DDevice9*>(dev));
        else
            WLOG_WARN("render: device not up, vtable hooks deferred to first world finalize");

        wxl::hook::Install("WorldRenderFinalize", off::kWorldRenderFinalize,
                           &hkWorldFinalize, &g_origWorldFinalize);
        wxl::hook::Install("WorldScenePass", off::kWorldOnRender,
                           &hkWorldScene, &g_origWorldScene);
        wxl::hook::Install("LiquidRenderPass", off::kLiquidRenderPass,
                           &hkLiquidRender, &g_origLiquidRender);

        WLOG_INFO("render: hooks installed (EndScene, Present, Reset, WorldFinalize, WorldScenePass, LiquidRenderPass)");
        return true;
    }
}

WXL_REGISTER_FEATURE("render", true, Install)
