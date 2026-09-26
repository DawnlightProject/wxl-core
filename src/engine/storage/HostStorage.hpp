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

#pragma once

#include <cstdint>

// The client still decides which archives to mount and at which priority; only the mount primitive changes
// hands, so the patch chain is the client's own. Once hosted, Wow.exe holds no archive: no handle, no hash
// or block table, no sector cache. If the host is lost for good, every recorded archive is mounted natively
// in its original priority and the client carries on (docs/host.md, "Fallback").
namespace wxl::runtime::storage::hosted
{
    /// Boot phase, before the client mounts anything: registers the archive-layer detours.
    void Install();

    /// True while wxl-host serves the client's archives.
    bool Serving();

    /// Handle predicates and operations for the file detours in StorageHook.
    bool IsHostHandle(void* handle);
    uint32_t Size(void* handle, uint32_t* sizeHigh);
    int Read(void* handle, void* dst, uint32_t len, uint32_t* read, void* overlapped);
    uint32_t Seek(void* handle, int32_t distLow, uint32_t* distHigh, uint32_t method);
    int Close(void* handle);
}
