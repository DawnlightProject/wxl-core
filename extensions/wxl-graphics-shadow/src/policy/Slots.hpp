// wxl-graphics-shadow: which lights cast shadows this frame -- the 16 slots, the cube maps some of
// them hold, and the core's omni maps those maps are drawn into. Every change fades.
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
#include "wxl/OmniShadowsApi.h"

#include <cstdint>
#include <vector>

namespace wxl::gfx::shadow::slots
{
    constexpr int kSlots = WXL_GFX_SHADOW_SLOTS;
    constexpr int kMaps = WXL_GFX_SHADOW_MAPS;

    enum MapKind : int { kMapNone = 0, kMapStill = 1, kMapMoving = 2 };

    /// A slot, world space.
    struct Slot
    {
        uint32_t           id = 0;          ///< 0 free
        WXL_GfxShadowLight light{};         ///< as last handed in
        float              weight = 0.0f;
        bool               leaving = false;
        double             since = 0.0;
        bool               moving = false;
        int                map = -1;        ///< filtered map index
        float              mapWeight = 0.0f;
        bool               mapLeaving = false;
        int                carrier = -1;    ///< capsule index of its carrier
        uint32_t           capsuleMask = 0;
        bool               contact = false; ///< one of the slots with a contact shadow
    };

    /// A filtered map and the core slots it is drawn from.
    struct Map
    {
        uint32_t lightId = 0;        ///< 0 free
        int      kind = kMapNone;
        int      coreMain = -1;      ///< core slot of its static (still) or every (moving) caster
        int      coreUnits = -1;     ///< core slot of its units (still only)
        uint32_t idMain = 0, idUnits = 0;
        float    housing = 0.0f;
        uint32_t faceMask = 0x3F;
        bool     ready = false;      ///< every face converted once since it was assigned
        bool     reset = true;       ///< convert every face on the next record
        uint32_t converted[2][6] = {};  ///< core face frames last converted, main and units
    };

    /// Before the world pass: reads the candidates (handed in, or wxl-graphics-lights' list), ranks,
    /// fades, chooses the maps and hands the core its lights. dt in seconds.
    void Choose(const float eye[3], double now, float dt);

    /// Drops every map and gives the core back (settings turned the maps off, or the service stops).
    void ReleaseCore();

    /// Nobody reads shadows this frame: the core draws no map (the slots keep their state).
    void Idle();

    const Slot* List();
    Map* Maps();

    /// The core's omni table, when offered with the claim (v3); null otherwise.
    const WXL_OmniShadowsApi* Core();

    /// Lights handed in by SetLights; kept until replaced.
    void SetPushed(const WXL_GfxShadowLight* lights, int count, uint32_t frame);

    /// One line for the panel: who hands the lights, slots and maps held, hand-overs, core faces.
    const char* Status();
}
