# wxl-graphics-extend

**Shared render plumbing for WarcraftXL graphics extensions.**

`wxl-graphics-extend` draws nothing on its own. It owns the resources and chores every render
extension needs and hands them to all of them through one service:

| Tool | What a consumer gets |
|---|---|
| **INTZ depth** | The world's depth as a texture it can sample after the world pass. |
| **G-buffer** | The view-space normals and the albedo the engine's rewritten materials write as render targets 1 and 2. |
| **HDR colour** | The world drawn into an FP16 target, which a pass later tonemaps to the back buffer. |
| **Engine light buffer** | The texture and constants the rewritten materials add during the world pass, bound and unbound for you. |
| **Pass scheduler** | Ordered passes after the world pass, a shared per-frame camera, and GPU time per pass. |
| **Render targets** | Textures that are re-created when the window is resized or the device is reset. |
| **Textures** | White, black and flat-normal stand-ins, plus a baked blue-noise texture. |
| **DDS** | Parse, decode (true colour, BC1-5) and create 2D, cube and volume textures with mips. |
| **BLS** | Read and write the client's shader container. |
| **Shaders** | HLSL compiled with `d3dcompiler_47`, cached in memory and on disk, plus a built-in HLSL library. |
| **Full-screen passes** | A pass-through `vs_3_0` and a clip-space quad. |
| **Baked assets** | Manifests (the CSV `5.tools/forever-bake` writes) and files streamed on a worker thread within a memory budget. |
| **Vulkan (DXVK)** | On DXVK: the Vulkan device behind the D3D9 one, the world's targets as Vulkan images, and one compute block per frame. |
| **C++ helpers** | `StateGuard`, samplers, blend presets, matrices, GPU timer, Vulkan dispatch, panel helpers (header-only, in `include/wxl/gfx/`). |

It is the shared, reusable version of what `wxl-forever` grew in its `core/` folder: `SceneDepth`,
`Passes`, `Dds`, `Bls`, `ShaderLibrary`, `RenderUtil`, `GpuTimer`, `Matrix` and `BlueNoise`.

## Why a service rather than a copy per extension

The world pass's extra targets (`WorldSceneBeginArgs::depthOverride`, `normalTarget`,
`colorOverride`) each have **one owner**: the last subscriber to write wins. Two extensions that each
create their own INTZ depth break each other without a word. The same goes for the light buffer
(sampler 12, `c26..c30`).

This service holds those targets and gives the same textures to every consumer. It creates them only
on frames where some pass asks for them. If another party already supplied a target (an extension
not yet ported to the service), the service leaves it in place. At the end of the pass it reads back
what the core actually used, whoever owns it. Every pass sees the same depth, normals and colour, and
a frame where nobody asks for anything costs nothing.

## Using it from an extension

The contract is `include/wxl/GraphicsExtendApi.h` (a plain C ABI, like every other service).
`include/wxl/gfx/Client.hpp` resolves it:

```cpp
#include "wxl/gfx/Client.hpp"
#include "wxl/gfx/RenderState.hpp"

namespace
{
    const WXL_GraphicsExtendApi* g_gfx = nullptr;
    IDirect3DPixelShader9*       g_ps  = nullptr;

    uint32_t __cdecl Wants(void*) { return g_enabled ? (WXL_GFX_NEED_DEPTH | WXL_GFX_NEED_NORMALS) : 0; }

    void __cdecl Draw(void*, const WXL_GfxFrame* f)
    {
        if (!(f->available & WXL_GFX_NEED_DEPTH) || !g_ps) return;
        auto* d = static_cast<IDirect3DDevice9*>(f->device);
        wxl::gfx::render::StateGuard guard(d, /*psConstants*/ 8, /*stages*/ 2);
        wxl::gfx::render::PlainState(d);
        wxl::gfx::render::Sampler(d, 0, static_cast<IDirect3DBaseTexture9*>(f->depthTexture), false);
        wxl::gfx::render::Sampler(d, 1, static_cast<IDirect3DBaseTexture9*>(f->normalTexture), false);
        d->SetPixelShader(g_ps);
        // ... constants: f->depthRange, f->view.invViewProjRel (matrix::Columns), ...
        g_gfx->DrawFullscreen(d, f->width, f->height);
    }
}

// From an event, not only WXL_Load: extensions load in folder order, and this one may come later.
void OnFrame(const wxl::events::FrameArgs&)
{
    if (g_gfx || !(g_gfx = wxl::gfx::Service(g_api))) return;
    const WXL_GfxPassDesc pass{ sizeof pass, "my-effect", WXL_GFX_ORDER_EFFECTS, &Wants, nullptr, &Draw, nullptr };
    g_gfx->AddPass(&pass);
}
```

### One frame

```
OnWorldSceneBegin
  poll every pass's wants()  --  none: return, the world pass is untouched
  supply the union: INTZ depth -> depthOverride, normals -> normalTarget, FP16 -> colorOverride
                    (adopting whatever another subscriber already put there)
  read the depth range, build the camera (WXL_GfxView)
  begin() of every wanting pass  --  SetEngineLightBuffer is valid here
[the engine draws the world]
OnWorldSceneEnd
  unbind the light buffer (sampler 12, c30 = 0)
  read back depth / normals / HDR from what the core reports
  render target 0 = the HDR target (if nobody resolved it yet), else the back buffer
  draw() of every wanting pass in order  --  GPU-timed
  frame kept for CurrentFrame() until the next world pass or device loss
```

Order anchors: `WXL_GFX_ORDER_LIGHTING` (100), `ATMOSPHERE` (200), `EFFECTS` (250), `POST` (300,
resolves HDR), `OVERLAY` (900). These match `wxl-forever`: surface 100, fog 200, terrain debug 250,
post 300.

### The camera (`WXL_GfxView`)

Everything is computed once per frame by the service. Matrices are row-major with row vectors (the
engine's convention). "Rel" spaces take a camera-relative point `r = world - eye`, which keeps full
precision far from the map origin.

- `viewRel`, `viewProjRel`, `invViewProjRel`: the camera, and back from clip space.
- `reprojectRel`: this frame's `r` to last frame's clip space, for temporal effects.
- `depthLinearize`: view-space depth from the INTZ value, read off the projection matrix, so it holds
  whatever convention the engine uses.
- `projection` and everything built on it are in the D3D form the depth buffer was written with
  (clip z 0..w). The camera itself holds the engine's GL form (-w..w) and the D3D device converts it
  before drawing; the service does the same conversion once. Shaders written against these
  matrices therefore use the INTZ depth as is, without `wxl-forever`'s `depth01 * 2 - 1`.

### Rules of the road

- **Resolve lazily.** Call `Service()` from an event. `Client.hpp` caches the result and retries
  cheaply while the service is absent.
- **Leave the device as you found it.** The engine caches D3D9 state; use `render::StateGuard`.
- **Check `available`.** A pass whose depth, normals or HDR could not be supplied still runs:
  - no INTZ, multisampling on or a pure device → no depth and no HDR;
  - `extShadowQuality` 0 → the materials write no normals.
- **Release what you create on `OnDeviceLost`**, or use `CreateTarget`, which does it for you.
- **Borrowed pointers** in a frame are valid until the next world pass begins or the device is lost.
- **Set `structSize`** on every struct you hand in. Descriptors from an older header are refused if too
  short, and structs the service fills are only written as far as the caller's size.
- **Blue noise is baked on first request** (a fraction of a second on a worker thread): ask for it
  every frame and use a fallback while it returns null. Nothing is baked if nobody asks.

## Vulkan compute through DXVK

The contract is `include/wxl/GraphicsVulkanApi.h`, published as `wxl.graphics-extend.vulkan`. When the
client runs on DXVK, which translates its D3D9 into Vulkan, the service finds DXVK's interop
interface. It then lets extensions run Vulkan compute on the same device, the same queue and the
same images the client draws with. That removes the shader model 3 limits:
- real 3D textures;
- storage images and buffers;
- no register or sampler ceiling.

On native D3D9, `Available()` is 0 and every compute pass stays inert, at no cost.

- **Choosing DXVK.** This extension ships DXVK 3.1.1 (`dxvk/`: the 32-bit `d3d9.dll`, its
  `dxvk.conf` and licence), deployed to `Extensions\wxl-graphics-extend\dxvk\`. Set
  `WXL_D3D9_BACKEND=dxvk` in `WarcraftXL.cfg`; `WXL_DXVK_PATH` names another `d3d9.dll`. The proxy
  loads it and falls back to the system D3D9 if it cannot. Under Wine with DXVK installed as the
  system `d3d9`, nothing is needed. The interop interfaces are the same in DXVK 2.7.1 and 3.1.1;
  3.x enables more of what compute uses (see Capabilities).
- **One block per frame.** Compute passes (`AddComputePass`) join the scheduler: their wants are
  polled with the others', so the INTZ depth, the G-buffer and the HDR target are supplied for them
  too. Once the world is drawn, the service does the following, whatever the number of passes:
  1. It has DXVK end its render pass for good, which returns every image to the layout the interop
     reports, and flushes the D3D9 work. This is the frame's one flush. It comes before any import
     because DXVK may move an image to new memory at a flush.
  2. It records every wanting compute pass, in order, into one command buffer.
  3. It submits on DXVK's own queue, behind that work.

  Every D3D9 pass drawn afterwards can use the results. A record callback records Vulkan only: a
  D3D9 draw, copy or upload made from one reaches the GPU after the block.
- **Images.** Images the service creates (`CreateImage`) always stay in `VK_IMAGE_LAYOUT_GENERAL`, so
  a pass only orders its accesses with `CmdBarrier` and never transitions them. D3D9 textures are
  imported per block (`ImportTexture`) and handed back to DXVK in its own layout.
  - A D3D9 pass reads a compute result through a D3D9 texture filled by `CopyToTexture`. Every D3D9
    texture can be a copy destination on DXVK 2.7.1 and 3.1.1 alike, so no version-specific interop
    call is needed.
  - A D3D9 texture a compute pass reads must already hold its data on the GPU: DEFAULT pool, filled
    by rendering or `UpdateTexture`.
- **Per-frame memory.** Uniform and staging rings, descriptor sets and timestamps all rotate over
  three frames in flight.
- **Helpers.** `include/wxl/gfx/Vk.hpp` loads the device functions a pass records with, and adds a
  descriptor-set writer and `GetCaps`. SPIR-V is compiled offline and embedded in the consumer, with
  DXC: `-spirv -T cs_6_0 -fspv-target-env=vulkan1.3` (SPIR-V 1.6). A module newer than the device
  accepts is refused with a reason rather than created.
- **Capabilities.** DXVK creates a Vulkan 1.3 device, even on a 1.4 driver, with the features it
  needs itself. Vulkan cannot report what a device was created with, so `GetCaps` gives two sets:
  - `enabled`: the hardware's support narrowed to what the running DXVK generation turns on. It
    reads that generation from the factory (`ID3D9VkExtInterface` answers there since 3.0).
  - `supported`: the hardware's support alone.

  `GetCaps` also gives the usable Vulkan and SPIR-V versions, and the subgroup size and operations
  that the SM6 wave intrinsics map to. `FormatFeatures` says, format by format, whether a storage
  image may be declared Unknown. The same facts are logged once per device and shown in the panel.

## Baked assets

A section of the main table, generalised from `wxl-forever`'s `core/BakedAssets`:
- `ManifestLoad(folder, file)` reads a bake manifest.
- `AssetRequest(path, priority, want)` streams a file as:
  - a MANAGED texture;
  - raw bytes;
  - a DEFAULT-pool texture a compute pass can import.
- Reads run on a worker thread. The service creates the textures a few per frame, and evicts the
  least recently used over the budget (`WXL_GFX_ASSET_BUDGET_MB`).
- A missing file never fails a consumer: it just stays null.

## The HLSL library

`#include "wxl/gfx/<file>.hlsli"` in any program compiled through `CompileShader`,
`CreatePixelShader` or `CreateVertexShader`: the service serves these names itself. For an offline
`fxc` build, add `extensions/wxl-graphics-extend/shaders` to the include path, or fetch the text with
`IncludeFile`.

| File | Contents |
|---|---|
| `common.hlsli` | `WxlScreenUv` (UV from `VPOS`, D3D9 pixel centres), `WxlUvToNdc` / `WxlNdcToUv`, `WxlDp4Rows` (the dp4 form of `matrix::Columns`), `WxlLuminance`, `WxlSafeRcp`. |
| `depth.hlsli` | `WxlDepthToNdc`, `WxlDepthIsSky`, `WxlLinearDepth`, `WxlReconstructRel`, `WxlReprojectClip` / `WxlReprojectUv`. |
| `gbuffer.hlsli` | `WxlGBufferNormal`, `WxlGBufferHasNormal`, `WxlGBufferEngineLit` (the alpha markers of `src/game/GBuffer.hpp`), `WxlGBufferKind`, `WxlGBufferGloss` (the albedo target's material code). |
| `color.hlsli` | Exact sRGB, the engine's gamma 2, `WxlTonemapAces` / `Reinhard` / `ReinhardWhite` / `Uncharted2`, `WxlDither`. |
| `noise.hlsli` | `WxlBlueNoise` (the service's tile with a per-frame R2 offset), `WxlInterleavedGradientNoise`, `WxlR2`. |

Every library function takes its samplers and values as parameters and declares no global resource,
so the consumer keeps control of its registers.

## Shader cache

`CompileShader` keys a program on its source, defines, entry point, target and flags. Every file it
included is recorded with a hash of its text. A hit, from memory or from
`Extensions\wxl-graphics-extend\cache\*.wgc` on disk, is re-checked against the current text of those
includes before it is used. An unchanged program never reaches the compiler twice, across
extensions and across sessions, and an edited include is never served stale. Disk entries are written
atomically and carry a checksum; a damaged one is only a cache miss.

## Configuration

`Extensions\wxl-graphics-extend\wxl-graphics-extend.cfg` (see `wxl-graphics-extend.cfg.example`). An
environment variable of the same name wins.

| Key | Default | Effect |
|---|---|---|
| `WXL_GFX_DEPTH` | 1 | Allow the INTZ depth redirect. |
| `WXL_GFX_NORMALS` | 1 | Allow the G-buffer normal target. |
| `WXL_GFX_ALBEDO` | 1 | Allow the G-buffer albedo target (`WXL_GFX_NEED_ALBEDO`, render target 2 beside the normals). |
| `WXL_GFX_HDR` | 1 | Allow the FP16 world target. |
| `WXL_GFX_PROFILE` | 1 | Time every pass on the GPU (timestamp queries, read a few frames late). |
| `WXL_GFX_SHADER_DISK_CACHE` | 1 | Keep compiled shaders on disk. |
| `WXL_GFX_VULKAN` | 1 | Expose DXVK's Vulkan device (compute passes). |
| `WXL_GFX_ASSET_BUDGET_MB` | 64 | Memory resident baked files may hold. |
| `WXL_GFX_ASSET_ASYNC` | 1 | Read baked files on a worker thread. |

The overlay panel **Graphics Extend** shows the device facts, what the passes asked for and got, the
GPU time of each pass, the render targets and the shader cache.

## Living next to wxl-forever

`wxl-forever` still carries its own copies of these tools. Both can be loaded together:
- The service never overwrites a target another subscriber already supplied.
- It reads back what the core used, so its consumers see `wxl-forever`'s depth, normals and HDR
  target.

Two limits remain:
- If a consumer of this service and `wxl-forever` both bind the engine light buffer on the same
  frame, the last one wins.
- `wxl-forever`'s own passes only run on frames where it owns the depth itself.

Porting `wxl-forever` to the service (its `passes`, `depth`, `dds`, `bls` and `shaders` namespaces map
one to one onto this API) removes both limits and its duplicated code.

## Layout

```
include/wxl/GraphicsExtendApi.h      the C ABI contract (core include dir, like every service)
include/wxl/GraphicsVulkanApi.h      the Vulkan side (DXVK interop, compute block)
include/wxl/gfx/                     header-only C++ helpers: Client, RenderState, Matrix, GpuTimer, Vk, Ui
deps/vulkan-headers/                 the official Vulkan headers (headers only; nothing links vulkan-1)
extensions/wxl-graphics-extend/
├── src/Module.cpp                   entry points, the service table, device lifecycle
├── src/frame/                       pass scheduler, world pass targets, camera, light buffer
├── src/targets/                     render target pool
├── src/textures/                    DDS, neutral textures, blue noise
├── src/io/                          client file reads (archives, then loose)
├── src/shaders/                     compiler + caches, BLS, full-screen VS
├── src/assets/                      baked assets: manifests, streaming, budget
├── src/vulkan/                      DXVK interop, capabilities, compute block, images, pipelines
├── src/ui/                          overlay panel
├── dxvk/                            DXVK 3.1.1: 32-bit d3d9.dll, dxvk.conf, licence (deployed beside the DLL)
├── shaders/wxl/gfx/                 the built-in HLSL library (embedded in the DLL)
└── cmake/EmbedFiles.cmake           embeds the library (shared.cmake wires it into the build)
```

## Building

Nothing to do beyond the core build: the root `CMakeLists.txt` builds every folder under
`extensions/`. The HLSL library is embedded by CMake; no Python or `fxc` is needed. Output:
`Extensions/wxl-graphics-extend/wxl-graphics-extend.dll`.

## License

GPL-3.0-or-later, like the rest of WarcraftXL.
