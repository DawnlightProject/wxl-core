# wxl-graphics-lights

**All light in the render stack: the sources, and their effect on surfaces and in the air.**

This extension owns:
- the lamps, torches, fires and candles of the world, and the sun, the moon and the sky as light
  definitions;
- their identity, colour temperature, intensity, cookies and rooms;
- the lighting of surfaces, deferred, in HDR, in Vulkan compute;
- a lamp light field for the air, which the fog samples;
- the HDR chain's composite and, until wxl-graphics-post exists, an interim resolve.

Shadows come from wxl-graphics-shadow, the medium from wxl-graphics-fog, and the scene data (depth,
G-buffer, HDR target, compute scheduler) from wxl-graphics-extend. Each is optional: without the
shadow service every lamp is unshadowed, and without the fog a thin analytic halo stands in for it.

The design, its reasons, the API contracts and the cost estimates are in [docs/design.md](docs/design.md).
The previous version is archived at `archive/wxl-graphics-lights-surface-v1/`.

## What it publishes

| Interface | Header | What |
|---|---|---|
| `wxl.graphics-lights` v1 | `GraphicsLightsApi.h` | The light list, the legacy light, cluster, omni and cookie textures, rooms, demand. Unchanged, so the current fog keeps running. |
| `wxl.graphics-lights.sources` v1 | `GraphicsLightsSourcesApi.h` | The same list in HDR (family, Kelvin, scene-referred intensity, reach, soft core, flicker, room gate), as structs and as a texture (`shaders/wxl/lights/sources.hlsli`); the sun, the moon and the sky. |
| `wxl.graphics-lights.field` v1 | `GraphicsLightsFieldApi.h` | The lamps' in-scattered light per unit scattering coefficient over a camera-frustum grid, with a settable phase and a two-lobe form (`shaders/wxl/lights/field.hlsli`). |
| `wxl.graphics-lights.resolve` v1 | `GraphicsLightsResolveApi.h` | The FP16 scene's encoding, the resolve curve (`shaders/wxl/lights/hdr.hlsli`), and `ClaimResolve` for wxl-graphics-post. |
| `wxl.graphics-lights.surface` v1 | `GraphicsLightsSurfaceApi.h` | Still published, retired: `Outputs` returns 0. |

## The frame

| Order | Pass | What |
|---|---|---|
| begin, -1000000 | `lights` | Gather, merge, families, flicker, rooms and gates, cookies; publish the legacy textures and the source texture; hand the lights to wxl-graphics-shadow. |
| compute, 100 | `lights` | The field and its halo (when wanted), then the surfaces: every lamp of each pixel's cluster, Burley and GGX, cookies, shadow masks, room gates, the sun and moon factor. Copied to a D3D9 texture. |
| D3D9, 100 | `lights.composite` | The engine's image expanded into scene-referred light, the lamps added, written back to the FP16 scene. |
| D3D9, 350 | `lights.resolve` | The interim tonemap to the back buffer, unless another pass resolved or claimed it. |

## Using the field (for the fog)

```cpp
const WXL_GraphicsLightsFieldApi* field = static_cast<const WXL_GraphicsLightsFieldApi*>(
    api->GetInterface(WXL_GRAPHICS_LIGHTS_FIELD_API_NAME, WXL_GRAPHICS_LIGHTS_FIELD_API_VERSION));

uint32_t __cdecl Wants(void*) { if (field) field->Want(); return WXL_GFX_NEED_DEPTH; }

void __cdecl Record(void*, const WXL_GfxVkFrame* vk)       // a compute pass ordered after 100
{
    WXL_GfxLightsField f{};
    f.structSize = sizeof f;
    if (!field->Get(&f)) return;                           // no field this frame: no lamp light
    // bind f.inscatter (and f.ambient, f.direction) as Texture3D, with f.viewProjRel and the range
    // radiance += transmittance * sigmaS * WxlLightsFieldInscatter(...) * step
}
```

Call `SetPhase` once with the fog's own phase; `inscatter` is then exact for it.

## Requirements

- DXVK and wxl-graphics-extend, for the surfaces and the field. Without DXVK the HDR chain still runs
  and the service still publishes its lights.
- `extShadowQuality` 1 or more, and the rewritten engine shaders for the G-buffer:

  ```
  python tools/bls_gbuffer.py <extracted client Shaders> <client>/Data/Patch-4.MPQ/Shaders
  ```

- Cookies: a bake under `Textures\Forever\Cookies` (`5.tools/forever-bake`, manifest format 2).

## Configuration

`Extensions\wxl-graphics-lights\wxl-graphics-lights.cfg`; every key is in
[wxl-graphics-lights.cfg.example](wxl-graphics-lights.cfg.example), and the panel **Graphics Lights**
sets them live. Every control has a "(?)" tooltip.

## Layout

```
src/Module.cpp            entry points, the published tables, the gather pass, the device lifecycle
src/core/Extension.hpp    the core's table, logs, config, lookups of the services it leans on
src/lights/               the service: gather, model table, families, flicker, rooms, cookies,
                          sky, legacy omni table, the published textures
src/render/               the compute pass (field, halo, surfaces), the composite and the resolve
src/gpu/                  the Vulkan layer and the embedded shaders (Shaders.gen.cpp)
src/ui/Panel.*            the panel and the light markers overlay
shaders/compute/          the compute shaders (DXC to SPIR-V at build time)
shaders/d3d9/             the composite and the resolve (ps_3_0, compiled at run time)
shaders/wxl/lights/       the public includes: sources, field, hdr (and the legacy lights, cookies)
tools/bls_gbuffer.py      the engine shader rewrite that writes the G-buffer
data/model-lights.csv     the model table (deployed beside the DLL)
docs/design.md            the design
```

## Building

Nothing to do beyond the core build: the root `CMakeLists.txt` builds every folder under
`extensions/`. `shared.cmake` compiles the compute shaders with DXC (`deps/dxc`, `WXL_DXC`, the Vulkan
SDK, `PATH`) into `src/gpu/Shaders.gen.cpp` whenever one changes. The field's point-shadow variant
compiles against `extensions/wxl-graphics-shadow/shaders/wxl/shadow/shadow.hlsli` when it is there.
Without DXC the committed file is used as it is.

## License

GPL-3.0-or-later, like the rest of WarcraftXL.
