// The d3d9.dll proxy: pass Direct3DCreate9(Ex) through to the system d3d9 and load WarcraftXL.dll.
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

// The client LoadLibrary's "d3d9.dll" from its own folder first, so this proxy loads ahead of the system
// one. It forwards the factory-create exports to the real d3d9 -- the system one, or DXVK's when
// WXL_D3D9_BACKEND=dxvk -- loads WarcraftXL.dll into the process, and has the factory create a non-pure
// device. Unless WXL_D3D9EX=0, the engine's Direct3DCreate9
// gets a D3D9Ex factory and its CreateDevice a D3D9Ex device, whose managed pool the proxy emulates
// (Managed.cpp); a refused CreateDeviceEx falls back to a classic device.

#include <windows.h>
#include <d3d9.h>

#include "common/Config.hpp"
#include "common/Log.hpp"
#include "common/Mem.hpp"
#include "engine/gpu/GpuStats.hpp"
#include "engine/gpu/Resources.hpp"

#include <cstdarg>

namespace
{
    using Create9Fn   = IDirect3D9* (WINAPI*)(UINT);
    using Create9ExFn = HRESULT     (WINAPI*)(UINT, IDirect3D9Ex**);
    Create9Fn   g_realCreate9   = nullptr;
    Create9ExFn g_realCreate9Ex = nullptr;

    /**
     * @brief Writes one Info line to the proxy's own log sink, opening it on first use.
     *
     * The proxy is a distinct module from WarcraftXL.dll and owns a separate log instance. Lines are
     * sparse boot/crash diagnostics and are flushed immediately -- the DLL has no orderly close on exit.
     * @param fmt  printf-style format string followed by its arguments.
     */
    void Log(const char* fmt, ...)
    {
        ::wxl::log::Open("Logs\\d3d9proxy.log");   // idempotent
        if (!::wxl::log::Enabled(::wxl::log::Level::Info)) return;
        va_list ap;
        va_start(ap, fmt);
        ::wxl::log::WriteV(::wxl::log::Level::Info, fmt, ap);
        va_end(ap);
        ::wxl::log::Flush();
    }

    /**
     * @brief Loads the system d3d9 from the system directory, falling back to a local d3d9_real.dll.
     *
     * A loaded module is keyed by full path, so the system d3d9.dll is a distinct module from this proxy
     * despite the shared base name.
     * @return Handle to the system d3d9 module, or null on failure.
     */
    HMODULE LoadSystemD3D9()
    {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (n != 0 && n < MAX_PATH - 16)
        {
            lstrcatA(path, "\\d3d9.dll");
            if (HMODULE r = LoadLibraryA(path)) return r;
        }
        return LoadLibraryA("d3d9_real.dll");
    }

    /**
     * @brief Loads DXVK's d3d9.dll when WXL_D3D9_BACKEND=dxvk (WarcraftXL.cfg or the environment).
     *
     * The file is WXL_DXVK_PATH, by default the copy wxl-graphics-extend ships; a relative path is taken
     * from the folder this proxy sits in (the client folder), not from the working directory, which the
     * client may change. Like the system one, it is a distinct module from the proxy by its full path.
     * Unless already set, DXVK reads the dxvk.conf beside its DLL and logs into the client's Logs folder.
     * @param self  the proxy's own module, for its folder; null falls back to LoadLibrary's own search.
     * @return Handle to DXVK's d3d9, or null when the backend is native or DXVK did not load (logged).
     */
    HMODULE LoadDxvkD3D9(HMODULE self)
    {
        char backend[32] = {};
        if (!::wxl::config::Raw("WXL_D3D9_BACKEND", backend, sizeof backend)) return nullptr;
        if (lstrcmpiA(backend, "dxvk") != 0)
        {
            if (lstrcmpiA(backend, "native") != 0)
                Log("d3d9proxy: WXL_D3D9_BACKEND=%s is not native or dxvk; using native", backend);
            return nullptr;
        }

        char path[MAX_PATH] = {};
        if (!::wxl::config::Raw("WXL_DXVK_PATH", path, sizeof path))
            lstrcpyA(path, "Extensions\\wxl-graphics-extend\\dxvk\\d3d9.dll");

        char full[MAX_PATH] = {};
        const bool absolute = path[0] == '\\' || path[0] == '/' || (path[0] != '\0' && path[1] == ':');
        if (!absolute && self)
        {
            const DWORD n = GetModuleFileNameA(self, full, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) full[0] = '\0';
            else
            {
                char* slash = nullptr;
                for (char* p = full; *p; ++p)
                    if (*p == '\\' || *p == '/') slash = p;
                if (slash) slash[1] = '\0';
                if (lstrlenA(full) + lstrlenA(path) >= MAX_PATH) full[0] = '\0';
                else lstrcatA(full, path);
            }
        }
        if (!full[0]) lstrcpynA(full, path, MAX_PATH);

        // DXVK reads its settings and log path from the environment when it loads.
        char dir[MAX_PATH] = {};
        lstrcpynA(dir, full, MAX_PATH);
        char* cut = nullptr;
        for (char* p = dir; *p; ++p)
            if (*p == '\\' || *p == '/') cut = p;
        if (cut)
        {
            cut[1] = '\0';
            char probe[8];
            char value[MAX_PATH] = {};
            if (!GetEnvironmentVariableA("DXVK_CONFIG_FILE", probe, sizeof probe)
                && lstrlenA(dir) + 9 < MAX_PATH)
            {
                lstrcpyA(value, dir);
                lstrcatA(value, "dxvk.conf");
                SetEnvironmentVariableA("DXVK_CONFIG_FILE", value);
            }
            if (!GetEnvironmentVariableA("DXVK_LOG_PATH", probe, sizeof probe) && self)
            {
                const DWORD n = GetModuleFileNameA(self, value, MAX_PATH);
                char* slash = nullptr;
                for (char* p = value; n && *p; ++p)
                    if (*p == '\\' || *p == '/') slash = p;
                if (slash && lstrlenA(value) + 4 < MAX_PATH)
                {
                    lstrcpyA(slash + 1, "Logs");
                    SetEnvironmentVariableA("DXVK_LOG_PATH", value);
                }
            }
        }
        // Vulkan drivers the loader skips (manifest globs such as *amd-vulkan*, never *amd*: NVIDIA's
        // path holds "amd64"). An unused GPU's driver otherwise maps into the 32-bit address space.
        char drivers[128] = {};
        char probe[8];
        if (::wxl::config::Raw("WXL_VK_DRIVERS_DISABLE", drivers, sizeof drivers) && drivers[0]
            && !GetEnvironmentVariableA("VK_LOADER_DRIVERS_DISABLE", probe, sizeof probe))
        {
            SetEnvironmentVariableA("VK_LOADER_DRIVERS_DISABLE", drivers);
            Log("d3d9proxy: backend dxvk: Vulkan drivers disabled: %s", drivers);
        }

        if (HMODULE r = LoadLibraryA(full))
        {
            Log("d3d9proxy: backend dxvk: %s loaded", full);
            return r;
        }
        Log("d3d9proxy: backend dxvk: %s did not load (win32=%lu); falling back to the system d3d9", full, GetLastError());
        return nullptr;
    }

    // --- non-pure device ---
    // The engine asks for a pure device whenever it takes hardware vertex processing. A pure device
    // answers every Get* state query with nothing, so any code that saves and restores device state
    // would restore garbage. The factory's CreateDevice (and CreateDeviceEx) drop that one flag.

    using CreateDeviceFn   = HRESULT (STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                          D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
    using CreateDeviceExFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                          D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*,
                                                          IDirect3DDevice9Ex**);
    constexpr unsigned kSlotCreateDevice   = 16;  // IDirect3D9 / IDirect3D9Ex
    constexpr unsigned kSlotCreateDeviceEx = 20;  // IDirect3D9Ex only

    // Separate originals per factory kind: the two vtables need not share an implementation.
    CreateDeviceFn   g_origCreateDevice9   = nullptr;
    CreateDeviceFn   g_origCreateDeviceOnEx = nullptr;
    CreateDeviceExFn g_origCreateDeviceEx  = nullptr;

    /// D3D9Ex unless WXL_D3D9EX=0.
    bool UseEx()
    {
        static const bool on = ::wxl::config::Env("WXL_D3D9EX", true);
        return on;
    }

    DWORD StripPure(DWORD flags, const char* entry)
    {
        const DWORD out = flags & ~static_cast<DWORD>(D3DCREATE_PUREDEVICE);
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            Log("d3d9proxy: %s behavior 0x%08lX -> 0x%08lX", entry, flags, out);
        }
        return out;
    }

    HRESULT STDMETHODCALLTYPE hkCreateDevice9(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND wnd,
                                              DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
    {
        const HRESULT hr = g_origCreateDevice9(self, adapter, type, wnd, StripPure(flags, "CreateDevice"), pp, out);
        if (SUCCEEDED(hr) && out && *out) ::wxl::gpu::resources::Attach(*out, false);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hkCreateDeviceOnEx(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND wnd,
                                                 DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
    {
        const HRESULT hr = g_origCreateDeviceOnEx(self, adapter, type, wnd,
                                                  StripPure(flags, "CreateDevice(Ex factory)"), pp, out);
        if (SUCCEEDED(hr) && out && *out) ::wxl::gpu::resources::Attach(*out, false);
        return hr;
    }

    /**
     * @brief The engine's CreateDevice on a D3D9Ex factory: a D3D9Ex device, or a classic one if refused.
     *
     * Same parameters; a fullscreen device also needs its display mode spelled out.
     */
    HRESULT STDMETHODCALLTYPE hkCreateDeviceAsEx(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND wnd,
                                                 DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
    {
        flags = StripPure(flags, "CreateDevice (as D3D9Ex)");
        if (pp && out && g_origCreateDeviceEx)
        {
            D3DDISPLAYMODEEX mode{};
            mode.Size = sizeof mode;
            mode.Width = pp->BackBufferWidth;
            mode.Height = pp->BackBufferHeight;
            mode.RefreshRate = pp->FullScreen_RefreshRateInHz;
            mode.Format = pp->BackBufferFormat == D3DFMT_A8R8G8B8 ? D3DFMT_X8R8G8B8 : pp->BackBufferFormat;
            mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
            IDirect3DDevice9Ex* ex = nullptr;
            const HRESULT hr = g_origCreateDeviceEx(reinterpret_cast<IDirect3D9Ex*>(self), adapter, type, wnd, flags,
                                                    pp, pp->Windowed ? nullptr : &mode, &ex);
            if (SUCCEEDED(hr) && ex)
            {
                *out = ex;
                Log("d3d9proxy: device created through CreateDeviceEx (D3D9Ex, %ux%u %s)", pp->BackBufferWidth,
                    pp->BackBufferHeight, pp->Windowed ? "windowed" : "fullscreen");
                ::wxl::gpu::resources::Attach(ex, true);
                return hr;
            }
            Log("d3d9proxy: CreateDeviceEx refused (0x%08lX); creating a classic device", static_cast<unsigned long>(hr));
        }
        const HRESULT hr = g_origCreateDeviceOnEx(self, adapter, type, wnd, flags, pp, out);
        if (SUCCEEDED(hr) && out && *out) ::wxl::gpu::resources::Attach(*out, false);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hkCreateDeviceEx(IDirect3D9Ex* self, UINT adapter, D3DDEVTYPE type, HWND wnd,
                                               DWORD flags, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode,
                                               IDirect3DDevice9Ex** out)
    {
        const HRESULT hr = g_origCreateDeviceEx(self, adapter, type, wnd, StripPure(flags, "CreateDeviceEx"), pp,
                                                mode, out);
        if (SUCCEEDED(hr) && out && *out) ::wxl::gpu::resources::Attach(*out, true);
        return hr;
    }

    /// Swaps one factory vtable slot once; a vtable already carrying the hook is left alone.
    template <class Fn>
    void HookSlot(void* factory, unsigned slot, Fn hook, Fn* orig)
    {
        void** vtbl = *reinterpret_cast<void***>(factory);
        if (vtbl[slot] == reinterpret_cast<void*>(hook)) return;
        // One original per hook: a second vtable with a different implementation stays unhooked.
        if (*orig && vtbl[slot] != reinterpret_cast<void*>(*orig)) return;
        void* previous = nullptr;
        if (::wxl::mem::SwapPointer(&vtbl[slot], reinterpret_cast<void*>(hook), &previous))
            *orig = reinterpret_cast<Fn>(previous);
    }

    /** @brief Lazily loads the real d3d9 and resolves its create entry points on first use. */
    void EnsureReal()
    {
        if (g_realCreate9 || g_realCreate9Ex) return;
        // The engine frees d3d9.dll after its hardware probe; the factory vtable keeps our hooks, so the
        // proxy is pinned to outlive that FreeLibrary.
        HMODULE self = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCSTR>(&EnsureReal), &self))
        {
            Log("d3d9proxy: could not pin the proxy (win32=%lu)", GetLastError());
            self = nullptr;
        }
        // DXVK when asked for and present; the system d3d9 otherwise, and when DXVK does not load.
        HMODULE r = LoadDxvkD3D9(self);
        const char* backend = r ? "dxvk" : "native";
        if (!r) r = LoadSystemD3D9();
        if (!r) { Log("d3d9proxy: FAILED to load the system d3d9.dll"); return; }
        // Through void*: a FARPROC is not the entry's type, and a direct cast is what GCC's
        // -Wcast-function-type flags (MSVC is silent either way).
        g_realCreate9   = reinterpret_cast<Create9Fn>(reinterpret_cast<void*>(GetProcAddress(r, "Direct3DCreate9")));
        g_realCreate9Ex = reinterpret_cast<Create9ExFn>(reinterpret_cast<void*>(GetProcAddress(r, "Direct3DCreate9Ex")));
        Log("d3d9proxy: backend %s: d3d9 loaded (9=%p Ex=%p)", backend, g_realCreate9, g_realCreate9Ex);
    }

    /**
     * @brief Loads WarcraftXL.dll once, from the first Direct3DCreate9* call.
     *
     * Deliberately NOT done in DllMain: calling LoadLibrary under the loader lock can deadlock against
     * WarcraftXL.dll's own attach work. The engine creates its factory long before the runtime is needed,
     * so first-create is early enough and runs outside the loader lock.
     */
    void EnsureRuntimeLoaded()
    {
        static bool attempted = false;
        if (attempted) return;
        attempted = true;
        if (!LoadLibraryA("WarcraftXL.dll"))
            Log("d3d9proxy: WarcraftXL.dll not loaded (win32=%lu)", GetLastError());
    }
}

/**
 * @brief Proxy entry point for Direct3DCreate9: loads the runtime, then forwards to the system d3d9.
 * @param sdkVersion  D3D SDK version passed by the caller.
 * @return The native IDirect3D9 factory, or null on failure.
 */
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion)
{
    EnsureReal();
    EnsureRuntimeLoaded();
    if (UseEx() && g_realCreate9Ex)
    {
        // A D3D9Ex factory answers every IDirect3D9 call; its CreateDevice makes a D3D9Ex device.
        IDirect3D9Ex* ex = nullptr;
        if (SUCCEEDED(g_realCreate9Ex(sdkVersion, &ex)) && ex)
        {
            HookSlot(ex, kSlotCreateDeviceEx, &hkCreateDeviceEx, &g_origCreateDeviceEx);
            HookSlot(static_cast<IDirect3D9*>(ex), kSlotCreateDevice, &hkCreateDeviceAsEx, &g_origCreateDeviceOnEx);
            return ex;
        }
        Log("d3d9proxy: Direct3DCreate9Ex failed; classic D3D9");
    }
    IDirect3D9* factory = g_realCreate9 ? g_realCreate9(sdkVersion) : nullptr;
    if (factory) HookSlot(factory, kSlotCreateDevice, &hkCreateDevice9, &g_origCreateDevice9);
    return factory;
}

/**
 * @brief Proxy entry point for Direct3DCreate9Ex: loads the runtime, then forwards to the system d3d9.
 * @param sdkVersion  D3D SDK version passed by the caller.
 * @param out         receives the native IDirect3D9Ex factory.
 * @return S_OK on success, E_NOINTERFACE when the system d3d9 has no Ex export.
 */
extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** out)
{
    EnsureReal();
    EnsureRuntimeLoaded();
    if (g_realCreate9Ex)
    {
        const HRESULT hr = g_realCreate9Ex(sdkVersion, out);
        if (SUCCEEDED(hr) && out && *out)
        {
            HookSlot(*out, kSlotCreateDeviceEx, &hkCreateDeviceEx, &g_origCreateDeviceEx);
            if (UseEx())
                HookSlot(static_cast<IDirect3D9*>(*out), kSlotCreateDevice, &hkCreateDeviceAsEx, &g_origCreateDeviceOnEx);
            else
                HookSlot(static_cast<IDirect3D9*>(*out), kSlotCreateDevice, &hkCreateDeviceOnEx, &g_origCreateDeviceOnEx);
        }
        return hr;
    }
    if (out) *out = nullptr;
    return E_NOINTERFACE;
}

/**
 * @brief Fills a snapshot of every live D3D resource by creator, type and pool, for the core's diagnostic.
 * @param out      receives the snapshot.
 * @param outSize  sizeof the caller's GpuStats, refused when smaller than this build's.
 * @return 1 on success, 0 when out is null or too small.
 */
extern "C" int __cdecl WXL_ProxyStats(::wxl::gpu::GpuStats* out, uint32_t outSize)
{
    return ::wxl::gpu::resources::Snapshot(out, outSize);
}

/**
 * @brief Process-attach entry point. Intentionally does no work.
 *
 * Loading the real d3d9 and WarcraftXL.dll happens lazily in Direct3DCreate9(Ex): a LoadLibrary issued
 * here would run under the loader lock and can deadlock against the loaded DLL's attach.
 * @param reason  DLL notification reason.
 * @return TRUE.
 */
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    (void)reason;
    return TRUE;
}
