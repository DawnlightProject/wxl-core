# wxl-graphics-shadow: design

`wxl-graphics-shadow` owns every shadow of the render stack:
- the lamps' shadow maps, omni and spot;
- body shadows for lamps without a map;
- the sun and the moon;
- contact shadows;
- the terrain.

It hands them out in two forms: screen-space masks for surfaces, and a binding set with an HLSL
include for any point, such as fog froxels or a lamp field. Without it, everything is unshadowed.

| Module | Owns |
|---|---|
| wxl-graphics-extend | Scene data: depth, G-buffer, HDR, the Vulkan scheduler, assets |
| **wxl-graphics-shadow** | **Every shadow** |
| wxl-graphics-lights | Light: sources, cookies, rooms, the lighting of surfaces and of the air |
| wxl-graphics-fog | The medium |

## 0. What went wrong before, and what this does instead

The previous shadows lived in wxl-graphics-lights' surface lighting
(`archive/wxl-graphics-lights-surface-v1`); `extensions/wxl-forever/docs/lighting-audit.md` holds
their history.

| Symptom | Cause | This design |
|---|---|---|
| Acne stripes | A fixed receiver bias against maps whose texel grows with distance; contact shadows compared against a fixed 3 cm | Normal offset in texels of the map at the receiver, slope-scaled; EVSM for the lamps (no depth compare to go wrong); the contact march rejects samples on the receiver's own plane |
| Dotted checkerboard | Half-resolution lighting on a checkerboard of near and far samples; PCSS discs turned per pixel and never filtered | Masks traced at full resolution by default; the half-resolution option upsamples with a joint-bilateral filter. No random rotation anywhere: every filter is a fixed, dense kernel |
| Flicker when the camera moves | Per-pixel noise tied to the screen; slots handed over at once | Every filter lives in the map's texel space, which is fixed to the world; slots and maps fade over half a second |
| Shadows depend on the light's origin, not the beam | A receiver skip sphere around the light; capsule tests against the light's position only | Capsules are tested along the whole segment from the receiver to the light, with a penumbra that widens along it. A carrier is skipped by its own capsule volume, not by a sphere around its torch |
| No shadow from a carried torch | Carried lights shared the four slots with every lamp | A carried light always gets a combined map when it matters (the player's own torch first), with the torch and hand excluded as housing |

## 1. Contract (for wxl-graphics-lights and wxl-graphics-fog)

The API is `include/wxl/GraphicsShadowApi.h`, published as `wxl.graphics-shadow` v1. The HLSL
include is `extensions/wxl-graphics-shadow/shaders/wxl/shadow/shadow.hlsli`, with its layout in
`layout.h` beside it.

### 1.1 Absent means lit

Every consumer must work without this service. Everything is fully lit when:
- the interface is missing;
- DXVK is not there;
- `GetFrame` returns 0.

The include returns 1 everywhere while the uniform block says "off", and `WriteBindings` writes that
block whenever the service did not run.

### 1.2 One identity

A slot is keyed by the light id wxl-graphics-lights publishes (`WXL_GfxLight::id`). The surfaces,
the fog and this service share that id space:
- `SlotOf(id)` gives a light's slot on the CPU.
- `ShadowSlotOf(listIndex)` gives it in a shader, by the index of lights' list of this frame.

A slot number stays the same while its light keeps it.

### 1.3 The frame

| When | Who | What |
|---|---|---|
| Scheduler begin phase, order -1000000 | lights | Publishes its list, then calls `SetLights` with its shadow candidates |
| Scheduler begin phase, order 90 | shadow | Chooses slots and maps, and hands the lights to the core's omni maps |
| World pass | core | Renders the cascades (with the terrain caster) and the omni maps |
| Compute block, order 90 | shadow | Filters the maps, writes the uniform block and the masks |
| Compute block, order 100 and up | lights, fog | `GetFrame` and `WriteBindings`, then their own dispatches |

A consumer calls `Want(WXL_GFX_SHADOW_WANT_MASKS)` or `Want(WXL_GFX_SHADOW_WANT_POINTS)` from its
wants callback on every frame it reads shadows. None of this service's own work (slots, maps, masks)
runs on a frame nobody wanted. What improves the engine's own shadows always applies while the
service is enabled: the cascades follow a lower, truer sun (or the moon), and the land casts into
them.

### 1.4 The lights, handed in

`SetLights` takes, per light:
- its id and its resting position (before the flicker);
- its radius, spot axis and cone, and source size;
- its importance: lights' own ranking, larger first;
- flags: carried, capsules only, or never shadowed;
- its list index, and the carrier's GUID when known.

Without a call for two frames, the service reads `WXL_GraphicsLightsApi::Current` itself and ranks
by brightness over distance. So lights may integrate at its own pace.

### 1.5 Surfaces: the masks

`GetFrame` returns a `VK_IMAGE_TYPE_3D`, `R8G8B8A8_UNORM` image of the world's size with 5 layers,
in GENERAL. It is always full resolution. Value 1 is lit, and every value already carries its fade.

| Layer | x | y | z | w |
|---|---|---|---|---|
| 0 | sun (horizon x cascades x contact) | moon (the same) | terrain horizon alone, towards the body that shines | contact alone, towards it |
| 1..4 | slot 4(l-1) | slot 4(l-1)+1 | slot 4(l-1)+2 | slot 4(l-1)+3 |

The engine already applies its own cascades to what it draws. A consumer that relights with its own
sun term uses x or y. A consumer that only corrects the engine's image uses z and w.

### 1.6 Any point: the binding set

On the host:
1. Append `DescribeBindings(first, ...)` to the pipeline's set-0 bindings.
2. Call `WriteBindings(set, first)` on every set you allocate for that pipeline.

In the shader:
```hlsl
#define WXL_SHADOW_BINDING 40
#include "wxl/shadow/shadow.hlsli"
float lamp = ShadowLight(ShadowSlotOf(lightIndex), pos);   // air: one filtered fetch
float sun  = ShadowSun(pos);                               // air: one gather
float body = ShadowBodies(pos, lampPos, sourceSize);       // capsules along the beam
```
Compile with `-I <wxl-core>/extensions/wxl-graphics-shadow/shaders`. Positions are camera-relative.

### 1.7 Who drives what

Only one party may drive the core's omni maps and the sun's shadow direction. From this version:
- `wxl.omnishadows` v3 and `wxl.shadowlight` v2 gain a `Claim`. While this service holds the claim,
  other callers of `SetLights`, `SetLightsEx`, `SetBudget`, `SetFaceSize` and `SetAdjust` are
  accepted and ignored.
- wxl-graphics-lights' read-back (`Count`, `Get`, `GetState` matched by id) keeps working. The first
  core slots hold each mapped light's static (or combined) map under the light's own id. That is how
  the fog's current omni path keeps its shadows until it moves onto this API.
- wxl-graphics-lights stops choosing omni slots when `wxl.graphics-shadow` is published, and should
  not install the sun adjuster then. The claims make either harmless if it still does.

## 2. Light shadow maps

### 2.1 The core, extended (`wxl.omnishadows` v3)

The v2 path re-issues the engine's sun-map caster lists from each chosen light, one face per atlas
cell. That covers units, doodads and WMOs within about 40 yd of the player. v3 adds, generically:

| Addition | What it does |
|---|---|
| 12 slots | v1 and v2 callers still see and set 4 |
| Caster classes | Per light: static (WMOs and models that belong to no unit), dynamic (units and everything attached to them), or both. The M2 batch lists are filtered by the model at the root of each entry's attachment chain. |
| Redraw occupied | A dynamic light redraws every frame the faces that see a unit, so animation shows. A face its last unit left is redrawn once, empty. |
| Face mask | Faces a light never renders (a spot's back faces) stay lit and cost nothing |
| Face size per light | 64..1024, with one shared depth atlas per size in use |
| Deferred release | An atlas replaced by a size change is released four frames later, so a consumer that read its pointer before the world pass never imports a freed texture |
| Claim | Exclusive driving, as above |

The M2 batch list format was read from the export:
- `CM2Model::RenderModelBatchesShadowMap` (0x0082DA40) draws the opaque list, then the alpha list.
- `CM2Model::RenderModelBatchListShadowMap` (0x00829E40) walks 12-byte entries
  `{CM2Model*, batch, group}` through `list[0]` (array) and `list[1]` (count).
- `CM2Model::RenderBatchShadowMap` (0x00829BA0) reads the same entries.

A filtered copy with a group of 1 per entry takes the plain, non-instanced path. An entry is dynamic
when the root of its model's attachment chain (`unit::ModelParent`) is a unit's body model
(`unit::Model`).

### 2.2 Policy

- **Slots.** 16 slots, ranked by importance with hysteresis. A slot is held at least 1 s, and a
  candidate must beat the weakest holder by 25 % to take it. A light gaining or losing a slot fades
  over 0.5 s.
- **Maps.** A subset of the slots gets maps:
  - Static lamps: two core slots each. The static casters are drawn once (redrawn round-robin only
    when the engine's static list changes), and the dynamic casters every frame, only on faces that
    see a unit.
  - Moving lights, carried torches first (the player's own first of all): one combined core slot,
    all six faces each frame.
  - A slot's map share fades over 0.5 s: capsules until the map is filled, then the map.
- **Carried lights.** Housing of 0.3 yd, so the torch and the hand cast nothing. The carrier is found
  by GUID, or as the capsule nearest the light. Receivers inside the carrier's capsule skip that
  light's shadow, so the body is lit by its own torch while its shadow falls on everything else.
- **Spots.** Faces outside the cone are masked out.

### 2.3 Filtering: EVSM

Each mapped light's cube is filtered in Vulkan into one shared atlas (6 x 8 faces, RGBA32F, 5 mips):
1. The static and dynamic core maps are read, the nearest caster taken, and the light's own housing
   dropped.
2. The result is warped to EVSM moments (positive and negative exponents) and averaged 2 x 2 from the
   core's 512 faces into 256.
3. The mips are built per face.
4. Only faces the core redrew this frame are converted.

A lookup is one trilinear fetch plus Chebyshev bounds and a light-bleeding reduction. Its footprint
grows with the source size and the receiver's distance, so penumbrae widen away from the caster.

Why EVSM rather than PCSS:
- It is filterable. A trilinear fetch at any width is pattern-free and stable, with no noise to hide.
- It is ideal for the fog: one fetch per froxel.
- The prefilter happens once per changed face, not per pixel.

What it costs: light bleeding where several casters overlap in depth, handled by the bleeding
reduction (panel), and 64 MB of atlas at 256-texel faces. Contact shadows (section 4) restore the
fine contact EVSM softens.

## 3. Bodies without a map

Capsules come from the core's unit walk:
- the player, NPCs and creatures;
- the 32 nearest the camera within 60 yd;
- feet to head height, radius from height.

Per slot, a mask holds the capsules within the light's reach.

The test runs along the segment from the receiver to the light, not towards the light's origin
alone:
- Take the closest points of that segment and the capsule's axis.
- The occlusion is smoothstep(r - w, r + w, d), where w is the source's footprint at that point along
  the segment.
- It fades out at both ends: behind the receiver and past the light.

Lamps outside the maps still have bodies cutting their beams, on surfaces and in the fog alike.

## 4. Contact shadows

Contact shadows are screen-space and part of the mask pass:
- **Sun and moon:** 10 fixed steps over 1.5 yd.
- **Up to four slots** (the most important): 8 steps over up to 1 yd, never further than half the
  distance to the light.

They stay stable:
- There is no jitter: the steps are fixed and quadratically spaced. Occlusion is a soft ramp on the
  depth difference, so the steps never band.
- **No self-intersection.** A sample is ignored when its depth lies on the receiver's own plane
  (from the G-buffer normal, or from depth where there is none), within a tolerance that grows with
  distance. That plane test, not a fixed thickness, is what removes stripes.
- A sample only occludes within a thickness of 0.6 yd behind the depth buffer, and the result fades
  out towards screen edges and in the distance.

## 5. Sun and moon

- **The direction.** The core's `wxl.shadowlight` adjuster, claimed here, renders the cascades:
  - along the true sun by day and the moon by night, whichever is higher;
  - with the height lifted by a factor (the engine uses 5), then floored at a minimum elevation.
  Long, soft dusk shadows come from a low floor (12 degrees by default).
- **The terrain in the cascades.** wxl-forever's terrain caster is ported. After each of the
  engine's cascade renders (`shadows::ChainAfter`, `DescribeRender`), a heightfield mesh of the
  resident horizon tiles is drawn into the same map, writing what the engine's caster writes, so
  hills shadow objects, the ground and the fog's shafts.
- **Horizon maps** (`Textures\Forever\Horizon`, 16 azimuths, 4 x 4 tiles around the camera) give the
  terrain's shadow at any distance and any sun angle, beyond the cascades too.
- **Reading the cascades.** A 3 x 3 tent of bilinear comparisons (nine gathers):
  - a normal offset and slope bias in texels of the cascade;
  - the tent spacing scaled towards the same width in yards in every cascade, within 0.5 to 2 texels
    (the coarse bands stay sharper per texel than the finest map, never sparser);
  - a blend over the last 15 % of each cascade into the next.
  Dusk widens the tent.
- **Moonlight.** The same, towards the moon, with the cascades rendered along it at night.

## 6. Outputs

See section 1. The masks are traced at full resolution by default (about 0.5 ms at 1080p). Half
resolution traces one pixel of each 2 x 2 quad, always the same one, and upsamples with four
bilinear taps weighted by how close each traced depth lies to the pixel's own.

## 6.1 Limits, known and accepted

- **Bodies are vertical capsules.** A corpse lying on the ground is still a standing capsule for
  lamps without a map. Mapped lamps draw the real body.
- **The static set.** A still lamp's static map is refreshed, in turn, whenever the engine's static
  caster list changes as a set (a hash of its M2 entries and WMO list). Only the M2 entries are
  hashed by content. The WMO part is hashed by its list pointers, so if the engine rebuilds those
  lists every frame, still lamps refresh continuously at the budget rate: the v2 behaviour, no worse.
- **The caster window.** The core's maps hold what the engine's main sun map holds: about 40 yd
  around the player. A lamp far from the player shadows only what lies inside that window.
- **Hardware PCF cascades** (D24X8 maps) are not read. The sun then comes from the terrain horizon
  and contact alone.
- **The fog, until it moves onto this API,** reads each mapped light's static (or combined) core map
  through wxl-graphics-lights' omni table, as before. Bodies in its beams come from its own capsules.

## 7. Settings, panel and debug views

Settings are `WXL_GFX_SHADOW_*` in `Extensions\wxl-graphics-shadow\wxl-graphics-shadow.cfg`; the
defaults and their meaning are in `wxl-graphics-shadow.cfg.example`.

The panel ("Graphics Shadow") has:
- the switches and strengths;
- the isolates for A/B: no maps, no capsules, no contact, no cascades, no terrain;
- the debug views: the map atlas, a slot's mask, the sun mask, the capsules as a heat view, the
  cascade index;
- the GPU time of each pass.

Every control has a "(?)" tooltip.

## 8. Cost (1080p, RTX 3060, estimates; the engine's cascade renders excluded)

| Pass | Estimate |
|---|---|
| Map conversion (only faces the core redrew; a moving light is 6 faces) | 0.02-0.12 ms |
| Mips | 0.01-0.04 ms |
| Mask: sun (9 gathers, horizon), 16 slots (EVSM fetch or capsules, only in reach), contact (sun + 4 slots) | 0.45-0.9 ms |
| Half-resolution option (trace plus upsample) | about 0.3 ms instead |
| Uniform upload, bindings | < 0.01 ms |
| **Total** | **0.5-1.1 ms** |

The core's omni renders are engine draws. With the split, a street of four still lamps costs almost
nothing once drawn: only faces that see a unit are redrawn, and those hold only the units. A moving
light costs six full caster-list draws a frame, as before.

GPU memory:
- the map atlas: 64 MB at 256-texel faces, 16 MB at 128;
- the masks: 5 bytes x 4 per pixel, 41 MB at 1080p;
- the core's atlases: 2-8 MB each.
