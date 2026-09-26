// wxl-host: reads ahead of the client what it is likely to need next.
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

#include "host/Cache.hpp"
#include "ipc/Protocol.hpp"

#include <cstdint>
#include <string>

// Two sources of work, both cheap guesses: the client's position and velocity (Op::Hint) name the terrain
// tiles ahead of it, and every tile or model the client reads names the textures and models it will read
// next. One background thread (lowered CPU and I/O priority) reads them into the cache, and waits while the
// client keeps every worker busy, so a prefetch never stands in front of a read the client is waiting on.
namespace wxl::hostd::prefetch
{
    /**
     * @param archiveSlot  archive thread slot of the prefetch thread.
     * @param workers      host workers serving the client: prefetch yields while all of them are busy.
     */
    void Init(uint32_t archiveSlot, uint32_t workers, ipc::HostCounters* counters, bool enabled);

    void Start();
    void Stop();

    /// Where the client is and where it is headed: tiles in the direction of travel are queued.
    void Hint(const char* mapFolder, float x, float y, float vx, float vy, float viewDistance);

    /// A file the client just read: what it references is queued ahead of the client asking for it.
    void Served(const char* name, const cache::Blob& bytes);

    /// The device's largest texture edge, learned from the client's texture reads (decodes ahead need it).
    void SetMaxEdge(uint32_t edge);

    /// Brackets a client read on a worker.
    void OnDemandBegin();
    void OnDemandEnd();
}
