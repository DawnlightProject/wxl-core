// wxl-forever: what the air between a light and a surface does to it, as one shared value.
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

#include "Passes.hpp"

// A feature that fills the air with a medium (the fog) states each frame how much it extinguishes
// light per yard around the camera; a feature that lights surfaces reads it to dim a light by the
// air it crosses. With nothing stated for a frame or more, the air is clear.
namespace wxl::forever::media
{
    namespace detail
    {
        inline float    g_extinction = 0.0f;
        inline uint32_t g_frame = 0;
    }

    /// Extinction per yard of the air near the camera this frame.
    inline void SetExtinction(float perYard)
    {
        detail::g_extinction = perYard > 0.0f ? perYard : 0.0f;
        detail::g_frame = passes::FrameIndex();
    }

    /// The last stated extinction, or 0 when nothing stated one this frame or the one before.
    inline float Extinction()
    {
        return passes::FrameIndex() - detail::g_frame <= 1 ? detail::g_extinction : 0.0f;
    }
}
