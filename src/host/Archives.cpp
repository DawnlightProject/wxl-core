// wxl-host: the archive set the client mounted, resolved in the client's own priority order.
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

#include "host/Archives.hpp"

#include "common/Log.hpp"

#include <windows.h>
#include <StormLib.h>

#include <algorithm>
#include <atomic>

namespace wxl::hostd::archives
{
    namespace
    {
        using ipc::Status;

        struct Entry
        {
            std::string  name;          // as the client passed it
            std::wstring path;          // absolute, for files and folders
            std::string  prefix;        // kKindPrefix: path prefix inside the parent MPQ
            uint32_t     parent = 0;    // kKindPrefix: the MPQ entry holding the files
            uint32_t     kind = 0;
            int          priority = 0;
            uint64_t     seq = 0;
            bool         alive = true;
        };

        std::wstring                         g_root;
        SRWLOCK                              g_lock = SRWLOCK_INIT;
        std::vector<Entry>                   g_entries;   // id = index + 1
        std::vector<uint32_t>                g_order;     // ids, highest priority first
        std::vector<std::vector<HANDLE>>     g_handles;   // [thread][id - 1], each thread touches only its row
        uint64_t                             g_seq = 0;

        std::wstring Widen(const std::string& s)
        {
            if (s.empty()) return {};
            const int n = MultiByteToWideChar(CP_ACP, 0, s.data(), int(s.size()), nullptr, 0);
            std::wstring w(size_t(n), L'\0');
            MultiByteToWideChar(CP_ACP, 0, s.data(), int(s.size()), w.data(), n);
            return w;
        }

        std::string Normalize(const char* name)
        {
            std::string s(name ? name : "");
            for (char& c : s)
                if (c == '/') c = '\\';
            return s;
        }

        std::wstring Absolute(const std::string& name)
        {
            std::wstring w = Widen(Normalize(name.c_str()));
            const bool rooted = (w.size() >= 2 && w[1] == L':') || (w.size() >= 2 && w[0] == L'\\' && w[1] == L'\\');
            if (rooted) return w;
            return g_root + L"\\" + w;
        }

        /// 1 for a file, 2 for a folder, 0 when absent.
        int Exists(const std::wstring& path)
        {
            const DWORD a = GetFileAttributesW(path.c_str());
            if (a == INVALID_FILE_ATTRIBUTES) return 0;
            return (a & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
        }

        void Reorder()
        {
            g_order.clear();
            for (uint32_t i = 0; i < g_entries.size(); ++i)
                if (g_entries[i].alive) g_order.push_back(i + 1);
            std::stable_sort(g_order.begin(), g_order.end(), [](uint32_t a, uint32_t b) {
                const Entry& ea = g_entries[a - 1];
                const Entry& eb = g_entries[b - 1];
                if (ea.priority != eb.priority) return ea.priority > eb.priority;   // higher priority first
                return ea.seq > eb.seq;                                            // later mount first on a tie
            });
        }

        HANDLE OpenMpq(const std::wstring& path)
        {
            HANDLE h = nullptr;
            // Lookups go by hash: skip parsing the listfile and attributes, which only cost memory here.
            const DWORD flags = MPQ_OPEN_READ_ONLY | MPQ_OPEN_NO_LISTFILE | MPQ_OPEN_NO_ATTRIBUTES;
            if (!SFileOpenArchive(path.c_str(), 0, flags, &h)) return nullptr;
            return h;
        }

        /// The calling thread's StormLib handle for an MPQ entry, opened on first use.
        HANDLE Handle(uint32_t thread, uint32_t id, const std::wstring& path)
        {
            if (thread >= g_handles.size()) return nullptr;
            std::vector<HANDLE>& row = g_handles[thread];
            if (row.size() < id) row.resize(id, nullptr);
            HANDLE& h = row[id - 1];
            if (!h) h = OpenMpq(path);
            return h;
        }

        /// Closes the calling thread's handles on entries that were unmounted.
        void CloseDead(uint32_t thread)
        {
            if (thread >= g_handles.size()) return;
            std::vector<HANDLE>& row = g_handles[thread];
            for (size_t i = 0; i < row.size() && i < g_entries.size(); ++i)
                if (row[i] && !g_entries[i].alive)
                {
                    SFileCloseArchive(row[i]);
                    row[i] = nullptr;
                }
        }

        /// Where a lookup found a file: the entry and the name to use inside it.
        struct Hit
        {
            uint32_t     id = 0;         // the entry the client sees
            uint32_t     storage = 0;    // the entry that holds the bytes (the parent for a prefix view)
            uint32_t     kind = 0;
            std::wstring path;           // folder file path, or the MPQ path
            std::string  inner;          // name inside the MPQ
        };

        /// Tests one entry. Called with g_lock held shared.
        bool Probe(uint32_t thread, uint32_t id, const std::string& name, Hit& hit)
        {
            const Entry& e = g_entries[id - 1];
            if (!e.alive) return false;
            switch (e.kind)
            {
            case kKindFile:
            {
                HANDLE h = Handle(thread, id, e.path);
                if (!h || !SFileHasFile(h, name.c_str())) return false;
                hit = Hit{ id, id, kKindFile, e.path, name };
                return true;
            }
            case kKindFolder:
            {
                std::wstring file = e.path + L"\\" + Widen(name);
                if (Exists(file) != 1) return false;
                hit = Hit{ id, id, kKindFolder, std::move(file), name };
                return true;
            }
            case kKindPrefix:
            {
                const Entry& p = g_entries[e.parent - 1];
                if (!p.alive) return false;
                const std::string inner = e.prefix + name;
                HANDLE h = Handle(thread, e.parent, p.path);
                if (!h || !SFileHasFile(h, inner.c_str())) return false;
                hit = Hit{ id, e.parent, kKindFile, p.path, inner };
                return true;
            }
            default:
                return false;
            }
        }

        bool Find(uint32_t thread, const char* rawName, uint32_t archive, Hit& hit)
        {
            const std::string name = Normalize(rawName);
            if (name.empty()) return false;
            AcquireSRWLockShared(&g_lock);
            CloseDead(thread);
            bool found = false;
            if (archive != ipc::kAnyArchive)
            {
                found = archive >= 1 && archive <= g_entries.size() && Probe(thread, archive, name, hit);
            }
            else
            {
                for (uint32_t id : g_order)
                    if (Probe(thread, id, name, hit)) { found = true; break; }
            }
            ReleaseSRWLockShared(&g_lock);
            return found;
        }

        /// Reads a folder-archive file. size receives its size; NeedMore when cap is too small.
        Status ReadLoose(const std::wstring& path, uint8_t* dst, uint64_t cap, uint64_t& size)
        {
            HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (f == INVALID_HANDLE_VALUE) return Status::NotFound;
            LARGE_INTEGER li{};
            if (!GetFileSizeEx(f, &li)) { CloseHandle(f); return Status::Failed; }
            size = uint64_t(li.QuadPart);
            if (size > cap || !dst) { CloseHandle(f); return size > cap ? Status::NeedMore : Status::Ok; }
            uint64_t done = 0;
            while (done < size)
            {
                DWORD got = 0;
                const DWORD want = DWORD(std::min<uint64_t>(size - done, 1u << 30));
                if (!::ReadFile(f, dst + done, want, &got, nullptr) || !got) break;
                done += got;
            }
            CloseHandle(f);
            return done == size ? Status::Ok : Status::Failed;
        }

        Status ReadMpq(HANDLE mpq, const std::string& inner, uint8_t* dst, uint64_t cap, uint64_t& size)
        {
            HANDLE f = nullptr;
            if (!SFileOpenFileEx(mpq, inner.c_str(), SFILE_OPEN_FROM_MPQ, &f)) return Status::NotFound;
            DWORD high = 0;
            const DWORD low = SFileGetFileSize(f, &high);
            if (low == SFILE_INVALID_SIZE) { SFileCloseFile(f); return Status::Failed; }
            size = (uint64_t(high) << 32) | low;
            if (size > cap || !dst) { SFileCloseFile(f); return size > cap ? Status::NeedMore : Status::Ok; }
            uint64_t done = 0;
            bool ok = true;
            while (done < size)
            {
                DWORD got = 0;
                const DWORD want = DWORD(std::min<uint64_t>(size - done, 1u << 30));
                if (!SFileReadFile(f, dst + done, want, &got, nullptr) && GetLastError() != ERROR_HANDLE_EOF) ok = false;
                if (!got) break;
                done += got;
            }
            SFileCloseFile(f);
            return ok && done == size ? Status::Ok : Status::Failed;
        }
    }

    void Init(const std::wstring& root, uint32_t threads)
    {
        g_root = root;
        while (!g_root.empty() && (g_root.back() == L'\\' || g_root.back() == L'/')) g_root.pop_back();
        g_handles.resize(threads);
        SFileSetLocale(LANG_NEUTRAL);
    }

    Status Mount(uint32_t thread, const char* name, int priority, uint32_t parent, uint32_t& id, uint32_t& kind,
                 uint32_t& error)
    {
        id = 0;
        kind = 0;
        error = 0;
        if (!name || !*name) { error = ERROR_FILE_NOT_FOUND; return Status::BadRequest; }

        Entry e;
        e.name = name;
        e.priority = priority;

        if (parent != ipc::kAnyArchive)
        {
            AcquireSRWLockShared(&g_lock);
            const bool valid = parent >= 1 && parent <= g_entries.size() && g_entries[parent - 1].alive;
            Entry p = valid ? g_entries[parent - 1] : Entry{};
            ReleaseSRWLockShared(&g_lock);
            if (!valid) { error = ERROR_FILE_NOT_FOUND; return Status::NotFound; }

            if (p.kind == kKindFolder)
            {
                // A folder parent joins the path and opens what is there, as the client does.
                e.path = p.path + L"\\" + Widen(Normalize(name));
            }
            else
            {
                // An MPQ parent: the nested archive is the parent's files under "<name>\".
                e.kind = kKindPrefix;
                e.parent = p.kind == kKindPrefix ? p.parent : parent;
                e.prefix = (p.kind == kKindPrefix ? p.prefix : std::string()) + Normalize(name) + "\\";
            }
        }
        else
        {
            e.path = Absolute(name);
        }

        if (e.kind != kKindPrefix)
        {
            const int exists = Exists(e.path);
            if (exists == 0) { error = ERROR_FILE_NOT_FOUND; return Status::NotFound; }
            e.kind = exists == 2 ? kKindFolder : kKindFile;
        }

        HANDLE validated = nullptr;
        if (e.kind == kKindFile)
        {
            validated = OpenMpq(e.path);
            if (!validated)
            {
                WLOG_WARN("archives: '%s' is not a readable MPQ (win32 %lu)", name, GetLastError());
                error = kErrorNotArchive;
                return Status::Failed;
            }
        }

        AcquireSRWLockExclusive(&g_lock);
        e.seq = ++g_seq;
        g_entries.push_back(std::move(e));
        id = uint32_t(g_entries.size());
        kind = g_entries.back().kind;
        Reorder();
        if (validated && thread < g_handles.size())
        {
            std::vector<HANDLE>& row = g_handles[thread];
            if (row.size() < id) row.resize(id, nullptr);
            row[id - 1] = validated;
            validated = nullptr;
        }
        const Entry& added = g_entries[id - 1];
        WLOG_INFO("archives: #%u '%s' priority %d (%s)", id, added.name.c_str(), added.priority,
                  added.kind == kKindFile ? "mpq" : added.kind == kKindFolder ? "folder" : "view in parent");
        ReleaseSRWLockExclusive(&g_lock);
        if (validated) SFileCloseArchive(validated);
        return Status::Ok;
    }

    void Unmount(uint32_t id)
    {
        AcquireSRWLockExclusive(&g_lock);
        if (id >= 1 && id <= g_entries.size()) g_entries[id - 1].alive = false;
        Reorder();
        ReleaseSRWLockExclusive(&g_lock);
    }

    Status Stat(uint32_t thread, const char* name, uint32_t archive, uint64_t& size, uint32_t& foundIn)
    {
        size = 0;
        foundIn = ipc::kAnyArchive;
        Hit hit;
        if (!Find(thread, name, archive, hit)) return Status::NotFound;
        foundIn = hit.id;
        if (hit.kind == kKindFolder) return ReadLoose(hit.path, nullptr, UINT64_MAX, size);
        AcquireSRWLockShared(&g_lock);
        HANDLE h = Handle(thread, hit.storage, hit.path);
        ReleaseSRWLockShared(&g_lock);
        return h ? ReadMpq(h, hit.inner, nullptr, UINT64_MAX, size) : Status::Failed;
    }

    Status Read(uint32_t thread, const char* name, uint32_t archive, uint8_t* dst, uint64_t cap, uint64_t& size,
                uint32_t& foundIn)
    {
        size = 0;
        foundIn = ipc::kAnyArchive;
        Hit hit;
        if (!Find(thread, name, archive, hit)) return Status::NotFound;
        foundIn = hit.id;
        if (hit.kind == kKindFolder) return ReadLoose(hit.path, dst, cap, size);
        AcquireSRWLockShared(&g_lock);
        HANDLE h = Handle(thread, hit.storage, hit.path);
        ReleaseSRWLockShared(&g_lock);
        return h ? ReadMpq(h, hit.inner, dst, cap, size) : Status::Failed;
    }

    Status ReadAll(uint32_t thread, const char* name, std::vector<uint8_t>& out)
    {
        Located at;
        const Status s = Locate(thread, name, ipc::kAnyArchive, at);
        if (s != Status::Ok) return s;
        return ReadLocated(thread, at, out);
    }

    Status Locate(uint32_t thread, const char* name, uint32_t archive, Located& out)
    {
        out = Located{};
        Hit hit;
        if (!Find(thread, name, archive, hit)) return Status::NotFound;
        out.id = hit.id;
        out.storage = hit.storage;
        out.kind = hit.kind;
        out.path = std::move(hit.path);
        out.inner = std::move(hit.inner);
        if (out.kind == kKindFolder)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(out.path.c_str(), GetFileExInfoStandard, &data)) return Status::NotFound;
            const uint64_t written = (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
            const uint64_t size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            out.stamp = (written ^ (size * 0x9E3779B97F4A7C15ull)) | 1;
        }
        return Status::Ok;
    }

    Status ReadLocated(uint32_t thread, const Located& at, uint8_t* dst, uint64_t cap, uint64_t& size)
    {
        size = 0;
        if (at.kind == kKindFolder) return ReadLoose(at.path, dst, cap, size);
        AcquireSRWLockShared(&g_lock);
        const bool alive = at.storage >= 1 && at.storage <= g_entries.size() && g_entries[at.storage - 1].alive;
        HANDLE h = alive ? Handle(thread, at.storage, at.path) : nullptr;
        ReleaseSRWLockShared(&g_lock);
        return h ? ReadMpq(h, at.inner, dst, cap, size) : Status::Failed;
    }

    Status ReadLocated(uint32_t thread, const Located& at, std::vector<uint8_t>& out)
    {
        out.clear();
        if (at.kind == kKindFolder)
        {
            uint64_t size = 0;
            Status s = ReadLoose(at.path, nullptr, 0, size);
            if (s != Status::NeedMore && s != Status::Ok) return s;
            out.resize(size_t(size));
            s = ReadLoose(at.path, out.data(), size, size);
            if (s == Status::Ok && size != out.size()) out.resize(size_t(size));
            return s;
        }
        AcquireSRWLockShared(&g_lock);
        const bool alive = at.storage >= 1 && at.storage <= g_entries.size() && g_entries[at.storage - 1].alive;
        HANDLE mpq = alive ? Handle(thread, at.storage, at.path) : nullptr;
        ReleaseSRWLockShared(&g_lock);
        if (!mpq) return Status::Failed;

        // One open for the size and the bytes.
        HANDLE f = nullptr;
        if (!SFileOpenFileEx(mpq, at.inner.c_str(), SFILE_OPEN_FROM_MPQ, &f)) return Status::NotFound;
        DWORD high = 0;
        const DWORD low = SFileGetFileSize(f, &high);
        if (low == SFILE_INVALID_SIZE || high) { SFileCloseFile(f); return Status::Failed; }
        out.resize(low);
        uint64_t done = 0;
        bool ok = true;
        while (done < out.size())
        {
            DWORD got = 0;
            if (!SFileReadFile(f, out.data() + done, DWORD(out.size() - done), &got, nullptr) && GetLastError() != ERROR_HANDLE_EOF)
                ok = false;
            if (!got) break;
            done += got;
        }
        SFileCloseFile(f);
        return ok && done == out.size() ? Status::Ok : Status::Failed;
    }

    bool Relocate(uint32_t storage, const std::string& inner, Located& out)
    {
        out = Located{};
        AcquireSRWLockShared(&g_lock);
        const bool ok = storage >= 1 && storage <= g_entries.size() && g_entries[storage - 1].alive
            && g_entries[storage - 1].kind == kKindFile;
        if (ok)
        {
            out.id = storage;
            out.storage = storage;
            out.kind = kKindFile;
            out.path = g_entries[storage - 1].path;
            out.inner = inner;
        }
        ReleaseSRWLockShared(&g_lock);
        return ok;
    }

    uint32_t Count()
    {
        AcquireSRWLockShared(&g_lock);
        const uint32_t n = uint32_t(g_order.size());
        ReleaseSRWLockShared(&g_lock);
        return n;
    }
}
