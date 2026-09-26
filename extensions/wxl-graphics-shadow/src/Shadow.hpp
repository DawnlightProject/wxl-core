// wxl-graphics-shadow: the orchestration -- the game tick, the passes registered with
// wxl-graphics-extend, and what one frame publishes to consumers.
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

#include "wxl/GraphicsShadowApi.h"

#include <cstdint>

namespace wxl::gfx::shadow
{
    /// Reads the settings, publishes the API, registers the passes and the panel. Once, from WXL_Load.
    void Install();

    /// What the frame being drawn publishes (GraphicsShadowApi.h's GetFrame and Slots).
    struct Published
    {
        uint32_t          frameIndex = 0;
        bool              ran = false;          ///< the compute pass recorded for frameIndex
        uint32_t          flags = 0;            ///< WXL_GFX_SHADOW_FRAME_*
        uint32_t          width = 0, height = 0;
        WXL_GfxVkImage    masks{};
        float             toSun[3] = { 0.0f, 0.0f, 1.0f };
        float             sunWeight = 0.0f;
        float             toMoon[3] = { 0.0f, 0.0f, 1.0f };
        float             moonWeight = 0.0f;
        WXL_GfxShadowSlot slots[WXL_GFX_SHADOW_SLOTS] = {};
        int8_t            slotByIndex[128] = {};  ///< light list index -> slot, -1 none
    };

    Published& Pub();

    /// Consumers' wants, kept for this frame and the next.
    void NoteWant(uint32_t what);
    uint32_t Wanted();

    /// Lights handed in by SetLights (a copy, kept until replaced).
    void SetPushedLights(const WXL_GfxShadowLight* lights, int count);

    /// True while the service draws: enabled, DXVK available, recorded within the last second.
    bool Active();

    /// One line for the API's Status and the panel.
    const char* StatusLine();

    /// The panel (ui/Panel.cpp).
    void Panel();
}
