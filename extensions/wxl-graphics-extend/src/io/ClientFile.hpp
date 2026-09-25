// wxl-graphics-extend: whole client files, through the client's file system first, then loose.
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

#include "wxl/ByteSink.h"

#include <string>

// Order: the client's file system (its archives and patch folders, wxl::game::io), then the path
// loose relative to the client folder, then loose under Data\ (a file written after the client
// mounted its patch folders is only found that way). Render thread for the first source: the client's
// file functions are not called from other threads.
namespace wxl::gfx::io
{
    /// Records the calling thread as the render thread. Called once from WXL_Load (the main thread).
    void Install();

    /// From any other thread the client's file system is skipped and only the loose sources are read.
    bool Read(const char* path, std::string& out);
    int  ReadToSink(const char* path, WXL_ByteSink* out);
}
