// Mrt: binds the G-buffer render targets (view-space normals, and optionally albedo) during the world pass.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#pragma once

// INTERNAL to the core; extensions reach it through WorldSceneBeginArgs::normalTarget and albedoTarget.
namespace wxl::runtime::mrt
{
    /**
     * @brief Clears target to 0 and arranges for it to be render target 1 whenever the world's
     *        render target 0 (the one bound now) is bound, until End. target2, when given and valid,
     *        is cleared and bound as render target 2 under the same rules.
     * @param albedoBound  receives whether target2 was taken; may be null.
     * @return false when the device cannot do it (caps, size, multisampling); nothing is bound then.
     */
    bool Begin(void* device, void* target, void* target2 = nullptr, bool* albedoBound = nullptr);

    /// Unbinds render targets 1 and 2 and restores their write masks.
    void End(void* device);

    /// With WXL_DIAG_NORMALS: the core's own normal target, used when no subscriber supplied one.
    void* DiagTarget(void* device);

    /// Releases the core's DEFAULT-pool diag target before a device Reset.
    void OnDeviceLost();

    /// With WXL_DIAG_NORMALS: draws target into the lower-right quarter of the screen.
    void DiagShow(void* device, void* target);
}
