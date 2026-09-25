// wxl-graphics-extend: the world pass's shared textures (INTZ depth, G-buffer normals, HDR colour)
// and the device facts they depend on.
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

#include <windows.h>
#include <d3d9.h>

#include <cstdint>

// Private to the scheduler. Every texture is D3DPOOL_DEFAULT, one level, created on first Ensure at
// the world's size and re-created when that size changes; Release drops them all before a device
// reset and forgets the probe, so the next world pass reads the device facts again.
namespace wxl::gfx::frame::scene
{
    /// Reads the device facts once per device: WXL_GFX_CAP_* and the one-line status. Logs them.
    void Probe(IDirect3DDevice9* dev, IDirect3DSurface9* sceneDepth);

    /// WXL_GFX_CAP_* bits; carries WXL_GFX_CAP_PROBED once Probe ran, 0 before.
    uint32_t Caps();

    /// One line on the depth path for the panel and WXL_GfxStatus::depthStatus.
    const char* Status();

    enum class Kind { Depth, Normals, Hdr, Count };

    /**
     * @brief The level-0 surface of a kind's texture at width x height, creating or re-creating it
     *        as needed.
     * @return the surface, borrowed; null when the device refused the texture (logged once, and not
     *         tried again until Release).
     */
    IDirect3DSurface9* Ensure(Kind kind, IDirect3DDevice9* dev, UINT width, UINT height);

    /// The texture behind one of our surfaces; null when surface is not ours.
    IDirect3DTexture9* Owned(Kind kind, const void* surface);

    /**
     * @brief The 2D texture a surface someone else owns is level 0 of; null for a standalone surface
     *        (CreateDepthStencilSurface / CreateRenderTarget) or, with requireIntz, a texture whose
     *        format is not INTZ.
     *
     * Borrowed: the reference GetContainer adds is dropped before returning, since the owner keeps
     * the surface -- and through it the texture -- alive for the frame.
     */
    IDirect3DTexture9* ContainerTexture(IDirect3DSurface9* surface, bool requireIntz);

    /// Releases every texture and forgets the probe.
    void Release();
}
