// Memory diagnostic: Wow.exe's address space, D3D resources by creator, and the archive layer's cost.
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

// Logged at world entry, every 30 s, and whenever the process reaches a new peak. It is the before/after
// proof for wxl-host: the address-space map, every D3D resource by pool and creator (from the d3d9 proxy),
// what the client's own archive layer holds (MPQ handles, table sizes, the mount phase's cost), and the
// memory map (engine allocations by source, process memory by owner, modules; engine/diag/MemoryMap).

#include "engine/diag/MemoryDiag.hpp"

#include "common/Config.hpp"
#include "common/Log.hpp"
#include "engine/diag/MemoryMap.hpp"
#include "engine/diag/MemoryStats.hpp"
#include "engine/events/EventScript.hpp"
#include "engine/gpu/GpuStats.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/ui/ImGuiHost.hpp"
#include "offsets/engine/Io.hpp"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstdarg>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#pragma intrinsic(_ReturnAddress)

namespace
{
    namespace io = wxl::offsets::engine::io;
    namespace gpu = wxl::gpu;
    namespace map = wxl::diag::memory::map;
    using wxl::diag::memory::Counters;
    using wxl::diag::memory::kMb;
    using wxl::diag::memory::ReadCounters;
    using wxl::diag::memory::VaMap;
    using wxl::diag::memory::WalkAddressSpace;

    constexpr DWORD kTickMs = 250;                // peak windows sample at this rate
    constexpr DWORD kTicksPerSecond = 1000 / kTickMs;
    constexpr DWORD kReportPeriodS = 30;
    constexpr uint64_t kPeakStep = 32ull << 20;   // a new peak is logged once it clears the last by this
    constexpr ULONGLONG kPeakLogGapMs = 10000;
    constexpr int kHostedWarnLimit = 20;

    // --- archives --------------------------------------------------------------------------------

    struct Archive
    {
        std::string name;
        int         priority = 0;
        bool        hosted = false;
        bool        ok = false;
        bool        folder = false;
        bool        measured = false;
        uint32_t    hashEntries = 0;
        uint32_t    blockEntries = 0;
        bool        hiBlock = false;
    };

    std::mutex            g_archiveMutex;
    std::vector<Archive>  g_archives;
    std::vector<std::string> g_mpqPaths;      // distinct .MPQ files Wow.exe opened
    std::atomic<uint32_t> g_mpqOpens{ 0 };      // successful CreateFile calls on .MPQ files
    std::atomic<uint32_t> g_mpqOpensHosted{ 0 };// the same, after the archive layer moved to the host
    std::atomic<uint32_t> g_folderOpens{ 0 };   // files opened inside a folder archive (Patch-N.MPQ\...)
    std::atomic<uint32_t> g_folderOpensHosted{ 0 };
    std::atomic<bool>     g_hosted{ false };
    std::atomic<int>      g_hostedWarns{ 0 };

    thread_local bool t_internal = false;       // the diagnostic's own reads are not client opens

    int64_t g_mountPrivateDelta = 0;
    int64_t g_mountVaDelta = 0;
    bool    g_mountMeasured = false;

    bool EndsWithMpq(const char* s, size_t n)
    {
        return n >= 4 && _strnicmp(s + n - 4, ".mpq", 4) == 0;
    }

    /// 1 for an archive file, 2 for a file inside a folder archive, 0 otherwise.
    int ClassifyPath(const char* path)
    {
        if (!path) return 0;
        const size_t n = std::strlen(path);
        if (EndsWithMpq(path, n)) return 1;
        for (size_t i = 0; i + 5 <= n; ++i)
            if (path[i] == '.' && _strnicmp(path + i, ".mpq", 4) == 0 && (path[i + 4] == '\\' || path[i + 4] == '/'))
                return 2;
        return 0;
    }

    void NoteOpen(const char* path, bool ok, void* caller)
    {
        const int kind = ClassifyPath(path);
        if (!kind || !ok) return;
        const bool hosted = g_hosted.load(std::memory_order_relaxed);
        if (kind == 1)
        {
            (hosted ? g_mpqOpensHosted : g_mpqOpens).fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(g_archiveMutex);
                if (std::find(g_mpqPaths.begin(), g_mpqPaths.end(), path) == g_mpqPaths.end())
                    g_mpqPaths.emplace_back(path);
            }
            if (hosted && g_hostedWarns.fetch_add(1) < kHostedWarnLimit)
                WLOG_WARN("memory: Wow.exe opened archive '%s' while archives are hosted (caller %p)", path, caller);
        }
        else
        {
            (hosted ? g_folderOpensHosted : g_folderOpens).fetch_add(1, std::memory_order_relaxed);
        }
    }

    using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    CreateFileAFn g_origCreateFileA = nullptr;
    CreateFileWFn g_origCreateFileW = nullptr;

    HANDLE WINAPI hkCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                                DWORD flags, HANDLE templ)
    {
        const HANDLE h = g_origCreateFileA(name, access, share, sa, disposition, flags, templ);
        if (!t_internal) NoteOpen(name, h != INVALID_HANDLE_VALUE, _ReturnAddress());
        return h;
    }

    HANDLE WINAPI hkCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                DWORD disposition, DWORD flags, HANDLE templ)
    {
        const HANDLE h = g_origCreateFileW(name, access, share, sa, disposition, flags, templ);
        if (!t_internal && name)
        {
            char narrow[MAX_PATH * 2];
            if (WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof narrow, nullptr, nullptr) > 0)
                NoteOpen(narrow, h != INVALID_HANDLE_VALUE, _ReturnAddress());
        }
        return h;
    }

    io::MopaqOpenArchiveFn g_origMopaqOpen = nullptr;

    char __cdecl hkMopaqOpen(const char* name, int priority, uint32_t flags, void** out)
    {
        const char ok = g_origMopaqOpen(name, priority, flags, out);
        if (!g_hosted.load(std::memory_order_relaxed))
            wxl::diag::memory::RecordArchive(name, priority, false, ok != 0);
        return ok;
    }

    io::InitializeWowConfigFn g_origInitConfig = nullptr;

    void __cdecl hkInitConfig()
    {
        const uint64_t privBefore = ReadCounters().privateUsage;
        const uint64_t vaBefore = WalkAddressSpace().Used();
        g_origInitConfig();
        g_mountPrivateDelta = int64_t(ReadCounters().privateUsage) - int64_t(privBefore);
        g_mountVaDelta = int64_t(WalkAddressSpace().Used()) - int64_t(vaBefore);
        g_mountMeasured = true;
    }

    /// Reads one archive's header to size the tables the client keeps for it.
    void MeasureArchive(Archive& a)
    {
        a.measured = true;
        t_internal = true;
        const HANDLE f = CreateFileA(a.name.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, 0, nullptr);
        t_internal = false;
        if (f == INVALID_HANDLE_VALUE)
        {
            const DWORD attr = GetFileAttributesA(a.name.c_str());
            a.folder = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
            return;
        }

        // The header sits at 0 or at a 512-byte boundary, possibly behind a user-data block.
        uint8_t buf[0x2C] = {};
        for (uint64_t offset = 0; offset < (64ull << 20); offset += 0x200)
        {
            LARGE_INTEGER at{};
            at.QuadPart = int64_t(offset);
            DWORD got = 0;
            if (!SetFilePointerEx(f, at, nullptr, FILE_BEGIN) || !ReadFile(f, buf, sizeof buf, &got, nullptr) || got < 0x20)
                break;
            if (std::memcmp(buf, "MPQ\x1B", 4) == 0)
            {
                uint32_t headerOffset = 0;
                std::memcpy(&headerOffset, buf + 8, 4);
                if (!headerOffset) break;
                offset += headerOffset;
                at.QuadPart = int64_t(offset);
                if (!SetFilePointerEx(f, at, nullptr, FILE_BEGIN) || !ReadFile(f, buf, sizeof buf, &got, nullptr) || got < 0x20)
                    break;
            }
            if (std::memcmp(buf, "MPQ\x1A", 4) == 0)
            {
                uint16_t version = 0;
                std::memcpy(&version, buf + 0x0C, 2);
                std::memcpy(&a.hashEntries, buf + 0x18, 4);
                std::memcpy(&a.blockEntries, buf + 0x1C, 4);
                uint64_t hiBlockPos = 0;
                if (version >= 1 && got >= 0x28) std::memcpy(&hiBlockPos, buf + 0x20, 8);
                a.hiBlock = hiBlockPos != 0;
                break;
            }
            if (offset >= (1ull << 20)) break;   // no header in the first MB: not worth a longer scan
        }
        CloseHandle(f);
    }

    // --- the proxy's resource statistics ---------------------------------------------------------

    gpu::ProxyStatsFn ProxyStats()
    {
        static gpu::ProxyStatsFn fn = nullptr;
        static bool looked = false;
        if (looked) return fn;

        // The proxy is the d3d9.dll next to Wow.exe; the system one carries no WXL_ProxyStats.
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* slash = n ? wcsrchr(path, L'\\') : nullptr;
        if (!slash) return nullptr;
        wcscpy_s(slash + 1, MAX_PATH - size_t(slash + 1 - path), L"d3d9.dll");
        if (HMODULE proxy = GetModuleHandleW(path))
            fn = reinterpret_cast<gpu::ProxyStatsFn>(GetProcAddress(proxy, "WXL_ProxyStats"));
        looked = fn != nullptr;   // the proxy loads after us: keep looking until it is there
        return fn;
    }

    const char* kPoolNames[gpu::kPoolCount] = { "DEFAULT", "MANAGED", "SYSTEMMEM", "SCRATCH" };
    const char* kTypeNames[gpu::kTypeCount] = { "tex", "cube", "vol", "vb", "ib", "surf" };

    /// Appends printf-style text to a fixed buffer, keeping it terminated.
    void Append(char* buf, size_t cap, const char* fmt, ...)
    {
        const size_t used = std::strlen(buf);
        if (used + 1 >= cap) return;
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf + used, cap - used, fmt, args);
        va_end(args);
    }

    void ReportGpu(const char* reason)
    {
        const gpu::ProxyStatsFn fn = ProxyStats();
        if (!fn)
        {
            WLOG_INFO("memory[%s]: d3d9 proxy statistics unavailable (proxy not loaded or too old)", reason);
            return;
        }
        static gpu::GpuStats s;   // too large for the stack of a worker; one reporter at a time
        if (!fn(&s, sizeof s) || s.version != gpu::kGpuStatsVersion)
        {
            WLOG_INFO("memory[%s]: d3d9 proxy statistics version mismatch", reason);
            return;
        }

        // Totals per pool, and the per-type split of each pool.
        uint64_t poolBytes[gpu::kPoolCount] = {};
        uint32_t poolCount[gpu::kPoolCount] = {};
        uint64_t typeBytes[gpu::kPoolCount][gpu::kTypeCount] = {};
        uint32_t typeCount[gpu::kPoolCount][gpu::kTypeCount] = {};
        for (uint32_t c = 0; c < s.creatorCount; ++c)
            for (uint32_t t = 0; t < gpu::kTypeCount; ++t)
                for (uint32_t p = 0; p < gpu::kPoolCount; ++p)
                {
                    const gpu::GpuCell& cell = s.cells[c][t][p];
                    poolBytes[p] += cell.bytes;
                    poolCount[p] += cell.count;
                    typeBytes[p][t] += cell.bytes;
                    typeCount[p][t] += cell.count;
                }

        char line[1024] = {};
        Append(line, sizeof line, "memory[%s]: d3d9 %s", reason, s.d3d9ex ? "Ex" : "classic");
        for (uint32_t p = 0; p < gpu::kPoolCount; ++p)
        {
            if (!poolCount[p]) continue;
            Append(line, sizeof line, " | %s %.1f MB in %u (", kPoolNames[p], poolBytes[p] / kMb, poolCount[p]);
            bool first = true;
            for (uint32_t t = 0; t < gpu::kTypeCount; ++t)
            {
                if (!typeCount[p][t]) continue;
                Append(line, sizeof line, "%s%s %.1f/%u", first ? "" : ", ", kTypeNames[t], typeBytes[p][t] / kMb,
                       typeCount[p][t]);
                first = false;
            }
            Append(line, sizeof line, ")");
        }
        if (s.untracked) Append(line, sizeof line, " | %llu unattributed", static_cast<unsigned long long>(s.untracked));
        WLOG_INFO("%s", line);

        // One line per creator: what each module holds, by pool.
        for (uint32_t c = 0; c < s.creatorCount; ++c)
        {
            char row[768] = {};
            bool any = false;
            Append(row, sizeof row, "memory[%s]:   %s", reason, s.creators[c]);
            for (uint32_t p = 0; p < gpu::kPoolCount; ++p)
            {
                uint64_t bytes = 0;
                uint32_t count = 0;
                for (uint32_t t = 0; t < gpu::kTypeCount; ++t)
                {
                    bytes += s.cells[c][t][p].bytes;
                    count += s.cells[c][t][p].count;
                }
                if (!count) continue;
                any = true;
                Append(row, sizeof row, " | %s %.1f MB in %u", kPoolNames[p], bytes / kMb, count);
            }
            if (any) WLOG_INFO("%s", row);
        }

        const gpu::GpuEmulation& e = s.emulation;
        if (s.d3d9ex)
            WLOG_INFO("memory[%s]: managed emulation: %llu objects, %.1f MB no longer mirrored in Wow.exe | mirrors "
                      "%.1f MB, staging %.1f MB | uploads %llu (%.1f MB), readbacks %llu | backing sent %llu (%.1f MB), "
                      "dropped %llu | restores %llu",
                      reason, static_cast<unsigned long long>(e.emulatedObjects), e.emulatedBytes / kMb,
                      e.mirrorBytes / kMb, e.stagingBytes / kMb, static_cast<unsigned long long>(e.uploads),
                      e.uploadBytes / kMb, static_cast<unsigned long long>(e.readbacks),
                      static_cast<unsigned long long>(e.backingSent), e.backingBytes / kMb,
                      static_cast<unsigned long long>(e.backingDropped), static_cast<unsigned long long>(e.restores));
    }

    void ReportArchives(const char* reason)
    {
        std::vector<Archive> snapshot;
        size_t distinctMpq = 0;
        {
            std::lock_guard<std::mutex> lock(g_archiveMutex);
            for (Archive& a : g_archives)
                if (!a.measured && !a.hosted) MeasureArchive(a);
            snapshot = g_archives;
            distinctMpq = g_mpqPaths.size();
        }

        uint32_t files = 0, folders = 0, hosted = 0;
        uint64_t hashBytes = 0, blockBytes = 0, hiBytes = 0, recordBytes = 0;
        for (const Archive& a : snapshot)
        {
            if (!a.ok) continue;
            if (a.hosted) { ++hosted; continue; }
            if (a.folder) { ++folders; continue; }
            ++files;
            hashBytes += uint64_t(a.hashEntries) * 16;
            blockBytes += uint64_t(a.blockEntries) * 16;
            if (a.hiBlock) hiBytes += uint64_t(a.blockEntries) * 2;
            recordBytes += uint64_t(a.blockEntries) * 0x31;   // the per-block record the client's archive object keeps
        }
        WLOG_INFO("memory[%s]: archives: %u mounted by the client (%u files, %u folders), %u hosted | .MPQ opened by "
                  "Wow.exe: %u handles on %zu files (%u after hosting) | folder-archive file opens %u (%u after "
                  "hosting) | tables ~%.1f MB (hash %.1f, block %.1f, hi-block %.1f) + per-block records ~%.1f MB",
                  reason, files + folders, files, folders, hosted, g_mpqOpens.load(), distinctMpq,
                  g_mpqOpensHosted.load(), g_folderOpens.load(), g_folderOpensHosted.load(),
                  (hashBytes + blockBytes + hiBytes) / kMb, hashBytes / kMb, blockBytes / kMb, hiBytes / kMb,
                  recordBytes / kMb);
        if (g_mountMeasured)
            WLOG_INFO("memory[%s]: archive mount phase: private %+.1f MB, address space %+.1f MB", reason,
                      g_mountPrivateDelta / kMb, g_mountVaDelta / kMb);
    }

    // --- reporting -------------------------------------------------------------------------------

    std::atomic<bool> g_worldEntered{ false };
    uint64_t g_peakPrivate = 0;
    uint64_t g_peakVa = 0;
    void (*g_extraLine)(wxl::diag::memory::HostLine&) = nullptr;

    void Report(const char* reason)
    {
        const VaMap m = WalkAddressSpace();
        const Counters c = ReadCounters();
        g_peakVa = std::max(g_peakVa, m.Used());
        g_peakPrivate = std::max(g_peakPrivate, c.privateUsage);

        WLOG_INFO("memory[%s]: address space used %.1f MB, free %.1f MB, largest free %.1f MB (%u free blocks >= 64 MB) "
                  "| image %.1f | mapped %.1f (+%.1f reserved) | private %.1f (+%.1f reserved) | private usage %.1f MB, "
                  "working set %.1f MB | peaks: private %.1f MB, address space %.1f MB",
                  reason, m.Used() / kMb, m.freeBytes / kMb, m.largestFree / kMb, m.freeBlocks64,
                  (m.imageCommit + m.imageReserve) / kMb, m.mappedCommit / kMb, m.mappedReserve / kMb,
                  m.privateCommit / kMb, m.privateReserve / kMb, c.privateUsage / kMb, c.workingSet / kMb,
                  g_peakPrivate / kMb, g_peakVa / kMb);
        ReportGpu(reason);
        ReportArchives(reason);
        if (g_extraLine)
        {
            wxl::diag::memory::HostLine extra{};
            g_extraLine(extra);
            if (extra.text[0]) WLOG_INFO("memory[%s]: %s", reason, extra.text);
        }
        map::LogSections(reason);
    }

    std::atomic<bool>      g_dumpRequested{ false };
    std::atomic<ULONGLONG> g_lastDumpMs{ 0 };

    DWORD WINAPI ReporterThread(LPVOID)
    {
        uint64_t loggedPeak = 0;
        ULONGLONG lastPeakLog = 0;
        for (DWORD tick = 1;; ++tick)
        {
            Sleep(kTickMs);
            map::Tick(GetTickCount64());
            if (g_dumpRequested.exchange(false) && map::WriteDump("requested")) g_lastDumpMs.store(GetTickCount64());
            if (tick % kTicksPerSecond) continue;
            const DWORD second = tick / kTicksPerSecond;

            const Counters c = ReadCounters();
            g_peakPrivate = std::max(g_peakPrivate, c.privateUsage);
            if (second % 5 == 0) g_peakVa = std::max(g_peakVa, WalkAddressSpace().Used());

            if (g_worldEntered.exchange(false)) Report("world-enter");
            if (second % kReportPeriodS == 0) Report("30s");

            const uint64_t peak = std::max(g_peakPrivate, g_peakVa);
            const ULONGLONG now = GetTickCount64();
            if (peak >= loggedPeak + kPeakStep && now - lastPeakLog >= kPeakLogGapMs)
            {
                loggedPeak = peak;
                lastPeakLog = now;
                Report("peak");
            }
        }
    }

    class MemoryDiagScript final : public wxl::events::EventScript
    {
    public:
        MemoryDiagScript() { on<&MemoryDiagScript::OnWorldEnter>(wxl::events::Event::OnWorldEnter); }

        void OnWorldEnter(const wxl::events::WorldEnterArgs&)
        {
            if (!m_reported)
            {
                m_reported = true;
                g_worldEntered.store(true);
            }
        }

    private:
        bool m_reported = false;
    };

    bool Enabled()
    {
        static const bool on = wxl::config::Env("WXL_DIAG_MEMORY", true);
        return on;
    }

    MemoryDiagScript g_script;

    /// The overlay's Memory panel: live figures and the button that writes the full map.
    void __cdecl Panel(void*)
    {
        namespace ui = wxl::ui::c;
        char text[512];
        map::Summary(text, sizeof text);
        ui::Text(text);
        ui::Separator();
        if (ui::Button("Write memory map")) g_dumpRequested.store(true);
        ui::ItemTooltip("Writes Logs\\memory-map.txt: every allocation source, heap, reservation and module.\n"
                        "It walks every heap, the engine's included: expect a short hitch.");
        const ULONGLONG last = g_lastDumpMs.load();
        char status[96] = "not written yet this session";
        if (last) std::snprintf(status, sizeof status, "written %llu s ago", (GetTickCount64() - last) / 1000);
        ui::TextDisabled(status);
    }

    /// Boot phase: the allocator and archive hooks must precede the client's CRT and mount, the reporter
    /// needs only a thread.
    bool Install()
    {
        if (!Enabled()) return true;

        // Before the client's CRT allocates anything: every engine block is then tagged from the first.
        map::Install();
        wxl::ui::AddPanel("Memory", &Panel, nullptr);

        if (HMODULE k32 = GetModuleHandleA("kernel32.dll"))
        {
            if (void* a = reinterpret_cast<void*>(GetProcAddress(k32, "CreateFileA")))
                wxl::hook::Install("Diag.CreateFileA", a, reinterpret_cast<void*>(&hkCreateFileA),
                                   reinterpret_cast<void**>(&g_origCreateFileA), -100);
            if (void* w = reinterpret_cast<void*>(GetProcAddress(k32, "CreateFileW")))
                wxl::hook::Install("Diag.CreateFileW", w, reinterpret_cast<void*>(&hkCreateFileW),
                                   reinterpret_cast<void**>(&g_origCreateFileW), -100);
        }
        wxl::hook::Install("Diag.MopaqOpenArchive", io::kMopaqOpenArchive, &hkMopaqOpen, &g_origMopaqOpen, -100);
        // After the archive guard's own party: that chain is already live, so this one cannot be its head.
        wxl::hook::Install("Diag.InitializeWowConfig", io::kInitializeWowConfig, &hkInitConfig, &g_origInitConfig,
                           100);

        CloseHandle(CreateThread(nullptr, 0, &ReporterThread, nullptr, 0, nullptr));
        WLOG_INFO("memory: diagnostic armed (world entry, every %lu s, each new peak, loading screens and zone changes; "
                  "full map from the overlay's Memory panel; WXL_DIAG_MEMORY=0 disables)",
                  kReportPeriodS);
        return true;
    }
}

namespace wxl::diag::memory
{
    void RecordArchive(const char* name, int priority, bool hosted, bool ok)
    {
        if (!Enabled()) return;
        Archive a;
        a.name = name ? name : "";
        a.priority = priority;
        a.hosted = hosted;
        a.ok = ok;
        std::lock_guard<std::mutex> lock(g_archiveMutex);
        g_archives.push_back(std::move(a));
    }

    void SetArchivesHosted(bool hosted)
    {
        g_hosted.store(hosted);
    }

    void SetExtraLine(void (*fill)(HostLine& out))
    {
        g_extraLine = fill;
    }
}

WXL_REGISTER_FEATURE_PHASED("memory-diag", true, Install, ::wxl::hook::Phase::Boot)
