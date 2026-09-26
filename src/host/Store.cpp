// wxl-host: a keyed store of compressed bytes, kept out of Wow.exe's address space.
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

#include "host/Store.hpp"

#include "host/Assets.hpp"
#include "host/Blp.hpp"

#include "common/Log.hpp"

#include "common/xxhash.h"
#include "lz4.h"
#include "tlsf.h"
#include "zstd.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace wxl::hostd::store
{
    namespace
    {
        using ipc::Codec;
        using ipc::Status;

        constexpr uint64_t kSectionBytes = 256ull << 20;
        constexpr int kZstdLevel = 3;
        constexpr uint64_t kMinReference = 256;       // smaller values are cheaper stored than indexed
        constexpr size_t kIndexCap = size_t(1) << 16;  // indexed levels (thousands of textures), oldest forgotten first

        /// A value that is a range of a served image, rebuilt from its archive when read.
        struct Reference
        {
            cache::Key key;
            uint64_t   offset = 0;
        };

        struct Entry
        {
            uint8_t*  ptr = nullptr;
            uint64_t  stored = 0;
            uint64_t  raw = 0;
            Codec     codec = Codec::Raw;
            ULONGLONG touched = 0;
            uint64_t  version = 0;
            std::shared_ptr<const Reference> ref;   // set: no bytes are stored
        };

        std::mutex                          g_mutex;
        std::unordered_map<uint64_t, Entry> g_entries;
        std::vector<uint8_t>                g_control;
        tlsf_t                              g_tlsf = nullptr;
        uint32_t                            g_pid = 0;
        uint64_t                            g_limit = 0;
        uint64_t                            g_capacity = 0;
        uint32_t                            g_sections = 0;
        ipc::HostCounters*                  g_counters = nullptr;
        uint32_t                            g_archiveSlot = 0;
        bool                                g_dedupe = false;

        // Levels of served images by content hash.
        struct Indexed
        {
            cache::Key key;
            uint64_t   offset = 0;
            uint64_t   size = 0;
        };
        std::mutex                            g_indexMutex;
        std::unordered_map<uint64_t, Indexed> g_index;
        std::deque<uint64_t>                  g_indexOrder;

        thread_local std::vector<uint8_t> t_packed;
        thread_local std::vector<uint8_t> t_plain;

        uint64_t Hash(const uint8_t* p, uint64_t n)
        {
            return XXH64(p, size_t(n), n);
        }

        /// Adds one section to the pool. Called with g_mutex held.
        bool Grow()
        {
            if (g_capacity + kSectionBytes > g_limit) return false;
            wchar_t name[96];
            _snwprintf_s(name, _TRUNCATE, L"Local\\wxl-host-%u-store-%u", g_pid, g_sections);
            HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                                DWORD(kSectionBytes >> 32), DWORD(kSectionBytes), name);
            if (!section) return false;
            void* view = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, SIZE_T(kSectionBytes));
            if (!view) { CloseHandle(section); return false; }
            if (!tlsf_add_pool(g_tlsf, view, size_t(kSectionBytes)))
            {
                UnmapViewOfFile(view);
                CloseHandle(section);
                return false;
            }
            // The section handle stays open for the process lifetime: the view keeps the pages alive.
            ++g_sections;
            g_capacity += kSectionBytes;
            if (g_counters) g_counters->storeCapacity.store(g_capacity);
            return true;
        }

        /// Allocates stored bytes, growing the pool when needed. Called with g_mutex held.
        uint8_t* Allocate(uint64_t bytes)
        {
            if (bytes + 4096 > kSectionBytes) return nullptr;
            void* p = tlsf_malloc(g_tlsf, size_t(std::max<uint64_t>(bytes, 16)));
            while (!p && Grow()) p = tlsf_malloc(g_tlsf, size_t(std::max<uint64_t>(bytes, 16)));
            return static_cast<uint8_t*>(p);
        }

        void Account(int64_t storedDelta, int64_t rawDelta, int64_t entryDelta)
        {
            if (!g_counters) return;
            g_counters->storeBytes.fetch_add(uint64_t(storedDelta));
            g_counters->storeRawBytes.fetch_add(uint64_t(rawDelta));
            g_counters->storeEntries.fetch_add(uint64_t(entryDelta));
        }

        /// Compresses into t_packed; returns the codec actually used (Raw when it did not pay).
        Codec Pack(const uint8_t* src, uint64_t size, Codec codec, const uint8_t*& out, uint64_t& outSize)
        {
            out = src;
            outSize = size;
            if (codec == Codec::Raw || size < 64 || size > 0x7FFFFFFF) return Codec::Raw;
            if (codec == Codec::Lz4)
            {
                t_packed.resize(size_t(LZ4_compressBound(int(size))));
                const int n = LZ4_compress_default(reinterpret_cast<const char*>(src),
                                                   reinterpret_cast<char*>(t_packed.data()), int(size),
                                                   int(t_packed.size()));
                if (n <= 0 || uint64_t(n) >= size) return Codec::Raw;
                out = t_packed.data();
                outSize = uint64_t(n);
                return Codec::Lz4;
            }
            t_packed.resize(ZSTD_compressBound(size_t(size)));
            const size_t n = ZSTD_compress(t_packed.data(), t_packed.size(), src, size_t(size), kZstdLevel);
            if (ZSTD_isError(n) || n >= size) return Codec::Raw;
            out = t_packed.data();
            outSize = n;
            return Codec::Zstd;
        }

        /// Rebuilds a reference's bytes from its image: the cache, else the archive (store thread only).
        bool Regenerate(const Entry& e, uint8_t* dst)
        {
            const Reference& r = *e.ref;
            cache::Blob image = cache::Find(r.key, false);
            if (!image)
            {
                image = assets::Rebuild(g_archiveSlot, r.key);
                if (g_counters) g_counters->dedupeRegenerated.fetch_add(1, std::memory_order_relaxed);
            }
            if (!image || image->size() < r.offset + e.raw)
            {
                if (g_counters) g_counters->dedupeFailed.fetch_add(1, std::memory_order_relaxed);
                WLOG_WARN("store: a backing copy could not be rebuilt from '%s'", r.key.inner.c_str());
                return false;
            }
            std::memcpy(dst, image->data() + r.offset, size_t(e.raw));
            return true;
        }

        /// Restores an entry's bytes into dst (raw size bytes). The caller holds a copy of the entry.
        bool Unpack(const Entry& e, uint8_t* dst)
        {
            if (e.ref) return Regenerate(e, dst);
            switch (e.codec)
            {
            case Codec::Raw:
                std::memcpy(dst, e.ptr, size_t(e.raw));
                return true;
            case Codec::Lz4:
                return LZ4_decompress_safe(reinterpret_cast<const char*>(e.ptr), reinterpret_cast<char*>(dst),
                                           int(e.stored), int(e.raw)) == int(e.raw);
            case Codec::Zstd:
            {
                const size_t n = ZSTD_decompress(dst, size_t(e.raw), e.ptr, size_t(e.stored));
                return !ZSTD_isError(n) && n == e.raw;
            }
            }
            return false;
        }

        std::atomic<uint64_t> g_version{ 0 };

        /// Replaces or inserts an entry with already-packed bytes. With ifVersion set, only replaces the
        /// entry if it is still that version (a background recompression never overwrites a newer value).
        Status Commit(uint64_t key, const uint8_t* packed, uint64_t packedSize, uint64_t raw, Codec codec,
                      uint64_t ifVersion = 0)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (ifVersion)
            {
                const auto it = g_entries.find(key);
                if (it == g_entries.end() || it->second.version != ifVersion) return Status::Busy;
            }
            uint8_t* p = Allocate(packedSize);
            if (!p)
            {
                if (g_counters) g_counters->errors.fetch_add(1);
                return Status::Failed;
            }
            std::memcpy(p, packed, size_t(packedSize));
            Entry& e = g_entries[key];
            const bool existed = e.ptr != nullptr || e.ref;
            if (existed)
            {
                Account(-int64_t(e.stored), -int64_t(e.raw), 0);
                if (e.ptr) tlsf_free(g_tlsf, e.ptr);
            }
            e.ptr = p;
            e.stored = packedSize;
            e.raw = raw;
            e.codec = codec;
            e.touched = GetTickCount64();
            e.version = ++g_version;
            e.ref.reset();
            Account(int64_t(packedSize), int64_t(raw), existed ? 0 : 1);
            return Status::Ok;
        }

        /// Replaces or inserts an entry that references a served image.
        void CommitReference(uint64_t key, std::shared_ptr<const Reference> ref, uint64_t raw)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            Entry& e = g_entries[key];
            const bool existed = e.ptr != nullptr || e.ref;
            if (existed)
            {
                Account(-int64_t(e.stored), -int64_t(e.raw), 0);
                if (e.ptr) tlsf_free(g_tlsf, e.ptr);
            }
            e.ptr = nullptr;
            e.stored = 0;
            e.raw = raw;
            e.codec = Codec::Raw;
            e.touched = GetTickCount64();
            e.version = ++g_version;
            e.ref = std::move(ref);
            Account(0, int64_t(raw), existed ? 0 : 1);
        }

        /// Raw size of a value; false when absent.
        bool Peek(uint64_t key, uint64_t& raw)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const auto it = g_entries.find(key);
            if (it == g_entries.end()) return false;
            raw = it->second.raw;
            return true;
        }

        /**
         * @brief Keeps a whole value as a reference when it equals an indexed level of a served image.
         * @return true when the value was committed as a reference.
         */
        bool TryReference(uint64_t key, const uint8_t* src, uint64_t size)
        {
            if (!g_dedupe || size < kMinReference) return false;
            Indexed found;
            {
                std::lock_guard<std::mutex> lock(g_indexMutex);
                const auto it = g_index.find(Hash(src, size));
                if (it == g_index.end() || it->second.size != size) return false;
                found = it->second;
            }
            // The image must still be at hand to prove the bytes are the same, not just their hash.
            const cache::Blob image = cache::Find(found.key, false);
            if (!image || image->size() < found.offset + size
                || std::memcmp(image->data() + found.offset, src, size_t(size)) != 0)
                return false;
            auto ref = std::make_shared<Reference>();
            ref->key = found.key;
            ref->offset = found.offset;
            CommitReference(key, std::move(ref), size);
            if (g_counters)
            {
                g_counters->dedupeValues.fetch_add(1, std::memory_order_relaxed);
                g_counters->dedupeBytes.fetch_add(size, std::memory_order_relaxed);
            }
            return true;
        }

        /// Copies an entry out under the lock so the bytes can be unpacked without holding it.
        bool Snapshot(uint64_t key, Entry& out, std::vector<uint8_t>& storedCopy)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const auto it = g_entries.find(key);
            if (it == g_entries.end()) return false;
            it->second.touched = GetTickCount64();
            out = it->second;
            if (out.ptr) storedCopy.assign(out.ptr, out.ptr + out.stored);
            else storedCopy.clear();
            out.ptr = storedCopy.data();
            return true;
        }
    }

    void Init(uint32_t clientPid, uint64_t limitBytes, ipc::HostCounters* counters, uint32_t archiveSlot, bool dedupe)
    {
        g_pid = clientPid;
        g_limit = limitBytes;
        g_counters = counters;
        g_archiveSlot = archiveSlot;
        g_dedupe = dedupe && cache::Enabled();
        g_control.resize(tlsf_size());
        g_tlsf = tlsf_create(g_control.data());
    }

    void IndexImage(const cache::Key& key, const cache::Blob& image)
    {
        if (!g_dedupe || !image || key.stamp) return;
        std::vector<blp::Level> levels;
        if (!blp::GpuLevels(image->data(), image->size(), levels)) return;
        std::vector<std::pair<uint64_t, Indexed>> hashed;
        hashed.reserve(levels.size());
        for (const blp::Level& l : levels)
        {
            if (l.bytes < kMinReference) continue;
            hashed.emplace_back(Hash(image->data() + l.offset, l.bytes), Indexed{ key, l.offset, l.bytes });
        }
        std::lock_guard<std::mutex> lock(g_indexMutex);
        for (auto& [hash, where] : hashed)
        {
            const auto [it, added] = g_index.insert_or_assign(hash, std::move(where));
            if (added) g_indexOrder.push_back(hash);
        }
        while (g_index.size() > kIndexCap && !g_indexOrder.empty())
        {
            g_index.erase(g_indexOrder.front());
            g_indexOrder.pop_front();
        }
    }

    Status Put(uint64_t key, const uint8_t* src, uint64_t size, Codec codec)
    {
        if (!g_tlsf) return Status::Failed;
        const uint8_t* packed = nullptr;
        uint64_t packedSize = 0;
        const Codec used = Pack(src, size, codec, packed, packedSize);
        return Commit(key, packed, packedSize, size, used);
    }

    Status Patch(uint64_t key, uint64_t offset, const uint8_t* src, uint64_t size)
    {
        Entry e;
        std::vector<uint8_t> stored;
        std::vector<uint8_t> plain;
        Codec codec = Codec::Lz4;
        if (Snapshot(key, e, stored))
        {
            plain.resize(size_t(std::max(e.raw, offset + size)));
            if (!Unpack(e, plain.data())) return Status::Failed;
            codec = e.codec == Codec::Raw ? Codec::Lz4 : e.codec;
        }
        else
        {
            plain.resize(size_t(offset + size));
        }
        std::memcpy(plain.data() + offset, src, size_t(size));
        return Put(key, plain.data(), plain.size(), codec);
    }

    Status PatchRows(uint64_t key, uint64_t dstOffset, uint64_t dstPitch, const uint8_t* src, uint64_t rowBytes,
                     uint64_t rows)
    {
        if (!rows || !rowBytes) return Status::Ok;
        const uint64_t end = dstOffset + (rows - 1) * dstPitch + rowBytes;

        // Packed rows from the start that cover the whole value: they are the new value, nothing to merge.
        uint64_t existing = 0;
        if (dstOffset == 0 && dstPitch == rowBytes && (!Peek(key, existing) || existing <= end))
        {
            if (TryReference(key, src, end)) return Status::Ok;
            return Put(key, src, end, Codec::Lz4);
        }

        Entry e;
        std::vector<uint8_t> stored;
        std::vector<uint8_t> plain;
        Codec codec = Codec::Lz4;
        if (Snapshot(key, e, stored))
        {
            plain.resize(size_t(std::max(e.raw, end)));
            if (!Unpack(e, plain.data())) return Status::Failed;
            codec = e.codec == Codec::Raw ? Codec::Lz4 : e.codec;
        }
        else
        {
            plain.resize(size_t(end));
        }
        for (uint64_t r = 0; r < rows; ++r)
            std::memcpy(plain.data() + dstOffset + r * dstPitch, src + r * rowBytes, size_t(rowBytes));
        return Put(key, plain.data(), plain.size(), codec);
    }

    Status Get(uint64_t key, uint8_t* dst, uint64_t cap, uint64_t& full)
    {
        full = 0;
        Entry e;
        std::vector<uint8_t> stored;
        if (!Snapshot(key, e, stored)) return Status::NotFound;
        full = e.raw;
        if (cap >= e.raw) return Unpack(e, dst) ? Status::Ok : Status::Failed;
        t_plain.resize(size_t(e.raw));
        if (!Unpack(e, t_plain.data())) return Status::Failed;
        std::memcpy(dst, t_plain.data(), size_t(cap));
        return Status::NeedMore;
    }

    void Drop(uint64_t key)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_entries.find(key);
        if (it == g_entries.end()) return;
        Account(-int64_t(it->second.stored), -int64_t(it->second.raw), -1);
        if (it->second.ptr) tlsf_free(g_tlsf, it->second.ptr);
        g_entries.erase(it);
    }

    bool SweepCold(uint64_t idleMs, uint64_t budgetBytes)
    {
        std::vector<uint64_t> keys;
        bool more = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const ULONGLONG now = GetTickCount64();
            uint64_t planned = 0;
            for (const auto& [key, e] : g_entries)
            {
                if (e.codec != Codec::Lz4 || now - e.touched <= idleMs) continue;
                if (planned >= budgetBytes) { more = true; break; }
                keys.push_back(key);
                planned += e.raw;
            }
        }
        uint64_t saved = 0;
        for (uint64_t key : keys)
        {
            Entry e;
            std::vector<uint8_t> stored;
            if (!Snapshot(key, e, stored) || e.codec != Codec::Lz4) continue;
            std::vector<uint8_t> plain(size_t(e.raw));
            if (!Unpack(e, plain.data())) continue;
            const uint8_t* packed = nullptr;
            uint64_t packedSize = 0;
            if (Pack(plain.data(), plain.size(), Codec::Zstd, packed, packedSize) != Codec::Zstd) continue;
            if (packedSize >= e.stored) continue;
            if (Commit(key, packed, packedSize, e.raw, Codec::Zstd, e.version) == Status::Ok)
                saved += e.stored - packedSize;
        }
        if (!keys.empty())
            WLOG_DEBUG("store: %zu cold values moved to zstd, %.1f MB saved", keys.size(), saved / (1024.0 * 1024.0));
        return more;
    }
}
