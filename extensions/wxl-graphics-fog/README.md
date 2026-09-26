# wxl-graphics-fog

**A simulated, volumetric fog that lives in the world: it lies in the land, runs down it, fills its
hollows and reaches the horizon, and gameplay can push it around.**

It runs as Vulkan compute on DXVK's device (through wxl-graphics-extend) and composites into the
D3D9 frame in the atmosphere slot (`WXL_GFX_ORDER_ATMOSPHERE`: after surface lighting, before effects
and post). Without DXVK it stays inert. The design, its reasons and
its budget are in `docs/design.md`.

| Part | What it does |
|---|---|
| **Terrain block** | 8 x 8 baked ADT tiles around the camera (height, sky share, water from `5.tools/forever-bake`), uploaded tile by tile into GPU images: the fog's floor with a full mip chain, hollows, sun visibility. |
| **Rivers** | A shallow-water simulation of cold air on that floor: fog forms at night on open ground, in hollows and over water, drains downhill, pools in basins, spills over cols, thins down cliffs, is pushed by the wind and burnt off by the sun. It warms up in a second or two after a teleport. |
| **Clipmaps** | Four camera-centred 3D levels (0.5, 2, 8, 32 yd cells), toroidal, terrain-following. Each steps at its own fixed rate: MacCormack advection by the rivers' flow, the wind deflected by relief and curl-noise turbulence, then renewal towards a target shaped by boiling Perlin-Worley noise. Deep pools climb higher in a soft, billowing veil (`CLIMB`). |
| **Atmosphere** | A smooth medium in the whole air column (`WXL_FOG_AIR_*`): full up to a base over the ground, thinning with a scale height, in large slow patches. It is summed with the lying fog in every sample of the march, so both share one transmittance; it has its own colour share, phase, and sun, moon, sky and lamp strengths. It gives aerial perspective, mood and lamp halos in the air where the lying fog is thin. |
| **March** | Half resolution to 60 yd with two octaves of curl-bent fraying, lamps and world shadows; quarter resolution to 2500 yd, a far step through the lying fog split in up to five; the haze and the atmosphere beyond in closed form. Blue-noise jitter, empty-space skipping, temporal reprojection with neighbourhood clipping (far fog keeps more history), an upsample in which each low-resolution texel carries two layers (the nearest and farthest scene distance of its footprint), so thin geometry against the sky keeps the sky's fog around it. |
| **Light** | Sun and moon with self-shadowing, terrain shadows and the engine's shadow cascades, dual-lobe phase, powder, multiple scattering; sky light darker in valleys and under deep fog; every lamp of wxl-graphics-lights with its cookie and omni shadows through a frustum grid, its light thinned by the medium on its way (tight halos in thick fog, wide ones in thin air). |
| **Indoor air** | Inside WMO rooms the air is its own medium (dust, a floor haze, lamp light), eased over 5 s at doorways. |
| **Cascades** | Fog banks lying behind crests spill over them and pour down the far side as a thick current that stretches into strands and evaporates as it descends (it warms as it falls). The spill points and their reservoirs are baked per tile by `5.tools/forever-bake` (`bake.py cascade`); the rivers carry a tracer of the cascade fog. Gameplay places its own cascades through the API. |
| **Wakes** | A 2D incompressible fluid at the ground around the player, 0.25 yd near it and 1 yd out to 128 yd: bodies (up to 192) carry the air with them, so the fog parts ahead, curls in eddies at their flanks and flows back in behind; the fluid's flow also moves the 3D fog. A horde ploughs through a fog field. |
| **Spells** | Missiles bore tunnels that close by rolling in round their axis; impacts and new spell effects send a shock ring that blows the fog out and lets it flow back; fire spells and flames evaporate the fog; frost spells and icy effects leave fog. A carried torch or lantern (anyone's) holds a pocket of clearer air around its bearer, a thin mist on the ground, the fog wall rolling at its edge; put away, the fog flows back in. Smoke emitters make plumes. Standing in thick fog closes it in around the screen. |
| **Threat API** | `include/wxl/GraphicsFogApi.h` v3: sources, fronts, flows, cascades, intensity, pulses, wind, and readbacks of the fog around the camera. |

## Requirements

- DXVK (the `d3d9.dll` proxy) and wxl-graphics-extend; wxl-graphics-lights for lamps and rooms.
- The baked tiles of `5.tools/forever-bake` (horizon and water) for the rivers, and its cascade maps
  (`bake.py cascade --map <Map>`) for the automatic cascades. A map without them
  still gets the mist, the banks and the haze on a floor traced under the camera.
- Shadow cascades (`extShadowQuality` 3 or more) for shafts through trees and buildings.

## Settings

`Extensions\wxl-graphics-fog\wxl-graphics-fog.cfg`; every key is documented in
`wxl-graphics-fog.cfg.example`. Outdoor keys are `WXL_FOG_*`, the indoor air's `WXL_FOG_INDOOR_*`.
An environment variable of the same name wins over the file. `WXL_FOG_QUALITY` (low, medium, high,
ultra) sets steps, grid sizes and simulation rates together.

`presets/dead-world.cfg` is a cold, grey-green "dead world": an atmosphere that holds the mood and the lamps' glow everywhere, over a lying fog that brings the relief and the rivers, with dark nights.

The overlay panel **Graphics Fog** changes everything live. Every control has a (?) that explains it.

| Tab | What is there |
|---|---|
| Overview | State, per-pass GPU timings, the simulation's status, "Start the simulation over". |
| Quality | Steps, ranges, temporal, simulation rates, lamp grid. |
| Fog, Shape, Motion | Where the lying fog lies (and how high it climbs), its substance, how it moves. |
| Air | The atmosphere: density, height, base, terrain following, patches, colour share, glow and its light. |
| Rivers | The terrain transport: formation, drainage, decay, pools, warm-up. |
| Light, Indoor, Effects | Outdoor light and colour, the indoor air, wakes, missiles, plumes, immersion. |
| Cascades | The automatic banks behind crests: strength, depth, threshold, the wind's choice, evaporation, strands. |
| Threat | Test buttons: a phantom horde crossing, one phantom runner, a cascade pouring towards you, a wall, a closing ring, a bank, a clearing, a gust, breath, surge. |
| Debug | Views (density slices, rivers, flow, formation, indoor/outdoor, lamps, sun, terrain, distance, skipping) and switches that remove one ingredient each, among them "No atmosphere" and "Only the atmosphere" to compare the two layers; maps of the wake fluid and of the cascades. |

## The threat API

Resolve it lazily and check `structSize` before touching a v2 field; the table is published under
version 2 and, for v1 callers, version 1.

```cpp
const WXL_GraphicsFogApi* fog = static_cast<const WXL_GraphicsFogApi*>(
    api->GetInterface(WXL_GRAPHICS_FOG_API_NAME, WXL_GRAPHICS_FOG_API_VERSION));

WXL_GfxFogFront ring{};
ring.structSize = sizeof ring;
ring.kind = WXL_GFX_FOG_FRONT_CLOSING;
ring.origin[0] = x; ring.origin[1] = y; ring.origin[2] = z;
ring.startRadius = 80.0f; ring.endRadius = 6.0f; ring.speed = 3.0f;
ring.height = 12.0f; ring.depth = 25.0f; ring.density = 0.3f; ring.billow = 0.8f;
ring.fadeIn = 2.0f; ring.fadeOut = 4.0f;
uint32_t handle = fog->AddFront(&ring);
// later
fog->RemoveFront(handle, -1.0f);
```

A cascade, for a server that wants "fog pouring down the hill towards the village":

```cpp
WXL_GfxFogCascade c{};
c.structSize = sizeof c;
c.mapId = 0;                                   // Eastern Kingdoms
c.position[0] = x; c.position[1] = y; c.position[2] = z;   // the spill point, on the crest
c.direction[0] = dx; c.direction[1] = dy;      // down towards the village
c.width = 40.0f; c.depth = 15.0f;
c.fadeIn = 10.0f; c.fadeOut = 20.0f;
uint32_t pour = fog->AddCascade(&c);
```

Everything fades in and out and becomes a simulation primitive, so the fog carries what it did: a
carved hole drifts and refills, a front shoves the fog ahead of it. Call from the render thread.

## Building

The shaders are HLSL compiled to SPIR-V by DXC (`deps/dxc`, or `WXL_DXC`, the Vulkan SDK, or
`PATH`) into `src/gpu/Spirv.gen.cpp` at build time. Without DXC the committed `Spirv.gen.cpp` is
used as it is. The composite is a D3D9 ps_3_0 shader compiled at run time through wxl-graphics-extend.
