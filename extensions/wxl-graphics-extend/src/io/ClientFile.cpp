// wxl-graphics-extend: whole client files, through the client's file system first, then loose.
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

#include "ClientFile.hpp"
#include "../core/Extension.hpp"

#include "game/Io.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    namespace game_io = wxl::game::io;

    /// Larger files are refused everywhere: a caller gets the whole file in memory, and nothing a
    /// render extension reads comes near this.
    constexpr uint64_t kMaxBytes = 256ull << 20;

    DWORD g_renderThread = 0;

    std::string Normalised(const char* path)
    {
        std::string s(path);
        for (char& c : s) if (c == '/') c = '\\';
        return s;
    }

    /// The client's file system (archives and mounted patch folders), the whole file at once. False
    /// on a plain miss; a read that failed after the open is logged. The handle is always closed.
    bool ReadClient(const std::string& path, std::string& out)
    {
        void* handle = nullptr;
        if (!game_io::FileOpen(path.c_str(), game_io::kOpenWholeFile, &handle) || !handle) return false;
        uint32_t high = 0;
        const uint32_t size = game_io::FileSize(handle, &high);
        bool ok = false;
        if (high || size > kMaxBytes)
            GFX_LOG_WARN("file %s: %s, refused", path.c_str(), high ? "over 4 GB" : "over 256 MB");
        else if (size)
        {
            out.resize(size);
            uint32_t got = 0;
            ok = game_io::FileRead(handle, out.data(), size, &got) != 0 && got == size;
            if (!ok) GFX_LOG_WARN("file %s: the client read %u of %u bytes", path.c_str(), got, size);
        }
        game_io::FileClose(handle);
        return ok;
    }

    /// A loose file through Win32. False on a plain miss (no log); an error after the open is logged.
    /// An empty file counts as missing, as it does through the client's file system.
    bool ReadLoose(const std::string& path, std::string& out)
    {
        const HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        bool ok = false;
        if (!GetFileSizeEx(h, &size))
            GFX_LOG_WARN("file %s: size unknown (error %lu)", path.c_str(), static_cast<unsigned long>(GetLastError()));
        else if (size.QuadPart > LONGLONG(kMaxBytes))
            GFX_LOG_WARN("file %s: over 256 MB, refused", path.c_str());
        else if (size.QuadPart > 0)
        {
            out.resize(size_t(size.QuadPart));
            size_t total = 0;
            ok = true;
            while (total < out.size())
            {
                DWORD got = 0;
                if (!ReadFile(h, out.data() + total, DWORD(out.size() - total), &got, nullptr) || got == 0)
                {
                    GFX_LOG_WARN("file %s: read %u of %u bytes (error %lu)", path.c_str(), unsigned(total), unsigned(out.size()),
                                 static_cast<unsigned long>(GetLastError()));
                    ok = false;
                    break;
                }
                total += got;
            }
        }
        CloseHandle(h);
        return ok;
    }

    bool EndsWithMpq(const char* name)
    {
        const size_t n = std::strlen(name);
        return n >= 4 && _stricmp(name + n - 4, ".MPQ") == 0;
    }

    /// The Data\*.MPQ directories (patch folders deployed as folders), highest name first, each with
    /// its trailing backslash. Enumerated once: the client mounts its folders at start, and so do we.
    /// A function-local static keeps the first call from any thread the only one that enumerates.
    const std::vector<std::string>& PatchDirs()
    {
        static const std::vector<std::string> dirs = [] {
            std::vector<std::string> v;
            WIN32_FIND_DATAA fd{};
            const HANDLE f = FindFirstFileA("Data\\*.MPQ", &fd);
            if (f == INVALID_HANDLE_VALUE) return v;
            do
            {
                // The pattern also matches short-name quirks such as "x.MPQ1": check the name itself.
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && EndsWithMpq(fd.cFileName))
                    v.push_back(std::string("Data\\") + fd.cFileName + "\\");
            } while (FindNextFileA(f, &fd));
            FindClose(f);
            std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) > 0; });
            return v;
        }();
        return dirs;
    }
}

namespace wxl::gfx::io
{
    void Install()
    {
        g_renderThread = GetCurrentThreadId();
    }

    bool Read(const char* path, std::string& out)
    {
        if (!path || !*path) return false;
        const std::string p = Normalised(path);
        out.clear();
        if (g_renderThread && GetCurrentThreadId() == g_renderThread && ReadClient(p, out)) return true;
        if (ReadLoose(p, out)) return true;
        if (ReadLoose("Data\\" + p, out)) return true;
        for (const std::string& dir : PatchDirs())
            if (ReadLoose(dir + p, out)) return true;
        out.clear();
        return false;
    }

    int ReadToSink(const char* path, WXL_ByteSink* out)
    {
        if (!out || !out->Write) return 0;
        std::string bytes;
        if (!Read(path, bytes)) return 0;
        out->Write(out->ctx, bytes.data(), unsigned(bytes.size()));   // at most 256 MB: fits
        return 1;
    }
}
