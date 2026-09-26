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

#pragma once

#include "ipc/Protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

// The client decides which archives exist and at which priority; this module only answers lookups the way
// the client's archive layer would: the highest priority first, the later mount first on a tie. MPQ files
// are read with StormLib (one handle per worker thread, since a StormLib handle is not thread-safe), folder
// archives straight from disk, and a nested archive inside an MPQ as a path prefix of its parent.
namespace wxl::hostd::archives
{
    /// Kinds reported to the client.
    constexpr uint32_t kKindFile = 1;
    constexpr uint32_t kKindFolder = 2;
    constexpr uint32_t kKindPrefix = 3;

    /// Win32 error the client's archive layer reports for a file that is not an MPQ.
    constexpr uint32_t kErrorNotArchive = 0x6C;

    /// Sets the client folder relative paths resolve against, and the number of worker threads.
    void Init(const std::wstring& root, uint32_t threads);

    /**
     * @brief Mounts an archive.
     * @param thread  worker slot of the caller (its StormLib handle is kept for later reads).
     * @param error   receives the client error code on failure (2 not found, 0x6C not an archive).
     */
    ipc::Status Mount(uint32_t thread, const char* name, int priority, uint32_t parent, uint32_t& id, uint32_t& kind,
                      uint32_t& error);

    void Unmount(uint32_t id);

    /// Looks a file up; foundIn receives the archive that holds it.
    ipc::Status Stat(uint32_t thread, const char* name, uint32_t archive, uint64_t& size, uint32_t& foundIn);

    /**
     * @brief Reads a whole file into dst.
     * @return Ok, NotFound, NeedMore (size set, dst untouched) or Failed.
     */
    ipc::Status Read(uint32_t thread, const char* name, uint32_t archive, uint8_t* dst, uint64_t cap, uint64_t& size,
                     uint32_t& foundIn);

    /// Reads a whole file into a vector (for jobs).
    ipc::Status ReadAll(uint32_t thread, const char* name, std::vector<uint8_t>& out);

    /// Where a lookup resolved: the entry the client sees, the one holding the bytes, and the name in it.
    struct Located
    {
        uint32_t     id = 0;        // the entry the client sees (reported back as the archive)
        uint32_t     storage = 0;   // the entry holding the bytes (the parent MPQ for a view)
        uint32_t     kind = 0;      // kKindFile or kKindFolder
        std::wstring path;          // the folder file, or the MPQ
        std::string  inner;         // the name inside the MPQ, or the name under the folder
        uint64_t     stamp = 0;     // folder files: last write time mixed with the size; 0 in an MPQ
    };

    /// Resolves a name exactly as Read would, without reading.
    ipc::Status Locate(uint32_t thread, const char* name, uint32_t archive, Located& out);

    /// Reads a located file into dst; size receives its size (NeedMore, dst untouched, when cap is short).
    ipc::Status ReadLocated(uint32_t thread, const Located& at, uint8_t* dst, uint64_t cap, uint64_t& size);

    /// Reads a located file whole.
    ipc::Status ReadLocated(uint32_t thread, const Located& at, std::vector<uint8_t>& out);

    /// Rebuilds a location inside one MPQ entry from its inner name; false when the entry is gone.
    bool Relocate(uint32_t storage, const std::string& inner, Located& out);

    /// Number of live mounts.
    uint32_t Count();
}
