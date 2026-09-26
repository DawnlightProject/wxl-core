# wxl-graphics-lights: design

This extension owns all light in the render stack:
- the sources: lamps, torches, fires, candles, and the sun, the moon and the sky as light definitions;
- their identity, colour temperature, intensity, cookies and rooms;
- the lighting of surfaces, deferred, in HDR, in Vulkan compute;
- a lamp light field for the air, which the fog samples.

The medium belongs to wxl-graphics-fog, the shadows to wxl-graphics-shadow, the scene data to
wxl-graphics-extend, and the final exposure, bloom and tonemap to wxl-graphics-post (to come).

The previous version is archived at `archive/wxl-graphics-lights-surface-v1/`. What is kept from it
and why is listed in section 11.

## 1. The diagnosis this design answers

The previous surface light was added onto the engine's final image, which is display-referred and
already clipped at 1. A strong lamp reached white at once, a weak one vanished under the soft
shoulder that stopped the first, and no single tuning worked for both. The rest of the complaints
follow from the same root or from sampling:

| Complaint | Cause | Answer here |
|---|---|---|
| Saturated orange-red mines | A flame's linear colour, clipped per channel in LDR | Kelvin colours with chromatic adaptation, light added in linear HDR, one per-channel shoulder that turns a bright flame yellow-white |
| Painted red discs with hard edges | A falloff normalised to a plateau, a window that cut early, a near cap | Inverse square with a soft core and a smooth window, no cap, no plateau |
| Stripes and dotted patterns | Checkerboard half resolution, contact shadow marches, unfiltered cookie bars | Full-resolution lighting, no screen-space marching here, cookies prefiltered by footprint |
| Flicker when the camera moves | Per-cluster cuts, lists sorted by a camera-dependent score, aliased cookies | Complete cluster lists in id order, importance fades, footprint prefiltering, froxels integrated along depth |
| Weak elsewhere | The LDR shoulder ate faint light | No toe: faint light on a dark surface adds linearly |

## 2. HDR

### The chain

1. **World.** The world draws into graphics-extend's FP16 colour target (`WXL_GFX_NEED_HDR`),
   requested by this extension's composite while it runs.
2. **Composite** (D3D9, `WXL_GFX_ORDER_LIGHTING`). Each pixel of the engine's image is decoded to
   linear and **expanded** into scene-referred HDR by the inverse of the resolve curve. The lamps'
   light is added there, in linear. The result is re-encoded and written back to the FP16 target.
3. **The fog and effects** (orders 200, 250) draw on that target.
4. **Resolve** (D3D9, `WXL_GFX_ORDER_RESOLVE` = 350). One tonemap with exposure writes the back
   buffer and sets `*colorResolved`. wxl-graphics-post will own this step (section 2.4).

### The curve

Per channel, in linear light, with a knee `k` (default 0.6):

```
T(x)    = x                                       for x <= k
T(x)    = k + (1 - k) (1 - exp(-(x - k) / (1 - k))) for x > k
T^-1(y) = y                                       for y <= k
T^-1(y) = k - (1 - k) ln(1 - (y - k) / (1 - k))     for k < y < 1
```

- **C1 and invertible.** Its slope is 1 at the knee and it has no toe, so darks stay exactly as the
  engine drew them.
- **Unlit pixels round-trip.** The expand is `T^-1`, so a pixel no lamp touches comes out of the
  resolve exactly as the engine drew it. The look of the game is unchanged where there is no light.
- **The top of the range.** Engine values at or past 1 (sky whites, additive glows stacking in FP16)
  expand to `T^-1(1 - 1/512)` (about 3.5 at k = 0.6), plus four times any excess past 1. That keeps
  their energy for bloom later.
- **Per channel.** A bright orange pool desaturates towards yellow-white as its red channel meets the
  shoulder first, which is what film and the eye do. This one choice removes the "orange-red mine".
- **Exposure.** The resolve computes `T(exposure * x)`. At 1, the default, unlit pixels are
  untouched. Other values darken or brighten the whole frame, engine included, as a real exposure
  does.

### Encoding between passes

The FP16 target holds `pow(x, 1 / 2.2)` of the linear scene-referred `x`. Values may exceed 1.
- Below the knee this is exactly the engine's own encoding, so the current fog's alpha blends behave
  as before.
- The gamma (2.2) and the knee are published (section 8.4) so every later pass decodes the same way.
- A pass that wants linear light decodes with `pow(c, 2.2)`.

### Resolve ownership

`wxl.graphics-lights.resolve` v1 (section 8.4) has `ClaimResolve(owner)`. wxl-graphics-post calls it
once, and the interim resolve then stands down for good. The curve and gamma post must honour for
unlit pixels to stay unchanged are published beside it.

The resolve also stands down on any frame where:
- `*colorResolved` is already set by a pass ordered before it;
- the composite did not expand the target that frame. The core's plain copy then shows the engine's
  image unchanged.

### Without HDR

With `WXL_GFX_LIGHTS_HDR=0`, an MSAA world, or a device without FP16 targets, the composite tonemaps
in place: it writes `encode(T(x))` with dither straight into the 8-bit target. The fog then draws
over tonemapped values, as it always did.

### graphics-extend changes (additive)

- `WXL_GFX_ORDER_RESOLVE` (350) in `GraphicsExtendApi.h`.
- The scheduler moves render target 0 (and `WXL_GfxFrame::target`) to the back buffer for the
  remaining passes once a pass has set `*colorResolved`. Without this, an overlay pass at 900 would
  draw into the FP16 target after the resolve, and its drawing would be lost.

## 3. The light model

### Families

Every light belongs to a family, which gives it:
- a colour temperature, or a tint;
- a luminous intensity `I`;
- a source radius `r0`;
- a flicker;
- an angular profile.

| Family | Kelvin | I (scene units at 1 yd) | r0 (yd) | Flicker | Profile |
|---|---|---|---|---|---|
| candle | 1850 | 0.8 | 0.03 | candle | flame |
| chandelier | 1950 | 5 | 0.30 | candle | none |
| torch | 2000 | 5 | 0.12 | fire | flame |
| fire | 1950 | 5 | 0.30 | fire | flame |
| campfire | 1900 | 10 | 0.50 | fire | flame |
| brazier | 1950 | 8 | 0.35 | fire | flame |
| hearth | 1900 | 7 | 0.30 | fire | flame |
| lantern | 2700 | 3.5 | 0.04 | lantern | cage |
| streetlamp | 2500 | 10 | 0.06 | lantern | downlight |
| walllight | 2600 | 4 | 0.05 | lantern | grille |
| greenlamp | tint (0.45, 1, 0.72) | 6 | 0.04 | lantern | cage |
| engine fire (a warm M2 light) | 2000 | from the engine | 0.25 | fire | flame |
| engine lamp (a warm WMO light) | 2700 | from the engine | 0.10 | lantern | none |
| tint (any cold or coloured light) | its own hue | from its source | 0.10 | its source's | its source's |

**Scene units.** A scene-referred linear value of 1 is the engine's white. `I` is the irradiance at
1 yd on a surface facing the light. A surface of albedo `a` there reflects `a * I`, before the
tonemap. At night the engine's own lighting sits around 0.02 to 0.08. A torch therefore gives about
1.2 at 2 yd and fades into the night at about 10 yd.

### Colour

- **Kelvin.** The colour is the Planckian locus at the family's temperature (the Kim et al. cubic
  fit of CIE 1931 xy), taken to linear sRGB and normalised to luminance 1.
- **Chromatic adaptation.** "Warmth adaptation" (default 0.25) moves that colour towards the
  luminance-matched white, as the eye does under warm light.
- **Tints.** A table row or engine light whose colour is not warm (magic blue, fel green, arcane
  purple) keeps its own hue as a tint, decoded to linear and normalised to luminance 1.
- **The legacy texture.** The fog's current light texture takes the same chroma at the luminance it
  has always had, so its halos match the surfaces' hue without being retuned.

### Falloff

```
E(d) = I * window(d / R) / (d^2 + r0^2)
window(x) = saturate(1 - x^4)^2
```

- **Soft core.** `r0` keeps a light finite at its source. A carried light takes at least the
  carried core (1 yd by default), so its carrier never flares.
- **The window.** It stays near 1 until about 0.6 R and reaches 0 with a zero slope, so a pool has no
  edge.
- **The reach.** `R = clamp(sqrt(I / cutoff), 4, maxRadius)`, with the cutoff default 0.01. It
  follows the light's brightness, not an authored radius, so a weak light is never stretched and a
  strong one is never cut early.
- **No cap, no plateau.** Nothing clamps the light near its source. The tonemap's shoulder handles
  it.

### Shape

- **Spots.** A smooth cone between `cosOuter` and `cosInner`.
- **Profiles.** The analytic profiles of the previous version (cage, downlight, flame, grille). The
  downlight is the street lamp's IES-like shape: a lobe down and out, 30 % up.
- **Cookies.** The baked cube cookies, kept and pushed further (section 5).
- **Tubes.** Windows and long fixtures use the closest point of their segment.

### Flicker

- Seeded by the light's id and evaluated from one clock, so every consumer sees the same flicker.
- Fire breathes and gutters. A candle trembles fast and small. A lantern barely moves.
- A fire's position jitters by up to 3 cm. The shadow service renders from `rest`, the position
  before the jitter. The jitter and the gain are published per light (`flickerOffset`,
  `flickerGain`), so a consumer can move its shadow lookup with the flame.

### Emissive heads

A surface within twice the family's source radius of a source (at least 5 cm, at most 40 cm)
receives an emissive term, whatever its normal, with a smooth radial falloff. Its luminance is
`I / (4 r^2)` with `r` half that radius, at most 400. That is the glass or wick of the lamp itself; a
carried light's wider soft core does not widen it, so the hand holding a torch does not glow. It is
published per light as `emissive` so bloom can find it later.

## 4. Gather, identity, budget

The gather of the previous version is kept as proven:
- the engine's M2 lights and WMO MOLT lights (core `wxl::game::lights`);
- the model table for models that carry none (`data/model-lights.csv`);
- fixture merging (table, then M2, then WMO wins, with a pair memory);
- `LightId` from the owner, the index and the kind;
- the list sorted by id.

**The budget.** At most 128 lights, the capacity of the legacy textures. Lights are ranked by
importance: brightness over distance, with a 25 % bonus for a light already chosen. A light that
leaves the chosen set fades out over 1 s, and a newcomer fades in, so a light never pops. Cluster
lists are complete: a light is never dropped from a pixel because another is brighter.

**Clusters.** 16 x 9 x 16 over the camera frustum, exponential from 0.5 to 250 yd, one texel per
cluster, variable-length lists in one pool, with no cap. The legacy layout is unchanged. A light is
assigned by the larger of its legacy radius and its HDR reach, so both families of consumers find it.

## 5. Cookies

Kept whole:
- baked cube-map cookies under `Textures\Forever\Cookies`, manifest format 2, from `5.tools/forever-bake`;
- tinted glass;
- the streamed atlas with its mean and tint from the manifest;
- a 0.5 s fade of the pattern over its mean.

Pushed further:
- **Footprint prefiltering.** The atlas has no mips. Where a pixel (or a froxel) covers more of the
  cookie than one texel, as seen from the lamp, the pattern blends towards its mean in proportion.
  Distant walls no longer shimmer with aliased bars, and the pattern stays sharp up close.
- **Cookies shape the air.** The field reads them, prefiltered the same way, so bars cut beams
  through the mist.
- **Bake coverage.** The cookie of a row is found by model and light index. The panel's Cookies tab
  counts the lights of the frame that have one, so missing bakes show.

## 6. Surfaces (Vulkan compute, full resolution)

**The compute pass `lights`** (`WXL_GFX_ORDER_LIGHTING`, after the shadow service's 90) records the
field first, then the surfaces: one thread per pixel, 8 x 8 groups. The surfaces read:
- the INTZ depth, RT1 normals and RT2 albedo with its material code;
- the HDR source texture (section 8.2) and the legacy cluster texture;
- the cookie atlas;
- the shadow masks (`GetFrame`).

Per pixel:
- **Surface.** Camera-relative position; world normal; albedo decoded from the engine's gamma; the
  material's kind and gloss. Roughness is 0.8 for terrain (down to 0.35 where its gloss is high),
  0.6 for buildings and 0.7 for models. Wetness lowers it on upward-facing outdoor surfaces.
- **Each light of the cluster's list:** window, soft core, cone, profile, cookie, room gate, the
  shadow slot's mask value, fog dimming (`exp(-k * extinction * d)`, below) and the share the engine
  does not already light (below). A flame's hot core whitens its light within three glow radii of
  the source (`max(3 * emissiveRadius, 0.3)` yd): the lamp head's own size, never a carried light's
  1 yd soft core, which had whitened six yards around every torch in hand.
- **Diffuse.** Burley, normalised like Lambert, energy-conserving with the specular: `(1 - F)`.
- **Specular.** GGX with height-correlated Smith visibility and Schlick Fresnel (F0 0.04).
- **Emissive heads** (section 3).

**Not twice what the engine lights.** The engine already lights terrain, buildings and models with
its own M2 lights (the carried torch, braziers, campfires), and a WMO light is baked into its
building's interior vertex colours. Added at full strength on top, either showed as a second pool on
the first. Each light's direct light and bounce are scaled by

```
keep  = engine light (WXL_GFX_LIGHT_SOURCE_ENGINE) ? engineKeep : 1           (default 0.5)
keep *= lerp(1, bakedKeep, roomW)   for a WMO light (..._BAKED) on a building pixel (default 0.35)
```

with `roomW` how much the pixel is inside a room: the bake is only on the building's interior. The
air is not scaled: the engine lights no fog.

**Bounce.** Without it, the back and the shadowed side of anything near a lamp stayed at the
engine's night level (about 0.04) while the pool reached 1 to 3, and pools looked pasted onto the
scene. Each light also adds, before the facing test and without its shadow slot (the bounce comes
from the lit surfaces around the pixel, not from the lamp):

```
core    = max(softRadius, bounceCore)                                          (default 1.5 yd)
bounce += I * mean(cookie) * window(d / R) / (d^2 + core^2) * cone * gate * thin * keep
            * (0.75 + 0.25 NdotL)
diffuse += share * bounce                                                      (default 0.15)
```

The wide core keeps it from peaking at the source; `mean(cookie)` is the cookie's mean colour, so
tinted glass tints its bounce and a cage takes its share away; a surface facing away gets half.

**Local adaptation.** Where lamp light far outshines the engine's own, the eye adapts to it and its
tint fades a little (von Kries), so a torch-lit wall reads warm white rather than orange while the
dim edge of the pool keeps its colour:

```
Le       = lerp(decode(saturate(amb + dif * saturate(towardsLight.z))), 0.1, roomW)
r        = luma(diffuse) / max(Le, 0.02)
a        = strength * r / (1 + r)                                              (default 0.4)
diffuse  = lerp(diffuse, luma(diffuse), a),  and the same for the specular
```

`amb` and `dif` are the lumas of the engine's ambient and diffuse (gamma); inside rooms, where the
engine lights with its own interior light, a fixed level stands in.

**Fog dimming.** The fog probes its extinction at the eye (seven samples, the eye and a yard either
side of it), read back a few frames late. Used as it came, a third-person orbit carried the eye
through thicker and thinner patches and every lamp pulsed with it. The extinction the lamps see is
eased once a frame towards the probe (an exponential with a 1 s time constant, frame-rate
independent) and capped at 0.03 per yard, a haze that still leaves three quarters of a lamp at
10 yd: the probe is not taken between the lamp and what it lights, and the fog already dims the way
to the eye. The surfaces and the field read the same value (`FogExtinction` in
`src/core/Extension.hpp`), times their own share.

It writes one RGBA16F image: rgb the added radiance (albedo times diffuse, plus specular and
emission), a the sun and moon factor (section 7). A copy goes to a D3D9 texture for the composite.

**The composite** (ps_3_0, D3D9, same order):
```
scene_lin = decode(scene * sunFactor)
x         = T^-1(scene_lin) + added + halo
out       = encode(x)                 (HDR)
out       = encode(T(x)) + dither     (LDR fallback)
```
The debug views are drawn here too.

**Stability.**
- Nothing is jittered or reprojected, and there is no history.
- Every light in the cluster's list is evaluated fully: no seams.
- Cookies are prefiltered by footprint.
- The list order is by id, and every change fades.
- The fog's dimming follows the fog, not the camera: it is eased over about a second.

## 7. Sun, moon and sky

Published each frame (`wxl.graphics-lights.sources`, `Sky`) from `wxl::game::sky`:
- the directions to the sun and the moon, and the engine's lighting direction;
- the sun's and the moon's colour and illuminance (the engine's diffuse, linearised), with their
  weights by elevation;
- the engine's ambient, the sky's zenith and horizon, and the fog colour, all linear;
- a day factor;
- a weather value, 0 until the core has a weather binding (section 12).

The fog and post read the same values.

**On surfaces.** The engine's own sun term stays. Only the shadow the engine lacks is added, as a
ratio in the engine's own terms:

```
V      = the shadow service's sun visibility (moon by night, weighted by their weights)
c      = V / (horizon * contact)        (the cascades' share of V)
engine = 0.7 + 0.3 c                    (what the engine itself applied)
want   = horizon * contact * lerp(engine, c, deepen)
ratio  = (amb + dif NdotL want) / (amb + dif NdotL engine)
```

- The ratio is exactly 1 where nothing more is shadowed, so the engine's sun is never doubled.
- Pixels inside a room are left alone: the engine lights interiors with its own interior light.

## 8. APIs

### 8.1 `wxl.graphics-lights` v1 (unchanged)

`GraphicsLightsApi.h`: the light list, the legacy light, cluster, omni and cookie textures, rooms,
and demand. Its layouts and units do not change; the colours take the family chroma at their old
luminance.

- **The omni table.** While wxl-graphics-shadow is loaded, it owns the core's omni maps. This
  extension then only reads them back, matching each by the id the shadow service hands the core (the
  same `LightId`). Without it, the previous chooser runs as before.
- **`wxl.graphics-lights.surface` v1** is still published, but its `Outputs` returns 0: the
  half-resolution grid it described no longer exists.

### 8.2 `wxl.graphics-lights.sources` v1 (new)

`GraphicsLightsSourcesApi.h`:
- `Sources(count, frame)`: the HDR light list, index for index the same as `Current`;
- `SourceTexture()`: the same list as `WXL_GFX_LIGHTS_MAX x WXL_GFX_LIGHT_SOURCE_ROWS` RGBA32F
  texels, D3D9 DEFAULT pool, importable into Vulkan, with the layout of
  `shaders/wxl/lights/sources.hlsli`;
- `Sky(out)`: section 7;
- `FamilyName(family)`.

### 8.3 `wxl.graphics-lights.field` v1 (new): the lamp light for the air

`GraphicsLightsFieldApi.h` and `shaders/wxl/lights/field.hlsli`.

**Demand and timing.**
- Call `Want()` from your wants callback. The field is computed in this extension's compute pass at
  `WXL_GFX_ORDER_LIGHTING`.
- A compute pass ordered after it in the same block calls `Get(out)` and binds the images. They are
  in GENERAL layout.
- `Get` returns 0 on a frame the field was not computed.

**The grid.**
- Camera-frustum froxels, 160 x 90 x 64 by default.
- x and y follow the screen uv of the scheduler's D3D-form `viewProjRel` (y down).
- z is exponential in Euclidean distance from the eye: `slice01 = log(dist / near) / log(far /
  near)`, near 0.5 yd, far 250 yd.
- The grid subdivides the light clusters exactly (10 x 10 x 4 froxels a cluster).
- Sample with a linear clamp sampler at `(u, v, slice01)`; `field.hlsli` has
  `WxlLightsFieldUvw(r, constants)`.

**The images** (RGBA16F 3D):

| Image | rgb | a |
|---|---|---|
| `inscatter` | lamp radiance scattered towards the eye per unit scattering coefficient, with the phase set by `SetPhase` | luminance of the isotropic part |
| `ambient` | the same with the isotropic phase `1 / 4 pi` | directionality 0..1: the length of the flux-weighted mean direction |
| `direction` | the flux-weighted mean propagation direction (unit, world axes) | total luminance flux |

**The phase.** `SetPhase(gForward, gBack, blend)` sets a dual Henyey-Greenstein phase, used from the
next frame on. The default is 0.6, -0.2, 0.25. A consumer with another phase uses `ambient` and
`direction` as a two-lobe model:

```
L(view) ~= ambient.rgb * lerp(1, 4 pi p(dot(direction, view)), ambient.a)
```

**What the values mean.** The fog multiplies by its local scattering coefficient `sigma_s` (1/yd) and
by the transmittance to the eye. The field does not include the transmittance to the eye; the fog's
march already has it.

**What is in a froxel.** For each light of the froxel's cluster:
- the analytic integral of `I / (h^2 + t^2)` along the froxel's depth span (closed form, `atan`),
  averaged over the span. `h^2` is the ray's closest approach to the light squared, plus its soft
  core squared, plus a quarter of the froxel's width squared: a column is evaluated along its centre
  ray only, and a core narrower than the column (a tenth of a yard against up to half a yard at a
  distance) made a lamp's halo peak swing two to four times over as it slid between columns;
- window, cone, profile and hot core (within three glow radii of the source, section 6), at the
  closest point of the span to the light;
- the cookie, prefiltered by the froxel's footprint seen from the lamp;
- the room gate (section 9);
- the shadow service's point lookup (`shadow.hlsli`), when it is loaded;
- a thinning by the medium between the lamp and the froxel, `exp(-extinction * d)`, with the fog's
  camera density eased over about a second and capped at 0.03 per yard (section 6). This stands in
  until the fog provides its own transmittance.

The depth integral makes the field smooth along depth whatever the froxel size, and the core widened
to the froxel's width does the same across it, so a lamp's core never flickers as the grid slides.
There is no temporal history in the field; the fog's march keeps its own.

**Without the fog.** When the fog is not active, the composite adds a small analytic halo. The
field's `inscatter` is integrated front to back into a fourth image (internal) with a constant thin
haze (`WXL_GFX_LIGHTS_HALO_DENSITY`), and each pixel reads it at its depth.

### 8.4 `wxl.graphics-lights.resolve` v1 (new)

`GraphicsLightsResolveApi.h` and `shaders/wxl/lights/hdr.hlsli`:

| Call | What it does |
|---|---|
| `Scene(out)` | This frame: whether the world is HDR, whether it was expanded, the gamma, the knee, the exposure, and who resolved |
| `ClaimResolve(owner)` | A later pass (post) takes the resolve for the process. The interim resolve stands down and never runs again |
| `Status()` | One line for a panel |

`hdr.hlsli` holds `WxlHdrCurve`, `WxlHdrCurveInverse`, `WxlHdrDecode` and `WxlHdrEncode`.

## 9. Rooms

Kept from the previous version:
- rooms from every placed WMO within reach, drawn or not;
- stable identity (the placement and its group);
- hysteresis (joining within 45 yd, leaving past 65 yd);
- a 0.3 s fade per room;
- published smallest first.

Added: a light's gate changes smoothly.
- Each light remembers its room by identity, not by index.
- When its room changes (a carried torch through a doorway), its gate eases to open over 0.15 s,
  the room switches, and the gate eases back over 0.15 s.
- The published gate weight also carries the fade of the light's own room, so a room joining or
  leaving the working set eases the gate too.
- A pixel blends two models by that weight: an outdoor light (full outdoors, the outdoor-indoors leak
  in a room) and a room light (its rooms fully, other floors by the floor leak, outdoors the
  indoor-outdoors leak). Each model is weighted by the pixel's room fade. Neither the light's gate nor
  a pixel's gate ever flips.
- Outdoor light reaching into a room and room light reaching outdoors keep their leak factors.

Where a point is (surfaces and froxels): soft, never a flip at a box face. A group's box only
approximates its room: it cuts through doorways and open halls, and reaches over the ground around a
hut. A hard in-or-out test drew those faces as straight edges and rectangles on floors and grass.
- **Membership per room**, 0..1: it ramps up across the box's 0.3 yd padding, so a point at the
  group's own bounds is fully in and the membership eases to nothing just outside them. Floors and
  ceilings stay sharp: they are real surfaces.
- **A wall's outside.** A surface within 0.8 yd of a box face and facing out of the box is the outside
  of that wall, not the room.
- **Terrain is never in a room.** The ground is not drawn indoors.
- **Scaled by the room's own fade.** A room joining or leaving the working set eases every point.
- **Any room of the light's mask.** A room light is full in whichever of its rooms holds the point
  most, not only in the smallest box around it. The two models above take these memberships in place
  of a single room.

Where a light is: within its group's own bounds, not the padding. A lantern standing just outside a
wall is an outdoor light, not a light of the room behind the wall.

## 10. UI and settings

The panel "Graphics Lights" has these tabs:
- **Light:** HDR, exposure, knee, lamp gain, warmth adaptation, cutoff, flicker, per-family intensity;
- **Surfaces:** diffuse model, specular, roughness, wetness, lamp heads, sun shadow deepening, fog
  dimming, room leaks, cookie prefilter;
- **Air:** the field, its shadows and thinning, the phase, the halo without fog;
- **Gather:** sources, radius, merge, room leak;
- **Cookies;**
- **Shadow maps:** the legacy omni table;
- **Debug:** GPU timers per span, views, isolates, markers, self-check.

Every control has a "(?)" tooltip. Every setting is a `WXL_GFX_LIGHTS_*` key in
`wxl-graphics-lights.cfg.example`.

**Debug views:** albedo, normals, material, light only, cluster heat, shadow slots, sun factor,
cookie factor, rooms, and field slices (a depth slice of the field over the screen, or the field
integrated along each pixel's ray).

**Isolates:** no cookies, no shadows, no specular, no room gate, no fog dimming, no sun factor,
white albedo, no emissive, no prefilter.

## 11. Kept from v1, and why

| Kept | Why |
|---|---|
| Gather, model table, fixture merge, `LightId` | Proven; the id space the shadow service keys on |
| Rooms (placements, identities, hysteresis, fades) | Proven view-independent rooms |
| Cookies (atlas, manifest, means, fades) | "Added something real" |
| Legacy textures and `GraphicsLightsApi.h` | The current fog reads them |
| Omni chooser with an ownership rule | The fog's legacy omni table, while the shadow service does not own the maps |
| `tools/bls_gbuffer.py` | Writes the G-buffer the surfaces read |

Not kept:
- the half-resolution checkerboard grid;
- contact shadows, the sun cascades, the horizon, and the sun adjuster (all now wxl-graphics-shadow);
- the screen-space bounce;
- the LDR composite's white point and saturation hacks.

## 12. Cost (1080p, RTX 3060, estimates; the user measures in game)

| Pass | Estimate |
|---|---|
| CPU: gather, clusters, uploads | 0.3-0.6 ms CPU |
| Surfaces (full res, 4-10 lights a pixel, cookies, masks) | 0.5-0.9 ms |
| Copy into D3D9 and composite (StretchRect and a full-screen pass) | 0.12-0.18 ms |
| Field (160 x 90 x 64, lights of each cluster, analytic integral) | 0.15-0.35 ms |
| Halo integration (without the fog only) | 0.02 ms |
| Resolve | 0.05 ms |
| **Total** | **0.85-1.5 ms**, inside the 1.5-2 ms target |

GPU memory: the field is 3 x 7.4 MB (4 x with the halo), the surface image and its D3D9 copy
2 x 16.6 MB, and the scene copy 16.6 MB. About 80 MB at 1080p.

**Wanted from the core** (asked, not written here): a weather binding, `wxl::game::weather`, with
storm intensity and kind (rain, snow, sand). The fields exist in `offsets/game/Weather.hpp`. The
sky definition would then carry the weather rather than 0.
