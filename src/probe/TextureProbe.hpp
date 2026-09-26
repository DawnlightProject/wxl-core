// wxl-host-probe --textures: the host's texture images, backing references and prefetch, checked offline.
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
#include <string>

namespace wxl::probe
{
    /**
     * @brief Runs this probe again with the client image's range reserved before the child's loader maps
     *        anything there (in a fresh process, an early mapped view can sit at 0x400000).
     * @return the child's exit code, or -1 when this process is the child or the respawn failed.
     */
    int RespawnWithClientRange(const std::wstring& client);

    /**
     * @brief Runs the texture checks against a started host with the client's archives mounted.
     * @param samples  files per group (palettized, the rest); 0 for every BLP in the extracted tree.
     * @return the number of failed checks.
     */
    int Textures(const std::wstring& client, const std::wstring& extracted, size_t samples);
}
