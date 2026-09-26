// The memory map: engine allocations by source and subsystem, process memory by owner, loaded modules.
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

#include <cstddef>

/// The report side of the allocation tracker. Everything here runs on the memory reporter thread.
namespace wxl::diag::memory::map
{
    /// Installs the allocation and process hooks (Boot phase).
    bool Install();

    /// Adds the map's sections to one memory[reason] report: engine heap, subsystems, top files,
    /// generic names by owner, private memory by owner, modules.
    void LogSections(const char* reason);

    /**
     * @brief One reporter tick (every 250 ms): opens and closes the peak windows around loading
     *        screens and zone changes, samples them, and keeps the per-file and per-subsystem peaks.
     * @param nowMs  GetTickCount64 at the tick.
     */
    void Tick(unsigned long long nowMs);

    /// Writes the full map to Logs\memory-map.txt. Walks every heap: a hitch of some milliseconds.
    bool WriteDump(const char* reason);

    /// A few live figures for the panel, refreshed by Tick.
    void Summary(char* out, size_t cap);
}
