// wxl-forever: the world's depth as a texture, shared by every render feature of the suite.
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

#include "engine/events/Event.hpp"
#include "game/Gx.hpp"

struct IDirect3DTexture9;

// On OnWorldSceneBegin the world pass is redirected to draw its depth into an INTZ texture, which
// any feature can then sample on OnWorldSceneEnd. The device is probed once: without INTZ, with a
// multisampled depth or a pure device, nothing is redirected and the reason is logged.
namespace wxl::forever::depth
{
    /// Redirects the pass about to draw; false when the device cannot give its depth as a texture.
    bool Redirect(const wxl::events::WorldSceneBeginArgs& args);

    /// True when the pass the end event reports drew its depth into the texture.
    bool Owns(const void* sceneDepth);

    IDirect3DTexture9* Texture();
    unsigned Width();
    unsigned Height();

    /// The depth range the world pass drew its geometry into, read before the pass.
    wxl::game::gx::DepthRange Range();

    /// One line for the panel: the device facts and whether the redirect is active.
    const char* Status();

    /// Frees the DEFAULT-pool texture before a device reset; the device is probed again after.
    void Release();
}
