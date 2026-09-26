# wxl-host

A 64-bit companion process that takes memory work out of the 32-bit `Wow.exe`. It serves every
file the client reads, keeps backing copies of GPU resources, and runs data jobs. It is part of the
core: `wxl-host.exe` is built from `src/host/`, and extensions reach it through the versioned C API
`wxl.host` (`include/wxl/HostApi.h`).

Kill switches, in `WarcraftXL.cfg` or the environment: `WXL_HOST=0` (no host at all),
`WXL_HOST_FILES=0` (the host runs, the client keeps its own archives), `WXL_HOST_TEXTURES=0` (the client
reads and decodes its own textures), `WXL_HOST_PREFETCH=0`, `WXL_HOST_BACKING_REFS=0`, `WXL_HOST_CACHE_MB=0`
(no cache, no prefetch, no references), `WXL_D3D9EX=0` (classic D3D9 device, runtime-managed pool).
Every tuning key is listed in `docs/WarcraftXL.cfg.example`.

## Processes

```
Wow.exe (x86, LAA)                                     wxl-host.exe (x64)
  WarcraftXL.dll                                         dispatcher     (enkiTS thread 0: rings, spin, doorbell)
    runtime/host    HostClient: spawn, channels, TLSF    workers        (reads, texture images, jobs)
    engine/storage  HostStorage: the archive layer       store thread   (store operations, in arrival order)
                    HostTextures: BLP loads              prefetch       (background priority: reads ahead)
                    HostPrefetch: where the client goes  heartbeat      (100 ms; exits when Wow.exe is gone)
    engine/diag     MemoryDiag: the memory report        cache          (files and decoded textures, LRU)
  d3d9.dll          Managed: the D3D9Ex managed pool --- backing copies --> store (TLSF over pagefile sections)
  extensions        wxl.host
```

The client spawns the host at its first archive mount, before the client's archive set exists:
suspended, assigned to a Job object with `KILL_ON_JOB_CLOSE`, then resumed. The host also watches
the client's process handle. One host per client process; every name carries the client PID.

## Memory layout

All shared memory is pagefile-backed (`CreateFileMapping(INVALID_HANDLE_VALUE)`).

| Section | Created by | Size | Mapped by Wow.exe |
|---|---|---|---|
| `Local\wxl-host-<pid>-ctl` | client | 0.5 MB | always |
| `Local\wxl-host-<pid>-xfer` | client | `WXL_HOST_WINDOW_MB`, default 32 | always |
| `Local\wxl-host-<pid>-big-<n>` | client, per payload above `WXL_HOST_BIG_KB` (4096) | payload size | while the file is open |
| `Local\wxl-host-<pid>-store-<n>` | host, 256 MB each, up to `WXL_HOST_STORE_MB` | can exceed 4 GB in total | never |

The client creates everything it maps, so a host crash invalidates nothing the client holds: a file
already served stays readable while the host restarts.

**Control section.** A 4 KB header (magic, protocol version, the size of every shared struct so a
mismatched build is refused, PIDs, generation, states, heartbeat, doorbell, host counters), then 16
channels. Every field is fixed-width, offsets are 64-bit, there are no pointers, and both builds
compile the same header (`src/ipc/Protocol.hpp`) whose `static_assert`s pin sizes and offsets.

**Transfer window.** Owned by the client: its TLSF control block lives in client memory, and only the
client allocates or frees. The host writes only inside blocks it is handed (file bytes, job output)
and reads blocks the client filled (backing uploads). Single ownership means no cross-process lock and
nothing to repair after a restart. Blocks are named by their window offset. A read whose size is not
known yet gets a block from a per-name size hint (256 KB by default) and is shrunk in place.

**Store.** Owned by the host: TLSF over 256 MB sections created as it grows. Values are LZ4 when
written; the store thread moves values untouched for a minute to zstd when idle.

**Cache.** Host heap, `WXL_HOST_CACHE_MB` (1024): files the client streams (`.blp .m2 .skin .anim .wmo
.adt .bls .wdt .wdl`) and decoded texture images, least recently used first. Keyed by where a lookup
resolved (the archive holding the bytes and the name in it; a folder file adds its write time and size), so
the client's priority order still decides which copy is read. A prefetched entry never evicts one the
client used in the last 20 s.

## Protocol (version 3)

Two rings per channel, rigtorp-style SPSC: power-of-two slot count, each index on its own cache line,
each side caching the other's index. Request slots are 384 bytes with a 320-byte inline name; response
slots are 64. Every response carries its request's ticket.

| Op | Request | Response |
|---|---|---|
| `Ping` | | host time |
| `Mount` | name, priority, parent archive | archive id, kind (MPQ, folder, view in parent), client error |
| `Unmount` | archive id | |
| `Stat` | name, archive or any | size, archive |
| `Read` | name, archive or any, window block or big section, capacity | size, archive; `NeedMore` with the size |
| `Read` + `kFlagTexture` | the same, and the device's largest texture edge | the texture image, its kind (file or decoded), served-from-cache and prefetched flags, host µs |
| `Hint` | map folder, focus x/y, velocity, view distance, map id | (fire and forget) |
| `StorePut` / `StoreGet` | key, window block, codec | size |
| `StorePatch` | key, bytes at an offset, or rows at a pitch | |
| `StoreDrop` | first key, count | |
| `Job` | kind, input and output blocks (or a name to prefetch into the cache) | output size |
| `Stats` | | counters written into the header |

The header also carries the cache size and the prefetch and backing-reference switches, and the host's
counters for textures, the cache, prefetch and references (`HostCounters`).

## Threads and waiting

- A client thread takes one of channels 1-15 on its first call and keeps it; a channel whose thread
  has exited is reclaimed; past 15 threads, channel 0 is shared behind a lock.
- Replies are drained into a ticket table under a per-channel consumer lock, so any thread can wait
  on any ticket. A caller that gives up hands its buffers to the ticket, freed when the reply comes.
- Waiting is an eventcount on both sides: spin `WXL_HOST_SPIN_US` (20 µs), publish a "sleeping" flag,
  re-check, then wait on a Win32 event. The other side signals only when the flag is set.
- StormLib handles are not thread-safe: each host thread opens its own handle per archive, lazily,
  with the listfile and attributes skipped (lookups go by hash).

## Lifecycle

1. **Start**: create the sections and events, write the header, spawn, wait for `Ready` up to
   `WXL_HOST_START_MS` (4000). The host checks magic, version and every struct size, else refuses.
2. **Heartbeat**: the host bumps a counter every 100 ms; the client's monitor checks every 250 ms. A
   dead process or a counter frozen for 2 s marks the host down.
3. **Restart**: up to 3 per minute. The old process is terminated first, its tickets fail, the rings
   are reset, a new generation starts, and every mount is replayed in its original order and priority.
4. **Give up**: after 3 restarts within a minute, the session carries on native (below).

## Fallback

The client never hangs and never crashes on the host's account: every call has a timeout, and a
failed call only fails itself.

- Host off (`WXL_HOST=0`, `wxl-host.exe` missing, refused, or not started in time): the archives mount
  natively from the first one, and the managed pool keeps no backing copies.
- Host lost after it took over the archives: opens wait out a restart; if the host is given up, every
  recorded archive is mounted natively at its recorded priority, the client's archive slots are
  patched, and everything continues native (`storage: ... mounting N archives natively`).
- Extensions get `WXL_HOST_E_UNAVAILABLE` and take their own path.

## The archive layer

The client's own logic still decides which archives to mount, in which order and at which priority
(locale, patch-N, the `Patch-N.MPQ` folders, alternate views, the required-archive gate). Only the
mount primitive changes hands, so the patch chain is the client's own. Its order, read from the
client's archive tree comparator: **the higher priority wins**, a later mount wins a tie. Patches
mount at 0x40 and up, base archives at 0x3F and down, so `Data\Patch-4.MPQ` wins over everything.

| Client function | Address | Replaced by |
|---|---|---|
| `Blizzard::Mopaq::SFileOpenArchive` (from `SFile::OpenArchive` only) | 0x0045C480 | `Mount` |
| nested open relative to a parent archive | 0x0045C5C0 | `Mount` with the parent |
| `Blizzard::Mopaq::SFileCloseArchive` | 0x00458980 | `Unmount` |
| `SFile::OpenEx` (end of the open chain, priority 1000) | 0x00424B50 | `Read` into a buffered handle |
| `System_SFile2::FindFile` | 0x00424780 | `Stat`, after the client's own loose-file lookup |
| `SFile::FileGetIsLocalAmount` | 0x004217E0 | `Stat` (everything is local) |
| archive-only existence test (loading screens) | 0x00421FC0 | `Stat` |
| `SFile` size / read / seek / close | existing detours | the mapped bytes |

Loose-file overrides (open flags 1 and 2) and absolute paths stay native. Archive enumeration
(built-in AddOns, macro icons) reads `(listfile)` through `OpenEx` on a specific archive, which
`Read` serves from that archive. Host handles are the client's own 0x30-byte buffered handle (kind 5)
from a 32768-slot slab, so `IsHostHandle` is a range test and any client path that switches on the
kind reads them correctly. Folder archives are checked on disk at every lookup, so a file written into
`Patch-4.MPQ` after start-up is found.

The survey and patch-download paths mount their own archives natively: they hand the archive to
functions this layer does not serve.

## Textures

How the client loads a BLP (Wow.exe 3.3.5a 12340; `offsets/engine/Texture.hpp`):

| Step | Address | What it does |
|---|---|---|
| `TextureCreate` | 0x004B9760 | cache lookup by name, then `CreateBlpTexture` (0x004B8BE0) |
| async create | 0x004B8A50 | opens the file, sizes a read object by SFile's file size, queues it |
| read issue | 0x004B64E0, poll 0x004B69E0 | `SMemAlloc(size)` (Texture.cpp:0x809), within a 4 MB in-flight budget |
| disk thread | `AsyncFileReadThread` 0x004BA680 | one `SFile::Read` of the whole file into that buffer |
| completion | 0x004B7E80 (main thread) | the loader, then closes the file and frees the buffer (Texture.cpp:0x7DD) |
| loader | `PumpBlpTextureAsync` 0x004B7BD0 | header, formats, mip table, `GxTexUpdate` |
| header | `CBLPFile::Source` 0x006AE900 | copies 0x494 bytes (magic to palette) from the buffer |
| formats | `GetTextureFormats` 0x004B5FE0 | preferred format and alpha depth to a pixel format and a texture format |
| mip table | `CBLPFile::LockChain2` 0x006AFFD0 | DXT and ARGB: in place, `table[i] = buffer + offset[i]`; palettized: `DecompPal` into the boot scratch (`kMipTablePtr`) |
| upload | `CGxDeviceD3d::ITexUpload` 0x006A2D80 | per level from the base mip: the callback (0x004B5E80) returns `table[mip]`, `Blit` (0x006AE7C0) into the level's lock |

Palettized files decode to ARGB8888 (`DecompPalFastPath` 0x006AE990 for 8-bit alpha, `DecompPalARGB8888`
0x006AE9E0 otherwise); `Blit` then converts to the texture format (RGB565, ARGB1555, ARGB4444) as it writes the
lock. DXT is copied as is (`Blit_Dxt1_Dxt1`). The texture resolution setting (`baseMip`, device +0x350) is applied
by `ITexWHDStartEnd` (0x006A5EF0): the texture is created smaller and the upload starts at that level. A texture
larger than the device's largest edge (caps +0x6C) has levels skipped by `RequestImageDimensions`. In Wow.exe the
cost of a load is the heap buffer of the file's size, the palettized decode on the main thread, and the upload.

With the host (`HostTextures`), `TextureCreate` arms its thread, and the archive layer's next `.blp` open gets a
texture handle: it reports 0x494 bytes, so the loader's heap buffer is the header only. Its read fetches the
texture image from the host (`Read` + `kFlagTexture`) into the transfer window: the file itself, or for a
palettized file with a full mip chain that the device holds whole, a decoded image (the same header with
compression 3 and ARGB8888 levels, `host/Blp.cpp`). The header the loader copies has each level's offset
rebased so that `buffer + offset` lands, modulo 2^32, on the level in the window; the loader then takes its
in-place path and `ITexUpload` blits from shared memory into the staging lock. The completion closes the file
after the upload, which releases the window block. Formats, texture flags, `baseMip`, atlases and the upload's
own conversions are all the loader's, unchanged.

A texture falls back to the native read per file: no host handle (loose override, native mode), a registered
`.blp` client transform, a file shorter than a header, or a failed host read (a plain host read, then the
client's own open). Texture images in the window are held to `WXL_HOST_TEXTURES_INFLIGHT_MB` (12): the loader's
own 4 MB budget counts the 1172-byte headers, so the disk thread waits (up to 500 ms) for uploads to catch up.

Verify mode (`WXL_HOST_TEXTURES_VERIFY=1`) reads every served texture again as a plain file and compares: the
file kind byte for byte, a decoded image level by level against the loader's own decode (`CBLPFile::Source` +
`LockChain2` into a private chain). A mismatch is logged and that texture is read natively.

## Prefetch

The client sends a `Hint` four times a second (`HostPrefetch`, on `OnUpdate`): the streaming focus
(`kFocusPos`, the point `CMap::PreUpdateAreas` 0x007B5950 loads tiles around), its velocity, the view distance
and the map folder. The host queues the tiles within the view distance plus one tile of where the focus will be
in 0, 2, 5 and 10 s (`CMap::LoadArea` 0x007D9A20 names them `<folder>\<map>_<y index>_<x index>.adt`). Every tile,
model and map object read, by the client or ahead of it, queues what it names: a tile's `MTEX`, `MMDX` and `MWMO`,
a model's textures and first skin, a map object's textures, doodads and groups.

One thread reads the queue in priority order (sooner and nearer first, what the client just read before any
hint) at background CPU and I/O priority, into the cache (textures as their texture image), and waits while
every host worker is serving the client. A file queued or read in the last three minutes is not queued again.

## GPU memory

The proxy gives the engine a D3D9Ex factory and device (`Direct3DCreate9Ex`, `CreateDeviceEx`; a
refused `CreateDeviceEx` falls back to a classic device). D3D9Ex refuses `D3DPOOL_MANAGED`, so managed
requests are created in DEFAULT and their locks emulated.

- **Per-class vtable hooks, not wrapper objects.** The engine, the core and extensions keep raw device
  and resource pointers, swap device vtable slots, compare surfaces and redirect depth. Wrappers would
  need unwrapping in some 40 device methods on every draw and would break pointer identity. Hooks
  touch only creation, lock, unlock, level lookup, `GetDesc` (which still reports MANAGED) and release,
  never the draw path. The cost: an object cannot be swapped, so a device removal is answered by
  refilling the same objects.
- **Engine textures** (2D and cube, created from `Wow.exe`): the engine only writes whole lock rects
  (`CGxDeviceD3d::ITexUpload` blits the full dirty rect, then calls `PreLoad`). A lock hands out a level
  of a pooled SYSTEMMEM staging texture of the same shape; the unlock uploads the rect with
  `UpdateSurface`. No system-memory copy stays. The staged rows go to the host as the backing copy.
- **Engine buffers**: a lock hands out pooled heap memory; the last unlock writes it through and backs
  it up. Nested locks are written through together.
- **Everything else** (core, extensions, all volume textures, non-engine buffers): a SYSTEMMEM or heap
  mirror uploaded with `UpdateTexture` or a write-through, exactly what MANAGED did. Their lock patterns
  are unknown (partial writes, reads), so they keep the copy; they are few and small.
- `GetDesc` / `GetLevelDesc` report MANAGED to every caller but the D3D runtime itself: the runtime
  validates `UpdateSurface` through the same vtables and refuses a destination it sees as MANAGED.
- A read lock on an engine resource is served from its backing copy.
- An API that reads a surface on the CPU refuses a DEFAULT one. The engine's hardware cursor is the case
  that matters: `CGxDeviceD3d::ICursorCreate` makes a 32x32 A8R8G8B8 MANAGED texture and `ICursorDraw`
  hands its level surface to `SetCursorProperties`. The proxy hooks that call (device slot 10): the texture
  is promoted once to a SYSTEMMEM mirror, filled from what the GPU holds (else its backing copy), and the
  runtime gets the mirror's surface. The d3d9 proxy log names the path (`d3d9ex: cursor: ...`).
  Screenshots are unaffected: `DeviceReadPixels` reads the lockable back buffer or a lockable render target.
  `UpdateSurface`/`UpdateTexture` sources, `GetRenderTargetData` and `StretchRect` never accepted a MANAGED
  surface either, so no caller depends on it.
- `D3DERR_DEVICEHUNG` / `REMOVED` from `Present` is reported to the engine as `D3DERR_DEVICELOST`, and
  `TestCooperativeLevel` answers `D3DERR_DEVICENOTRESET`, so the engine runs its own reset; after it,
  every emulated resource is refilled from its backing copy or mirror.
- **Backing references.** When the host serves a texture image read from an MPQ, it indexes the content hash
  of each level a GPU copy of it holds verbatim (DXT levels, and ARGB8888 levels uploaded as ARGB8888). A
  backing copy written whole that matches an indexed level byte for byte (checked against the cached image,
  not just the hash) is kept as a reference to that file: no bytes. Reading it back, for a device loss or a
  read lock, rebuilds it from the cache or the archive (re-decoding a palettized image). A later partial write
  turns it back into bytes. Folder files are never referenced, since they can change on disk.
  `WXL_HOST_BACKING_REFS=0` keeps every copy as bytes. Nothing changes on the proxy's side.

The client's own `CGxDeviceD3d9Ex` backend (`gxApi d3d9ex`) was considered and rejected: it keeps a
CPU copy of every texture (`CGxD3d9ExTexture`, `SMemAlloc` at +0x3C), and its device layout differs
from the `CGxDeviceD3d` every core binding reads.

## API (`wxl.host`, version 1)

`WXL_Api::GetInterface("wxl.host", WXL_HOST_API_VERSION)`, or `WarcraftXL.dll!WXL_GetHostApi` from
outside the extension loader (the d3d9 proxy). Plain C, `__cdecl`, `structSize` first.

- `IsAvailable`, `GetStats`
- `ReadFile` (sync), `ReadFileAsync` then `Poll` or `Wait`: a whole file as a zero-copy view; `Release` it
- `FileExists`
- `Alloc`, `Release`: a shared block both processes see
- `StorePut`, `StorePatch`, `StorePatchRows`, `StoreGet`, `StoreDrop`, `StoreDropRange`: park bytes in
  the host under a 64-bit key (the core uses keys with the top bit set)
- `SubmitJob`: LZ4 and zstd, compress and decompress; `PREFETCH` a file

`wxl-forever` reads its baked assets and shader files through it (`src/core/HostFiles.hpp`).

## Counters and the memory report

`engine/diag/MemoryDiag.cpp` logs `memory[...]` lines at world entry, every 30 s and at each new peak:
the address-space map by type (image, mapped, private, free, largest free block), every live D3D
resource by pool, type and creator module (tracked by the proxy from device creation, read through
`d3d9.dll!WXL_ProxyStats`), the managed emulation counters, the archive layer (mounts, `.MPQ` handles
opened by `Wow.exe` before and after hosting, table sizes read from each archive header, the mount
phase's private-memory cost), and one host line (reads, mean and max latency, window use, store size,
restarts, fallbacks). Any `.MPQ` `CreateFile` by `Wow.exe` while the host serves the archives is a
warning. They are counters for reading the log, not a benchmark.

Two more lines come with the host line. `memory[textures]: textures:` counts textures served by the host
(decoded there, from its cache, prefetched), those read natively (fallbacks, and opens not claimed), verify
checks and mismatches, the fetch latency (mean, and p99 over the last 2048), the heap the loader did not
allocate (in total, now, and at peak: each texture's file size less the 1172-byte header), the images waiting
in the window, and throttle waits. `memory[textures]: host cache` gives the cache's size and hits, prefetch
(hints, queued, read, used, evicted unused), the host's texture reads and decode time, and the backing copies
kept as references (count, bytes, rebuilt, failed).

**The memory map** (`engine/diag/AllocTrack`, `ProcessMap`, `MemoryMap`) adds, at the same times: the engine
heap (Storm live bytes, blocks, peak; what bypasses Storm on the CRT heap), live/peak bytes per subsystem, the
top 40 sources, the owners of names that say nothing (`new`, containers of a built-in type, BlizzardCore's
allocator), private memory by owner (heaps, VirtualAlloc by caller module, stacks, write-combined GPU
mappings), loaded modules by group (game, ours, dxvk, system, drivers, other) and one DXVK line.
`SMemAlloc(size, file, line, flags)` is `malloc`/`calloc` on the CRT heap with no header of its own: each
block is asked 8 bytes larger and ends with a tag naming its call site, found again at free time through
`HeapSize`, which is exact on the system heap. A generic name is attributed to the nearest named call site of
its owner's code (marked `~`). A loading screen opens a peak window until 5 s after it closes, a zone change
one of 20 s; closing it logs `memory[load-peak]` or `memory[zone-peak]` with the growth to the peak by
subsystem and source. The overlay's Memory panel (F9) writes the full map to `Logs\memory-map.txt`: every
source and call site, every heap walked (the CRT heap's walk against the tracker's count), every reservation
labelled, every module, the last 24 windows. The hooks cost about 30 ns per allocate-and-free pair; with
`WXL_DIAG_MEMORY=0` nothing is installed or allocated.

## Checking the host without the game

`wxl-host-probe.exe` (Win32 build tree, never deployed) links the client's `HostClient`, starts a real
host on the real archives, and compares what it serves against an independently extracted copy of
the client:

```
build\dll\Release\wxl-host-probe.exe <client folder> <extracted client folder> [samples]
build\dll\Release\wxl-host-probe.exe <client folder> <extracted folder> --where <file>   (which archives hold it)
```

It checks mounts, reads (byte for byte), big files, the store, jobs, four concurrent readers, ping
latency, and a killed host being replaced with its mounts replayed. Measured on this machine: start
18-62 ms, round trip about 1 µs while the host spins and 21-41 µs from a sleeping host, 594 of 595
sampled files identical (138 MB); the one difference was the extracted copy's fault (it holds the
`common-2.MPQ` version of a file `patch.MPQ` overrides), restart in about 0.7 s.

`--textures [files per group]` (0 for all) checks the texture path. It runs in a child process whose
0x400000 range was reserved before its loader ran, maps the client's image there and runs the loader's own
decoder (`CBLPFile::Source` + `LockChain2`) on each palettized file, to compare with what the host serves; every
other BLP must come back as the file itself. Then it runs the loader's own steps (`Source`, `GetTextureFormats`
0x004B5FE0 with a blank device holding the DXT caps, `LockChain2` in place) twice, over the whole file and over the
1172-byte stand-in a texture handle returns, and compares every level the upload would read. It also checks a
backing copy kept as a reference (read back, then patched), and a hint: the tile under it and a texture that tile
names must be served as prefetched. Measured over the whole extracted 3.3.5a tree (111,891 BLP): 36,697
palettized files decoded by the host, all identical to the loader's decode level by level; 4 without mips served
as the file; 75,190 DXT and ARGB files served byte for byte (5,764 of them empty); through the stand-in the
loader's upload chain equals its native one for all 36,701 palettized and all 69,426 other non-empty files. A
texture read took about 270 µs from the archives (read, decode, copy into the window) and 80 µs from the cache.

`wxl-gpu-probe.exe` does the same for the managed-pool emulation. Run it from a folder holding only it
and the proxy `d3d9.dll` (no `WarcraftXL.dll`, so nothing patches the probe): the probe then stands where
`Wow.exe` does and takes the engine path. It checks the D3D9Ex factory and device, MANAGED ARGB and DXT1
textures down to 1x1 uploaded through surface and texture locks (whole and partial) and read back from
the GPU, an extension-created texture on the mirror path (partial writes, a read lock), a volume texture,
a vertex buffer written through two nested locks and drawn, the engine's hardware-cursor sequence, both
screenshot readbacks, `Reset` with emulated resources alive, and that every emulated resource is
forgotten on release. It caught the `GetDesc` rule above before any game session did; the cursor rule
came from the first session (no cursor) and the probe reproduces it against the older proxy. It cannot reach the host (the backing copies), which needs `WarcraftXL.dll`.

## Phase D

Done: texture images (above), the cache, prefetch, and backing references. Still planned:

- **Levels below the base mip.** With `baseMip` 1 the upload never reads level 0, but the host still
  transfers it. Skipping it needs the texture's final flags, which only exist after the loader ran.
- **Converted formats in the host.** An opaque palettized texture is uploaded as RGB565 by `Blit`; producing
  that in the host would let its backing copy be a reference too.
- **wxl-forever baked data in the host.** Cookie, horizon and bake loading moved to host jobs, with the
  decoded atlases parked in the store and uploaded by rows.

## Risks

- **Fidelity of the archive layer.** A client path that reaches the native archive list directly would
  find it empty; the known ones are replaced above, anything missed shows up as a missing file.
  `WXL_HOST_FILES=0` restores the native layer.
- **StormLib vs the client's Mopaq.** Locale selection (the host reads the neutral locale, as a 3.3.5a
  client does for its own archives) and corrupted archives could resolve differently.
- **TDR under D3D9Ex.** Whether `Reset` succeeds after a device removal with live DEFAULT objects is
  driver-dependent and untested here (`dxcap -forcetdr` reproduces one). If it fails, the engine keeps
  retrying and the session must be restarted; `WXL_D3D9EX=0` returns to the runtime-managed pool.
- **Static buffers in DEFAULT.** A write-through lock on a buffer the GPU is still reading can stall.
  Watch the frame-hitch lines.
- **Address space for mappings.** The transfer window (32 MB) is a fixed reservation in `Wow.exe`, and
  pooled staging for uploads takes up to `WXL_D3D9EX_STAGING_MB` (32) more; big files map one view each
  while open. These are what the host costs `Wow.exe`, against what it removes.
- **Host memory.** Backing copies cost system memory in the host: DXT data barely compresses, so they
  approach the size of the engine's textures. `WXL_D3D9EX_BACKING=0` trades device-loss refill for it.
  Backing references take the textures served from MPQs out of that; the cache adds up to
  `WXL_HOST_CACHE_MB`.
- **The texture header stand-in.** It relies on the loader reaching levels only as `buffer + offset` and on
  the completion closing the file after the upload (0x004B7E80). A client path reading a texture handle
  another way would read the header only; `WXL_HOST_TEXTURES=0` restores whole-file reads.
- **Texture images in the window.** The loader's own 4 MB in-flight budget no longer bounds texture reads
  (it counts 1172-byte headers); `WXL_HOST_TEXTURES_INFLIGHT_MB` does, and overflow goes to big sections.
- **References after an unmount.** A reference into an archive that is unmounted cannot be rebuilt (logged,
  counted as failed); the client unmounts archives only when it exits.
