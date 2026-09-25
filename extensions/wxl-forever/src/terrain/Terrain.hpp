// wxl-forever terrain: baked horizon maps and the terrain as a sun shadow caster.
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

// Two services and their debug views. Horizon (Horizon.hpp): the baked terrain horizon around the
// camera, resident as wrapping atlases, read by any shader through shaders/horizon.hlsli for sun
// visibility and sky occlusion off screen. Caster (Caster.hpp): the same tiles drawn depth-only
// into the engine's sun shadow maps after each of its own render callbacks, and a lower shadow
// light at dusk. Works with every other feature off; the fog and the surface consume the results.
namespace wxl::forever::terrain
{
    /// Reads the configuration, subscribes to the world pass, adds the debug pass. Called once at load.
    void Install();

    /// The Forever window: the feature's switches on the Overview, its tab, its Debug section.
    void UiOverview();
    void UiSettings();
    void UiDebug();
}
