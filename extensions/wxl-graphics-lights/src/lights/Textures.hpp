// wxl-graphics-lights: a texture consumers of every kind may read -- DEFAULT pool, filled by copying a
// system-memory twin up, never by locking it.
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

// Why a twin. A D3DPOOL_MANAGED texture filled with LockRect reaches the GPU only when a D3D9 draw
// first binds it (DXVK uploads managed textures on bind), so a Vulkan compute pass importing it
// (GraphicsVulkanApi.h) would read last frame's texels, or none. A DEFAULT-pool texture written with
// UpdateTexture / UpdateSurface from a SYSTEMMEM twin is copied on the command stream at the call,
// ahead of whatever reads it, D3D9 sampler or Vulkan image alike. The twin is locked and filled on
// the CPU; the DEFAULT one is what consumers see. Both are released before a device reset.
namespace wxl::gfx::lights::tex
{
    struct Twin
    {
        IDirect3DTexture9* gpu = nullptr;         // D3DPOOL_DEFAULT: what consumers bind and import
        IDirect3DTexture9* cpu = nullptr;         // D3DPOOL_SYSTEMMEM: locked and filled here
        IDirect3DSurface9* gpuSurface = nullptr;  // level 0 of each, for the partial copies
        IDirect3DSurface9* cpuSurface = nullptr;
        unsigned  width = 0, height = 0;
        D3DFORMAT format = D3DFMT_UNKNOWN;
        unsigned  texelBytes = 0;

        explicit operator bool() const { return gpu != nullptr; }
    };

    /// Creates both textures, one level, no usage. False (and nothing kept) when the device refuses;
    /// the reason goes to the log under name.
    bool Create(IDirect3DDevice9* dev, unsigned width, unsigned height, D3DFORMAT format, Twin& out, const char* name);

    void Release(Twin& t);

    /// Locks the twin (the whole level, or rect); null when the lock fails. Unlock when done.
    uint8_t* Lock(Twin& t, const RECT* rect, int& pitch);
    void     Unlock(Twin& t);

    /// Copies the whole twin to the DEFAULT texture.
    bool Upload(IDirect3DDevice9* dev, Twin& t);

    /// Copies the first rows of the twin (rows 0..rows - 1, every column), for a texture whose tail
    /// no consumer reads this frame.
    bool UploadRows(IDirect3DDevice9* dev, Twin& t, unsigned rows);

    /// Copies one rectangle of the twin to the same place of the DEFAULT texture.
    bool UploadRect(IDirect3DDevice9* dev, Twin& t, const RECT& rect);
}
