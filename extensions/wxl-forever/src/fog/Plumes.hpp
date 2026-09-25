// wxl-forever fog: smoke and steam emitters become plumes in the volume.
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

#include "FogInputs.hpp"

// The particle emitters of world models (core's wxl::game::effects) that make smoke or steam, from
// chimneys, campfires and spell smoke, each become a plume: a cone rising from the emitter as high
// as its particles travel in their life, widening from the particle's first size to its last,
// denser the more particles it keeps alive. Smoke is tinted a darker grey-brown, steam is not. A
// fire emitter can add a faint plume of heat and smoke above itself, since many fires carry none.
namespace wxl::forever::fog::plumes
{
    struct Tuning
    {
        float smoke     = 1.0f;   // scale on every plume's density
        float fireSmoke = 0.3f;   // density of the plume a fire adds above itself; 0 none
    };

    constexpr int kMaxPlumes = 6;

    /// Writes the plumes nearest the camera; at most cap. Call once per frame.
    int Emit(const float eye[3], const Tuning& tuning, inputs::Volume* out, int cap);

    /// One line for the panel: emitters seen by kind, plumes written.
    const char* Status();
}
