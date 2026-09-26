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

#pragma once

#include <cstdint>

namespace wxl::diag::memory
{
    /**
     * @brief Records one archive mount the client asked for.
     * @param name      path as the client passed it.
     * @param priority  the client's search priority for it.
     * @param hosted    true when wxl-host serves it instead of the client's own archive layer.
     * @param ok        whether the mount succeeded.
     */
    void RecordArchive(const char* name, int priority, bool hosted, bool ok);

    /// Marks the archive layer as served by wxl-host: any .MPQ CreateFile from then on is logged as a warning.
    void SetArchivesHosted(bool hosted);

    /// Host counters, pushed by the host client so the periodic report can include them.
    struct HostLine
    {
        char text[512];
    };

    /// Registers a callback the periodic report calls for one extra line (the host client's counters).
    void SetExtraLine(void (*fill)(HostLine& out));
}
