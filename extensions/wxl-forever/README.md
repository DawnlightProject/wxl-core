# wxl-forever

A render-feature suite for the client. The shared machinery (the world's depth as a texture, GPU
timers, shader compile, the config and the panel) lives in `src/core`; each feature lives in its
own folder, `src/fog` first. The DLL deploys to `Extensions/wxl-forever`, reads
`Extensions/wxl-forever/wxl-forever.cfg`, and keeps the fog's keys under `WXL_FOG_*`; keys for the
suite as a whole use `WXL_FOREVER_*`. The suite is a render engine: `src/core` holds no feature, and
each feature (the fog is the first) works on its own.

## Shaders

Every program is a file: `src/<feature>/shaders/<stem>.ps.hlsl` (or `.vs.hlsl`), with shared
includes in `src/<feature>/shaders/*.hlsli` and `src/core/shaders`. Includes are written from
`src`, as `#include "fog/shaders/common.hlsli"`. The build (`shared.cmake`) compiles each with fxc
(`/O3`, ps_3_0 or vs_3_0), wraps the bytecode in the client's own shader format with
`tools/bls.py` and deploys `<feature>.<stem>.bls` to the client patch folder in the client's own
layout: `Data/Patch-4.MPQ/Shaders/pixel/ps_3_0/Forever/` and `Shaders/vertex/vs_3_0/Forever/`. At
runtime `core/ShaderLibrary` reads them through the client's file system with `core/Bls` (a loose
read of the patch folder backs it up). A missing file is compiled from the source embedded in the
DLL, and the log says so. The log names each program's origin and slot count, and once per session
a client BLS file is read with the same reader and created on the device as a check.

### The BLS format

As `CGxDevice::IShaderLoad` (0x00684970) and `CGxShader::Load` (0x00689A70, the shader's vtable
slot 0) read it, little-endian:

| Field | Size | Meaning |
|---|---|---|
| magic | u32 | 0x47585348, "HSXG" on disk; checked by the loader |
| version | u32 | 0x00010003; checked by the loader |
| count | u32 | permutations in the file (a caller asks for "name:0", "name:1", ...) |
| per permutation: inputs | u32 | CGxShader+0x3C. Vertex: attributes read; pixel: interpolants read |
| outputs | u32 | CGxShader+0x40. Vertex: interpolants written, bit 0 the position; pixel: 0x200 |
| samplers | u16 | CGxShader+0x44. Pixel: one bit per declared sampler; vertex: 0 |
| extra | u16 | CGxShader+0x46. 0 in every D3D profile |
| size | u32 | bytecode length (CGxShader+0x4C) |
| bytecode | size | the D3D9 token stream (CGxShader+0x50) |
| padding | 0..3 | to a multiple of four |

Established on the 592 files of the extracted client: 567 parse exactly to their end and write back
byte for byte (`python tools/bls.py check <files>`), 25 are empty; the sampler mask matches the
declared samplers on 1598 of 1602 pixel permutations (the procedural water declares one it does not
list). A vertex shader's output mask equals its pixel shader's input mask plus bit 0. None of the
four mask fields is read on the way to CreatePixelShader or when binding. The writer is generic:
`tools/bls.py write out.bls perm0.cso perm1.cso ...` for any number of permutations.

For tuning, `WXL_FOREVER_SHADER_DEV=1` compiles the sources deployed under
`Data/Patch-4.MPQ/Shaders/Forever/src` first; edit one there and press Debug -> "Reload shaders".
Copy the edit back into the repository when it is right.

# Fog

## Work in progress

Done:
- Jitter flicker, light injection, tangibility, lights and realism (earlier rounds).
- One path: the per-pixel raymarch and the gamma composite are gone; the depth and indoor debug
  views live in the froxel apply pass.
- Optimisation: per-pass GPU timers, block integration, half-resolution near wisps, early-outs in
  injection, one-fetch ground, shared warp for the shadow steps, shader slot counts logged.
- Renamed to wxl-forever, a render-feature suite: src/core and src/fog.
- Panel in tabs with a help marker on every setting; quality presets.

- World shadows: a visibility pass per froxel marches towards the sun or moon through the screen's
  depth and over the heightfield; shafts default lowered to complement it.
- Large-scale structure: a drifting macro map of fog banks with eroded edges, and a thin high haze
  layer summed with the ground mist.
- Ambient variation: sky occlusion (heightfield horizon and screen), bank shading (a long step
  towards the light and one upwards), aerial perspective on by default (chromatic extinction 0.3,
  desaturation 0.25).

- Noise: every per-pixel and per-froxel offset (near wisps, shafts, dither, froxel jitter) reads a
  128 x 128 four-channel void-and-cluster blue-noise tile baked at load, rotated each frame by an
  R4 sequence; the near wisps and the shafts gained a temporal resolve (reprojection, distance
  rejection, neighbourhood clamp), so no fixed lattice remains. Debug view "Noise pattern".

- Shaders as files: offline fxc, wrapped in the client's BLS format and read through the client's
  file system, embedded fallback, live reload, dev sources.
- Lights: the service in src/lights (model table, selection, clusters, overlay); physical falloff,
  phase, multiple scattering, soft gating and occlusion in the fog; wxl-lightdebug retired.
- Projectiles: missiles in flight (core's effects::CollectMissiles) carve segment tunnels that
  close towards the tail in a swirl; fire spells thin the fog around themselves.
- Smoke and immersion: smoke and steam emitters (effects::CollectEmitters) become rising, widening
  plumes with their own noise and a darker tint, fires can add a faint plume; standing in dense fog
  or smoke eases in a veil towards the screen edges, lens wisps and softened lights.

- Surface lighting: every light of the service on terrain, buildings and models (src/surface),
  as deferred lighting into a shareable FP16 light buffer, then an apply step; ordered before the
  fog by core/Passes. Cluster indices are 16-bit, so the light capacity can grow past 255.
- Injection cost: the shadow steps read the noise once each, coarser; the bank steps read the
  macro map only; thin fog skips both; the high haze reads its noise only where it shows; the
  volume loop is skipped past the farthest volume. Debug -> "Count injection paths" logs the share
  of froxels taking each path.
- Projectile trails: persistent holes (a pool of 64 segments, sampled every 0.08 s of flight, plus
  a crater where a missile lands) that hold, then refill over seconds from an irregular edge, with
  billows rolling in at the rim; tested per screen tile.
- Surface softness: normals reconstructed at half resolution with a curvature-tolerant choice of
  neighbours and a bilateral blur (depth and normal similarity; creases stay sharp); a softer
  falloff tail, larger wrap, broad and faint specular, blue-noise jittered shadows averaged by a
  light-buffer history with depth rejection, a highlight roll-off, and one Softness control.
- Flicker: per-family light animation in the light service (fire, candle, lantern), seeded per
  light, so the fog halo and the lit surfaces flicker together.
- Post (src/post): bloom from the image's bright pixels and the light buffer's peaks, a 5-level
  FP16 dual-filter pyramid, tint and intensity; panel "Forever: post".
- Premium lights:
  - post: an ACES-fit filmic tonemap and eye adaptation (log-luminance pyramid to one texel, eased
    faster going brighter than darker, clamped), on an inverse-tonemap estimate of the LDR scene
    unless "HDR world" draws the world into an FP16 target;
  - surface: one bounce of indirect light at quarter resolution (six jittered hemisphere samples
    through the depth, the light buffer times the albedo plus the scene's glowing pixels, with a
    history); contact-hardening shadows (rays aimed across each light's own source, sized per
    family); angular profiles per family (lantern cage, street lamp hood, flame, window grille,
    also in the fog's halos); tube lights by their closest point (optional half length in the
    model table); GGX specular with a roughness from the albedo, a wetness control and an
    energy-conserving diffuse; Isolate switches and views for each.
- Quality: the light buffer and the indirect light are accumulated over time with a history
  length per pixel (up to 32 frames still) and a variance clamp in YCoCg, rejected by distance and
  normal mismatch, kept short on units (a screen mask from the units near the camera), then
  denoised by edge-aware a-trous passes; normals and indirect light come up by joint bilateral
  upsamples that reject taps across depth edges; the engine's model lights are not added again on
  units; the apply compresses highlights by luminance keeping the hue; the filmic curve works on
  luminance; the LDR inverse tonemap is off by default; bloom spares skin tones. Isolate rows in
  Forever: surface and Forever: post.
- Core targets consumed (each behind a toggle): G-buffer normals from the engine's shaders
  (rotated from view space, blended at the mask edge), the engine's materials reading last frame's
  light buffer (apply skips the pixels they lit), omni shadow maps for the four most important
  lights (PCF, screen-space steps as contact detail, debug view of the first atlas), and the HDR
  world ("HDR world" in Forever: post, off by default) resolved by the post chain.
- Omni shadows on the core's 4 x 2 atlas: each face's cell comes from the documented layout,
  PCF taps stay half a texel inside it, and points past the radius, before the near plane, on an
  unrendered face or off their cell read lit; debug view "Omni faces". Stable light ids through
  SetLightsEx. The core tracks the units each face sees: a unit moving near a lamp redraws only the
  faces it touches, a moved light all six; still faces refresh in turn, `_OMNI_REFRESH` a frame (3).
- Cost cuts: pixels (surface light pass) and froxels (fog inject) whose light cluster is empty stop
  before any normal, omni map or cookie is read, and a froxel reads an omni map only when that
  map's light reaches it.
- Carried lights: a model light whose instance rides another model (a torch in a hand) is flagged
  by the light service; it gets a wider soft core, lights its own carrier only by a share, and every
  light is capped near its source (core and cap are the light service's, shared with the fog's
  halos). Carried lights take no omni shadow map by default (their carrier and item shadow the
  floor), always shorten the surface history around them, and the bounced light on characters is
  a share. The published light buffer carries the highlight roll-off, so the engine's materials
  get it too.
- Moving lights: the light service reports lights that moved lately; the surface and fog histories
  are short and tightly clamped within their radius, and the omni shadow term joins the light
  buffer after the history (its own target, a combine pass), so a moving shadow leaves no trail.
- Doorways: the camera's indoor state eases over the indoor/outdoor transition (default the
  profile transition, 5 s in the user's config); cells near the camera ease with it while cells of
  the other kind keep theirs, the camera's profile (far distance, exposure, near effects, shafts,
  horizon haze, extinction) is mixed, and a change of far distance reprojects the history into the
  new slices instead of resetting it.

- Light look and cost round (src/surface, src/post):
  - tone: the per-light roll-off that divided every light by the scene's ambient (at night, 1 / (1 +
    7 L): a flat disc) is gone; the light buffer rolls off towards an absolute highlight cap, the
    albedo estimate divides the scene by the light that made it (ambient plus what the engine's
    material already added) and soft-caps at 0.85 instead of clipping to white, and apply's final
    compression has a knee and an asymptote of 1;
  - the lighting (lights, shadows, history, denoise) runs at half resolution by default and a
    resolve pass brings it up along depth edges, publishing two buffers: one for apply and the
    indirect light, one for the engine's own materials with units and a margin cleared from it
    (that buffer is last frame's, reprojected as if the world stood still; apply lights units);
  - omni shadow maps: a blocker search then a Poisson disc as wide as the penumbra the blockers
    cast from the light's real source size (small: a lantern's flame, so its own cage, already among
    the engine's casters at shadow quality 5, stays crisp on the wall), a bias in yards plus a slope
    term in texels instead of a share of the radius, a face size control, three rows a face;
  - contact shadows: a light with a map is only checked a couple of yards from the surface for what
    the map lacks (door frames, props, terrain); a light without one is still marched all the way;
  - the a-trous passes skip settled pixels; the raw target doubles as the a-trous scratch and the
    combine pass folded into resolve (two full-resolution FP16 targets fewer); every constant is
    uploaded before a shader is bound, since fxc parks a shader's literals in the registers it
    leaves free;
  - bloom: a softness that fades the fine pyramid levels (energy kept), 3 to 6 levels, and a
    brightness-weighted 4-tap bright pass against fireflies;
  - the fog's immersion switch has its own key, WXL_FOG_IMMERSION_ENABLED (the old single key
    still counts as on when its strength is above zero).

- Grain and doorway round:
  - the grain in front of a torch-carrying runner was the moving-light history cut: a carried light
    counts as moving every frame, so the surface history was capped at 2 frames and the fog's at
    half over the whole lit ground, and each frame's jitter showed. Both resolves now cut the
    history only where it changed: the surface where the history's luminance sits further from the
    local mean than the local noise explains (`WXL_FOREVER_SURFACE_CHANGE_CUT` sigmas, default 1;
    the old radius cap is gone), the fog where the neighbourhood clamp had to move
    the history by more than the neighbourhood's spread; the a-trous filters short histories wider;
  - the doorway transition is a cross-fade: the inject evaluates the outdoor and the indoor fog
    each with its own rows (chosen at compile time, which also freed the registers) and blends the
    two results by the cell's eased share, so the pattern never re-shapes; the near wisps take the
    leading profile's shape and dip to zero at the switch; the ease is a smootherstep. Isolates "No
    change-based history cut" (surface and fog) and "No doorway cross-fade" (a hard switch).

- Lights and fog round:
  - hot core (`WXL_FOREVER_LIGHTS_HOT_CORE`, light service, `LightHot` in lights.hlsli): a flame's
    light runs whiter and brighter within its reference radius and deep in colour in the falloff, on
    surfaces and in the fog alike (lamps take a third); Isolate "No hot core" in Forever: surface;
  - the omni shadow maps in the fog (`WXL_FOG_OMNI_SHADOWS`, `_OMNI_BIAS`): surface lighting publishes
    the maps it chose (`surface::CurrentOmniMaps`), the froxel pass writes them to a 128 x 2 table
    (row 0 light index to slot, row 1 the slot's position, atlas scale and face rows) and the lamp
    term of the injection reads one atlas texel per shadowed light, so a lantern's cage, a lamp post
    and passers-by throw beams and shadows through the mist; the froxel jitter and the history
    soften them. Isolate "No lamp shadow maps in the fog";
  - heat (profile `HEAT`, default 0.6): flames thin the fog within a few source sizes, a clear pocket
    around torches, braziers and campfires; Isolate "No heat";
  - the light cookies (the baked-data agent's `lights::cookies`) multiply into `LightMask` in the
    surface light pass (s15, c130..c132) and in the fog's lamps pass (s15, c222..c224): the lantern's
    own housing shapes its light and its halo, and its glass colours them pane by pane; the cage and
    grille analytic profiles give way to a cookie where one exists. The light service's "Cookie
    factor" view shows the factor. Every light of the cluster (up to 16) samples its own cookie inside
    the light loop (`LightCookie`: its cell and quaternion from the cookie rows, the face pick, one
    atlas fetch; a light without a cookie stops after one fetch). The loops have the register file to
    themselves: the omni shadow maps are evaluated in passes of their own (`surface/omni.ps`,
    `fog/omni.ps`) and the fog's lamps in theirs (`fog/lamps.ps`, below);
  - the fog's lamps are a froxel pass of their own before the injection (`fog/lamps.ps`): per froxel
    their single scattering with its mean distance, and their multiple-scattering halo with the
    heat; the injection adds them through the froxel's extinction (the halo's lighter, by
    `MS_EXTINCTION`) and saturates them softly. The omni maps' visibility per froxel comes from
    `fog/omni.ps` (drawn only while a map is bound; c221.x is their count), and the lamps loop picks
    each light's slot from the table. The fog's GPU timer shows the lamps as their own span;
  - omni shadow denoise (`WXL_FOREVER_SURFACE_OMNI_DENOISE`, default 2): the omni share joins after
    the history, so nothing averaged the PCSS disc's per-pixel blue-noise turn; `omnifilter.ps` runs
    that many depth- and normal-aware a-trous passes (steps 1, 2, 4) over it at the lighting
    resolution before the resolve. Isolate "No omni shadow denoise" shows the raw grain;
  - cloud light (profile `CLOUD_LIGHT`, code default 0, `CLOUD_SIZE`): the sun or moon light in the
    fog is shaded by coarse noise read where the ray towards it crosses a layer 120 yards above the
    camera, drifting 2.5 times as fast as the fog; Isolate "No cloud light";
  - `LightProfile` uses Chebyshev identities and squaring instead of atan2, asin and pow (same values
    to 1e-13, about 50 instructions less per light in each light loop).
- Fixes after the in-game test:
  - flashes when turning: the eye metered the whole view and made up all of a change, so a bright
    sky or lamp swung the exposure. It now meters centre-weighted (`lumavg`), makes up a share of the
    change in the log (`WXL_FOREVER_POST_ADAPT_STRENGTH`, 0.5) within a narrower range; isolate
    "No centre-weighted metering";
  - bloom: the level weights were normalised by `levels / sum`, so the glow came out levels times too
    strong; now `1 / sum`;
  - overcooked: the image the game already toned went through ACES, whose toe crushed darks about
    threefold. A gentle curve (extended Reinhard on luminance, anchored so mid grey stays) is the
    default (`WXL_FOREVER_POST_CURVE`, `_WHITE`), then a saturation (`_SATURATION`); the hot core
    whitens less (lights.hlsli, `LightHot`);
  - a lamp's own housing: the omni maps hold the lamp's own head and glass, so a lamp that earned a
    map (the nearest four) darkened the ground and its own halo in the fog as the camera came close.
    Casters within `WXL_FOREVER_SURFACE_OMNI_SELF` yards (0.5) of the light no longer shadow it, on
    surfaces and in the fog (the omni table's slot texel 1 w);
  - cookies: the bakes block 60 to 80 % of directions (median) since they are seen from one point;
    the floor is 0.25 and a flame takes `WXL_FOREVER_COOKIES_FLAME` (0.35) of its pattern;
  - near wisps: at a strength above 1 their modulation went negative in the gaps, emptying the fog
    and a lamp's halo within the near distance; it is floored at 0.35 of the volume's density. Heat
    thins the fog by 60 % at most;
  - fog time: a clock of capped frame steps (a hitch pauses the drift instead of leaping to catch
    up), no hourly wrap. The body wakes spread their eight trail points
    over the whole trail, so a running body no longer drops half-strength capsules every few frames.

- One fog round: the near wisps left their half-resolution screen layer (near, neardist,
  nearresolve) and became a finer noise in the injection's density over the first yards, read in
  the billows' own warped and wind-driven frame with the profile's churn, mean-preserving and
  floored (the fog gathers into curls, none is added, a lamp's halo in a gap stays), so they are
  lit, shadowed, carved and reprojected with the volume; the slices bent towards the camera
  (`WXL_FOG_SLICE_BEND`, `SliceToDistL`) and slice 0 integrates from the camera, first slice 1 yd;
  the horizon haze band and its tint became the volume's continuation past its far distance (the
  last slice's own extinction and light, thinning upwards with the height falloff, for the sky and
  the ground beyond alike: no seam, `HORIZON_HAZE` the share); the screen-space light shafts and
  their three passes went (the visibility pass's world shadows are the one shadow term); the
  immersion reads the volume at 1 and 2.5 yd and lost its lens wisps; the self-shadow march
  collapses to one step where a froxel outgrows half the reach. Keys gone: `WXL_FOG_SHAFTS`,
  `WXL_FOG_NEAR_STEPS`, `SHAFT_INTENSITY`, `SHAFT_DECAY`, `SHAFT_LENGTH`, `HORIZON_FALLOFF`,
  `IMMERSION_WISPS` (unknown keys are ignored). The terrain parts of the visibility pass are two
  functions (`TerrainSunVisibility`, `TerrainSkyVisibility`) a baked horizon map replaces.

Next:
- In-game check of the omni faces, the carried torch and the doorway easing.
- In-game tuning of the new defaults.
- Lights round: physical falloff, soft interior gating, clustered lights.

Volumetric fog: exponential height fog with animated noise, integrated through a froxel volume from the
world's sampled depth. Every sample along a ray takes the **indoor** or the **outdoor** profile
depending on whether it lies inside an interior WMO group, so from a doorway the room keeps its
own haze while the street outside gets the outdoor fog.

Needs INTZ support and multisampling off; otherwise it logs why and stays inert.

## The froxel volume (main path)

The fog is a W x H x Z grid of camera-aligned cells (160 x 90 x 64 by default), slices spaced
exponentially out to the camera profile's max distance and bent towards the camera
(`WXL_FOG_SLICE_BEND`: the near slices thinner, the far ones thicker by the same share; slice 0
starts at the camera itself), stored as a 2D atlas of slice tiles in a
half-float target (full float when half cannot be filtered). The passes, all ps_3_0:

0. **Visibility**, one pixel per froxel at the same jittered point injection samples: how much of
   the sun or moon reaches it past the world (a screen-space march through the depth towards the
   light with a thickness tolerance, then a few steps over the heightfield for what is off
   screen), how much sky it sees (the heightfield's horizon around it and the screen above it),
   and whether it lies inside an interior box. Injection scales the direct light and the ambient
   by these; the resolve then averages the per-frame offsets.
1. **Injection**, one pixel per froxel, jittered per frame: density from a baked 64^3 tileable
   noise volume (Perlin-Worley and billow shapes, domain warp, Worley erosion, a coverage cut
   that opens pockets and gaps), height fog and density volumes; indoor/outdoor profile per cell
   from the interior boxes; in-scattered light -- ambient graded with height, the drawn sun and
   moon through a dual-lobe Henyey-Greenstein phase, attenuated by a three-step self-shadow march
   towards them (Beer's law with an optional powder term), and the point-light list.
2. **Resolve**: last frame's volume, reprojected, is clamped to the range this frame's six
   neighbouring froxels allow (plus a little slack) and blended in; nothing is thrown away whole.
3. **Integration**, front to back along each froxel column (energy-conserving per-slice step):
   accumulated in-scatter in rgb, transmittance in a.
4. **Apply** at full resolution: each pixel reads the integrated volume at its distance (bilinear
   in the slice, linear across slices) and composites scene x transmittance + in-scatter. The sky
   reads the last slice.

Large-scale structure comes from a weather map: a 2D, very low frequency field sampled from the
noise volume at bank scale (100 to 300 yards), drifting with the wind. Inside a bank the fog is
the profile's own; towards a bank's boundary the coverage cut rises and the erosion strengthens,
so banks end in torn wisps, and between banks the air is clear. A thin high haze layer, with its
own height, falloff, density and slower noise, is summed with the ground mist. Whole banks shade
themselves: one long, coarse self-shadow step reaches beyond the short ones, and one step upwards
darkens the ambient under fog piled above.

Each pass has its own GPU timer in the panel's Overview tab (visibility, inject, resolve,
integrate, copy, apply), and the compiler's instruction slot count of every shader is logged once. Four
quality presets set the grid, the near wisps and the world shadow steps together.

One medium, no layers: the near wisps are a finer noise folded into the injection's density over
the first `NEAR_DISTANCE` yards (read in the same warped, wind-driven frame as the billows, with
its own churn, mean-preserving so the fog gathers into curls without thickening), so they are lit,
shadowed, carved and reprojected with everything else; past the far distance the medium is carried
on analytically from the last slice's own extinction and light, thinning upwards with the
profile's height falloff (`HORIZON_HAZE` is how much), which is the horizon haze and the fog on
the ground beyond the volume in one continuous term; the immersion veil is the volume's own light
read at the camera; the sun and moon shafts are the world-shadow term of the visibility pass.

The sun and moon come from where the sky draws them, not from the engine's lighting direction
(fixed azimuth, 20-37 degrees of elevation). The engine dims its lighting colour every frame by
the sun-glare occlusion query (up to 35%); the fog divides that back out and eases all celestial
inputs over about 0.3 s, so looking at the sun does not make the fog pulse with the query.

The density noise is sampled from a mip chain at the level matching each froxel's footprint, so a
distant froxel sees the noise averaged over the tens of yards it covers instead of one arbitrary
point of it (which changed with every jittered frame and was the flicker at distance).

Sample offsets come from a tileable blue-noise texture (`src/core/BlueNoise`, 128 x 128, four
independent channels, void-and-cluster, baked on a worker at load) rotated every frame, never from
a fixed per-pixel pattern: undersampling then leaves fine grain that the history averages out.

Debug -> "Isolate" switches remove one ingredient at a time (jitter, noise time, history, the
history clamp, the noise filter, the noise itself, self-shadow, sun and moon, lights, indoor
boxes, wakes, near wisps, the horizon continuation): the one that stops an artefact names it.

Other extensions feed the volume through the interface `wxl.forever.fog.inputs` v2 (see `FogInputs.hpp`):
up to 8 point/spot lights (position, radius, colour, intensity, cone) injected with an
inverse-square falloff, and up to 24 density volumes (spheres, boxes or vertical capsules) that
add density or carve a share of it away.

| Key | Default | Meaning |
|---|---|---|
| `WXL_FOG_QUALITY` | (unset) | `low`, `medium`, `high` or `ultra`; explicit keys still override it |
| `WXL_FOG_WORLD_SHADOWS` | 1 | the world shades the fog from the sun and moon (strength per profile) |
| `WXL_FOG_SHADOW_STEPS` | 10 | screen-space steps per froxel for it (4..12) |
| `WXL_FOG_SKY_OCCLUSION_ENABLED` | 1 | less sky light where the world hides the sky (strength per profile) |
| `WXL_FOG_FROXEL_X` / `_Y` / `_Z` | 160 / 90 / 64 | grid size |
| `WXL_FOG_FROXEL_NEAR` | 1.0 | far edge of the first slice, yards (it starts at the camera) |
| `WXL_FOG_SLICE_BEND` | 0.5 | 0 plain exponential slices; towards 0.9 the near slices thin and the far ones thicken |
| `WXL_FOG_TEMPORAL` | 0.9 | share of last frame's volume kept |
| `WXL_FOG_DEBUG_LANTERN` | 0 | a warm point light at the player |
| `WXL_FOG_DIAGNOSTICS` | 0 | log the sun, moon and glare inputs' spread once a second |
| `WXL_FOG_WAKES` | 1 | players and creatures push the fog away and leave a fading wake |
| `WXL_FOG_WAKE_RANGE` | 40 | bodies this far from the player carve the fog (the 16 nearest) |
| `WXL_FOG_WAKE_TRAIL` | 1.2 | seconds a wake takes to fade |
| `WXL_FOG_NEAR_FIELD` | 1 | near wisps: the finer noise in the density over the first yards (strength, reach, scale and churn per profile) |
| `WXL_FOG_TERRAIN` | 1 | fog height measured from a heightfield around the camera |
| `WXL_FOG_TERRAIN_BUDGET` | 48 | ground traces per tick for it |
| `WXL_FOG_DITHER` | 1 | dither the composite against 8-bit banding |
| `WXL_FOG_PHASE` | hg | `hg` dual Henyey-Greenstein, `cs` Cornette-Shanks |
| `WXL_FOG_NATIVE_FOG` | 1 | push the client's own distance fog to the far clip while the volume draws |
| `WXL_FOG_NATIVE_FOG_START` | 0.85 | where the client's fog then starts, as a share of its end |
| `WXL_FOG_IMMERSION_ENABLED` | 1 | the immersion effects at all (the profile's `IMMERSION` is their strength) |

## Lights (`src/lights`)

The suite's light service, used by the fog and by any later feature, working without the fog:

- **Sources:** the engine's M2 lights and the WMO (MOLT) lights, from core's `wxl::game::lights`,
  and the model table (`data/model-lights.csv`, built by `tools/gen_model_lights.py` from the
  extracted client, deployed as `Extensions/wxl-forever/model-lights.csv`). The table gives light
  to models that carry none; core's `lights::CollectModels` lists the unlit instances and the
  service matches their model stems. A light of a culled model is kept about ten seconds.
- **Selection:** up to 128 lights around the camera, scored by brightness over distance (not by
  where the camera looks; a light chosen last frame counts 1.25 times, so two near-equal lights at the
  128th place do not swap), each fading in and out over a second. Up to 4096 engine lights and model
  instances are read around the camera before the ranking, so the cut is never the engine's walk order.
- **Identity:** each light carries a stable id (its owner, index and kind); the published list is
  sorted by it, so the same lights keep the same indices from frame to frame, and every consumer
  (surface, fog, omni slots, cookies) names a light by that index.
- **Clusters:** `Publish` writes every light to a 128 x 4 float texture and, per cluster of a
  16 x 9 x 16 split of the consumer's frustum (exponential in depth), the list of every light that
  may reach it (no cap: the index pool holds every light in every cluster) to a second texture.
  `shaders/lights.hlsli` reads both (`LightList`, `LightAt`) and holds the falloff: a soft-core inverse
  square, 1 / (d^2 + r0^2), normalised at the light's reference radius and windowed to zero at its
  radius.
- **Panel "Forever: lights":** the former wxl-lightdebug overlay (M2 crosses, WMO boxes, table
  rings, spheres of each radius, missiles, emitters, histograms, "Log lights near me" and "Log
  effects near me"). Keys `WXL_FOREVER_LIGHTS_ENABLED`, `_WMO`, `_TABLE`, `_THROUGH`, `_RADIUS`,
  `_LOG_STATS`, `_MISSILES`, `_EMITTERS`, `_EMITTER_RADIUS`.

### Cookies (`src/lights/Cookies`)

A lantern throws the shadow of its own cage. `5.tools/forever-bake` renders, for every light source
of the client (model-table lights, every engine M2 light, every WMO MOLT light through the groups
within 1.5 yd of it and the doodads beside it, and fire emitters for later), the transmittance of
the fixture seen from the light as a 64-per-face cube map in the source's frame
(`Textures/Forever/Cookies/`, `manifest.csv` keyed `model|light` with a `kind` column, format 2).
At runtime every light carries `cookieSource` (the hash of its file's stem) and its rotation: the
table fills them for its rows, the gather for engine lights from the model instances and the WMO
placements (core `lights::CollectWmoPlacements`), and the service resolves `(kind, source, index)`.
Lit opaque geometry blocks, alpha keys cut, alpha-blended glass tints, and geometry flagged unlit
(painted lantern panes) passes by its texture's luminance, so struts show; open flames get no
cookie. Baked with `--colour`, a cookie whose glass is tinted keeps that tint per texel
(A8R8G8B8, one scalar scale for the three channels, so a grey strut stays grey); a neutral one
stays L8. Every runtime source is baked at 128 per face (`bake.py cookies --size 128 --colour`:
4650 cookies, 1103 of them tinted, 564 MB of loose files; a smaller file is resampled into the
atlas cell, 512 KB each in colour). At runtime the service maps each gathered light to its
cookie, keeps `WXL_FOREVER_COOKIES_BUDGET` (32, at most 64) resident in one atlas (L8, or
X8R8G8B8 as soon as the manifest holds a tinted file; streamed by `core/BakedAssets` from a worker
thread, a missing file reading as white): those of lamps within 30 yards first, never evicted while
wanted, then the nearest and brightest. A light has its cookie's mean (1 - the manifest's `blocked`,
its recorded tint) from its first frame; once the file is read, the mean eases to the file's own
over half a second, and a pattern that becomes resident fades in over the mean over half a second,
so a lamp never steps. It publishes per light the rotation from world directions into the model's
frame, its cell code (atlas cell + 1, plus 128 times the pattern's share) and its mean transmittance
rgb in the cluster texture, past the per-cluster texels. The light's
colour is published untinted: `shaders/cookies.hlsli` `LightCookie` returns the cookie's rgb (its
glass colour by `WXL_FOREVER_COOKIES_TINT`, 1), or its mean where it is not resident, so a lamp
keeps its colour and energy without its bars rather than turning into a round ball. Panel
"Forever: lights", tab Cookies: on/off, strength, floor (0.1: struts and pane grids read on walls
and in the fog), flame share (0.35: open flames stay soft), tint, resident count, debug view (the
atlas in a corner, or the factor where a consumer supports it). Keys `WXL_FOREVER_COOKIES_ENABLED`,
`_STRENGTH`, `_FLOOR`, `_FLAME`, `_TINT`, `_BUDGET`, `_DEBUG`; `WXL_FOREVER_BAKED_ASYNC` and
`_BUDGET_MB` for the asset manager.

The omni shadow maps and the cookies share the work: surface lighting gives its four maps first to
lights with a unit near them (a zombie passing a lantern must cast its shadow; a unit counts as near
for a second and a half after it leaves), then to lights without a cookie; a still lamp whose cookie
throws its own cage takes none. The camera's room comes first within each tier
(`WXL_FOREVER_SURFACE_OMNI_UNITS_FIRST`, 1; off restores importance order). A map is a bonus on a
light that always shines: a slot's shadow fades in and out over half a second, a light keeps its
slot a second at least, and a slot is never dropped at once. Which slot a light holds is one table
the light service publishes (`lights::PublishOmni`, read by `OmniSlotOf`), by list index, for the
surface and the fog alike; the fog reads a map with four taps.

In the fog each froxel loops over its cluster's lights only: the falloff above, the profile's
phase towards the camera (a lamp is brighter looked at), one isotropic multiple-scattering octave
with a wider core that spreads the halo, soft saturation instead of a clip, a soft interior gate
(interior lights fade across `WXL_FOG_LIGHT_MARGIN` yards at a box's boundary). Nothing on screen
occludes a halo (the former screen-space test, `WXL_FOG_LIGHT_OCCLUSION`, carved the halo by
whatever stood in front of the lamp from the camera). Debug view "Lights only" shows the result.

## Projectiles, smoke and immersion

- **Trails** (`fog/Projectiles`): each missile in flight leaves a segment every 0.08 s of flight,
  `PROJECTILE_RADIUS` times its model's radius thick, kept in a pool of 64 that outlives the
  missile; where it lands, a crater (`PROJECTILE_CRATER`). A segment holds its hole open
  (`PROJECTILE_HOLD`, carving `PROJECTILE_CARVE`), then refills over `PROJECTILE_REFILL` seconds
  with an ease in and out: the hole shrinks from an edge perturbed by noise, and a shell of thicker
  fog at that edge (`TRAIL_BILLOW`, `TRAIL_BILLOW_SIZE`) rolls around the trail's axis
  (`TRAIL_BILLOW_ROLL`) and drifts, thickest mid-refill. A fire spell's hole widens as it refills
  (`PROJECTILE_HEAT`) and its billows are darker and warmer (`TRAIL_SMOKE`). The billows scale the
  fog that is there, so clear air gets none. A full pool merges the two consecutive segments whose
  ages differ least, so nothing pops. The shader reads the pool from a texture and tests only the
  segments of its screen tile (16 x 9, up to 8 each). Debug view "Projectile trails".
- **Plumes** (`fog/Plumes`): the 6 nearest smoke or steam emitters, kept for about ten seconds
  after their model was last drawn, become cones rising as high as their particles travel in
  their life (speed times lifespan, 1.5 to 15 yd), widening from the first particle size to the
  last, denser the more particles they keep alive, times `SMOKE`. They carry their own rising
  noise; smoke is tinted by `SMOKE_DARKNESS`, steam is not. A fire emitter adds a faint plume
  (`FIRE_SMOKE`), since many fires, Elwynn's campfires among them, have no smoke emitter. Panel
  Overview shows the emitters seen by kind, and how many were disabled.
- **Immersion**: a one-texel pass reads the resolved volume across a small cone 1 and 2.5 yd
  ahead of the camera and eases the result over about half a second; nothing is modelled apart
  from the volume. At `IMMERSION_DENSITY` the effect is full: bright lights soften into a glow
  (`IMMERSION_BLOOM`) and a veil of the fog's own light there thickens towards the screen edges
  (`IMMERSION`); the centre keeps little of it, since the volume itself fogs the first yards.

## Surface lighting (`src/surface`)

The engine lights each model with its four nearest lights and terrain and buildings with none.
After the world pass, and before the fog so the fog veils lit surfaces, this feature lights every
pixel of the world with every light of its cluster from the light service. It works with the fog
off (it then writes the finished image itself).

- Normals come from the depth: on each axis the neighbour closer in depth is used, so edges do not
  bend them; "Smooth normals" widens the baseline. `SurfaceNormal` is the one place a G-buffer
  normal would replace.
- Each light: the service's falloff (soft-core inverse square, windowed), Lambert with a wrap, an
  optional soft Blinn specular, a few screen-space shadow steps towards the light, and the fog's
  extinction over the distance when the fog runs (`core/Media`).
- The scene colour divided by the scene's own lighting (ambient plus half the sun or moon, from the
  sky) estimates the albedo, so a lamp tints what it lights: lit = scene + albedo x light.
- Pixels of the sky or of a cluster no light reaches keep the scene untouched (the pass discards
  them early).
- Normals: reconstructed at half resolution, on each axis from the side whose second neighbour
  continues the first in a straight line (a crease or silhouette breaks it on one side only), then
  a separable bilateral blur weighted by distance and by normal similarity below a crease angle
  ("Normal smoothing" widens it and raises the angle). The engine's G-buffer normals, with a
  validity mask in alpha, win where valid.
- Softness: the falloff blends the inverse-square core with an inverse-distance tail, the shadow
  rays aim at a blue-noise jittered point of a disc at the light and start at a jittered step, and
  the light buffer keeps a history (reprojected, rejected where the distance disagrees) that turns
  the jitter into penumbras. Bright light rolls off before apply.
- Resolution: `WXL_FOREVER_SURFACE_RESOLUTION` 2 (default) computes the light, its shadows, history
  and denoise at half resolution, each texel standing for the first full texel of its block
  (`PassUv`), and the resolve pass upsamples along depth; 1 is full resolution.
- Tone: the light buffer rolls off towards `_HIGHLIGHT_CAP` (2.5, in multiples of applied light; 0
  off); apply compresses the lit result towards 1 from `_HIGHLIGHT_KNEE` (0.6); the albedo estimate
  divides the scene by its ambient plus the light the engine's material already added and soft-caps
  at 0.85. Where the scene is too dark to divide (most of a night), the albedo is an assumed 0.35
  times the pixel's brightness against the scene's around it (`LocalLuma`), so the texture survives:
  a flat grey there lit every crevice like the planks around it and washed the view into one colour.
- Shadows: `_OMNI_TAPS` (12; below 5 the plain 4-tap filter), `_OMNI_FACE_SIZE` (512), `_SHADOW_BIAS`
  (0.03 yd), `_SHADOW_SLOPE` (1 texel per unit of slope), `_CONTACT_LENGTH` (2 yd, the same short
  screen test for every light, mapped or not), `_SHADOW_STEPS`.
- Engine materials: the rewritten engine shaders (`tools/bls_normals.py`, regenerated into
  `Data/Patch-4.MPQ/Shaders/pixel/`) add last frame's buffer where the reprojection is valid (w > 0,
  uv inside the screen, the buffer's 0.5 alpha marker), in linear space (gamma 2), faded by the
  shader's own fog and capped per channel; elsewhere they mark the pixel for apply. `_UNIT_MARGIN`
  (0.5 yd) is how far around each unit the buffer they read is cleared; apply lights those pixels
  from this frame's buffer instead. `_ENGINE_MATERIALS=0` leaves every pixel to apply. Debug view
  "Engine materials added light" shows exactly what they added.
- Panel "Forever: surface": Overview (GPU times per pass), Lighting (resolution, softness, strength,
  cap and knee, wrap, specular, wetness, indirect, shadows, history, engine materials, carried
  lights), Debug (views: lit, lights only, normals, indirect, specular, omni atlas, omni faces, engine
  buffer, albedo estimate, engine materials added light; an Isolate switch per ingredient).
  Keys `WXL_FOREVER_SURFACE_ENABLED`, `_RESOLUTION`, `_STRENGTH`, `_SOFTNESS`, `_WRAP`, `_SPECULAR`,
  `_WETNESS`, `_HIGHLIGHT_CAP`, `_HIGHLIGHT_KNEE`, `_INDIRECT`, `_INDIRECT_RADIUS`, `_EMISSIVE`,
  `_SHADOW_STEPS`, `_SHADOW_BIAS`, `_SHADOW_SLOPE`, `_CONTACT_LENGTH`, `_OMNI_SHADOWS`, `_OMNI_TAPS`,
  `_OMNI_FACE_SIZE`, `_OMNI_REFRESH`, `_UNIT_MARGIN`, `_FOG_DIMMING`, `_NORMAL_SMOOTHING`, `_HISTORY`,
  `_DENOISE`, `_GBUFFER`, `_ENGINE_MATERIALS`, `_ENGINE_ON_UNITS`,
  `_CARRIED_ON_CARRIER`, `_CARRIED_SHADOW_MAPS`, `_INDIRECT_ON_UNITS`.
- Post keys: `WXL_FOREVER_POST_BLOOM`, `_THRESHOLD`, `_KNEE`, `_INTENSITY`, `_SOFTNESS` (0.7),
  `_LEVELS` (5), `_STABLE` (1), `_LIGHT_SHARE`, `_TONEMAP`, `_FILMIC`, `_ADAPTATION`, `_KEY`,
  `_MIN_EXPOSURE`, `_MAX_EXPOSURE`, `_ADAPT_UP`, `_ADAPT_DOWN`, `_UNCLIP`, `_HDR_WORLD`.

The order is kept by `core/Passes`: the world's depth is redirected whenever any feature wants to
draw, surface lighting draws at order 100 and the fog at 200. The light service gathers and
publishes once per frame for both, over a fixed cluster space (0.5 to 250 yards), and its gather
settings live in "Forever: lights", Gather tab (`WXL_FOREVER_LIGHTS_GATHER_*`).

Volumes share 20 slots per frame: fed volumes, then plumes, then bodies and wakes.

## Terrain (`src/terrain`)

Two services, no consumer of their own yet besides the debug views; both work with everything else
off.

- Horizon (`Horizon.cpp`, `shaders/horizon.hlsli`): the baked horizon tiles of `forever-bake
  horizon` (L8 volume 128 x 128 x 16 azimuths, L8 sky share, R16F height per ADT tile) streamed
  through `core/BakedAssets` into a 4 x 4 block of tiles around the camera, each tile at cell
  (cx mod 4, cy mod 4) of three MANAGED atlases (512 x 512 x 16 L8, 512 x 512 L8, 512 x 512 R16F,
  4.75 MB), so the atlases wrap and a shift refills only the changed cells. A cell without its tile
  holds neutral values. `horizon::Bind(dev, volumeStage, skyStage, heightStage)` and
  `horizon::Constants(horizonC, horizonD, withHeight)` feed a shader that set the slot and register
  macros before including `terrain/shaders/horizon.hlsli`:
  `TerrainSunVisibility(worldPos, toSun)` compares the sun's elevation with the baked horizon at
  its azimuth (trilinear between the two nearest azimuths), a smoothstep of the penumbra wide,
  lowered for points above the ground by the occluder distance; `TerrainSkyOcclusion(worldPos)`
  is the cosine-weighted sky share; `TerrainHeight(worldPos, valid)`. Outside the block or with no
  tile resident every function returns 1 (neutral).
- Caster (`Caster.cpp`, `shaders/caster.vs.hlsl`, `shaders/depth.ps.hlsl`): the resident tiles as
  129 x 129 heightfield meshes (MANAGED, the seam row and column from the neighbours) drawn
  depth-only into the engine's sun shadow maps after each of its render callbacks (core
  `shadows::ChainAfter(Render)`), with the pass's own look-at, projection and viewport from core
  `shadows::DescribeRender`, writing light-view z / 4000 as the engine's ShadowMapSL does; 32 x 32
  patches culled per pass, step 1 / 1 / 2 / 4 cells for the main map and the three bands. The
  interior map gets no terrain. Every D3D state touched is read back and put back. With "low sun"
  the core's `wxl.shadowlight` adjuster replaces the engine's direction (the sun lifted five times,
  floored at 50 degrees) with the true sun lifted by a scale (1.5) and floored lower (15 degrees).
- Debug views (Debug tab, Terrain): sun visibility, sky occlusion, horizon towards the sun,
  cascade coverage (which map holds each pixel), terrain height, resident tiles.

Consumers to wire (owned by the fog and surface features): the fog's visibility pass multiplies
`SunVisibility` by `TerrainSunVisibility(cam + r, L)` and `SkyVisibility` by
`TerrainSkyOcclusion(cam + r)`; the surface's dusk rim reads `TerrainSunVisibility` at the pixel.

## How indoors is decided

Every placed WMO within 80 yd of the camera (core `lights::CollectWmoPlacements`, with its CMapObj
root), drawn or not, gives the groups of its root's group table whose flags carry 0x2000
(interior); each becomes an oriented box: the group's model-space bounds under the inverse of the
placement, padded by 0.3 yd. The 12 nearest (last frame's counted 10 yd nearer) go to the shaders,
which test each froxel and each surface against them; a light in one of them is interior and lights
its own room. What the camera looks at never changes which rooms exist.

Known approximations:
- A box is the group's bounding box, not its shape: an L-shaped room, or a room whose box pokes
  outside the walls (porches, corners), classes that outdoor space as indoor.
- A group flagged interior but open to the sky (some courtyards) is indoor.
- Only the 12 nearest rooms count.
- The ray is sampled 10 times, crowded towards the camera; the doorway boundary is located within
  one step, softened by per-pixel jitter.

## Settings

Environment variables, or `KEY=value` lines in `Extensions\wxl-forever\wxl-forever.cfg`. Everything can
also be changed live from the WarcraftXL overlay, in the "Fog" panel.

General:

| Key | Default | Meaning |
|---|---|---|
| `WXL_FOG_ENABLED` | 1 | master switch |
| `WXL_FOG_INDOOR_DETECT` | 1 | classify samples by interior WMO groups; 0 makes everything outdoor |
| `WXL_FOG_TRANSITION` | 3 | seconds a profile change takes to blend in |
| `WXL_FOG_DEBUG_FAR` | 200 | distance shown as white in the debug views |

Profile keys, once with the prefix `WXL_FOG_` (outdoor) and once with `WXL_FOG_INDOOR_` (indoor):

Every per-profile knob of `FogProfile.hpp` (`WXL_FOG_PROFILE_FLOATS`) has a key, among them
ground follow, valley pooling, flow layer and speed, roll, contact and its thickness, wake swirl,
near wisps (strength, distance, scale, turbulence), multiple scattering (MS_STRENGTH,
MS_EXTINCTION, MS_PHASE, MS_OCTAVES), SKY_AMBIENT, and chromatic extinction (CHROMA and its
CHROMA_R/G/B tint), world shadows (WORLD_SHADOW, WORLD_SHADOW_SOFTNESS), fog banks
(MACRO_STRENGTH, MACRO_SCALE, MACRO_CONTRAST, MACRO_COVERAGE, MACRO_DRIFT, BANK_EDGE_EROSION),
the high haze (HAZE_DENSITY, HAZE_HEIGHT, HAZE_FALLOFF, HAZE_SPEED, HAZE_NOISE), SKY_OCCLUSION,
BANK_SHADING, BANK_SHADOW_REACH and AERIAL_DESATURATION. Older ones: coverage,
erosion, billow, warp, detail scale, ambient and its low/high grade, sun, moon and light scatter,
forward and back lobe g and their blend, self-shadow strength and reach, powder, max scatter,
exposure and tonemap white. The main ones:

| Suffix | Outdoor default | Indoor default | Meaning |
|---|---|---|---|
| `DENSITY` | 0.015 | 0.02 | extinction per yard at the base height |
| `HEIGHT_FALLOFF` | 0.06 | 0 | per-yard thinning above the base height |
| `BASE_Z` | player's feet at first world entry | same | absolute world height the fog is densest at |
| `NOISE_SCALE` | 0.035 | 0.08 | noise frequency, cycles per yard |
| `NOISE_STRENGTH` | 0.8 | 0.5 | 0 flat fog, 1 fully noise-modulated |
| `WIND_SPEED` | 2 | 0.3 | noise drift, yards per second |
| `MAX_DISTANCE` | 300 | 120 | how far the volume reaches when the camera is in this profile |
| `COLOR_MODE` | native | native | `native`, `custom` or `blend` |
| `COLOR` | 8C9499 | 8C9499 | custom colour, `RRGGBB` or `r,g,b` in 0..1 |
| `COLOR_BLEND` | 0.5 | 0.5 | blend mode: 0 native .. 1 custom |
| `BRIGHTNESS` | 1 | 1 | multiplies the final colour |

## Proposed fog table (not created yet)

A client table the resolver can read in place of the two built-in profiles. One row is one
profile; the resolver picks the best row per slot from what the client already knows about where
the player stands, and blends to it over `TransitionTime`.

| Column | Type | Meaning |
|---|---|---|
| `ID` | uint32 | row id; also the resolver's identity, so a change of row blends |
| `AreaTableID` | uint32 | AreaTable row (zone or subzone); 0 matches anywhere |
| `WMOAreaTableID` | uint32 | WMOAreaTable row, for one building or one room; 0 = not WMO-specific |
| `Slot` | uint8 | 0 outdoor, 1 indoor |
| `Priority` | int32 | higher wins when several rows match |
| `Density` | float | as the profile |
| `HeightFalloff` | float | |
| `BaseZ` | float | |
| `BaseZMode` | uint8 | 0 absolute world Z, 1 relative to the player's feet at entry |
| `NoiseScale` | float | |
| `NoiseStrength` | float | |
| `WindSpeed` | float | |
| `WindDirection` | float | degrees; today the heading is fixed |
| `ColorMode` | uint8 | 0 native, 1 custom, 2 blend |
| `Color` | uint32 | 0xAARRGGBB |
| `ColorBlend` | float | |
| `Brightness` | float | |
| `MaxDistance` | float | |
| `TransitionTime` | float | seconds to blend into this row |
| `Flags` | uint32 | reserved (time-of-day or weather gating) |

Match order: a `WMOAreaTableID` row for the current WMO group beats an `AreaTableID` row, which
beats a zero/zero default. A server-driven override would later arrive as a row id to force.

Requires the `OnWorldSceneBegin` event from wxl-core.
