// wxl-host-probe: drives a real wxl-host from a 32-bit process and checks every byte it serves.
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

// A correctness check, not a benchmark: the same client code WarcraftXL.dll runs (runtime/host), a real
// host, the real archives, and a byte comparison against an independently extracted copy of the client.
// Usage: wxl-host-probe <client folder> <extracted client folder> [samples]
//        wxl-host-probe <client folder> <extracted client folder> --textures [files per group, 0 for all]

#include "probe/TextureProbe.hpp"
#include "runtime/host/HostClient.hpp"

#include "common/Log.hpp"
#include "ipc/Ring.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace
{
    namespace h = wxl::host;
    using wxl::ipc::Status;

    std::wstring g_client;
    std::wstring g_extracted;
    int g_failures = 0;

    void Check(bool ok, const char* what)
    {
        std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++g_failures;
    }

    std::wstring Widen(const std::string& s)
    {
        std::wstring w(s.size(), L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.data(), int(s.size()), w.data(), int(w.size()));
        return w;
    }

    std::string Narrow(const std::wstring& w)
    {
        std::string s(w.size(), '\0');
        WideCharToMultiByte(CP_ACP, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr);
        return s;
    }

    bool ReadDisk(const std::wstring& path, std::vector<uint8_t>& out)
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        GetFileSizeEx(f, &size);
        out.resize(size_t(size.QuadPart));
        DWORD got = 0;
        const bool ok = out.empty() || (::ReadFile(f, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size());
        CloseHandle(f);
        return ok;
    }

    /// Collects up to limit file names under a folder of the extracted tree, relative to its root.
    void Collect(const std::wstring& rel, std::vector<std::string>& out, size_t limit, int depth)
    {
        if (out.size() >= limit || depth < 0) return;
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((g_extracted + L"\\" + rel + L"\\*").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) return;
        std::vector<std::wstring> dirs;
        do
        {
            if (fd.cFileName[0] == L'.') continue;
            const std::wstring child = rel + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirs.push_back(child);
            else if (out.size() < limit) out.push_back(Narrow(child));
        } while (FindNextFileW(find, &fd));
        FindClose(find);
        for (const std::wstring& d : dirs) Collect(d, out, limit, depth - 1);
    }

    /// The bytes the client should see: the Patch-4.MPQ folder wins over the archives.
    bool Expected(const std::string& name, std::vector<uint8_t>& out)
    {
        if (ReadDisk(g_client + L"\\Data\\Patch-4.MPQ\\" + Widen(name), out)) return true;
        return ReadDisk(g_extracted + L"\\" + Widen(name), out);
    }

    struct Tally
    {
        std::atomic<int> ok{ 0 }, mismatch{ 0 }, missing{ 0 }, bytes{ 0 }, staleReference{ 0 };
    };

    /// True when the extracted copy is an older version from a lower-priority archive than the one served.
    bool ReferenceIsShadowed(const std::string& name, uint32_t servedFrom, const std::vector<uint8_t>& expected)
    {
        for (uint32_t id = 1; id <= 17; ++id)
        {
            if (id == servedFrom) continue;
            h::FileData f;
            if (h::ReadFile(name.c_str(), id, f) != Status::Ok) continue;
            const bool same = f.size == expected.size() && std::memcmp(f.data, expected.data(), expected.size()) == 0;
            h::ReleaseFile(f);
            if (same) return true;
        }
        return false;
    }

    void Verify(const std::vector<std::string>& names, Tally& t, bool print)
    {
        for (const std::string& name : names)
        {
            std::vector<uint8_t> expected;
            if (!Expected(name, expected)) continue;
            h::FileData file;
            const Status s = h::ReadFile(name.c_str(), wxl::ipc::kAnyArchive, file);
            if (s != Status::Ok)
            {
                ++t.missing;
                if (print) std::printf("    missing via host: %s (status %d)\n", name.c_str(), int(s));
                continue;
            }
            const bool same = file.size == expected.size()
                && (expected.empty() || std::memcmp(file.data, expected.data(), expected.size()) == 0);
            if (same) { ++t.ok; t.bytes += int(file.size); }
            else if (ReferenceIsShadowed(name, file.archive, expected))
            {
                ++t.staleReference;
                if (print) std::printf("    extracted copy is the shadowed version: %s (served from #%u)\n", name.c_str(),
                                       file.archive);
            }
            else
            {
                ++t.mismatch;
                if (print) std::printf("    differs: %s (host %llu B, extracted %zu B)\n", name.c_str(),
                                       static_cast<unsigned long long>(file.size), expected.size());
            }
            h::ReleaseFile(file);
        }
    }

    void MountClientArchives()
    {
        // The set and priorities InitializeWowConfig computes for a frFR install (docs/host.md): patches from
        // 0x40 up in reverse name order, then the base rows from 0x3F down in table order.
        struct M { const char* name; int priority; };
        const M mounts[] = {
            { "Data\\frFR\\patch-frFR.MPQ", 0x40 }, { "Data\\patch.MPQ", 0x41 },
            { "Data\\frFR\\patch-frFR-2.MPQ", 0x42 }, { "Data\\frFR\\patch-frFR-3.MPQ", 0x43 },
            { "Data\\patch-2.MPQ", 0x44 }, { "Data\\patch-3.MPQ", 0x45 }, { "Data\\Patch-4.MPQ", 0x46 },
            { "Data\\expansion.MPQ", 0x31 }, { "Data\\lichking.MPQ", 0x30 }, { "Data\\common.MPQ", 0x2F },
            { "Data\\common-2.MPQ", 0x2E }, { "Data\\frFR\\locale-frFR.MPQ", 0x2D },
            { "Data\\frFR\\speech-frFR.MPQ", 0x2C }, { "Data\\frFR\\expansion-locale-frFR.MPQ", 0x2B },
            { "Data\\frFR\\lichking-locale-frFR.MPQ", 0x2A }, { "Data\\frFR\\expansion-speech-frFR.MPQ", 0x29 },
            { "Data\\frFR\\lichking-speech-frFR.MPQ", 0x28 },
        };
        int mounted = 0;
        for (const M& m : mounts)
        {
            uint32_t id = 0, kind = 0;
            const uint64_t t0 = wxl::ipc::NowUs();
            const Status s = h::Mount(m.name, m.priority, wxl::ipc::kAnyArchive, id, kind);
            std::printf("    mount %-40s prio 0x%02X -> %s id %u kind %u (%llu ms)\n", m.name, m.priority,
                        s == Status::Ok ? "ok" : "FAILED", id, kind,
                        static_cast<unsigned long long>((wxl::ipc::NowUs() - t0) / 1000));
            if (s == Status::Ok) ++mounted;
        }
        uint32_t id = 0, kind = 0;
        Check(h::Mount("Data\\no-such-archive.MPQ", 1, wxl::ipc::kAnyArchive, id, kind) == Status::NotFound,
              "a missing archive mounts as not found");
        Check(mounted >= 10, "the client's archives mount");
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        std::fwprintf(stderr, L"usage: wxl-host-probe <client folder> <extracted client folder> [samples]\n");
        return 2;
    }
    g_client = argv[1];
    g_extracted = argv[2];
    const size_t samples = argc > 3 ? size_t(_wtoi(argv[3])) : 400;
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash loses nothing already printed

    // The texture check runs in a child whose 0x400000 range was reserved before its loader ran.
    if (argc > 3 && !wcscmp(argv[3], L"--textures"))
    {
        const int child = wxl::probe::RespawnWithClientRange(g_client);
        if (child >= 0) return child;
    }

    wxl::log::EnableConsole("[probe] ");
    h::SetClientRoot(g_client.c_str());

    std::printf("start\n");
    const uint64_t t0 = wxl::ipc::NowUs();
    Check(h::Start(), "the host starts and accepts the session");
    std::printf("  started in %llu ms, host pid %u\n", static_cast<unsigned long long>((wxl::ipc::NowUs() - t0) / 1000),
                h::HostPid());
    if (!h::Available()) return 1;

    std::printf("mounts\n");
    MountClientArchives();

    // --textures [samples]: texture images against the client's own decoder, backing references, prefetch.
    if (argc > 3 && !wcscmp(argv[3], L"--textures"))
    {
        const size_t perGroup = argc > 4 ? size_t(_wtoi(argv[4])) : 2000;
        const int failures = g_failures + wxl::probe::Textures(g_client, g_extracted, perGroup);
        char line[1024];
        h::CounterLine(line, sizeof line);
        std::printf("%s\n", line);
        std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
        return failures ? 1 : 0;
    }

    // --where <name>: which archives hold a file, and at what size (the first hit is what the client reads).
    if (argc > 4 && !wcscmp(argv[3], L"--where"))
    {
        const std::string name = Narrow(argv[4]);
        for (uint32_t id = 1; id <= 17; ++id)
        {
            uint64_t size = 0;
            if (h::StatFile(name.c_str(), id, 0, &size, nullptr) == Status::Ok)
                std::printf("  archive #%u holds it: %llu bytes\n", id, static_cast<unsigned long long>(size));
        }
        uint32_t where = 0;
        uint64_t size = 0;
        h::StatFile(name.c_str(), wxl::ipc::kAnyArchive, 0, &size, &where);
        std::printf("  served from #%u (%llu bytes)\n", where, static_cast<unsigned long long>(size));
        return 0;
    }

    std::printf("reads\n");
    std::vector<std::string> names;
    const wchar_t* folders[] = { L"DBFilesClient", L"Interface\\Glues", L"Character\\Human\\Male", L"Textures",
                                 L"World\\Generic", L"Sound\\Music", L"Interface\\FrameXML" };
    for (const wchar_t* f : folders) Collect(f, names, names.size() + samples / 7, 4);
    std::shuffle(names.begin(), names.end(), std::mt19937(12340));
    std::printf("  %zu sample files\n", names.size());
    Tally tally;
    Verify(names, tally, true);
    std::printf("  %d identical (%.1f MB), %d with a shadowed reference, %d differ, %d missing via host\n", tally.ok.load(),
                tally.bytes.load() / (1024.0 * 1024.0), tally.staleReference.load(), tally.mismatch.load(), tally.missing.load());
    Check(tally.ok.load() > 0 && tally.missing.load() == 0 && tally.mismatch.load() == 0, "every sampled file is served, byte for byte");

    {
        h::FileData f;
        Check(h::ReadFile("Interface\\No\\Such\\File.blp", wxl::ipc::kAnyArchive, f) == Status::NotFound,
              "a missing file is reported as not found");
        uint64_t size = 0;
        Check(h::StatFile("DBFilesClient\\Spell.dbc", wxl::ipc::kAnyArchive, 0, &size, nullptr) == Status::Ok && size > 0,
              "stat finds Spell.dbc");
        Check(h::ReadFile("(listfile)", 10, f) == Status::Ok && f.size > 0, "an archive's own listfile reads from it");
        std::printf("    common.MPQ (listfile): %llu bytes\n", static_cast<unsigned long long>(f.size));
        h::ReleaseFile(f);
    }

    std::printf("big files\n");
    {
        std::vector<std::string> music;
        Collect(L"Sound\\Music", music, 4000, 6);
        size_t checked = 0;
        for (const std::string& name : music)
        {
            std::vector<uint8_t> expected;
            if (!Expected(name, expected) || expected.size() < h::BigThreshold()) continue;
            h::FileData f;
            const bool ok = h::ReadFile(name.c_str(), wxl::ipc::kAnyArchive, f) == Status::Ok && f.size == expected.size()
                && std::memcmp(f.data, expected.data(), expected.size()) == 0;
            Check(ok, ("big file through a section: " + name).c_str());
            h::ReleaseFile(f);
            if (++checked == 3) break;
        }
        if (!checked) std::printf("  (no sample above %llu KB)\n", static_cast<unsigned long long>(h::BigThreshold() >> 10));
    }

    std::printf("store\n");
    {
        std::vector<uint8_t> value(3u << 20);
        std::mt19937 rng(1);
        for (size_t i = 0; i < value.size(); ++i) value[i] = uint8_t((i / 64) ^ (rng() & 3));
        Check(h::StorePut(0x8000000000000001ull, value.data(), value.size(), wxl::ipc::Codec::Lz4, true), "put (LZ4)");
        std::vector<uint8_t> back(value.size());
        uint64_t full = 0;
        Check(h::StoreGet(0x8000000000000001ull, back.data(), back.size(), &full, 5000) && back == value,
              "get returns the same bytes");
        const char patch[] = "patched bytes";
        Check(h::StorePatch(0x8000000000000001ull, 1000, patch, sizeof patch), "patch");
        std::memcpy(value.data() + 1000, patch, sizeof patch);
        Check(h::StoreGet(0x8000000000000001ull, back.data(), back.size(), &full, 5000) && back == value,
              "get after a patch sees it");
        Check(h::StorePut(0x8000000000000002ull, value.data(), value.size(), wxl::ipc::Codec::Zstd, true), "put (zstd)");
        Check(h::StoreGet(0x8000000000000002ull, back.data(), back.size(), &full, 5000) && back == value, "zstd round trip");
        h::StoreDrop(0x8000000000000001ull);
        h::StoreDrop(0x8000000000000002ull);
        Check(!h::StoreGet(0x8000000000000001ull, back.data(), back.size(), &full, 5000), "a dropped key is gone");

        // A dirty rectangle of a 64-byte-wide level: 4 rows of 32 bytes at column 16, row 2.
        const uint64_t key = 0x8000000000000010ull;
        std::vector<uint8_t> rect(4 * 48, 0xAB);   // 48-byte source pitch, 32 bytes used per row
        Check(h::StorePatchRows(key, 2 * 64 + 16, 64, rect.data(), 32, 4, 48), "patch rows into a new value");
        std::vector<uint8_t> level(64 * 6, 0x55);
        uint64_t size = 0;
        h::StoreGet(key, level.data(), level.size(), &size, 5000);
        bool rowsOk = size == 2 * 64 + 16 + 3 * 64 + 32;
        for (uint64_t i = 0; i < size && rowsOk; ++i)
        {
            const uint64_t row = i / 64, col = i % 64;
            const bool inside = row >= 2 && row < 6 && col >= 16 && col < 48;
            rowsOk = level[i] == (inside ? 0xAB : 0x00);
        }
        Check(rowsOk, "rows land at their pitch, the rest of the new value is zero");
        h::StorePatchRows(key + 1, 0, 64, rect.data(), 32, 1, 48);
        h::StoreDropRange(key, 2);
        Check(!h::StoreGet(key, level.data(), level.size(), &size, 5000)
                  && !h::StoreGet(key + 1, level.data(), level.size(), &size, 5000),
              "a dropped key range is gone");
    }

    std::printf("jobs\n");
    {
        h::Block in, packed, out;
        const uint32_t n = 1u << 20;
        Check(h::Alloc(n, in) && h::Alloc(n + n / 255 + 64, packed) && h::Alloc(n, out), "window blocks");
        for (uint32_t i = 0; i < n; ++i) in.data[i] = uint8_t(i / 128);
        h::Params p;
        p.op = wxl::ipc::Op::Job;
        p.a0 = uint64_t(wxl::ipc::JobKind::Lz4Compress);
        p.a1 = in.offset; p.a2 = n; p.a3 = packed.offset; p.a4 = packed.size;
        h::Reply r;
        Check(h::Call(p, r, 5000) && r.status == Status::Ok && r.r0 > 0, "LZ4 compress job");
        const uint64_t packedSize = r.r0;
        p.a0 = uint64_t(wxl::ipc::JobKind::Lz4Decompress);
        p.a1 = packed.offset; p.a2 = packedSize; p.a3 = out.offset; p.a4 = out.size;
        Check(h::Call(p, r, 5000) && r.status == Status::Ok && r.r0 == n && std::memcmp(in.data, out.data, n) == 0,
              "LZ4 decompress job restores the input");
        h::Free(in); h::Free(packed); h::Free(out);
    }

    std::printf("threads\n");
    {
        Tally t;
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i)
            threads.emplace_back([&names, &t, i] {
                std::vector<std::string> mine(names.begin() + (names.size() * i) / 4, names.begin() + (names.size() * (i + 1)) / 4);
                Verify(mine, t, false);
            });
        for (std::thread& th : threads) th.join();
        Check(t.mismatch.load() == 0 && t.missing.load() == 0 && t.ok.load() > 0, "four threads read concurrently");
    }

    std::printf("latency\n");
    {
        std::vector<uint64_t> us;
        for (int i = 0; i < 2000; ++i)
        {
            h::Params p;
            p.op = wxl::ipc::Op::Ping;
            h::Reply r;
            const uint64_t a = wxl::ipc::NowUs();
            if (!h::Call(p, r, 1000)) break;
            us.push_back(wxl::ipc::NowUs() - a);
        }
        std::sort(us.begin(), us.end());
        if (!us.empty())
            std::printf("  ping: min %llu us, median %llu us, p99 %llu us over %zu\n",
                        static_cast<unsigned long long>(us.front()), static_cast<unsigned long long>(us[us.size() / 2]),
                        static_cast<unsigned long long>(us[us.size() * 99 / 100]), us.size());
        Sleep(50);   // let the dispatcher fall asleep, then time a cold wake
        h::Params p;
        p.op = wxl::ipc::Op::Ping;
        h::Reply r;
        const uint64_t a = wxl::ipc::NowUs();
        h::Call(p, r, 1000);
        std::printf("  ping after the host slept: %llu us\n", static_cast<unsigned long long>(wxl::ipc::NowUs() - a));
    }

    std::printf("restart\n");
    {
        const uint32_t pid = h::HostPid();
        HANDLE proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        Check(proc && TerminateProcess(proc, 9), "the host is killed");
        if (proc) { WaitForSingleObject(proc, 2000); CloseHandle(proc); }
        const uint64_t lost = wxl::ipc::NowUs();
        while (h::HostPid() == pid || !h::Available())
        {
            if (wxl::ipc::NowUs() - lost > 15000000) break;
            Sleep(20);
        }
        Check(h::Available() && h::HostPid() != pid, "a new host takes over");
        std::printf("  back in %llu ms, pid %u, generation %u\n",
                    static_cast<unsigned long long>((wxl::ipc::NowUs() - lost) / 1000), h::HostPid(), h::Generation());
        Tally t;
        std::vector<std::string> some(names.begin(), names.begin() + std::min<size_t>(names.size(), 60));
        Verify(some, t, true);
        Check(t.ok.load() > 0 && t.mismatch.load() == 0 && t.missing.load() == 0, "mounts were replayed: reads still work");
    }

    char line[1024];
    h::CounterLine(line, sizeof line);
    std::printf("%s\n", line);
    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
