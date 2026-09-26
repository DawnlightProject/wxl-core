// Proxy-side D3D9 resource tracking: every texture, buffer and surface the device creates, by creator.
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

namespace wxl::gpu::resources
{
    /**
     * @brief Hooks the device's resource-creation slots, once per device vtable.
     *
     * Runs from the proxy's CreateDevice hooks, before the engine creates a single resource, so every
     * resource is seen. A later device reusing the same vtable is only re-recorded.
     * @param device  the device just created.
     * @param ex      true when it was created through CreateDeviceEx.
     */
    void Attach(IDirect3DDevice9* device, bool ex);

    /// Records a resource created outside the hooked slots (the managed emulation creates its own).
    void Track(void* object, ResType type, D3DPOOL pool, uint64_t bytes, void* returnAddress);

    /// Estimated bytes of one mip chain or surface of the given format.
    uint64_t SurfaceBytes(D3DFORMAT format, uint32_t width, uint32_t height);

    /// Fills a statistics snapshot. Returns 0 when out is null or smaller than GpuStats.
    int Snapshot(GpuStats* out, uint32_t outSize);

    /// The emulation counters, owned by the managed-pool layer and read by Snapshot.
    GpuEmulation& Emulation();
}
