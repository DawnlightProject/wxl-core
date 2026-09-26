// wxl-host: a bounded cache of files and decoded texture images, kept out of Wow.exe's address space.
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

#pragma once

#include "ipc/Protocol.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Keyed by where a lookup resolved (the archive holding the bytes and the name inside it), so the client's
// own priority order still decides which copy of a file is read: the cache only saves the reading. Least
// recently used entries go first; a prefetched entry never pushes out one the client used in the last 20 s.
namespace wxl::hostd::cache
{
    using Blob = std::shared_ptr<const std::vector<uint8_t>>;

    /// What an entry holds.
    enum class Kind : uint8_t
    {
        File = 0,      // the file as read
        Texture = 1,   // the decoded texture image of a BLP (blp::Decode)
    };

    struct Key
    {
        uint32_t    storage = 0;   // host archive id holding the bytes
        uint64_t    stamp = 0;     // folder files: last write time mixed with the size; 0 inside an MPQ
        std::string inner;         // lowercase, backslashes
        Kind        kind = Kind::File;

        bool operator==(const Key& o) const
        {
            return storage == o.storage && stamp == o.stamp && kind == o.kind && inner == o.inner;
        }
    };

    struct KeyHash
    {
        size_t operator()(const Key& k) const;
    };

    /// Lowercase, backslashes: the form keys use.
    std::string Normalize(const std::string& name);

    void Init(uint64_t limitBytes, ipc::HostCounters* counters);

    bool Enabled();

    /**
     * @brief Looks an entry up.
     * @param onDemand    a client read: counted as a hit (or a prefetch hit) and marks the entry used.
     * @param prefetched  receives whether the entry came from a prefetch the client had not used yet.
     */
    Blob Find(const Key& key, bool onDemand, bool* prefetched = nullptr);

    /**
     * @brief Marks an entry used by a client read that is being answered with it.
     * @return true when it came from a prefetch the client had not used yet.
     */
    bool Touch(const Key& key);

    /// True when the key is held, without touching it.
    bool Contains(const Key& key);

    /// Counts an on-demand lookup that had to read the archives.
    void CountMiss();

    /**
     * @brief Inserts or replaces an entry.
     * @return false when the entry was not kept (too large, or a prefetch that would evict recent use).
     */
    bool Insert(const Key& key, Blob blob, bool prefetched);

    /// Forgets every entry read from one archive (it was unmounted).
    void DropStorage(uint32_t storage);
}
