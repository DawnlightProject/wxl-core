# wxl-graphics-fog: design

A world-space, simulated, ray-marched fog for the 3.3.5a client on DXVK's Vulkan device. The fog is
the threat of a zombie survival mode: it lies in the land, runs down it, fills its hollows, reaches
the horizon, and gameplay can push it around.

This replaces the froxel fog (`archive/wxl-graphics-fog-froxel`). What the user rejected there:
it was a disc of fog around the camera, it had no substance, and its motion came from a two-phase
flow map that visibly swung back and forth. Everything below is chosen against those three.

## 1. Representation

Three layers of state, all on the GPU. The CPU holds only settings, small lists and transient tile
bytes (the 32-bit client has little address space to spare).

**Terrain block (2D).** 8 x 8 ADT tiles around the camera, 1024 x 1024 texels at 4.17 yd: the
baked tile grid of `5.tools/forever-bake` (`_height` R16F, `_sky` L8, water G16R16F from MH2O),
uploaded tile by tile straight into toroidal GPU atlases. Nothing is copied into CPU atlases. A
derived `floor` image (R32F, full mip chain) holds the fog's floor, the terrain or the water surface
above it. Its mips give local relief for free: `hollow = mip4 - floor` is positive in hollows. The
camera stays in the central 2 x 2 tiles, so the block reaches at least 1600 yd around it, past the
client's 1277 yd far clip.

**Transport layer (2D).** A shallow gravity-current simulation on the block's grid: fog depth `d`
and four outflow fluxes per cell (below). It is the macro "where is the fog" field.

**Density clipmaps (3D).** Four camera-centred levels, 128 x 128 x 32 cells each, addressed
toroidally in x and y and terrain-following in z (a cell's height is measured from the floor under
it, so 32 layers are enough everywhere):

| Level | Cell (xy, z) | Covers (xy, height above ground) | Steps |
|---|---|---|---|
| L0 | 0.5 yd, 0.5 yd | 64 yd, -1..15 yd | 30 Hz |
| L1 | 2 yd, 1 yd | 256 yd, -2..30 yd | 15 Hz |
| L2 | 8 yd, 2 yd | 1024 yd, -4..60 yd | 7.5 Hz |
| L3 | 32 yd, 4 yd | 4096 yd, -8..120 yd | 3.75 Hz |

Each cell holds fog and smoke density (RG16F). A level scrolls by whole cells when the camera moves:
a cell entering the window takes its value from the next coarser level, so nothing pops and no
data is recomputed from scratch. Above L3 the only fog is an analytic haze, integrated in closed
form. There is no fixed radius anywhere: L3 reaches beyond the far clip, and the haze goes on to
infinity.

Why clipmaps rather than froxels: the fog lives in the world, so it has memory (a wake, a hole, a
bank drifting across a valley persists and moves), and it is the same fog seen from anywhere. Why
terrain-following: the fog hugs the ground, and 32 layers per level cover it at 0.5 yd near the
camera without spending cells on air above hills.

## 2. Transport: the fog rivers

**The model.** Fog on the ground is cold, dense air: a gravity current. The transport layer uses the
virtual-pipe shallow-water scheme (O'Brien and Hodgins; Mei et al.): each cell pushes flux to its
four neighbours in proportion to the difference of fog-surface height `floor + d`, keeps part of
last step's flux (inertia, damped by friction), and never sends out more than it holds. What this
gives with no special cases:
- Katabatic drainage: fog on a slope accelerates downhill; rivers form in the valleys.
- Pooling: a basin fills until its fog surface is flat, a lake of fog.
- Spill over cols: a full basin overflows at its lowest rim and a new river starts there.
- Thin sheets down cliffs: steep drops mean fast flow, and by continuity thin fog.
- Wind: a wind stress term pushes the layer downwind; relief holds it back, since climbing costs
  surface height, so wind streams fog along valleys rather than over ridges.

**Sources and sinks** (per cell, per second):
- Formation: night radiative cooling on open ground (sky share), extra in hollows, extra over water;
  a share of it by day.
- Mixing decay.
- Sun burn-off where the sun actually reaches the ground: a heightfield shadow test, so shaded
  valleys keep their fog.
- Wind scour on exposed ridges.
- Extra decay past a pool depth, so basins never fill without bound.
- Gameplay: fronts and sources add, flows push.

**Fixed rate and stability.** It steps at a fixed 10 Hz of simulated time, whatever the frame rate.
The scheme is flux-limited and stays stable at the time steps used; a panel slider scales simulated
time.

**Warm-up.** When a tile arrives, its cells start at an equilibrium estimate (formation over decay,
plus hollows pre-filled from `hollow`). The layer then runs settling steps with a long time step for
about a second: after a teleport or a loading screen, rivers and pools are there within a second or
two.

**Into 3D.** The clipmaps are advected by a kinematic velocity field, rebuilt at half resolution
every step. It is the sum of:
- the layer's flow, inside the layer, fading above its top;
- the wind, with a height profile, its up-slope part removed near steep ground so relief deflects it,
  and a vertical part that keeps fog following the terrain;
- divergence-free curl-noise turbulence, strongest near the layer top;
- local flows from gameplay, bodies (wakes), plumes (buoyancy) and projectiles (inflow).

**Advection.** Semi-Lagrangian with a MacCormack correction and a min/max limiter: second order,
little numerical blur, no overshoot. Two passes per step, at a fixed rate per level. Motion is
continuous: there is no flow-map phase to reset, so nothing swings back and forth.

**Sources in 3D.** After advection each cell relaxes towards a target over a renewal time (about 8
s). The target is the layer (dense below `floor + d`, a soft billowing top), a thin ground mist,
drifting macro banks, and the shaping noise below. Gameplay and wakes act at the same point:
- carve: remove a share per second;
- add: inject density;
- hold: keep at least a density.

A carved hole therefore refills by inflow and renewal, lumpy rather than as an even fade.

## 3. Substance: cloud-style shaping

**Shaping.**
- **Shape noise.** A tileable 128³ Perlin-Worley texture: R Perlin-Worley, G B A Worley at doubling
  frequencies. It is generated on the GPU at start, so no CPU memory is used.
- **In the simulation.** The target's density is the macro coverage reshaped by that noise, Schneider
  style: where coverage is low only the noise peaks survive (wisps, holes, tendrils); where it is
  high the fog is full and lumpy. Each level takes the octaves its cells can hold, so levels agree at
  large scale and the fine level adds detail.
- **Boiling.** The injected noise is two copies of the noise scrolling in opposite directions and
  summed. The pattern evolves without moving coherently, so the structure that persists is what the
  simulation carries: the billows travel with the local flow.
- **At render time.** Worley noise erodes the edges near the camera in two octaves (base, and a fine
  one near the camera), bent by curl noise that evolves in time (magnified for the fine octave, so
  its wisps twist around the base's), drifting with the wind, never reset. The fine octave fades by
  half the detail distance, the base by the detail distance (60 yd, the near range at most); each
  also fades once a pixel covers a sixth of it, and the blend is stretched back to one octave's
  contrast. Near march only: in the far march's long steps it turned to grain. The result: full
  cores, eroded fringes, fine tendrils.
- **Climb.** Above the layer's top a thin veil decays over `CLIMB x saturate(depth / 4)` yards, so
  deep pools climb higher in soft billows while shallow sheets stay flat; the shaping noise turns
  the veil into wisps.

Near the camera the shape lives in L0 at 0.5 yd, the erosion below that: wisps pass in front of the
lens.

## 3b. The atmosphere

The lying fog hugs the ground, which gives it its volume, but leaves the air column above it empty. A
second, simple medium fills it: the atmosphere.
- **Profile.** Full density up to a base over the ground, then thinning with a scale height. Its
  ground blends the smoothed ground under the camera and level 3's (the terrain over 32 yd) by a
  follow share: 0 lets hills rise out of it, 1 blankets them.
- **Variation.** Large patches of the shape noise's Perlin-Worley channel, flattened in height and
  drifting with the wind.
- **Composition.** It is summed with the fog, smoke, haze and dust in every sample of both marches,
  so one transmittance covers every medium and the order is exact by construction. Past the far
  march it continues in closed form with the haze. Chosen over a separate froxel integration (for
  example in the lamp grid) because the march already evaluates light at every step (the haze made
  every step non-empty): the atmosphere costs one noise tap and one ground tap per step, where a grid
  would need its own integration pass and an interleaved composite to be correct.
- **Light.** Its own phase (one HG lobe), colour share (the fog's colour against neutral), and sun,
  moon, sky and lamp strengths. The sun reaches it through the lying fog's light volume and the
  terrain and world shadows (shafts in the air), and through the air above: `1 / (1 + 0.6 tau)`
  with `tau` its density times its scale height over the sun's elevation, which also thins the sun
  on the lying fog. Indoors it gives way to the indoor air.
- **Lamps.** The lamp grid stores lamp light per unit of scattering before any medium's strength;
  each medium scales it by its own. The grid also thins each lamp's light by the medium around the
  froxel over the lamp's distance: the direct part exponentially, the wide halo as a diffusion.

## 4. Rendering: a ray march of the density

**Two marches, one image.**
- **Near.** Half resolution: from the camera to 60 yd or the scene, about 32 steps spaced
  quadratically (dense by the lens), with full detail and lamps.
- **Far.** Quarter resolution: from 60 yd to the scene or 2500 yd for the sky, about 32 steps spaced
  exponentially, levels L1 to L3 only.

Beyond the far march, the haze is added analytically. The near and far results are combined at half
resolution: `S = Sn + Tn * Sf`, `T = Tn * Tf`. The far part is upsampled with depth weights.

**Per sample.**
1. The finest level holding the point, blended with the next near its edges: fog and smoke. The
   level's ground map gives the height above ground.
2. The erosion noise, near only.
3. The light volume of the same level (section 5).
4. The lamp and indoor froxel grid, near only.
5. The analytic haze.

**Sampling.**
- **Jitter.** A blue-noise start offset per pixel, rotated each frame along an R2 sequence.
- **Empty-space skipping.** An occupancy mip per level (max density per 8³ cells) skips the full
  sample where it is empty.
- **Early termination.** Below 1 % transmittance.

**Temporal.**
- **Reprojection.** Each pixel stores its fog's representative distance: the transmittance-weighted
  mean distance of what it scattered. It reprojects through last frame's camera.
- **History rejection.** The history is clipped to the current 3 x 3 neighbourhood (mean plus or
  minus gamma sigma). It is dropped off screen, on a camera jump, and where the stored distance
  disagrees (disocclusion).
- **Blend.** About 12 % new per frame, more while the camera moves fast. No ghosting behind moving
  bodies (the clip), no crawling (blue noise plus R2, and a simulation that never pops).

**Two layers per texel.** Each low-resolution texel takes the nearest and the farthest scene distance
of its whole footprint. The ray is marched to the farthest and its state kept where it passes the
nearest, so a texel holding a branch against the sky carries the branch's fog and the sky's. The
combine gives each layer its far part from the quarter-resolution texel layer that matches its
distance (a quarter texel's footprint holds its half texels', so a match always exists); the temporal
pass reprojects each layer by its own distance and takes the history layer that saw the same surface,
point-sampled at silhouettes.

**Composite (D3D9, `WXL_GFX_ORDER_ATMOSPHERE`).** Full-resolution upsample from the four
half-resolution neighbours: of each, the layer whose distance matches the pixel's, weighted
bilinearly and by that match, so thin geometry against the sky keeps the sky's fog round it. Scene
depth decides:
- **World pixels.** World depth is 0..0.94 in this client; the scene decodes to linear, then
  `scene * T + S`, then re-encodes.
- **Sky pixels.** Near 1: the march has already run to 2500 yd plus the haze.

**Immersion.** The camera's own density (read back two frames late) softens the scene slightly and
adds a faint veil. Visibility falls by itself, because the near march samples wisps 0.2 yd from the
lens.

The client's own distance fog is pushed to the far clip while this fog draws.

## 5. Lighting

**Sun and moon.**
- **Light volume.** Per level at half resolution, rebuilt when the level steps, holding:
  - `tau_sun`: a cone-free, geometrically growing light march through the clipmaps towards the
    active body;
  - `tau_up`: fog overhead;
  - a terrain shadow: a heightfield march over the `floor` mips towards the body, soft by
    elevation.
- **Direction and colour.** The active body's direction blends the sun and the moon by their
  weights. Its colour is the engine's diffuse, with glare dimming undone, linearised.
- **Phase.** Dual-lobe Henyey-Greenstein: a forward lobe, a back lobe and a blend.
- **Powder.** `1 - exp(-2 sigma d)`: dark, soft edges facing the light.
- **Multiple scattering.** Wrenninge's octave approximation (a = b = c = 1/2, two or three octaves)
  on `tau_sun`, plus a diffusion term inside thick fog that fades as `1 / (1 + 0.75 tau_sun)`
  rather than exponentially. Thick fog glows through instead of going black.
- **World shadows.** On the near march, the engine's sun cascades (`wxl::game::shadows`, imported
  into the block) shade the fog: shafts through trees and between buildings. The cascades are
  rendered along the engine's own shadow light, whose elevation is exaggerated, so the shafts are
  steeper than the sun at dusk; a strength slider tempers them. Ridges and hills shade through the
  heightfield march, along the true sun.

**Sky ambient.** The engine's sky gradient (zenith and horizon), tinted towards the zone's fog colour
by a share. It is scaled by the baked sky share (hollows and valleys darker) and by the two-stream
diffuse transmittance `1 / (1 + k tau_up)`: dim bellies under deep fog, bright tops, never black.

**Lamps.** A camera-frustum froxel grid (160 x 90 x 64, exponential 0.5 to 250 yd, the light
service's cluster range) holds, per froxel, the in-scattered radiance of every lamp in its cluster:
- falloff and cone;
- the analytic profile;
- the baked cookie (cage bars, tinted glass);
- the omni shadow maps (beams through the mist);
- a flame's hot core;
- the phase towards the camera;
- room gating (a lamp stays in its room).

The march multiplies it by the local density, so a halo is exactly as thick as the fog around the
lamp. A torch in the hand is a carried light and lights the fog around the player. Lamps only use
the grid; the sun, moon and sky are evaluated per sample.

## 6. Indoor and outdoor

**Classification per sample.**
- **Indoor weight.** The lamp grid's alpha holds the indoor weight of each froxel, from
  wxl-graphics-lights' rooms: WMO interior group boxes, 3 rows each. The weight is a smooth step of
  the signed distance to the nearest box face, so outdoor fog seeps a short way (0.75 yd) inside.
- **No pops.** The grid keeps a reprojected history: a room box that appears or vanishes as groups
  stream fades over half a second.
- **Doorways.** Looking out of a doorway, the samples outside the box are outdoor, the ones inside
  indoor: street fog outside, stale air inside.

**Two media.**
- **Outdoor.** The simulated field, sun, moon, sky and outdoor lamps.
- **Indoor.** An analytic medium: dust of the indoor density, slow drifting noise, a low haze near
  the room's floor, no terrain rivers, ambient of its own and indoor lamps only.

A sample's medium is the mix by its weight.

**Transitions.** The camera's own indoor state is eased over 5 s (smootherstep). For samples within
a few yards of the camera, the weight is pulled towards that eased value while it lags. Stepping in,
the outdoor fog you carried thins out around you over 5 s; stepping out, it closes in. The far view
never waits.

**Profiles.** Outdoor keys `WXL_FOG_*`, indoor keys `WXL_FOG_INDOOR_*`. Each profile has its own
colour, density, noise and lighting.

## 7. The threat API (`include/wxl/GraphicsFogApi.h`, version 3)

The table keeps every earlier version's functions at their offsets and is published under versions
3, 2 and 1, so an older caller still finds it. Version 3 appends cascades (`AddCascade`,
`UpdateCascade`, `RemoveCascade`: a map, a spill point on a crest, a fall direction, a width, a bank
depth, a life).

**What v2 adds.** Handle-based, fading in and out, render thread:
- **Sources:** sphere, box, capsule or vertical cylinder that add, carve, hold density or add
  smoke, with falloff, billow, their own push, radial push and swirl, and a lifetime.
- **Fronts:** a fog wall advancing along a direction at a speed, or a ring closing towards a point.
  A front has height, depth behind it, a billowing edge and a push on the fog ahead.
- **Flows:** wind and flow volumes (uniform, radial, swirl) added to the velocity field.
- **Global:** `SetIntensity` (a fade to a new level), `Pulse` (breathing or a one-shot surge),
  `SetWind` (override, faded).
- **Queries:** `CameraDensity`, `Visibility` (read back from the GPU a few frames late),
  `ClearAll`.

All of them become simulation primitives: up to 128 per frame (API, wakes, trails, plumes, fire
heat), culled per level into 8 x 8 bins of bitmasks, and applied in the source pass.

**Built-in inputs.**
- **The wake fluid.** A 2D incompressible flow at the ground around the player (stable fluids:
  advect, splat, project with Jacobi iterations warm-started from the last step, vorticity
  confinement) in two toroidal levels of 256 cells, 0.25 yd and 1 yd, stepped at 30 Hz. A texel holds
  the air's velocity, the change made to the fog (cleared, piled, thickened) and the height it
  reaches. Up to 192 bodies are moving obstacles: the air inside moves with them, so the projection
  parts it ahead, turns it round them and closes it behind; a moving body clears a space two and a
  half times its own (at rest the fog closes in round it) and sheds eddies from alternate flanks.
  The change moves with the flow, so a wake refills by the fog flowing back in. The march multiplies
  the lying fog by it near the ground; the 3D velocity field takes its flow.
- **Projectile tunnels.** A missile carves a tube along its path, which holds for a moment, then
  closes by rolling in: inflow and a swirl round the tunnel's own axis. Burning missiles evaporate
  and leave smoke; icy ones leave fog. Near the ground they also drag the wake fluid.
- **Blasts.** An impact, or a new spell effect nearby, sends a ring through the wake fluid (blown
  clear inside, piled at its front) and a 3D sphere that blows out, then draws the fog back turning.
- **Smoke plumes.** Smoke emitters inject smoke with buoyancy. The plume rises, bends with the wind
  and spreads by advection.
- **Heat and frost.** Fire lights and fire emitters evaporate the fog around them; icy emitters add
  fog that sinks.
- **Body shadows in the fog.** A lamp without an omni shadow map (a carried torch by default, most
  lamps) is shadowed in the lamp grid by the 16 bodies nearest the camera: vertical capsules tested
  per froxel along the segment from the lamp, soft by the distance behind the body; a lamp inside a
  body is its own. The CPU gives each lamp the bodies within its reach. Where a map exists (units are
  casters of the core's omni maps), the map decides.

## 7b. Cascades

A bank of moist air behind a ridge spills over the crest and pours down the far side, warming and
evaporating as it falls.
- **The bake.** `5.tools/forever-bake` (`bake.py cascade`) scores every terrain vertex of a tile's
  3 x 3 neighbourhood as a spill point for 16 fall directions: a drop of tens of yards within 125 yd,
  a steep start, a crest (level behind), a reservoir (ground at the crest's height over most of the
  next 150 yd, not a mountain side) and a col (higher both ways along the crest). The best direction
  wins; ground up to 60 yd behind a spill point at its height is its reservoir. A8R8G8B8 per tile:
  spill score, reservoir weight, fall azimuth, drop.
- **At run time.** The tiles stream with the horizon maps into a block image. The rivers' depth pass
  fills each spill point and reservoir towards a bank depth (the wind blowing over the crest picks
  which pour; banks hold over half by day), and the shallow-water flow spills at the lowest rim and
  runs down. The layer carries a tracer, the share of its depth that came from a bank, moved by the
  same fluxes; that fog evaporates at a rate proportional to its descent speed.
- **Shape.** Cascade fog is drawn as a density current a few yards thick; where the layer runs fast
  the shaping noise is stretched along its fall, so it pours in long strands.
- **API cascades** add a bank behind their spill point and a push along their direction over 250 yd.

## 7c. Staying on the ground

Nothing of the fog's vertical placement follows the camera's height: the levels follow the terrain
under each cell, the atmosphere's reference is the terrain under the sample (the coarsest level's
ground above every level), and the ground under the camera (the haze's and the atmosphere's regional
base) is traced 3000 yd down, a miss keeping the last ground found.

## 8. Budget (1080p, RTX 3060, estimates)

| Pass | Estimate |
|---|---|
| Terrain tiles, floor mips (on arrival) | < 0.05 ms, rare |
| Transport layer (10 Hz, 1024²) | 0.02 ms/frame |
| Clipmap steps (4 levels, fixed rates) | 0.06-0.12 ms/frame |
| Light volumes (with the steps) | 0.05-0.1 ms |
| Lamp and indoor grid (lights present) | 0.1-0.25 ms |
| Near march (half res, 518k rays, ~32 steps, early out) | 0.8-1.2 ms |
| Far march (quarter res, 130k rays, ~32 steps) | 0.2-0.3 ms |
| Combine and temporal | 0.08 ms |
| Copies, composite (full res) | 0.15-0.2 ms |
| **Total** | **1.6-2.4 ms** |

Measured in the offline harness (same shaders, RTX 3060, 1080p, the game closed). Before the
atmosphere: 2.1-2.6 ms in all. With the atmosphere, two-octave fraying to 60 yd, the climbing veil
and far-step splitting, at 32 near steps: 2.8 ms at eye level (near 1.8-2.0, far 0.33-0.37), up to
3.8 ms looking down into the fog from 60 yd (near 2.9, far 0.5). The fraying costs about 0.5 ms of
that, the atmosphere 0.2-0.4. High now marches 28 near steps (about 0.25-0.35 ms less) and keeps a
little more history (0.9) instead. Rivers 0.06 per step (0.18 per frame at 20 Hz), the lamp grid
0.12-0.16 with lamps, temporal 0.2, copies 0.08; the D3D9 composite comes on top (about 0.2).

- **GPU memory:** about 110 MB: terrain block 25, transport 32, clipmaps 36, lamp grid 15,
  march targets 20, noise 8.
- **CPU memory:** under 1 MB, plus tile bytes in wxl-graphics-extend's asset budget until they are
  uploaded.
- **Presets.** Low, Medium, High and Ultra set steps, resolutions, grid sizes and simulation rates
  together.
- **Measurement.** Every pass is timed on the GPU (Vulkan timestamps, the composite by D3D9 queries)
  and shown in the panel.

## 9. Risks

- **Blind tuning.** The look is tuned in game by the user, so the shaders expose their ingredients:
  debug views and Isolate switches. The defaults are conservative, and a preset reproduces the
  "dead world".
- **Terrain-following near WMOs.** The floor is the baked terrain. Bridges and building floors do
  not lift the fog, and under the terrain (caves) outdoor fog fades out. Interiors use the indoor
  medium.
- **Unbaked maps.** Northrend and instances have no tiles. The floor then falls back to the ground
  traced under the camera: no rivers, only mist, banks and haze. Baking a map with
  `5.tools/forever-bake` (horizon and water) turns them on.
- **Room boxes.** wxl-graphics-lights keeps the 12 nearest rooms, so a street of houses may drop
  one far away (it fades, never pops). There are no portals: seep is uniform, not per doorway.
- **Cascade direction.** The engine's shadow light is steeper than the sun, so tree shafts do not
  follow a low sun exactly. Terrain shafts do.
- **Temporal.** A fast pan over thin wisps may show brief softness; rejection favours clean over
  smooth.
- **Numerical diffusion.** Coarse levels blur what they carry. The renewal and the render-time
  erosion restore detail, but a wake seen from 200 yd is soft.
