// wxl-graphics-fog: the orchestration -- the game tick, the passes registered with wxl-graphics-extend,
// the fog's clock, the sun and moon, the camera's indoor state and the client's own distance fog.
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

namespace wxl::gfx::fog
{
    /// Reads the settings, publishes the API, registers the passes and the panel. Once, from WXL_Load.
    void Install();

    /// True while the fog draws: enabled, DXVK available, and drawn within the last second.
    bool Active();

    /// The panel's lines about the fog's state.
    struct State
    {
        bool     registered = false;
        uint32_t computePass = 0, applyPass = 0;
        bool     cameraIndoor = false;
        float    indoorEased = 0.0f;
        bool     havePlayer = false;
        float    player[3] = {};
        float    ground = 0.0f;
        double   clock = 0.0;
    };
    const State& GetState();

    /// The panel (ui/Panel.cpp).
    void Panel();
}
