// D3DPOOL_MANAGED emulation for a D3D9Ex device: managed resources live in DEFAULT, locks go through staging.
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

#include "engine/gpu/GpuStats.hpp"

#include <d3d9.h>
#include <cstdint>

namespace wxl::gpu::managed
{
    /// What a creation call should really ask the device for.
    struct Plan
    {
        D3DPOOL pool;
        DWORD   usage;
        bool    emulate;
    };

    /// Decides the pool and usage for a texture request. On a classic device this is the request itself.
    Plan PlanTexture(void* caller, DWORD usage, D3DFORMAT format, D3DPOOL pool);

    /// Decides the pool and usage for a vertex or index buffer request.
    Plan PlanBuffer(void* caller, DWORD usage, D3DPOOL pool);

    /// Takes over a resource created MANAGED and emulated in DEFAULT.
    void AdoptTexture(IDirect3DDevice9* device, IDirect3DTexture9* texture, void* caller, uint64_t bytes);
    void AdoptCube(IDirect3DDevice9* device, IDirect3DCubeTexture9* texture, void* caller, uint64_t bytes);
    void AdoptVolume(IDirect3DDevice9* device, IDirect3DVolumeTexture9* texture, void* caller, uint64_t bytes);
    void AdoptVertexBuffer(IDirect3DDevice9* device, IDirect3DVertexBuffer9* buffer, void* caller, uint32_t length,
                           DWORD usage);
    void AdoptIndexBuffer(IDirect3DDevice9* device, IDirect3DIndexBuffer9* buffer, void* caller, uint32_t length,
                          DWORD usage);

    /// Called for every created resource, before it is recorded: drops what a dead object left at its address.
    void OnCreated(void* object);

    /// Called from the tracked Release when an object's last reference goes.
    void OnDestroyed(void* object);

    /// Called once per created device, after the creation slots are hooked.
    void AttachDevice(IDirect3DDevice9* device, bool ex);

    /// Copies the emulation counters.
    void FillEmulation(GpuEmulation& out);
}
