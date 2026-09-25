// wxl-forever fog: what the froxel volume takes from the outside: point lights and local density volumes.
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

#include "../core/ExtensionApi.hpp"
#include "FogInputs.hpp"

#include <algorithm>

namespace
{
    namespace in = wxl::forever::fog::inputs;

    in::Light         g_lights[in::kMaxLights];
    int               g_lightCount = 0;
    in::DensityVolume g_volumes[in::kMaxVolumes];
    int               g_volumeCount = 0;

    void __cdecl ApiSetLights(const in::Light* lights, int count) { in::SetLights(lights, count); }
    void __cdecl ApiSetVolumes(const in::DensityVolume* v, int count) { in::SetDensityVolumes(v, count); }
}

namespace wxl::forever::fog::inputs
{
    void SetLights(const Light* lights, int count)
    {
        g_lightCount = lights ? std::clamp(count, 0, kMaxLights) : 0;
        std::copy(lights, lights + g_lightCount, g_lights);
    }

    void SetDensityVolumes(const DensityVolume* volumes, int count)
    {
        g_volumeCount = volumes ? std::clamp(count, 0, kMaxVolumes) : 0;
        std::copy(volumes, volumes + g_volumeCount, g_volumes);
    }

    int LightCount() { return g_lightCount; }
    const Light* Lights() { return g_lights; }
    int VolumeCount() { return g_volumeCount; }
    const DensityVolume* Volumes() { return g_volumes; }

    void Publish()
    {
        static InputsApi api = { &ApiSetLights, &ApiSetVolumes };
        wxl_forever::g_api->PublishInterface("wxl.forever.fog.inputs", 2, &api);
    }
}
