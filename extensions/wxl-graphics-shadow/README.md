# wxl-graphics-shadow

**Every shadow of the render stack, in one place.**

`wxl-graphics-shadow` owns every shadow:
- the lamps' shadow maps;
- body shadows for lamps without one;
- the sun and the moon;
- the terrain;
- contact shadows.

It hands them to the other render extensions through the C ABI of
`include/wxl/GraphicsShadowApi.h` (`wxl.graphics-shadow` v1), in two forms:
- **screen-space masks** for surfaces;
- **a Vulkan binding set with an HLSL include** (`shaders/wxl/shadow/shadow.hlsli`) for any point,
  such as fog froxels or a lamp field.

Without it, or without DXVK, everything is simply unshadowed.

| What | How |
|---|---|
| **Lamp maps** | The core's omni maps (`wxl.omnishadows` v3, claimed here). A still lamp's static casters are drawn once; only moving bodies are drawn again, on the faces that see one. Moving and carried lights redraw every caster every frame. |
| **Filtering** | Each mapped light's cube is converted into EVSM moments with mips in one shared atlas. Lookups are trilinear, pattern-free and world-anchored: one fetch per froxel for the fog. |
| **Carried torches** | A map of their own, with the torch and hand as housing. Receivers inside the carrier's capsule skip its light, so the carrier is never self-shadowed while its shadow falls on the rest. |
| **Bodies** | 32 capsules (player, NPCs, creatures), tested along the whole segment from a receiver to the light, with a penumbra that widens along it. |
| **Sun and moon** | The engine's cascades, rendered along a lower, truer sun by day and the moon by night (`wxl.shadowlight` v2, claimed). The terrain is drawn into them, the baked horizon maps give the land's shadow beyond them, and they are read with a 3 x 3 tent of bilinear comparisons. |
| **Contact** | Fixed-step screen-space marches with a receiver-plane test, so a surface never shadows itself. |
| **Policy** | 16 slots ranked by importance with hysteresis. Slots and maps fade over 0.5 s. |

Read `docs/design.md` for the design, the contract (section 1), the limits and the budget.

## Using it

1. Resolve `wxl.graphics-shadow` lazily (extensions load in folder order).
2. Call `Want(WXL_GFX_SHADOW_WANT_MASKS | WXL_GFX_SHADOW_WANT_POINTS)` from your wants callback on
   every frame you read shadows.
3. Order your compute pass after `WXL_GFX_SHADOW_ORDER` (90). In its record:
   - `GetFrame` gives the masks and slots;
   - `WriteBindings(set, first)` fills the binding set that `shadow.hlsli` declares from
     `WXL_SHADOW_BINDING` on.
4. Compile your shaders with `-I <wxl-core>/extensions/wxl-graphics-shadow/shaders`.

Positions handed to the include are camera-relative.

wxl-graphics-lights hands its lights in with `SetLights` (id, rest position, radius, cone, source
size, importance, carried). Without it, the service reads lights' published list itself.

## Requirements

- DXVK and wxl-graphics-extend.
- `extShadowQuality` 1 or more, for the cascades and the omni maps (the engine's caster lists).
- For terrain shadows, the baked horizon maps under `Textures\Forever\Horizon` (5.tools/forever-bake).

## Settings and panel

The settings are `WXL_GFX_SHADOW_*` in `Extensions\wxl-graphics-shadow\wxl-graphics-shadow.cfg`;
`wxl-graphics-shadow.cfg.example` lists every one with its default.

The panel is "Graphics Shadow". It has:
- status, slots and the GPU time of each pass;
- every setting with a "(?)";
- the isolates for A/B;
- the debug views: sun, moon, one slot, every slot, contact, terrain, cascades, capsules, the map
  atlas, normals.

## Building

The compute shaders compile offline with DXC (`deps/dxc`) into `src/gpu/Spirv.gen.cpp`, which is
committed, so a build needs no shader compiler. The D3D9 shaders (the debug overlay, the terrain
caster) are compiled at runtime by wxl-graphics-extend from the sources embedded in the same file.

## License

GPL-3.0-or-later.
