// Wow.exe's process memory outside the engine allocator: heaps, VirtualAlloc regions, stacks, modules.
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

// The heap manager takes its segments from NtAllocateVirtualMemory, so the VirtualAlloc hook sees only
// what modules reserve themselves (the M2 arena, D3D, drivers). Heaps are summarised with HeapSummary,
// which walks every block under the heap's lock: the CRT heap (the engine's, counted exactly by the
// allocation hooks) and any heap that took long are walked only for the full dump.

#include "engine/diag/ProcessMap.hpp"

#include "common/Log.hpp"
#include "engine/hook/Hook.hpp"
#include "offsets/engine/Crt.hpp"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_map>

#pragma intrinsic(_ReturnAddress)

namespace
{
    namespace crt = wxl::offsets::engine::crt;
    using wxl::diag::process::Group;

    constexpr double kSlowWalkMs = 4.0;   // a heap walk longer than this waits for the full dump

    // --- VirtualAlloc: who reserved each allocation granule -------------------------------------------

    constexpr uint32_t kGranules = 0x10000;        // 64 KB allocation granules in 4 GB
    std::atomic<uint32_t>* g_vaCreator = nullptr;  // allocation base >> 16 -> the caller's return address; by Install
    std::vector<uintptr_t>* g_preBoot = nullptr;   // private reservations older than the hooks (the M2 arena among them)

    using VirtualAllocFn = LPVOID(WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
    using VirtualFreeFn  = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD);
    VirtualAllocFn g_origVirtualAlloc = nullptr;
    VirtualFreeFn  g_origVirtualFree = nullptr;

    LPVOID WINAPI hkVirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect)
    {
        LPVOID p = g_origVirtualAlloc(address, size, type, protect);
        if (p && (!address || (type & MEM_RESERVE)))
            g_vaCreator[reinterpret_cast<uintptr_t>(p) >> 16].store(uint32_t(reinterpret_cast<uintptr_t>(_ReturnAddress())),
                                                                   std::memory_order_relaxed);
        return p;
    }

    BOOL WINAPI hkVirtualFree(LPVOID address, SIZE_T size, DWORD type)
    {
        // Cleared first: once freed, another thread may reserve the same base before we return.
        if (type & MEM_RELEASE) g_vaCreator[reinterpret_cast<uintptr_t>(address) >> 16].store(0, std::memory_order_relaxed);
        return g_origVirtualFree(address, size, type);
    }

    // --- HeapCreate: who created each heap ------------------------------------------------------------

    struct HeapRecord
    {
        std::atomic<uintptr_t> handle;
        uint32_t               creator;
        DWORD                  options;
    };
    HeapRecord g_heapRecords[256];

    using HeapCreateFn  = HANDLE(WINAPI*)(DWORD, SIZE_T, SIZE_T);
    using HeapDestroyFn = BOOL(WINAPI*)(HANDLE);
    HeapCreateFn  g_origHeapCreate = nullptr;
    HeapDestroyFn g_origHeapDestroy = nullptr;

    HANDLE WINAPI hkHeapCreate(DWORD options, SIZE_T initial, SIZE_T maximum)
    {
        const HANDLE h = g_origHeapCreate(options, initial, maximum);
        if (!h) return h;
        const uint32_t creator = uint32_t(reinterpret_cast<uintptr_t>(_ReturnAddress()));
        for (HeapRecord& r : g_heapRecords)
        {
            uintptr_t expected = 0;
            if (r.handle.load(std::memory_order_relaxed) == 0 &&
                r.handle.compare_exchange_strong(expected, reinterpret_cast<uintptr_t>(h)))
            {
                r.creator = creator;
                r.options = options;
                break;
            }
        }
        return h;
    }

    BOOL WINAPI hkHeapDestroy(HANDLE h)
    {
        for (HeapRecord& r : g_heapRecords)
            if (r.handle.load(std::memory_order_relaxed) == reinterpret_cast<uintptr_t>(h))
            {
                r.creator = 0;
                r.handle.store(0);
            }
        return g_origHeapDestroy(h);
    }

    const HeapRecord* RecordOf(uintptr_t h)
    {
        for (const HeapRecord& r : g_heapRecords)
            if (r.handle.load(std::memory_order_relaxed) == h) return &r;
        return nullptr;
    }

    // --- modules --------------------------------------------------------------------------------------

    std::string Utf8(const wchar_t* w)
    {
        char buf[MAX_PATH * 3];
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf, nullptr, nullptr);
        return n > 0 ? std::string(buf) : std::string();
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) c = char(tolower(static_cast<unsigned char>(c)));
        return s;
    }

    const char* BaseName(const std::string& path)
    {
        const size_t slash = path.find_last_of("\\/");
        return path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
    }

    bool StartsWith(const std::string& s, const std::string& prefix)
    {
        return s.compare(0, prefix.size(), prefix) == 0;
    }

    std::string GameDir()
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::string s = Lower(Utf8(path));
        const size_t slash = s.find_last_of('\\');
        return slash == std::string::npos ? s : s.substr(0, slash + 1);
    }

    std::string WindowsDir()
    {
        wchar_t path[MAX_PATH] = {};
        GetSystemWindowsDirectoryW(path, MAX_PATH);
        std::string s = Lower(Utf8(path));
        if (!s.empty() && s.back() != '\\') s += '\\';
        return s;
    }

    Group Classify(const std::string& path)
    {
        static const std::string game = GameDir();
        static const std::string windows = WindowsDir();
        const std::string p = Lower(path);
        const std::string name = BaseName(p);

        if (p.find("\\dxvk\\") != std::string::npos || StartsWith(name, "dxvk")) return Group::Dxvk;
        if (StartsWith(name, "wxl-") || name == "warcraftxl.dll" || StartsWith(p, game + "extensions\\") ||
            p == game + "d3d9.dll")
            return Group::Ours;
        const bool gpu = StartsWith(name, "nv") || StartsWith(name, "ati") || StartsWith(name, "amd") ||
                         StartsWith(name, "ig") || StartsWith(name, "intel");
        if (p.find("\\driverstore\\") != std::string::npos || (gpu && StartsWith(p, windows))) return Group::Driver;
        if (StartsWith(p, windows)) return Group::System;
        if (StartsWith(p, game)) return Group::Game;
        return Group::Other;
    }

    uint32_t ImageSize(uintptr_t base)
    {
        __try
        {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
            return nt->Signature == IMAGE_NT_SIGNATURE ? nt->OptionalHeader.SizeOfImage : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    struct ModuleName
    {
        std::string name;
        Group       group = Group::Other;
    };
    std::mutex g_moduleMutex;
    /// Image base -> name, filled on first sight. Built on first use: nothing exists while the diagnostic is off.
    std::unordered_map<uintptr_t, ModuleName>& ModuleNames()
    {
        static std::unordered_map<uintptr_t, ModuleName> names;
        return names;
    }

    ModuleName NameOfImage(uintptr_t base)
    {
        std::lock_guard<std::mutex> lock(g_moduleMutex);
        const auto it = ModuleNames().find(base);
        if (it != ModuleNames().end()) return it->second;
        wchar_t path[MAX_PATH] = {};
        ModuleName m;
        if (GetModuleFileNameW(reinterpret_cast<HMODULE>(base), path, MAX_PATH))
        {
            const std::string full = Utf8(path);
            m.group = Classify(full);
            // DXVK's d3d9.dll shares its file name with our proxy.
            m.name = std::string(m.group == Group::Dxvk ? "dxvk/" : "") + BaseName(full);
        }
        else if (K32GetMappedFileNameW(GetCurrentProcess(), reinterpret_cast<void*>(base), path, MAX_PATH))
        {
            m.name = BaseName(Utf8(path));   // an image the loader does not list
        }
        else
        {
            m.name = "(image)";
        }
        ModuleNames().emplace(base, m);
        return m;
    }

    // --- heaps ----------------------------------------------------------------------------------------

    struct SummaryData
    {
        DWORD  cb;
        SIZE_T allocated, committed, reserved, maxReserve;
    };
    using HeapSummaryFn = BOOL(WINAPI*)(HANDLE, DWORD, SummaryData*);

    bool GuardedSummary(HeapSummaryFn fn, HANDLE h, SummaryData* s)
    {
        __try
        {
            return fn(h, 0, s) != FALSE;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    /// The last walk of each heap, reused while a heap is not walked. Reporter thread only.
    std::unordered_map<uintptr_t, wxl::diag::process::Heap>& LastHeaps()
    {
        static std::unordered_map<uintptr_t, wxl::diag::process::Heap> heaps;
        return heaps;
    }

    struct ThreadBasicInformation
    {
        LONG      exitStatus;
        PVOID     teb;
        PVOID     clientId[2];
        ULONG_PTR affinity;
        LONG      priority;
        LONG      basePriority;
    };
    using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

    uintptr_t ReadStackBase(uintptr_t teb)
    {
        __try
        {
            return *reinterpret_cast<const uintptr_t*>(teb + 0xE0C);   // TEB.DeallocationStack (x86)
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }
}

namespace wxl::diag::process
{
    const char* GroupName(Group g)
    {
        switch (g)
        {
        case Group::Game:   return "game";
        case Group::Ours:   return "ours";
        case Group::Dxvk:   return "dxvk";
        case Group::System: return "system";
        case Group::Driver: return "drivers";
        default:            return "other";
        }
    }

    bool Install()
    {
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        if (!kb) kb = GetModuleHandleW(L"kernel32.dll");
        if (!kb) return false;
        void* table = VirtualAlloc(nullptr, sizeof(std::atomic<uint32_t>) * kGranules, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!table) return false;
        g_vaCreator = new (table) std::atomic<uint32_t>[kGranules]{};

        g_preBoot = new std::vector<uintptr_t>();
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        for (uintptr_t address = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
             address < reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof mbi) || !mbi.RegionSize) break;
            const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
            if (mbi.State != MEM_FREE && mbi.Type == MEM_PRIVATE && (g_preBoot->empty() || g_preBoot->back() != base))
                g_preBoot->push_back(base);
            address += mbi.RegionSize;
        }
        bool ok = true;
        // kernel32's exports jump here, so one hook sees both, with the original caller's return address.
        if (void* a = reinterpret_cast<void*>(GetProcAddress(kb, "VirtualAlloc")))
            ok &= wxl::hook::Install("Diag.VirtualAlloc", a, reinterpret_cast<void*>(&hkVirtualAlloc),
                                     reinterpret_cast<void**>(&g_origVirtualAlloc), -100);
        if (void* a = reinterpret_cast<void*>(GetProcAddress(kb, "VirtualFree")))
            ok &= wxl::hook::Install("Diag.VirtualFree", a, reinterpret_cast<void*>(&hkVirtualFree),
                                     reinterpret_cast<void**>(&g_origVirtualFree), -100);
        if (void* a = reinterpret_cast<void*>(GetProcAddress(kb, "HeapCreate")))
            ok &= wxl::hook::Install("Diag.HeapCreate", a, reinterpret_cast<void*>(&hkHeapCreate),
                                     reinterpret_cast<void**>(&g_origHeapCreate), -100);
        if (void* a = reinterpret_cast<void*>(GetProcAddress(kb, "HeapDestroy")))
            ok &= wxl::hook::Install("Diag.HeapDestroy", a, reinterpret_cast<void*>(&hkHeapDestroy),
                                     reinterpret_cast<void**>(&g_origHeapDestroy), -100);
        return ok;
    }

    std::vector<Module> Modules()
    {
        std::vector<Module> out;
        HMODULE mods[1024];
        DWORD needed = 0;
        if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) return out;
        const DWORD n = std::min<DWORD>(needed / sizeof(HMODULE), 1024);
        for (DWORD i = 0; i < n; ++i)
        {
            Module m;
            m.base = reinterpret_cast<uintptr_t>(mods[i]);
            m.size = ImageSize(m.base);
            wchar_t path[MAX_PATH] = {};
            if (!GetModuleFileNameW(mods[i], path, MAX_PATH)) continue;
            m.path = Utf8(path);
            m.group = Classify(m.path);
            m.name = std::string(m.group == Group::Dxvk ? "dxvk/" : "") + BaseName(m.path);
            out.push_back(std::move(m));
        }
        std::sort(out.begin(), out.end(), [](const Module& a, const Module& b) { return a.size > b.size; });
        return out;
    }

    std::string ModuleOf(uintptr_t address, Group* group)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!address || !VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof mbi) || mbi.Type != MEM_IMAGE)
        {
            if (group) *group = Group::Other;
            return "?";
        }
        const ModuleName m = NameOfImage(reinterpret_cast<uintptr_t>(mbi.AllocationBase));
        if (group) *group = m.group;
        return m.name;
    }

    std::vector<Heap> Heaps(bool walkAll)
    {
        static const auto summary = reinterpret_cast<HeapSummaryFn>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "HeapSummary"));
        std::vector<Heap> out;
        HANDLE handles[256];
        const DWORD n = std::min<DWORD>(GetProcessHeaps(256, handles), 256);
        const uintptr_t processHeap = reinterpret_cast<uintptr_t>(GetProcessHeap());
        const uintptr_t crtHeap = *reinterpret_cast<const uintptr_t*>(crt::kHeap);

        for (DWORD i = 0; i < n; ++i)
        {
            Heap h;
            h.handle = reinterpret_cast<uintptr_t>(handles[i]);
            const HeapRecord* record = RecordOf(h.handle);
            if (h.handle == processHeap) h.owner = "process";
            else if (h.handle == crtHeap) h.owner = "Wow.exe CRT";
            else if (record && record->creator) h.owner = ModuleOf(record->creator, &h.group);
            else h.owner = "(before boot)";

            auto& lastHeaps = LastHeaps();
            const auto last = lastHeaps.find(h.handle);
            const bool unserialized = record && (record->options & HEAP_NO_SERIALIZE);
            const bool slow = last != lastHeaps.end() && last->second.walkMs > kSlowWalkMs;
            if (!summary || unserialized || (!walkAll && (h.handle == crtHeap || slow)))
            {
                // Keep the last walk's figures; an unserialized heap is never walked from another thread.
                if (last != lastHeaps.end())
                {
                    h.allocated = last->second.allocated;
                    h.committed = last->second.committed;
                    h.reserved = last->second.reserved;
                    h.walkMs = last->second.walkMs;
                    h.walked = last->second.walked;
                }
                h.skipped = true;
                out.push_back(h);
                continue;
            }

            SummaryData s{};
            s.cb = sizeof s;
            LARGE_INTEGER f, a, b;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&a);
            h.walked = GuardedSummary(summary, handles[i], &s);
            QueryPerformanceCounter(&b);
            h.walkMs = double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart);
            if (h.walked)
            {
                h.allocated = s.allocated;
                h.committed = s.committed;
                h.reserved = s.reserved;
            }
            lastHeaps[h.handle] = h;
            out.push_back(h);
        }
        return out;
    }

    std::vector<uintptr_t> StackBases()
    {
        std::vector<uintptr_t> out;
        static const auto query = reinterpret_cast<NtQueryInformationThreadFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
        if (!query) return out;
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return out;
        const DWORD pid = GetCurrentProcessId();
        THREADENTRY32 te{};
        te.dwSize = sizeof te;
        for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te))
        {
            if (te.th32OwnerProcessID != pid) continue;
            const HANDLE t = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!t) continue;
            ThreadBasicInformation tbi{};
            if (query(t, 0, &tbi, sizeof tbi, nullptr) >= 0 && tbi.teb)
                if (const uintptr_t base = ReadStackBase(reinterpret_cast<uintptr_t>(tbi.teb))) out.push_back(base);
            CloseHandle(t);
        }
        CloseHandle(snap);
        return out;
    }

    std::vector<Allocation> Allocations(const std::vector<Heap>& heaps, bool names)
    {
        std::vector<Allocation> out;
        const std::vector<uintptr_t> stacks = StackBases();
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        uintptr_t address = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
        const uintptr_t maximum = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
        while (address < maximum)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof mbi) || !mbi.RegionSize) break;
            if (mbi.State != MEM_FREE)
            {
                const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
                if (out.empty() || out.back().base != base)
                {
                    Allocation a;
                    a.base = base;
                    a.type = mbi.Type;
                    out.push_back(a);
                }
                Allocation& a = out.back();
                a.reserved += mbi.RegionSize;
                if (mbi.State == MEM_COMMIT)
                {
                    a.committed += mbi.RegionSize;
                    if (mbi.Protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE)) a.gpuMapped = true;
                }
            }
            const uintptr_t next = address + mbi.RegionSize;
            if (next <= address) break;
            address = next;
        }

        for (Allocation& a : out)
        {
            if (a.type == MEM_IMAGE)
            {
                const ModuleName m = NameOfImage(a.base);
                a.label = m.name;
                a.group = m.group;
                continue;
            }
            if (a.type == MEM_MAPPED)
            {
                wchar_t path[MAX_PATH] = {};
                if (a.gpuMapped)
                    a.label = "gpu mapping";
                else if (names && K32GetMappedFileNameW(GetCurrentProcess(), reinterpret_cast<void*>(a.base), path, MAX_PATH))
                    a.label = std::string("file ") + BaseName(Utf8(path));
                else
                    a.label = "section";
                continue;
            }
            a.creator = g_vaCreator ? g_vaCreator[a.base >> 16].load(std::memory_order_relaxed) : 0;
            if (std::find(stacks.begin(), stacks.end(), a.base) != stacks.end())
                a.label = "stack";
            else if (const auto h = std::find_if(heaps.begin(), heaps.end(), [&](const Heap& x) { return x.handle == a.base; });
                     h != heaps.end())
            {
                a.label = "heap " + h->owner;
                a.group = h->group;
            }
            else if (a.creator)
                a.label = "VirtualAlloc " + ModuleOf(a.creator, &a.group);
            else if (a.gpuMapped)
                a.label = "gpu mapping";   // made by the kernel for a driver: no user-mode call to hook
            else if (g_preBoot && std::binary_search(g_preBoot->begin(), g_preBoot->end(), a.base))
                a.label = "private (before boot)";
            else
                a.label = "private";
        }
        return out;
    }
}
