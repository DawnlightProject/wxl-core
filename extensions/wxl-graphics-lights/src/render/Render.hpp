// wxl-graphics-lights: the render passes and their order -- the compute pass (the field, then the
// surfaces), the D3D9 composite at WXL_GFX_ORDER_LIGHTING and the interim resolve at WXL_GFX_ORDER_RESOLVE.
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

namespace wxl::gfx::lights::render
{
    /// Reads the settings of every render module. Called once at load.
    void Install();

    /// From the lifecycle's OnFrame: registers the passes once the services are there.
    void Register();

    /// Before a device reset: every D3D9 resource of the render modules goes.
    void OnDeviceLost();

    /// Whether surface lighting and the HDR chain are on (WXL_GFX_LIGHTS_SURFACE).
    int& Enabled();

    /// Whether the engine's own point lights are held off this frame (the surfaces light the world
    /// alone); the surfaces then keep all of a light the engine would otherwise have lit too.
    bool EngineLightsOff();

    /// One line for the panel: what ran last frame.
    const char* Status();
}
