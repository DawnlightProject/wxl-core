// wxl-graphics-lights: the omni shadow slots -- which lights hold the core's four maps, and the table every consumer reads them from.
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

#include "Lights.hpp"

#include <cstdint>

struct IDirect3DDevice9;

// The core renders six-face depth maps for up to four point lights (include/wxl/OmniShadowsApi.h);
// one party chooses them. wxl-graphics-shadow owns every shadow, so while it is loaded (or wxl-forever
// before it) this module only reads the core's maps back and matches each to this frame's list by the
// light id (LightId), for the legacy omni table the current fog reads. Without either, the chooser of
// the previous version runs here: tiers (a unit near the light, then lights without a cookie), the
// camera's own room first, a hold before a slot changes hands and a fade around it.
namespace wxl::gfx::lights::omni
{
    struct Settings
    {
        int   enabled    = 1;      // drive the core's maps at all (when this extension owns the slots)
        int   unitsFirst = 1;      // the maps go first to lights with a unit near them; a still lamp with a cookie waits
        int   carried    = 1;      // carried lights may take a map: the carrier casts on its surroundings
        float self       = 0.5f;   // yards around a light whose casters (its own housing) do not shadow it
        float carriedSelf = 0.3f;  // the same for a carried light: its item and the hand holding it
        float carriedSkip = 1.0f;  // yards around a carried light where receivers ignore its map (the carrier's body)
        int   faceSize   = 512;    // texels per cube face of the core's maps
        int   refresh    = 3;      // faces of still lights redrawn a frame, in turn (moved ones go at once)
    };

    Settings& Get();

    /// Reads WXL_GFX_LIGHTS_OMNI_*. Called once at load.
    void Install();

    /// Per frame, once the list is final: chooses the core's lights (when owned), reads the maps it
    /// rendered and publishes the omni table for the frame.
    void Frame(IDirect3DDevice9* dev, const Light* list, int count, const float eye[3], uint32_t frame);

    /// Whether another extension (wxl-graphics-shadow, or wxl-forever) owns the core's slots.
    bool ForeverLoaded();

    /// Whether the core offers omni shadow maps with change tracking.
    bool CoreAvailable();

    /// One line for a panel: who owns the slots, how many maps this frame's lights hold, hand-overs.
    const char* Status();

    /// The Shadow maps tab of the lights panel.
    void Panel();
}
