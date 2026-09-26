// The client's archive layer served by wxl-host: mounts, opens, lookups, and the native fallback.
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

#include "engine/storage/HostStorage.hpp"

#include "common/Config.hpp"
#include "common/Log.hpp"
#include "engine/diag/MemoryDiag.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/storage/HostTextures.hpp"
#include "engine/storage/StorageHook.hpp"
#include "offsets/engine/Io.hpp"
#include "runtime/host/HostClient.hpp"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#pragma intrinsic(_ReturnAddress)

namespace wxl::runtime::storage::hosted
{
    namespace
    {
        namespace io = wxl::offsets::engine::io;
        namespace h = wxl::host;
        using wxl::ipc::Status;

        constexpr uint32_t kArchiveMagic = 0x414C5857;   // 'WXLA' where a native archive holds its vtable
        constexpr uint32_t kHandleSlots = 32768;         // open host files at once
        constexpr uint32_t kSettleMs = 6000;             // how long an open waits out a host restart
        constexpr ULONGLONG kStatTtlMs = 10000;
        constexpr size_t kStatCacheCap = 16384;

        bool FilesEnabled()
        {
            static const bool on = wxl::config::Env("WXL_HOST_FILES", true);
            return on;
        }

        // --- mode --------------------------------------------------------------------------------

        enum class Mode : uint32_t { Undecided, Hosted, Native };
        std::atomic<Mode> g_mode{ Mode::Undecided };
        std::mutex        g_modeMutex;

        // --- archives handed to the client -------------------------------------------------------

        /// Stands where a native Mopaq archive would: its first dword tells it apart from a native one.
        struct ArchiveHandle
        {
            uint32_t       magic = kArchiveMagic;
            uint32_t       id = 0;            // host client id
            int            priority = 0;
            uint32_t       flags = 0;
            std::string    name;
            ArchiveHandle* parent = nullptr;  // for a view inside a parent archive
            void*          native = nullptr;  // the native archive after a late native mount
        };

        std::mutex                  g_archiveMutex;
        std::vector<ArchiveHandle*> g_archives;   // mount order

        bool IsHostArchive(const void* p)
        {
            return p && *static_cast<const uint32_t*>(p) == kArchiveMagic;
        }

        // --- file handles --------------------------------------------------------------------------

#pragma pack(push, 1)
        /// The client's 0x30-byte file handle, filled as a buffered (kind 5) whole file.
        struct HostFile
        {
            uint32_t kind;       // +0x00  io::kHandleKindBuffered
            uint32_t osFile;     // +0x04
            uint32_t archive;    // +0x08
            char*    shortName;  // +0x0c
            char*    fullName;   // +0x10
            uint32_t size;       // +0x14
            uint8_t* buffer;     // +0x18
            uint32_t position;   // +0x1c
            uint32_t mopaqFile;  // +0x20
            uint32_t keepOpen;   // +0x24  written by SFile::Load
            uint32_t slot;       // +0x28
            uint32_t lock;       // +0x2c
        };
#pragma pack(pop)
        static_assert(sizeof(HostFile) == 0x30, "HostFile must match the client's file handle");

        namespace tex = wxl::runtime::storage::textures;

        /// What a handle's bytes live in, and where to fetch them from when it was opened lazily.
        struct SlotInfo
        {
            h::FileData file;
            uint32_t    archive = wxl::ipc::kAnyArchive;
            uint8_t*    owned = nullptr;   // bytes read natively after the host was given up
            // A texture handle: reads return the header stand-in for image (HostTextures).
            bool        texture = false;
            uint32_t    fileSize = 0;
            tex::Image  image;
            uint8_t*    stub = nullptr;    // the stand-in rebased on itself, for readers of +0x18
        };

        HostFile*             g_slab = nullptr;   // the handles, so IsHostHandle is a range test
        std::vector<SlotInfo> g_side;
        std::vector<uint32_t> g_free;
        SRWLOCK               g_slotLock = SRWLOCK_INIT;
        SRWLOCK               g_fetchLocks[64] = {};   // a lazy handle's first read, striped by slot
        std::atomic<uint32_t> g_openFiles{ 0 };

        void SetClientError(uint32_t code)
        {
            *reinterpret_cast<uint32_t*>(io::kSErrLastError) = code;
            SetLastError(code);
        }

        bool IsAbsolute(const char* name)
        {
            return (name[0] && name[1] == ':') || (name[0] == '\\' && name[1] == '\\');
        }

        uint32_t TakeSlot()
        {
            uint32_t slot = UINT32_MAX;
            AcquireSRWLockExclusive(&g_slotLock);
            if (!g_free.empty())
            {
                slot = g_free.back();
                g_free.pop_back();
            }
            ReleaseSRWLockExclusive(&g_slotLock);
            if (slot == UINT32_MAX)
            {
                static std::atomic<bool> warned{ false };
                if (!warned.exchange(true))
                    WLOG_WARN("storage: %u host files open at once; further files are copied", kHandleSlots);
            }
            return slot;
        }

        HostFile* InitHandle(uint32_t slot, const char* name, uint64_t size)
        {
            HostFile* f = &g_slab[slot];
            std::memset(f, 0, sizeof *f);
            f->kind = io::kHandleKindBuffered;
            f->size = static_cast<uint32_t>(size);
            f->slot = slot;
            f->shortName = _strdup(name);
            g_openFiles.fetch_add(1, std::memory_order_relaxed);
            return f;
        }

        /// A handle over bytes the host already wrote (whole-file opens), or a copy when every slot is taken.
        void* MakeHandle(const char* name, h::FileData& file)
        {
            const uint32_t slot = TakeSlot();
            if (slot == UINT32_MAX)
            {
                void* copy = MakeBufferedHandle(name, file.data, static_cast<uint32_t>(file.size));
                h::ReleaseFile(file);
                return copy;
            }
            HostFile* f = InitHandle(slot, name, file.size);
            f->buffer = const_cast<uint8_t*>(file.data);
            g_side[slot].file = file;
            g_side[slot].archive = file.archive;
            return f;
        }

        /**
         * @brief A handle whose bytes are fetched at its first read.
         *
         * The client opens many files long before it reads them (its async queue), and a native handle
         * holds none of their bytes meanwhile; neither does this one.
         */
        void* MakeLazyHandle(const char* name, uint64_t size, uint32_t archive, bool texture)
        {
            const uint32_t slot = TakeSlot();
            if (slot == UINT32_MAX)
            {
                h::FileData file;
                if (h::ReadFile(name, archive, file) != Status::Ok) return nullptr;
                void* copy = MakeBufferedHandle(name, file.data, static_cast<uint32_t>(file.size));
                h::ReleaseFile(file);
                return copy;
            }
            SlotInfo& info = g_side[slot];
            info.file = h::FileData{};
            info.archive = archive;
            info.texture = texture;
            info.fileSize = texture ? static_cast<uint32_t>(size) : 0;
            // The texture loader sizes its buffer by this: the header, not the file.
            InitHandle(slot, name, texture ? tex::kStubBytes : size);
            return &g_slab[slot];
        }

        void EnterNativeMode(const char* why);

        /// Reads a whole file through the client's own open, once the archives are native again.
        bool ReadNativeBytes(const char* name, uint8_t*& out, uint32_t& outSize)
        {
            void* native = nullptr;
            const auto open = reinterpret_cast<io::Storage_FileOpenFn>(io::kFileOpen);
            const auto size = reinterpret_cast<io::Storage_FileSizeFn>(io::kFileSize);
            const auto read = reinterpret_cast<io::Storage_FileReadFn>(io::kFileRead);
            const auto close = reinterpret_cast<io::Storage_FileCloseFn>(io::kFileClose);
            if (!open(nullptr, name, 0, &native) || !native) return false;
            uint32_t high = 0;
            const uint32_t n = size(native, &high);
            auto* bytes = static_cast<uint8_t*>(malloc(n ? n : 1));
            uint32_t got = 0;
            const bool ok = bytes && !high && (!n || (read(native, bytes, n, &got, nullptr, 0) && got == n));
            close(native);
            if (!ok) { free(bytes); return false; }
            out = bytes;
            outSize = n;
            return true;
        }

        bool ReadNatively(HostFile* f, SlotInfo& info)
        {
            uint8_t* bytes = nullptr;
            uint32_t n = 0;
            if (!ReadNativeBytes(f->shortName, bytes, n)) return false;
            if (n < f->size) f->size = n;
            info.owned = bytes;
            f->buffer = bytes;
            return true;
        }

        /// A texture handle's image: from the host, else read natively; then its header stand-in.
        bool MaterializeTexture(HostFile* f, SlotInfo& info)
        {
            bool ok = g_mode.load() == Mode::Hosted && tex::Fetch(f->shortName, info.archive, info.fileSize, info.image);
            if (!ok && !h::Available())
            {
                EnterNativeMode("wxl-host could not serve a texture after its open");
                uint8_t* bytes = nullptr;
                uint32_t n = 0;
                if (ReadNativeBytes(f->shortName, bytes, n))
                {
                    tex::Adopt(bytes, n, info.fileSize, info.image);
                    ok = true;
                }
            }
            if (!ok) return false;
            info.stub = static_cast<uint8_t*>(malloc(tex::kStubBytes));
            if (!info.stub)
            {
                tex::Release(info.image);
                return false;
            }
            tex::WriteStub(info.image, reinterpret_cast<uintptr_t>(info.stub), 0, info.stub, tex::kStubBytes);
            f->buffer = info.stub;
            return true;
        }

        /// Fetches a lazy handle's bytes: from the host, through a restart, or natively once it is given up.
        bool Materialize(HostFile* f)
        {
            SRWLOCK& lock = g_fetchLocks[f->slot % 64];
            AcquireSRWLockExclusive(&lock);
            bool ok = f->buffer != nullptr;
            SlotInfo& info = g_side[f->slot];
            if (!ok && info.texture)
            {
                ok = MaterializeTexture(f, info);
                if (!ok) WLOG_WARN("storage: texture '%s' could not be read after its open", f->shortName);
                ReleaseSRWLockExclusive(&lock);
                return ok;
            }
            for (int attempt = 0; !ok && attempt < 2 && g_mode.load() == Mode::Hosted; ++attempt)
            {
                h::FileData file;
                const Status s = h::ReadFile(f->shortName, info.archive, file);
                if (s == Status::Ok)
                {
                    // A folder-archive file rewritten since the open keeps the size the client was told.
                    if (file.size < f->size) f->size = static_cast<uint32_t>(file.size);
                    info.file = file;
                    f->buffer = const_cast<uint8_t*>(file.data);
                    ok = true;
                }
                else if (s == Status::NotFound || !h::WaitSettled(kSettleMs))
                {
                    break;
                }
            }
            if (!ok && !h::Available())
            {
                EnterNativeMode("wxl-host could not serve a file after its open");
                ok = ReadNatively(f, info);
            }
            if (!ok) WLOG_WARN("storage: '%s' could not be read after its open", f->shortName);
            ReleaseSRWLockExclusive(&lock);
            return ok;
        }

        // --- lookups, cached briefly for the per-frame progress queries ---------------------------

        struct StatEntry
        {
            uint64_t  size;
            bool      found;
            ULONGLONG at;
        };
        std::mutex                                 g_statMutex;
        std::unordered_map<std::string, StatEntry> g_stats;

        std::string Key(const char* name)
        {
            std::string k(name);
            for (char& c : k) c = c == '/' ? '\\' : char(tolower(static_cast<unsigned char>(c)));
            return k;
        }

        bool StatCached(const char* name, uint64_t& size)
        {
            const std::string key = Key(name);
            const ULONGLONG now = GetTickCount64();
            {
                std::lock_guard<std::mutex> lock(g_statMutex);
                const auto it = g_stats.find(key);
                if (it != g_stats.end() && now - it->second.at < kStatTtlMs)
                {
                    size = it->second.size;
                    return it->second.found;
                }
            }
            uint64_t s = 0;
            const Status st = h::StatFile(name, wxl::ipc::kAnyArchive, 0, &s, nullptr);
            if (st != Status::Ok && st != Status::NotFound) return false;   // unknown: not cached
            std::lock_guard<std::mutex> lock(g_statMutex);
            if (g_stats.size() >= kStatCacheCap) g_stats.clear();
            g_stats[key] = StatEntry{ s, st == Status::Ok, now };
            size = s;
            return st == Status::Ok;
        }

        // --- detours ---------------------------------------------------------------------------------

        io::MopaqOpenArchiveFn     g_nextOpen = nullptr;
        io::MopaqNestedOpenFn      g_nextNested = nullptr;
        io::MopaqCloseArchiveFn    g_nextClose = nullptr;
        io::FindFileFn             g_nextFindFile = nullptr;
        io::FileGetIsLocalAmountFn g_nextIsLocal = nullptr;
        io::ArchiveFileExistsFn    g_nextExists = nullptr;
        io::Storage_FileOpenFn     g_nextFileOpen = nullptr;

        /**
         * @brief Mounts every recorded archive natively, in its recorded priority, and patches the client's
         *        archive slots. The host is gone for good: the session carries on native.
         */
        void EnterNativeMode(const char* why)
        {
            std::lock_guard<std::mutex> modeLock(g_modeMutex);
            if (g_mode.load() == Mode::Native) return;

            std::vector<ArchiveHandle*> archives;
            {
                std::lock_guard<std::mutex> lock(g_archiveMutex);
                archives = g_archives;
            }
            WLOG_WARN("storage: %s; mounting %zu archives natively for the rest of the session", why, archives.size());

            size_t mounted = 0;
            for (ArchiveHandle* a : archives)
            {
                if (a->native) continue;
                char ok = 0;
                if (a->parent)
                {
                    if (a->parent->native) ok = g_nextNested(a->parent->native, a->name.c_str(), a->priority, &a->native);
                }
                else
                {
                    ok = g_nextOpen(a->name.c_str(), a->priority, a->flags, &a->native);
                }
                if (ok) ++mounted;
                else WLOG_WARN("storage: native mount of '%s' failed", a->name.c_str());
            }

            // The boot mount's slots now name native archives; any other holder is patched when it next opens.
            auto** slots = *reinterpret_cast<void***>(io::kArchiveSlots);
            const uint32_t count = *reinterpret_cast<uint32_t*>(io::kArchiveSlotCount);
            for (uint32_t i = 0; slots && i < count; ++i)
            {
                auto* archive = static_cast<void**>(slots[i]);
                if (archive && IsHostArchive(archive[1]))
                    archive[1] = static_cast<ArchiveHandle*>(archive[1])->native;
            }

            g_mode.store(Mode::Native);
            wxl::diag::memory::SetArchivesHosted(false);
            h::CountFallback();
            WLOG_WARN("storage: %zu of %zu archives mounted natively", mounted, archives.size());
        }

        /// Decides once, at the first boot mount, whether the host serves the archives.
        bool Hosting()
        {
            const Mode m = g_mode.load();
            if (m != Mode::Undecided) return m == Mode::Hosted;
            std::lock_guard<std::mutex> lock(g_modeMutex);
            if (g_mode.load() != Mode::Undecided) return g_mode.load() == Mode::Hosted;

            if (!FilesEnabled())
            {
                WLOG_INFO("storage: WXL_HOST_FILES=0; the client mounts its own archives");
                g_mode.store(Mode::Native);
                return false;
            }
            if (!h::Start())
            {
                WLOG_WARN("storage: wxl-host is not available; the client mounts its own archives");
                g_mode.store(Mode::Native);
                return false;
            }

            g_side.resize(kHandleSlots);
            g_free.reserve(kHandleSlots);
            for (uint32_t i = kHandleSlots; i-- > 0;) g_free.push_back(i);
            g_mode.store(Mode::Hosted);
            wxl::diag::memory::SetArchivesHosted(true);
            WLOG_INFO("storage: wxl-host serves the client's archives (WXL_HOST_FILES=0 keeps them native)");
            return true;
        }

        /**
         * @brief Mounts on the host and returns a host archive handle, or the client's own failure.
         * @return the value the client's mount primitive would return.
         */
        char MountHosted(const char* name, int priority, uint32_t flags, ArchiveHandle* parent, void** out)
        {
            if (out) *out = nullptr;
            uint32_t id = 0, kind = 0, error = 0;
            const uint32_t parentId = parent ? parent->id : wxl::ipc::kAnyArchive;
            Status s = h::Mount(name, priority, parentId, id, kind, &error);
            bool transport = (s == Status::Failed && !error) || s == Status::Busy;
            if (transport && h::WaitSettled(kSettleMs))
            {
                s = h::Mount(name, priority, parentId, id, kind, &error);
                transport = (s == Status::Failed && !error) || s == Status::Busy;
            }
            if (transport)
            {
                EnterNativeMode("wxl-host did not answer a mount");
                if (parent) return parent->native ? g_nextNested(parent->native, name, priority, out) : 0;
                return g_nextOpen(name, priority, flags, out);
            }

            if (s != Status::Ok)
            {
                *reinterpret_cast<uint32_t*>(io::kMopaqLastError) = error ? error : ERROR_FILE_NOT_FOUND;
                wxl::diag::memory::RecordArchive(name, priority, true, false);
                return 0;
            }

            auto* a = new ArchiveHandle();
            a->id = id;
            a->priority = priority;
            a->flags = flags;
            a->name = name;
            a->parent = parent;
            {
                std::lock_guard<std::mutex> lock(g_archiveMutex);
                g_archives.push_back(a);
            }
            if (out) *out = a;
            wxl::diag::memory::RecordArchive(name, priority, true, true);
            return 1;
        }

        char __cdecl hkMopaqOpen(const char* name, int priority, uint32_t flags, void** out)
        {
            // Only the boot mount path is taken over; the survey and patch-download paths keep a native
            // archive, since they hand it to archive functions this layer does not serve.
            const auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            const bool bootPath = ret >= io::kSFileOpenArchive && ret < io::kSFileOpenArchive + io::kSFileOpenArchiveLen;
            if (!bootPath || !name || !Hosting()) return g_nextOpen(name, priority, flags, out);
            return MountHosted(name, priority, flags, nullptr, out);
        }

        char __cdecl hkMopaqNested(void* parent, const char* name, int priority, void** out)
        {
            if (!IsHostArchive(parent)) return g_nextNested(parent, name, priority, out);
            auto* p = static_cast<ArchiveHandle*>(parent);
            if (p->native) return g_nextNested(p->native, name, priority, out);
            return MountHosted(name, priority, p->flags, p, out);
        }

        char __cdecl hkMopaqClose(void* archive)
        {
            if (!IsHostArchive(archive)) return g_nextClose(archive);
            auto* a = static_cast<ArchiveHandle*>(archive);
            char result = 1;
            if (a->native) result = g_nextClose(a->native);
            else h::Unmount(a->id);
            {
                std::lock_guard<std::mutex> lock(g_archiveMutex);
                for (auto it = g_archives.begin(); it != g_archives.end(); ++it)
                    if (*it == a) { g_archives.erase(it); break; }
            }
            a->magic = 0;
            delete a;
            return result;
        }

        int __cdecl hkFindFile(const char* name, char* outName, uint32_t outCap, uint32_t flags, uint32_t* kind,
                               void** archive)
        {
            // The client's own lookup first: its loose-file map, and archives it still mounts natively.
            const int found = g_nextFindFile(name, outName, outCap, flags, kind, archive);
            if (found || g_mode.load() != Mode::Hosted || !name || !*name || IsAbsolute(name)) return found;
            uint64_t size = 0;
            if (!StatCached(name, size)) return 0;
            if (outName && outCap) strncpy_s(outName, outCap, name, _TRUNCATE);
            if (kind) *kind = 3;           // an archive member
            if (archive) *archive = nullptr;
            return 1;
        }

        void __stdcall hkIsLocalAmount(const char* name, uint32_t* local64, uint32_t* total64)
        {
            if (g_mode.load() != Mode::Hosted) { g_nextIsLocal(name, local64, total64); return; }
            uint64_t size = 0;
            if (!name || !StatCached(name, size)) return;
            // Everything the host serves is local; the counts are 64-bit pairs of dwords.
            auto add = [size](uint32_t* v) {
                if (!v) return;
                const uint64_t sum = (uint64_t(v[1]) << 32 | v[0]) + size;
                v[0] = uint32_t(sum);
                v[1] = uint32_t(sum >> 32);
            };
            add(local64);
            add(total64);
        }

        int __stdcall hkArchiveExists(const char* name)
        {
            if (g_mode.load() != Mode::Hosted) return g_nextExists(name);
            uint64_t size = 0;
            if (!name || !StatCached(name, size)) return 0;
            SetClientError(ERROR_FILE_NOT_FOUND);   // the client's own function sets this on a hit
            return 1;
        }

        /// Serves an open that reached the end of the chain. -1 hands it to the client's own open.
        int OpenHosted(void* archive, const char* name, uint32_t flags, void** out)
        {
            if (!name || !*name || !out) return -1;

            uint32_t id = wxl::ipc::kAnyArchive;
            if (archive)
            {
                void** sarchive = static_cast<void**>(archive);   // {kind, mopaq handle}
                if (!IsHostArchive(sarchive[1])) return -1;
                auto* a = static_cast<ArchiveHandle*>(sarchive[1]);
                if (a->native)
                {
                    sarchive[1] = a->native;   // a holder the late native mount did not reach
                    return -1;
                }
                id = a->id;
            }
            else
            {
                if (IsAbsolute(name)) return -1;
                const uint32_t lookup = flags | *reinterpret_cast<uint32_t*>(io::kDirectAccessFlags);
                if (lookup & 3)
                {
                    // A loose file on disk overrides for these opens: let the client open it itself.
                    char path[MAX_PATH];
                    uint32_t kind = 0;
                    void* where = nullptr;
                    if (g_nextFindFile(name, path, sizeof path, flags, &kind, &where)) return -1;
                }
            }

            // A whole-file open hands the bytes over with the handle (callers may read +0x18 directly);
            // any other open is lazy, as a native archive handle is.
            const bool whole = (flags & io::kOpenWholeFile) != 0;
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                Status s;
                if (whole)
                {
                    h::FileData file;
                    s = h::ReadFile(name, id, file);
                    if (s == Status::Ok)
                    {
                        *out = MakeHandle(name, file);
                        return *out ? 1 : 0;
                    }
                }
                else
                {
                    uint64_t size = 0;
                    uint32_t where = wxl::ipc::kAnyArchive;
                    s = h::StatFile(name, id, 0, &size, &where);
                    if (s == Status::Ok && size <= 0xFFFFFFFFull)
                    {
                        *out = MakeLazyHandle(name, size, where, !archive && tex::Claim(name, size));
                        return *out ? 1 : 0;
                    }
                }
                if (s == Status::NotFound)
                {
                    *out = nullptr;
                    SetClientError(ERROR_FILE_NOT_FOUND);
                    return 0;
                }
                if (s == Status::BadRequest) return -1;
                if (attempt == 0 && h::WaitSettled(kSettleMs)) continue;
            }
            EnterNativeMode("wxl-host could not serve a file");
            if (archive)
            {
                void** sarchive = static_cast<void**>(archive);
                if (IsHostArchive(sarchive[1])) sarchive[1] = static_cast<ArchiveHandle*>(sarchive[1])->native;
            }
            return -1;
        }

        int __stdcall hkHostOpen(void* archive, const char* name, uint32_t flags, void** out)
        {
            if (g_mode.load() == Mode::Hosted)
            {
                const int r = OpenHosted(archive, name, flags, out);
                if (r >= 0) return r;
            }
            return g_nextFileOpen(archive, name, flags, out);
        }
    }

    void Install()
    {
        for (SRWLOCK& l : g_fetchLocks) InitializeSRWLock(&l);
        g_slab = static_cast<HostFile*>(VirtualAlloc(nullptr, sizeof(HostFile) * kHandleSlots,
                                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!g_slab)
        {
            WLOG_WARN("storage: no room for the host file handles; the client keeps its own archives");
            g_mode.store(Mode::Native);
            return;
        }

        using wxl::hook::Install;
        // The mount primitive's head: the caller's return address decides, and a party in front would hide it.
        Install("HostStorage.MopaqOpenArchive", io::kMopaqOpenArchive, &hkMopaqOpen, &g_nextOpen, -200);
        Install("HostStorage.MopaqNestedOpen", io::kMopaqNestedOpen, &hkMopaqNested, &g_nextNested);
        Install("HostStorage.MopaqCloseArchive", io::kMopaqCloseArchive, &hkMopaqClose, &g_nextClose);
        Install("HostStorage.FindFile", io::kFindFile, &hkFindFile, &g_nextFindFile);
        Install("HostStorage.FileGetIsLocalAmount", io::kFileGetIsLocalAmount, &hkIsLocalAmount, &g_nextIsLocal);
        Install("HostStorage.ArchiveFileExists", io::kArchiveFileExists, &hkArchiveExists, &g_nextExists);
        // Last in the open chain: redirects and providers upstream see the request first.
        Install("HostStorage.FileOpen", io::kFileOpen, &hkHostOpen, &g_nextFileOpen, 1000);
    }

    bool Serving()
    {
        return g_mode.load() == Mode::Hosted;
    }

    bool IsHostHandle(void* handle)
    {
        const auto p = reinterpret_cast<uintptr_t>(handle);
        const auto base = reinterpret_cast<uintptr_t>(g_slab);
        return g_slab && p >= base && p < base + sizeof(HostFile) * kHandleSlots;
    }

    uint32_t Size(void* handle, uint32_t* sizeHigh)
    {
        if (sizeHigh) *sizeHigh = 0;
        return static_cast<HostFile*>(handle)->size;
    }

    int Read(void* handle, void* dst, uint32_t len, uint32_t* read, void* overlapped)
    {
        auto* f = static_cast<HostFile*>(handle);
        if (overlapped) return 0;   // a buffered handle refuses an overlapped read, as the client's does
        if (!len)
        {
            if (read) *read = 0;
            return 1;
        }
        if (!f->buffer && f->position < f->size && !Materialize(f))
        {
            // A texture handle's reader then finds no BLP header rather than whatever its buffer held.
            if (g_side[f->slot].texture) std::memset(dst, 0, len);
            if (read) *read = 0;
            SetClientError(0x26);
            return 0;
        }
        const uint32_t left = f->position < f->size ? f->size - f->position : 0;
        const uint32_t n = len < left ? len : left;
        const SlotInfo& info = g_side[f->slot];
        if (n && info.texture)
            tex::WriteStub(info.image, reinterpret_cast<uintptr_t>(dst) - f->position, f->position, static_cast<uint8_t*>(dst), n);
        else if (n)
            std::memcpy(dst, f->buffer + f->position, n);
        f->position += n;
        if (read) *read = n;
        if (n != len) SetClientError(0x26);   // ERROR_HANDLE_EOF, as the client reports a short read
        return n == len ? 1 : 0;
    }

    uint32_t Seek(void* handle, int32_t distLow, uint32_t* distHigh, uint32_t method)
    {
        auto* f = static_cast<HostFile*>(handle);
        const int64_t base = method == 1 ? int64_t(f->position) : method == 2 ? int64_t(f->size) : 0;
        int64_t pos = base + distLow;
        if (pos < 0) pos = 0;
        if (pos > int64_t(f->size)) pos = f->size;
        f->position = static_cast<uint32_t>(pos);
        if (distHigh) *distHigh = 0;
        return f->position;
    }

    int Close(void* handle)
    {
        auto* f = static_cast<HostFile*>(handle);
        const uint32_t slot = f->slot;
        free(f->shortName);
        std::memset(f, 0, sizeof *f);
        SlotInfo& info = g_side[slot];
        h::ReleaseFile(info.file);
        free(info.owned);
        info.owned = nullptr;
        info.archive = wxl::ipc::kAnyArchive;
        if (info.texture)
        {
            tex::Release(info.image);
            free(info.stub);
            info.stub = nullptr;
            info.texture = false;
            info.fileSize = 0;
        }
        AcquireSRWLockExclusive(&g_slotLock);
        g_free.push_back(slot);
        ReleaseSRWLockExclusive(&g_slotLock);
        g_openFiles.fetch_sub(1, std::memory_order_relaxed);
        return 1;
    }
}
