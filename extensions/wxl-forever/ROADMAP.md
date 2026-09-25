# wxl-forever roadmap

wxl-forever owns every effect improvement of the client. The fog is its first brick, not a
dependency: each feature must work and be tunable with the others off. wxl-core only reads the
engine (offsets and raw walks); decisions, content tables and debug views live here.

The guiding idea for pushing DX9: what the GPU cannot compute live is baked offline and stored in
textures (2D, 3D, cube), which shaders then read cheaply. The reference bar is Crysis 1 (DX9),
S.T.A.L.K.E.R. and GTA IV.

Legend: `[x]` done (built, deployed), `[~]` in progress, `[ ]` to do. `needs:` lists dependencies.

---

## 1. Foundations

- [x] World depth as a texture: INTZ redirect of the world pass (`OnWorldSceneBegin` depth override)
- [x] Pure device stripped at CreateDevice; d3d9 proxy pinned against the engine's FreeLibrary
- [x] World depth range [0, 0.94] handled (`gx::WorldDepthRange`); sky in [0.999, 1]
- [x] Per-pass GPU timers, shader slot counts logged
- [x] Panel in tabs, "(?)" help on every setting, Low/Medium/High/Ultra presets
- [x] Suite layout: `src/core` shared machinery, one folder per feature
- [x] Shaders as real files: HLSL sources, fxc at build time, runtime-HLSL dev mode, "Reload shaders"
- [x] Real BLS files: header decoded, writer (fxc output to .bls), reader through the client file
      system, byte-identical round trip on a client BLS; ours under `Shaders/*/Forever/` in Patch-4
- [x] Light system migrated into Forever (`src/lights`); wxl-lightdebug retired into a Lights tab
- [ ] Engine shader override pipeline: our BLS replace the engine's (terrain, MapObj, Model2,
      Particle, liquids), honouring the engine's constant and input contract. needs: BLS, contract RE
- [ ] WFX routing: point engine effects at our shaders, declare new effects
- [x] DDS loader: volume textures, cube maps, FP16/FP32 formats (`core/Dds`), and a baked-asset
      manager streaming manifest-keyed files within a budget (`core/BakedAssets`)
- [ ] Data-driven effect descriptors (an .fx-like file with annotated parameters that generate the
      panel sliders and help text)
- [ ] HDR pipeline: redirect the world colour target to FP16, like the depth. needs: nothing
- [ ] G-buffer: normals (and material bits) written as a second render target (MRT) by the
      overridden engine shaders. needs: engine shader override
- [ ] MSAA replacement, since INTZ forbids MSAA: SMAA or FXAA, later TAA on our reprojection

## 2. Fog

Done:
- [x] Froxel volume (160x90x64 default), exponential slices, atlas in FP16, block integration
- [x] Temporal resolve: per-froxel decorrelated jitter, reprojection, neighbourhood clamp
- [x] Blue-noise offsets everywhere, rotated per frame (no fixed lattice)
- [x] Baked 64^3 Perlin-Worley / billow noise with mips, domain warp, erosion, coverage
- [x] Profiles (outdoor, indoor) with smooth transitions; colour modes, brightness
- [x] Per-sample indoor/outdoor split from interior WMO group boxes (doorway correct)
- [x] Sun and moon scattering, dual-lobe HG and Cornette-Shanks, glare-safe celestial inputs
- [x] Self-shadowing, bank shading, powder
- [x] Realism: multiple scattering (octaves), linear light, sky-driven ambient, chromatic
      extinction, aerial desaturation, dither
- [x] World shadows: screen-space march through depth plus heightfield steps
- [x] Sky occlusion (heightfield horizon plus screen), fog banks (macro map), high haze layer
- [x] Tangibility: near wisps (in the volume's density, bent slices), ground following, valley pooling, flow layers,
      vertical roll, surface contact
- [x] Bodies carve the fog: capsules plus fading wakes with swirl
- [x] One medium: the horizon haze is the volume carried on past its far distance, the shafts are
      the world-shadow term, the immersion veil is the volume at the camera; native distance fog
      pushed out

Done since:
- [x] Lights in the fog, rework: soft-core inverse-square falloff, per-light phase, multiple
      scattering for halos, soft interior gating, clustered lights (64-128), the missing green
      Duskwood-style lamp
- [x] Projectiles carve the fog (engine missile list) with a closing, swirling tunnel; heat disperses
- [x] Camera immersion inside dense fog or smoke: veil, edge contrast loss, near-lens wisps
- [x] Smoke as volume: smoke/steam emitters become density plumes; optional smoke above fires

To do:
- [ ] Fog profile DBC: keyed by AreaTable and WMOAreaTable, plus Light.dbc-style placement
      (map, position, inner/outer radius, blended); columns proposed in README
- [ ] Server opcode: force or blend a profile (hordes, infection, scripted events)
- [ ] Time-of-day curves per profile (dense at dawn, thin at noon), Light.dbc-style keyframes
- [ ] Weather coupling: rain thickens the fog; wind taken from wxl-grasswind
- [ ] Fog on transparents: particles, water, spells sample the volume at their own depth.
      needs: engine shader override (Particle, liquids, Model2 blend modes)
- [ ] Underwater fog as a water profile of the same system
- [ ] Soft-particle fog cards near the camera (lower priority: the near wisps live in the volume)
- [ ] Death-screen fog override coexistence

## 3. Lights

- [x] Engine M2 lights collected (rare in content: about 100 of 20,000 models)
- [x] WMO MOLT lights read from the files and placed in the world (verified on Goldshire)
- [x] Model-light table: 536 lights on 491 models (lamps, torches, braziers, candles, hearths)
- [x] Missiles in flight and particle emitters exposed (engine lists)
- [x] Clustered light list as a Forever service, consumed by the fog and later the surfaces
- [ ] Review and extend the model-light table (colours, radii, flicker per family)
- [ ] Light flicker and animation for fire families (torches breathe, candles tremble)
- [x] Light cookies: each light's own fixture baked as a cube map (`5.tools/forever-bake`,
      `lights/Cookies`): model-table lights, every engine M2 light, every WMO MOLT light (its groups
      within 1.5 yd and the doodads beside it); resident atlas, per-light rotation in the cluster
      data, `shaders/cookies.hlsli`, multiplied in by the surface and fog light masks
- [ ] Cookies for fire emitters are baked (manifest kind `emitter`) but have no runtime light yet:
      extend the model table or gather emitter lights to use them
- [ ] IES light profiles as textures: realistic lamp distributions
- [~] Light occlusion: short screen-space test done; SDF later for off-screen walls
- [x] Room gating (`lights/Rooms`): each light and each receiver take the smallest interior WMO
      group holding them (every group of each map object drawn, culled ones included); a light
      reaches another floor only by `WXL_FOREVER_LIGHTS_ROOM_LEAK`, on surfaces and in the fog.
      A building made of one interior group still leaks between floors: the SDF below fixes that
- [ ] Player flashlight: spot with a cookie (gobo) texture, lights surfaces and fog, screen-space
      shadows; server toggles it (battery, broken). needs: surface lighting for full effect
- [ ] Gameplay lights from the server (flares, burning zombies, car headlights)

## 4. Surface lighting and shadows

- [ ] Dynamic lights on surfaces (light pre-pass, Crysis style): normals from the G-buffer, or
      reconstructed from depth until then. Light reaching a wall is attenuated by the fog between
- [ ] Sun and moon contact shadows on surfaces, sharing the fog's visibility pass
- [x] Engine ShadowMap investigation: the client has real sun shadow maps behind
      `extShadowQuality` (0-5; 5 = cascaded 40/80/320/1280 yd, off-screen WMO and doodad casters,
      terrain never casts). Four callback pointers (0x00D43158..64) are the extension points
- [ ] Test `extShadowQuality 5` + `hwPCF 1` in game with the fog on (caps check, cost, redirect)
- [x] Core binding: read-only access to the shadow maps, their matrices, light direction, centre
- [ ] Fog world shadows sample the engine's cascades (off-screen occluders), screen march on top
- [x] Omni shadow maps for point lights (core `wxl.omnishadows`, 4x2 R32F atlas): moving lights
      redraw all six faces, static ones round-robin, atlas cleared to lit
- [x] Forever consumes omni v2 (stable light ids, history rejection on `lastMovedFrame`)
- [x] Omni per-face tracking: a unit moving near a lamp redraws only the faces that see it; still
      faces refresh `_OMNI_REFRESH` a frame (3)
- [ ] Omni per-face caster culling at draw time: each face still re-issues the whole main caster
      list (WMO culled against the main shadow frustum, M2 not at all); needs the engine's frustum
      layout to pass a face frustum to RenderWmoShadow and a filtered M2 batch list
- [ ] Fog inject at half rate (checkerboard with the temporal history) or 128 x 72: to judge in game
- [~] Carried torch on its carrier: near-light cap and carrier scale done; units cleared from the
      buffer the engine's materials read (it is last frame's, reprojected); verify in game
- [x] Omni filtering: blocker search and penumbra-sized disc from the light's source size, bias in
      yards plus slope, face size control; a lamp's own cage among the casters at shadow quality 5
- [x] Combine the omni maps with the baked light cookies: the cookie multiplies in `LightMask`, the
      map keeps the live casters, and the four maps go first to lights with a unit near them, last
      to still lamps a cookie already shapes (`_OMNI_UNITS_FIRST`)
- [ ] Fog world shadows sample the engine's cascades (off-screen occluders), screen march on top;
      today the cascades are only read for their caster lists (the omni maps re-issue them)
- [x] Terrain as a shadow caster (`src/terrain/Caster`): after each engine render callback the
      resident horizon tiles draw depth-only into that map with the pass's own view, projection and
      viewport (core `shadows::DescribeRender`), 32 x 32 patches culled per cascade
- [x] Longer, lower sun shadows at dusk: the engine lifts the sun five times and floors it at 50
      degrees; core `wxl.shadowlight` lets the terrain feature hand back a gentler lift and floor
- [ ] Larger cascade extents (the engine's 40 / 80 / 320 / 1280 yd are fixed in CShadowCache::Create)
- [ ] GTAO / SSAO, also feeding the fog's sky occlusion
- [ ] Screen-space reflections (wet ground, water, metal)
- [ ] Specular and rim lighting on models. needs: engine shader override (Model2)
- [ ] Parallax occlusion mapping on terrain and walls from baked height maps
- [ ] Light probes (spherical harmonics) placed in the world: correct ambient indoors and in forests
- [ ] Approximate indirect light (one bounce) from a voxelised world

## 5. Baked data (the DX9 route to "fake ray tracing")

Offline tools in `5.tools/forever-bake` (readers for M2, skin, BLP, WMO, ADT, WDT; a CPU cube
rasteriser; a DDS writer; per-bake manifests; incremental rebuilds by input hash), output as loose
files under `Patch-4.MPQ/Textures/Forever/<BakeType>/`, streamed by `core/BakedAssets`.
- [x] Horizon maps per ADT tile: terrain shadows at any sun angle and sky occlusion, off screen.
      Bake (`bake horizon --map X`: L8 volume of 16 azimuths, sky share, R16F height per tile) done
      for Azeroth and Kalimdor; runtime service `src/terrain/Horizon` keeps a 4 x 4 block resident
      as wrapping atlases, `terrain/shaders/horizon.hlsli` gives `TerrainSunVisibility` and
      `TerrainSkyOcclusion`. Consumers to wire: fog world shadows and sky occlusion, surface rim
- [ ] Signed distance fields of WMOs and doodads per tile (3D textures): soft off-screen shadows,
      large-scale AO, light occlusion (true occlusion by floors and walls for lights without an omni
      map, replacing the room gating's approximation)
- [ ] Voxelised world (occupancy and colour): indirect light, fog pooling behind obstacles
- [ ] Light probes baked per area (SH)
- [ ] Detail normal and height maps generated from BLP textures
- [ ] 3D LUT converter (.cube from Resolve or Photoshop to a volume texture)
- [ ] Atmospheric scattering tables (Bruneton-style transmittance and in-scattering, FP textures)

## 6. Post-processing

needs: HDR pipeline for the full effect; each works in LDR with reduced quality.
- [ ] Auto exposure (eye adaptation entering dark buildings)
- [ ] Bloom with lens dirt, replacing the engine glow; lights glow through the fog
- [ ] Filmic tonemapping of the whole scene
- [ ] Colour grading with 3D LUTs per zone and time of day
- [ ] Film grain, vignette, subtle chromatic aberration
- [ ] Optional depth of field
- [ ] Gameplay screen effects from the server: frost at the edges when cold, desaturation and
      pulses with infection, blurred vision with hunger or thirst

## 7. Sky and atmosphere

- [x] Moon rendering in the fog, horizon haze band
- [ ] Physical sky from scattering tables, consistent with the fog's phase and colours
- [ ] Clouds driven by the same weather map as the fog banks
- [ ] Night sky polish (stars, moonlight colour)

## 8. Water

- [ ] Underwater fog profile, light shafts through the surface
- [ ] Depth-based water colour and absorption
- [ ] Caustics (flipbook textures) on the seabed
- [ ] Screen-space reflections of the fog and lights on the surface
- [ ] Shore foam

## 9. Weather

- [ ] Rain and snow particles lit by the light system and fogged by the volume
- [ ] Wet surfaces: darkening, puddle masks, reflections (SSR)
- [ ] Splash and ripple flipbooks
- [ ] Rain raises fog density through the profile
- [ ] Lightning flashes lighting the fog and the scene
- [ ] Wind shared by fog, grass, foliage and particles (from wxl-grasswind)

## 10. Vegetation and geometry

- [ ] Vertex texture fetch: grass and foliage bend with wind and part around bodies
- [ ] Detail normals on terrain

## 11. Performance and quality

- [x] Quality presets, per-pass timers, early-outs, half-resolution passes
- [x] Surface lighting at half resolution with a depth-aware resolve; a-trous skips settled pixels;
      two full-resolution targets fewer
- [ ] Scalability per feature (every feature has a Low path)
- [ ] Budget view: total frame cost of Forever at each preset

## 12. To verify in game

- [ ] Model-light positions (rings on lamp heads) and the table match counter
- [ ] WMO light placement inside buildings
- [ ] Particle emitter enabled flag meaning (if "disabled" is high, it reads inverted)
- [ ] Missile spell id offset against Spell.dbc
- [ ] Sun direction sign in the fog at dawn and dusk
- [ ] Room gating: interior groups of a building stack by floor as the inns' do (Duskwood, Goldshire)
