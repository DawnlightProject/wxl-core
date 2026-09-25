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
// one. It forwards the factory-create exports to the real system d3d9, loads WarcraftXL.dll into the
// process, and has the factory create a non-pure device. All rendering runs on the client's native device.

#include <windows.h>
#include <d3d9.h>

#include "common/Log.hpp"
#include "common/Mem.hpp"

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
     * @brief Loads the real d3d9 from the system directory, falling back to a local d3d9_real.dll.
     *
     * A loaded module is keyed by full path, so the system d3d9.dll is a distinct module from this proxy
     * despite the shared base name.
     * @return Handle to the real d3d9 module, or null on failure.
     */
    HMODULE LoadRealD3D9()
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
        return g_origCreateDevice9(self, adapter, type, wnd, StripPure(flags, "CreateDevice"), pp, out);
    }

    HRESULT STDMETHODCALLTYPE hkCreateDeviceOnEx(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND wnd,
                                                 DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
    {
        return g_origCreateDeviceOnEx(self, adapter, type, wnd, StripPure(flags, "CreateDevice(Ex factory)"), pp, out);
    }

    HRESULT STDMETHODCALLTYPE hkCreateDeviceEx(IDirect3D9Ex* self, UINT adapter, D3DDEVTYPE type, HWND wnd,
                                               DWORD flags, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode,
                                               IDirect3DDevice9Ex** out)
    {
        return g_origCreateDeviceEx(self, adapter, type, wnd, StripPure(flags, "CreateDeviceEx"), pp, mode, out);
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
            Log("d3d9proxy: could not pin the proxy (win32=%lu)", GetLastError());
        HMODULE r = LoadRealD3D9();
        if (!r) { Log("d3d9proxy: FAILED to load the system d3d9.dll"); return; }
        g_realCreate9   = reinterpret_cast<Create9Fn>(GetProcAddress(r, "Direct3DCreate9"));
        g_realCreate9Ex = reinterpret_cast<Create9ExFn>(GetProcAddress(r, "Direct3DCreate9Ex"));
        Log("d3d9proxy: system d3d9 loaded (9=%p Ex=%p)", g_realCreate9, g_realCreate9Ex);
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
            HookSlot(*out, kSlotCreateDevice, &hkCreateDeviceOnEx, &g_origCreateDeviceOnEx);
            HookSlot(*out, kSlotCreateDeviceEx, &hkCreateDeviceEx, &g_origCreateDeviceEx);
        }
        return hr;
    }
    if (out) *out = nullptr;
    return E_NOINTERFACE;
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
