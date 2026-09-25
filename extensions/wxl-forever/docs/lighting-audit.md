# wxl-forever lighting audit (phase 1: analysis and cleanup)

Scope: the light service (`src/lights`), surface lighting (`src/surface`), the fog's lamp terms
(`src/fog`), post, and the core parts they lean on (`src/game/Lights.*`,
`src/client/CShadowCache/OmniShadows.cpp`, `src/client/CM2Scene/SceneLights.cpp`). Evidence comes
from the code, the fxc register listings of every shader, `2.clients/Logs/wxl-core.log` (session
21:17-21:23) and screenshots 25-28.

Symptoms: (A) a lamp goes dark or flickers in the fog depending on the view and the distance;
(B) indoors, with many lights, everything flickers instead of adding up.

## 1. Pipeline map (one world frame N)

| Stage | When | Reads | Writes / state across frames | Keys, UI |
|---|---|---|---|---|
| Rooms collect | world pass N, `Wmo.RenderGroup` hook | WMO groups **drawn** this frame | instance list (cleared each `OnWorldSceneBegin`) | - |
| Omni render (core) | world pass N, after the engine's main sun map | lights set by Forever at N-1; the **sun map's caster lists** (~40 yd around the player, culled by the sun frustum) | 4 R32F atlases 4x2 cells; per slot: id, anchor, per-face caster hash, faceFrame, round-robin cursor | `WXL_OMNI_SHADOWS`; budget and face size from Forever |
| Scene lights (core) | `CM2Scene::Animate` / `AddLight` | engine point lights | policy `off`: no point light reaches world receivers | `WXL_LIGHT_POLICY=off` |
| Gather | first pass of frame N (`lights::Frame`) | core `lights::Collect` (M2, MOLT), model table, instances, WMO placements | up to 512 found (walk order), fixture merge with pair memory (600 gathers), score `luma/(1+d²/r²)`, top 128, `g_tracked` fade 0.25 s, list sorted by chosen then weight*score | `WXL_FOREVER_LIGHTS_GATHER_*`, `_MERGE*`, `_FLICKER` |
| Moves, flicker | same | list positions | `g_seen` (pre-flicker), moving list; flicker scales intensity and moves Fire lights by up to ~0.06 yd/axis | `_FLICKER` |
| Rooms build | same | drawn instances' interior groups within 120 yd | 12 nearest (10 yd keep bonus), sorted by volume; `interior` and `room` per light (same `RoomOf` test) | `_ROOMS`, `_ROOM_LEAK`, fog `INDOOR_DETECT` |
| Cookies | same | manifest, `BakedAssets` stream | 32-cell atlas (pinned <30 yd, keep 120 frames), per record mean/tint, 4 mean reads in flight | `WXL_FOREVER_COOKIES_*` |
| Publish | same | list | light texture 128x4 (A32F); cluster texture 16x9x16 clusters x **16 slots** (16-bit) + cookie rows; list order = gather order | - |
| Surface Begin | before world pass N | last frame's light buffer | engine materials read it (c-rows via gbuffer constants) | `_ENGINE_MATERIALS` |
| Surface omni chooser | surface pass | list, units | 4 held slots (tiers: unit near > no cookie > cookie; camera room first; hold 60 frames, fade 18 frames); `SetLightsEx` (core renders them at N+1); `Get` returns maps rendered at N from N-1 positions; slot matched **by id** to list index (`OmniMaps.lightIndex`) | `_OMNI_*` |
| Surface passes | order 100 | depth, scene copy, G-buffer, light/cluster/cookie textures, omni atlases | normals (half res, blur), room (lighting res), omni share (4 channels), light (+ ratio MRT), temporal (acc/meta ping-pong), a-trous, omnifilter, resolve (light + engine buffers), gi (quarter res, own history), apply | `WXL_FOREVER_SURFACE_*` |
| Fog passes | order 200 | same light textures, `CurrentOmniMaps` | visibility (box/room per froxel, soft indoor), omni (1 texel per slot per froxel), lamps (cluster loop), inject, resolve (history), blocks, integrate, apply | `WXL_FOG_*` |
| Post | order 300 | scene, light buffer | bloom pyramid, eye adaptation history | `WXL_FOREVER_POST_*` |

Light identity today: the service's list index (reordered every frame), `LightId` (hash of owner
pointer, index, kind) for the omni slots, and in the surface light pass **the slot's rendered
position** (a light within 0.3 yd of it). The fog uses the list index through the omni table.

## 2. Mechanisms that can make a light appear, disappear, jump or switch

| # | Mechanism | Symptom | Evidence | Status |
|---|---|---|---|---|
| 1 | **16 lights per cluster, filled in list order** (`Clusters.cpp Assign`). The list is sorted by `weight*score`, score depends on the eye, so the dropped lights change as the camera moves; a cluster's cut is hard (no fade) and differs cluster to cluster (tile seams). A light whose sphere holds the camera covers **all 144 screen clusters** of its slices, so indoors nearly every light lands in nearly every near cluster. | B (strong), A in lamp-dense streets | code; overflow is only printed inside self-check warnings, none fired this session (outdoors, ≤19 lights) | confirmed mechanism, not yet measured indoors |
| 2 | **Rooms from drawn WMO groups, 12 nearest** (`Rooms.cpp`). Turning the camera culls a building and its rooms vanish; the 12-cap churns. A light's `interior` flag and room mask, and each pixel's/froxel's room, follow. The fog gates a light by it hard: interior lights light only indoor froxels, outdoor lights only outdoor ones (`lamps.ps`, `gate`). | A (lamp halo vanishes when its building is not drawn or a padded group box holds it), B (room gate 1 vs 0.1 flips) | log: "rooms entered the list" 10, 11, 31 per 5 s, 31 while turning at 139 px/frame | confirmed |
| 3 | **Fog screen-space light occlusion** (`lamps.ps LightOcclusion`, 2 steps, 2.5 yd thickness): anything on screen between a froxel and its light darkens the halo, including the lamp's own post. | A (view-dependent dark wedges) | code; screenshot 25 shows a dark band under the head along the post | confirmed mechanism, screenshot consistent |
| 4 | **Omni maps: casters are the sun map's lists** (~40 yd around the player, culled by the sun frustum; WMO casters culled with the sun frustum too, `RenderFaces` passes the main query's frustum). A lamp's map content changes with the player's position and view; its own post/arm/hood beyond `OMNI_SELF` 0.5 yd blocks whole directions; the fog reads it as a hard 0/1 per froxel. | A (dark wedges or a dark halo once the lamp takes a slot, i.e. near the player) | API doc + code | confirmed mechanism |
| 5 | **4 omni slots, tiered and held**: slots follow units near lights and cookies; a held light that leaves the candidates is freed **at once** (weight 1 → 0 in one frame); a slot switch also switches the surface's occlusion model (mapped: 2 yd contact march; unmapped: full-length screen march with 1.5 yd+ thickness). | A and B (pop of shadows, light through/behind walls changes) | log: 1-7 hand-overs per 5 s with only 3-5 lights in the list (the chooser only hands over then when a held light leaves the list) | confirmed |
| 6 | **Surface matches omni slot to light by position (0.3 yd), fog by list index**. Two lights within 0.3 yd (candelabra flames, unmerged M2+WMO pairs) both take the map on surfaces, only the chosen one in the fog. A reassigned slot keeps its previous light's position until re-rendered, so the old light can match a map with no rendered face for a frame. | A (surface and fog disagree) | code (`light.ps`, `Surface.cpp`, `Froxel.cpp BindOmni`) | confirmed disagreement |
| 7 | **Surface omni share reads exactly 1 everywhere**: every self-check of the session shows `omni share 1.000/1.000/1.000`, while the fog's maps carve beams (screenshots 25/26). The surface term may be inert (see 6) while the fog's is active. | A (fog dark where the surface stays lit) | log, 30+ probes | suspected |
| 8 | **Cookie unknown → known**: until a cookie's mean is read, its light publishes "no cookie" (factor 1; fog halo 0.6). Once measured (pinned within 30 yd first), the light drops to its mean (bakes block 60-80 %) and the fog halo to the pattern floored at 0.04. Past the 32-cell budget, lights swap between pattern and mean every ~2 s (keep 120 frames). | A (a lamp dims as you approach it, once per cookie), B (bars pop when >32 cookies wanted) | code (`Cookies.cpp`, `Clusters.cpp` row B, `cookies.hlsli`) | confirmed mechanism |
| 9 | **Omni per-face refresh and round-robin** (3 faces/frame of 24, moved/unit faces at once): static casters entering or leaving the sun map's lists as the player walks update one face at a time; unrendered faces read lit. | A (shadow pieces pop per face) | code | suspected (minor) |
| 10 | **Fire flicker moves the omni light**: the chooser passes post-flicker positions; the core re-renders all six faces once the light drifts 0.05 yd from its anchor and reports it as moved, which shortens the surface history around it. | A (grain and flicker around torches with a map) | code (`Service.cpp Animate`, `OmniShadows.cpp UpdateTracking`) | suspected |
| 11 | **Gather edges**: 512 found in engine walk order (not distance) before scoring, table lights get only what the engine left; 70 yd radius edge; culled models kept 600 frames; 0.25 s fades; >128 lights re-rank every frame. | A (a lamp absent until closer; in cities, arbitrary) | code | suspected (cities) |
| 12 | **Fixture merge pair memory**: stand-in light under the winner's identity when the winner drops; merge by kind only. | A (jump of a lamp's reach/colour) | log: one merge this session | suspected (minor) |
| 13 | **Omni one-frame lag**: maps at N are rendered from N-1 positions. | carried/moving lights only; carried lights take no map by default | code | harmless today |
| 14 | **Temporal**: surface history cut near moving lights and changed maps (4 nearest only); fog clamp and change cut. Hides nothing structural; amplifies 1, 5, 8 into visible flashes. | amplifier | code | confirmed as amplifier |
| 15 | **Policy off**: the engine no longer lights models with point lights, so a light dropped by 1 or 11 leaves models fully dark (no engine fallback). | amplifier | `WarcraftXL.cfg`, log line 9 | confirmed as amplifier |

Where systems disagree about which light is which: surface omni (position) versus fog omni (index)
(6); the core's slot order versus Forever's list (bridged by id, correct since the fallback was
removed); `interior` (fog gate) and `room` (room gate) computed by the same test but consumed as two
different gates (2).

Where a cap makes lights fight instead of blend: the 16-slot cluster (1), the 12-room list (2), the
4 omni slots with instant release (5), the 32 cookie cells (8), the 512 collect and 128 gather (11).

## 3. What was removed (behaviour unchanged)

24 files touched, 136 lines removed, 45 added (comments and replacements). Built with
`build.ps1`, compiled with unreferenced-symbol warnings (C4505, C4189, C4101): none left in
wxl-forever.

| Item | Where |
|---|---|
| `Light::tableRow` (written, never read) | `lights/Lights.hpp`, `ModelTable.cpp` |
| `table::RowOf`, `table::Family` (never called) | `lights/ModelTable.*` |
| `cookies::Atlas()` (never called) | `lights/Cookies.*` |
| `surface::SetExternalNormals` and `g_external` (never called) | `surface/Surface.*` |
| Omni compatibility fallback: `SetLights` path, `g_omniEx`, position matching of core slots | `surface/Surface.cpp` |
| Dead constant writes: `c21.zw` (the blur pass sets c21 itself), `c128.x = 512` (overwritten when maps exist, unread otherwise) | `surface/Surface.cpp` |
| Sampler s7 unbound before the light pass (the shader has no s7) | `surface/Surface.cpp` |
| Isolate "Full resolution lighting" (same as Lighting resolution: Full; the PassUv bug it checked is fixed, self-check distance off 1-2 %) | `surface/Surface.*` |
| `froxel::ResetHistory` (never called) | `fog/Froxel.*` |
| Noise bound on s3 for the apply pass (apply has no s3) | `fog/Froxel.cpp` |
| Omni table texel 1 xyz (atlas scale, face size: never read) | `fog/Froxel.cpp`, doc in `omni.ps.hlsl` |
| Second `lights::Frame` call per fog frame (a cached no-op) | `fog/Fog.cpp` |
| Legacy interface name `wxl.fog.inputs` (no consumer) | `fog/FogInputs.*` |
| `post::SetHdrScene`, `post::SetLensDirt`, `g_hdr`, `g_dirt`, the "Lens dirt" slider and setting, `hdrTex`/`dirtTex` samplers and branches (never set; HDR world uses its own path) | `post/*`, `composite.ps`, `lumlog.ps`, `post.hlsli` (c3 is now `viewC`) |
| Omni API alias published as version 1 (no v1 consumer) | core `OmniShadows.cpp`, `OmniShadowsApi.h` |
| Stale docs: README (lens dirt, SetHdrScene, SetExternalNormals, old interface name), `common.hlsli` (c18, c92, 16-bit cluster lists), `surface.hlsli` (c129 was undocumented), `apply.ps` (c30.z), a stray comment in `Fog.cpp` | docs |

User config: every key in `wxl-forever.cfg` and `WarcraftXL.cfg` is read, so no key was removed.
Two stale comments were corrected (highlight cap 2x, wash log cadence). Backup:
`scratchpad/baseline/wxl-forever.cfg` (and the whole pre-cleanup tree under `scratchpad/baseline/`).

Kept on purpose, dead or unused but outside the lighting fix: terrain services with no consumer yet
(`horizon::Bounds`, `caster::ReleaseBuffers`), `baked::Budget/WhiteCube/TilesAround`, and
`baked::Shutdown` (never called: its worker thread is never joined, worth a look). Core generic
bindings (`PointLight.ambient/falloff`, scene-lights influence policy, `WXL_DIAG_LIGHTS` orbs) are
live code for other settings. The isolates were kept: phase 2 will use them.

## 4. Register and sampler maps after the cleanup (from fxc listings)

```
surface.light [1792 slots]
  c: c0-3 inv, c4 screen, c5 range, c6 lightsC, c7 clusterC, c8 clusterD, c9 shading, c10 c11 c13 vp,
     c14 albedoC, c15 halfC, c22 blueRot, c23 sourcesC, c26-41 units, c42 unitsC, c45 resC,
     c46-48 viewRows, c49-52 omniLights, c53-70 omniRows (slot 0, faces view), c125 shadowC, c127 toneC,
     c128 faceC, c129 omniSizes, c130-132 cookieC/D/E, c149 omniC, c151 omniCell, c152 carriedC,
     c153 movingC, c158 unitsGiC, c195 omniFade
  s: s0 depth, s1 normals, s2 scene, s4 G-buffer, s5 room, s6 omniVis, s8 lights, s9 clusters,
     s10 blue noise, s11 omni0 (faces view), s15 cookie atlas
surface.omni [1672]  c: c0-5, c15, c23, c45, c46-48, c49-52, c53-124, c125, c128, c129, c149, c151, c195
                     s: s0, s1, s4, s10, s11-s14
surface.room [195]   c: c0-5, c45, c158, c159-194 roomRows         s: s0
surface.temporal [377] c: c0-5, c16 c17 c19 prev, c20 moved, c26-42 units, c43 temporalC, c45, c153, c154-157 movingLights
                     s: s0, s1 normals, s3 raw, s4 previous normals, s5 history, s6 meta
surface.atrous [105] c: c44 atrousC, c45                           s: s1, s3, s6
surface.omnifilter [70] c: c44, c45                                s: s1, s6 meta, s7 ratio
surface.resolve [213] c: c0-6, c26-42, c126 lowC, c127, c128, c131, c153   s: s0, s3 light, s6 meta, s7 ratio
surface.gi [274]     c: c0-6, c9, c10 c11 c13, c14, c22, c25 giC, c45, c127, c150 engineC
                     s: s0, s1, s2, s3 light buffer, s6 G-buffer, s10
surface.apply [458]  c: c0-6, c9, c14, c16 c17 c19, c20, c24 quarterC, c25, c26-42, c127, c131, c150, c158
                     s: s0, s2 scene, s3 light buffer, s4 gi, s5 gi meta, s6 G-buffer, s7 engine buffer, s11 omni0 (atlas view)
surface.normals [229] c: c0-5                                      s: s0
surface.blur [78]    c: c15, c21 blurC                             s: s1
surface.probe [501]  c: c0-9, c14, c25, c45, c127, c153            s: s0-s9, s12
fog.visibility [1019] c: c0-3, c5-8, c14, c18, c19, c44-79 boxes, c92, c178-179 horizon, c182, c191-194, c196-199, c204
                     s: s0 depth, s1-s3 horizon, s7 ground, s10
fog.omni [345]       c: c0-3, c5, c6, c8, c192, c221 omniFogC      s: s4 omni table, s10, s11-s14
fog.lamps [655]      c: c0-3, c5-8, c15, c16, c19, c20-43 profiles, c92-94, c177, c187, c191-194, c196, c221-223
                     s: s0, s1 omniVis, s4 omni table, s5 vis, s8 lights, s9 clusters, s10, s15 cookie atlas
fog.inject [2108]    c: c0-6, c8, c14-19, c20-43, c116-175 volumes, c182-185, c187-190, c192, c199-208, c213-215
                     s: s1 lamps, s2 trail tiles, s3 noise, s5 vis, s6 trails, s7 ground, s8 lamps halo, s10
fog.resolve [314]    c: c0-7, c9 c10 c12, c13, c19, c192, c216-220 moving      s: s1 history, s4 raw
fog.immersion [212]  c: c5, c6, c192, c211                         s: s1, s2
fog.blocks [75]      c: c5, c6, c191, c192                         s: s1
fog.integrate [105]  c: c5, c6, c191, c192                         s: s1, s2
fog.apply [710]      c: c0-8, c13, c14, c20, c44-79, c176, c182, c185-187, c191, c192, c209, c211
                     s: s0, s1 history, s2 integrated, s4 immersion, s7 ground, s8 scene, s10
post.bright [112]    c: c0, c1, c8          s: s0 scene, s1 light buffer
post.down [29] / lumdown [27]  c: c0       s: s0
post.up [52]         c: c0, c7              s: s0, s1
post.lumlog [95]     c: c0, c5              s: s0            (was c3 + s1 hdrTex)
post.lumavg [39]     c: c0, c10             s: s0
post.adapt [16]      c: c6                  s: s0, s1
post.composite [136] c: c0, c2, c3 viewC, c4, c5, c9   s: s0, s1 bloom, s3 adapted   (was s2 dirt, s4 hdrTex)
lights.cookiedebug [8] c: c0                s: s0
```

Every register a shader reads is written by its pass; the remaining unread writes are the zero
rows the code documents as free (fog c212, surface c133-c148 unwritten).

## 5. Fix plan (for approval; nothing of it is implemented)

Goal: every gathered light always contributes; indoors lights add up; one identity shared by the
core and Forever; budgeted extras (shadow maps, cookie patterns) fade in and out slowly and never
remove the base light.

1. **One identity, one order.** The service gives each light a stable id (`LightId`) and publishes
   the list sorted by id, so equal sets give equal lists. The service, not surface, owns the omni
   slot table (list index → slot, weight) as a texture; surface's light pass reads it like the fog
   does, and the 0.3 yd position match goes. Omni positions are passed pre-flicker (the anchor
   `TrackMoves` already keeps). Removes 6, 10 and half of 7. Cost: nil.
2. **Clusters without dropping.** Assign by sphere against each cluster's box (not "whole screen
   when inside"), then per-cluster variable-length lists (offset + count into an index texture) up to
   the full 128. A hard safety limit stays, chosen by id (stable) and logged. Removes 1. Cost: CPU
   assignment ~0.2-0.4 ms for 128 lights over 2304 clusters (per-slice early-outs); GPU cost scales
   with lights actually overlapping a pixel: outdoors unchanged, a 20-30 light room roughly
   1.5-2x today's light pass there (estimate: +0.3-1 ms at half resolution, +0.2-0.5 ms fog lamps).
3. **Shadow maps as a bonus.** Slots fade in and out over 0.5 s and are never freed at once (a
   leaving light fades its map out first); every light uses the same short contact test whether
   mapped or not, so a slot change no longer switches the occlusion model; the fog reads the map
   with 4 taps instead of a hard texel; lamps with a cookie keep their housing in the cookie and take
   maps only for units near them. Removes 5, softens 4 and 9. Cost: fog +3 fetches per mapped light.
4. **Rooms that do not depend on the view.** Build rooms from WMO placements within reach (not from
   drawn groups), keyed by identity with hysteresis, no 12-cap churn; derive `interior` from `room`
   (drop the `SetInteriorTest` indirection); make the fog's indoor/outdoor gate soft across the box
   edge for lights too. Needs a core binding: `wxl::game::lights::CollectWmoPlacements` must expose
   the placement's `CMapObj` root (or `wmo::GetGroupInfo` by placement) so undrawn buildings give
   their groups; asked for wxl-forever (lights service). Removes 2.
5. **Drop the fog's screen-space light occlusion** (the lamp's own post and any foreground object
   carve its halo by view). Walls are handled by rooms (4) and maps (3). Removes 3. Cost: saves time.
6. **Cookies without a step.** Bake each cookie's mean and tint into the manifest
   (`5.tools/forever-bake`, one column pair), so every light has its energy from the first frame;
   residency cross-fades mean → pattern over 0.5 s; eviction does the reverse. Removes 8.
7. **Gather by distance.** Collect all candidates within the radius and keep the nearest by score
   (no walk-order cut at 512), fades of ~1 s and hysteresis at the 128 edge. Removes 11 in cities.

Order: 1 → 2 → 5 → 3 → 6 → 4 → 7 (the first three remove the confirmed indoor flicker and the
surface/fog disagreement; 4 needs the core binding agreed first). After each step: the self-check
lines, the "stability over 5 s" line (hand-overs, rooms), and a cluster overflow counter logged
every 5 s.

## 6. Build and deploy state

- Scratch build (`cmake -S . -B scratchpad/build`, CLIENT_PATH in the scratchpad) while the game
  ran: green, before and after the cleanup; a second scratch build with C4505/C4189/C4101 warnings
  on for wxl-forever: no warning.
- Wow.exe then closed; `build.ps1` built and deployed to `2.clients` (WarcraftXL.dll, d3d9.dll,
  every extension DLL, all Forever BLS shaders): green. The core DLL includes whatever else is
  uncommitted in the working tree.
- No commit.
