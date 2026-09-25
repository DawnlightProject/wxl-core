// wxl-graphics-extend: the world pass's shared render resources (INTZ depth, G-buffer normals, HDR
// colour), an ordered post-world pass scheduler and the graphics tools every render extension needs
// (render targets, DDS, BLS, shader compilation, neutral textures). Published by the
// wxl-graphics-extend extension as WXL_Api::GetInterface(WXL_GRAPHICS_EXTEND_API_NAME,
// WXL_GRAPHICS_EXTEND_API_VERSION).
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#ifndef WXL_GRAPHICS_EXTEND_API_H
#define WXL_GRAPHICS_EXTEND_API_H

#include <stddef.h>
#include <stdint.h>

#include "wxl/ByteSink.h"

// Why a service. The world pass's extra targets (WorldSceneBeginArgs::depthOverride, normalTarget,
// colorOverride) each have one owner -- the last subscriber to write wins -- so two extensions that
// each create their own INTZ depth or G-buffer silently break each other. This service owns them,
// creates them only on the frames some pass asks for them, and hands the same textures to every
// consumer. When another party already supplied one (an extension not ported to this service yet),
// the service leaves it in place and still reports it: whoever owns a target, every pass sees it.
//
// Resolving. Extensions load in folder order, so a consumer may load before this service: resolve
// the table lazily (first frame, first event), never only from WXL_Load. The table lives for the
// process; the pointer can be kept once found. include/wxl/gfx/Client.hpp does exactly that.
//
// Threads. Every function is for the render thread (the client's main thread) unless it says
// otherwise. Every callback is invoked on it.
//
// Struct sizes. Every struct with a structSize crosses in both directions safely: set it to
// sizeof(the struct) before handing one in -- descriptors (WXL_Gfx*Desc) are refused when it is
// smaller than this version's, and for the ones the service fills (WXL_GfxFrame, WXL_GfxStatus,
// WXL_GfxDdsInfo) it writes only what fits and sets structSize to what it wrote.
//
// COM pointers. D3D9 interfaces cross as void* (IDirect3DDevice9*, IDirect3DTexture9*, ...), so this
// header needs no d3d9.h. "Borrowed" means the service keeps the reference: do not Release it, and do
// not keep it past the validity the field states. "Caller owns" means Release it when done.
//
// Matrices are 16 floats, row-major, row-vector convention (the engine's: v' = v * M). "Rel" names a
// camera-relative space: the point is (world - eye, 1), which keeps precision far from the origin.

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_GRAPHICS_EXTEND_API_NAME    "wxl.graphics-extend"
#define WXL_GRAPHICS_EXTEND_API_VERSION 1

// --- what a pass asks for --------------------------------------------------------------------------

/// The world's depth as an INTZ texture (sample .r: the hardware depth, in depthRange).
#define WXL_GFX_NEED_DEPTH   0x00000001u
/// The G-buffer: view-space normals written by the engine's rewritten materials as render target 1
/// of the world pass (encoding in src/game/GBuffer.hpp; needs extShadowQuality >= 1).
#define WXL_GFX_NEED_NORMALS 0x00000002u
/// The world drawn into an A16B16G16R16F target instead of the back buffer; the passes draw on it and
/// one of them resolves it (tonemap) to the back buffer, or the core copies it across unchanged.
#define WXL_GFX_NEED_HDR     0x00000004u
/// None of the above, but run this pass's begin and draw this frame.
#define WXL_GFX_NEED_RUN     0x00000008u

// --- pass order anchors (lower draws first) --------------------------------------------------------

#define WXL_GFX_ORDER_LIGHTING   100   ///< surface lighting, before anything veils it
#define WXL_GFX_ORDER_ATMOSPHERE 200   ///< fog, volumetrics
#define WXL_GFX_ORDER_EFFECTS    250   ///< world-space effects drawn over the atmosphere
#define WXL_GFX_ORDER_POST       300   ///< bloom, exposure, tonemap (resolves HDR)
#define WXL_GFX_ORDER_OVERLAY    900   ///< debug views, drawn last over the finished image

// --- device facts (WXL_GfxStatus::caps) ------------------------------------------------------------

#define WXL_GFX_CAP_INTZ         0x00000001u  ///< INTZ depth textures
#define WXL_GFX_CAP_MRT          0x00000002u  ///< at least two simultaneous render targets
#define WXL_GFX_CAP_FP16_TARGET  0x00000004u  ///< A16B16G16R16F render target textures
#define WXL_GFX_CAP_FP16_BLEND   0x00000008u  ///< alpha blending into A16B16G16R16F
#define WXL_GFX_CAP_MSAA         0x00000010u  ///< the world's depth is multisampled: no depth or HDR
#define WXL_GFX_CAP_PURE         0x00000020u  ///< pure device: state cannot be read back
#define WXL_GFX_CAP_PROBED       0x80000000u  ///< the device was probed (a world pass was seen)

/**
 * @brief The camera the world was drawn with, computed once per frame by the service.
 *
 * Every matrix built on the projection is in the D3D form (clip z 0..w), the one the depth buffer was
 * written with: wxl-forever's GL-form "depth01 * 2 - 1" is not needed with these.
 * Reprojection: a point at r = world - eye this frame was at clip = (r, 1) * reprojectRel last frame.
 * Linear depth: with ndc = (depth - depthRange[0]) / (depthRange[1] - depthRange[0]), the view-space
 * depth is (L[0] - ndc * L[1]) / (ndc * L[2] - L[3]), L = depthLinearize (read off the projection;
 * valid for perspective and orthographic alike). wxl/gfx/depth.hlsli implements both.
 */
typedef struct WXL_GfxView
{
    float eye[3];
    float prevEye[3];            ///< last frame's eye (equal to eye on the first frame)
    float view[16];              ///< the engine's view matrix, as the camera holds it
    float projection[16];        ///< the engine's projection in the D3D form the device draws with
                                 ///< (clip z in 0..w; the camera itself holds the GL form, -w..w)
    float viewRel[16];           ///< (r, 1) -> the engine's view space
    float viewProjRel[16];       ///< (r, 1) -> clip
    float invViewProjRel[16];    ///< clip -> (r, 1) before the divide by w
    float prevViewProjRel[16];   ///< last frame's viewProjRel, taking last frame's r
    float reprojectRel[16];      ///< this frame's (r, 1) -> last frame's clip
    float depthLinearize[4];     ///< (P[14], P[15], P[11], P[10]) of the projection
} WXL_GfxView;

/**
 * @brief What a pass sees before the world pass draws.
 *
 * Valid for the duration of the begin callback. What the service supplied may still be replaced by a
 * subscriber that runs after it; WXL_GfxFrame::available is what actually happened.
 */
typedef struct WXL_GfxBeginFrame
{
    uint32_t    structSize;
    void*       device;          ///< IDirect3DDevice9*, borrowed
    uint32_t    frameIndex;      ///< the index the coming frame will carry
    uint32_t    width, height;   ///< the world's render target (and depth surface) size
    uint32_t    requested;       ///< union of every pass's wants this frame
    uint32_t    supplied;        ///< WXL_GFX_NEED_* arranged for the coming pass
    float       time;            ///< seconds since the service started (QueryPerformanceCounter)
    float       deltaTime;       ///< seconds since the previous frame the passes ran in (0 on the first)
    WXL_GfxView view;
} WXL_GfxBeginFrame;

/**
 * @brief What every pass of one frame shares, once the world is drawn.
 *
 * The pointers are borrowed and valid until the next world pass begins or the device is lost,
 * whichever comes first. While the passes draw, render target 0 is `target`: the HDR scene colour when
 * the world was drawn in HDR and nobody resolved it yet, the back buffer otherwise.
 */
typedef struct WXL_GfxFrame
{
    uint32_t    structSize;
    void*       device;             ///< IDirect3DDevice9*
    uint32_t    frameIndex;         ///< rises by one every frame the passes run in
    uint32_t    width, height;      ///< the world's render target size
    uint32_t    available;          ///< WXL_GFX_NEED_* present this frame (NEED_RUN always set)
    void*       depthTexture;       ///< IDirect3DTexture9*, INTZ; null without WXL_GFX_NEED_DEPTH
    float       depthRange[2];      ///< viewport minZ, maxZ the world drew its geometry into
    void*       normalTexture;      ///< IDirect3DTexture9*, A8R8G8B8 G-buffer; null without NORMALS
    void*       sceneColorTexture;  ///< IDirect3DTexture9*, A16B16G16R16F; null without HDR
    void*       target;             ///< IDirect3DSurface9*, render target 0 while the passes draw
    void*       backBuffer;         ///< IDirect3DSurface9*, where the finished image goes
    int*        colorResolved;      ///< set *colorResolved = 1 once sceneColor is written to backBuffer
    float       time, deltaTime;    ///< as in WXL_GfxBeginFrame
    WXL_GfxView view;
} WXL_GfxFrame;

/// Polled once per frame before the world pass: the WXL_GFX_NEED_* this pass wants, 0 to sit out.
typedef uint32_t(__cdecl* WXL_GfxWantsFn)(void* user);
/// Before the world pass: bind what the engine's own materials read (SetEngineLightBuffer), etc.
typedef void(__cdecl* WXL_GfxBeginFn)(void* user, const WXL_GfxBeginFrame* frame);
/// After the world pass, in pass order. Leave the device as found (include/wxl/gfx/RenderState.hpp).
typedef void(__cdecl* WXL_GfxDrawFn)(void* user, const WXL_GfxFrame* frame);

typedef struct WXL_GfxPassDesc
{
    uint32_t       structSize;
    const char*    name;     ///< for the log and the panel; copied
    int32_t        order;    ///< lower draws first (WXL_GFX_ORDER_*); equal orders keep add order
    WXL_GfxWantsFn wants;    ///< required
    WXL_GfxBeginFn begin;    ///< optional
    WXL_GfxDrawFn  draw;     ///< optional: without it the pass only asks for resources
    void*          user;     ///< handed back to every callback
} WXL_GfxPassDesc;

// --- render targets ----------------------------------------------------------------------------------

/// Allocate the full mip chain with D3DUSAGE_AUTOGENMIPMAP (call GenerateMipSubLevels after drawing).
#define WXL_GFX_TARGET_MIPMAPS 0x00000001u
/// Clear to zero whenever the texture is (re)created.
#define WXL_GFX_TARGET_CLEAR   0x00000002u

typedef struct WXL_GfxTargetDesc
{
    uint32_t    structSize;
    const char* name;            ///< for the log and the panel; copied
    uint32_t    format;          ///< D3DFORMAT
    uint32_t    width, height;   ///< fixed size; both 0 = follow the world's render target
    float       scale;           ///< with a followed size: fraction of it (0.5 = half); 0 means 1
    uint32_t    flags;           ///< WXL_GFX_TARGET_*
} WXL_GfxTargetDesc;

typedef struct WXL_GfxTarget
{
    void*    texture;       ///< IDirect3DTexture9*, borrowed
    void*    surface;       ///< IDirect3DSurface9*, level 0, borrowed
    uint32_t width, height;
    uint32_t generation;    ///< changes whenever the texture was (re)created: its old content is gone
} WXL_GfxTarget;

// --- textures ------------------------------------------------------------------------------------------

#define WXL_GFX_TEX_WHITE        0u  ///< 1 x 1 A8R8G8B8 (255, 255, 255, 255)
#define WXL_GFX_TEX_BLACK        1u  ///< 1 x 1 A8R8G8B8 (0, 0, 0, 0)
#define WXL_GFX_TEX_FLAT_NORMAL  2u  ///< 1 x 1 A8R8G8B8 (128, 128, 255, 255)
#define WXL_GFX_TEX_WHITE_CUBE   3u  ///< 1 x 1 cube, every face white
#define WXL_GFX_TEX_WHITE_VOLUME 4u  ///< 1 x 1 x 1 volume, white
#define WXL_GFX_TEX_BLUE_NOISE   5u  ///< 128 x 128 A8R8G8B8, four independent void-and-cluster channels;
                                     ///< the first request starts a background bake: null until it
                                     ///< finishes (a fraction of a second), so ask early and every frame

#define WXL_GFX_DDS_2D     0u
#define WXL_GFX_DDS_CUBE   1u
#define WXL_GFX_DDS_VOLUME 2u

/**
 * @brief A parsed DDS header. Legacy headers and DX10 headers (single texture, no arrays) are read.
 *
 * Formats: DXT1/3/5 (BC1-3), ATI1/ATI2 (BC4/BC5, when the device takes them), A8R8G8B8, X8R8G8B8,
 * A8B8G8R8 and X8B8G8R8 (swizzled to A8R8G8B8 on upload), R5G6B5, A1R5G5B5, X1R5G5B5, A4R4G4B4,
 * A2B10G10R10, G16R16, A16B16G16R16, L8, A8, A8L8, L16, R16F, G16R16F, A16B16G16R16F, R32F, G32R32F,
 * A32B32G32R32F. Surfaces follow each other as the format defines: every mip of each cube face in
 * +X -X +Y -Y +Z -Z order; every slice of each mip for a volume.
 */
typedef struct WXL_GfxDdsInfo
{
    uint32_t structSize;
    uint32_t kind;           ///< WXL_GFX_DDS_*
    uint32_t width, height, depth;   ///< depth is 1 unless a volume
    uint32_t mips;           ///< levels present in the file (clamped to the full chain)
    uint32_t fileFormat;     ///< D3DFORMAT of the data as stored
    uint32_t format;         ///< D3DFORMAT the texture is created with (differs only for swizzled ones)
    uint32_t blockBytes;     ///< bytes per texel, or per 4 x 4 block when compressed
    uint32_t compressed;     ///< non-zero for block-compressed formats
    uint32_t dataOffset;     ///< where the first surface starts (128, or 148 after a DX10 header)
    uint32_t dataSize;       ///< bytes of every surface the header implies
} WXL_GfxDdsInfo;

// --- BLS (the client's shader container) ----------------------------------------------------------------

/**
 * @brief One permutation of a BLS file ("HSXG", version 0x00010003), as CGxShader::Load reads it.
 * @c code points into the bytes given to BlsPermutation; it is a D3D9 token stream.
 */
typedef struct WXL_GfxBlsPermutation
{
    uint32_t    inputs;      ///< CGxShader+0x3C
    uint32_t    outputs;     ///< CGxShader+0x40
    uint16_t    samplers;    ///< CGxShader+0x44
    uint16_t    extra;       ///< CGxShader+0x46
    const void* code;
    uint32_t    codeSize;
} WXL_GfxBlsPermutation;

// --- shaders -------------------------------------------------------------------------------------------

/**
 * @brief Resolves an #include the service does not own.
 *
 * The service serves every "wxl/..." name itself (the built-in library, wxl/gfx/<name>.hlsli) and asks this
 * callback for the rest. Return the file's text, valid until the compile call returns, and its size
 * in *size; NULL when unknown.
 */
typedef const char*(__cdecl* WXL_GfxIncludeFn)(void* user, const char* name, size_t* size);

/// Neither read nor write the memory and disk caches.
#define WXL_GFX_SHADER_NO_CACHE 0x00000001u
/// Skip optimisation and keep debug information (for PIX / RenderDoc captures).
#define WXL_GFX_SHADER_DEBUG    0x00000002u

typedef struct WXL_GfxShaderDesc
{
    uint32_t           structSize;
    const char*        name;          ///< file name for messages, e.g. "fog/shaders/apply.ps.hlsl"
    const char*        source;
    size_t             sourceSize;    ///< 0 = NUL-terminated
    const char*        entry;         ///< NULL = "main"
    const char*        target;        ///< "ps_3_0", "vs_3_0", ...
    const char* const* defines;       ///< NULL, or name, value, name, value, ..., NULL
    WXL_GfxIncludeFn   include;       ///< optional
    void*              includeUser;
    uint32_t           flags;         ///< WXL_GFX_SHADER_*
} WXL_GfxShaderDesc;

// --- status ------------------------------------------------------------------------------------------------

typedef struct WXL_GfxStatus
{
    uint32_t    structSize;
    uint32_t    caps;            ///< WXL_GFX_CAP_*
    uint32_t    frameIndex;      ///< as WXL_GfxFrame::frameIndex of the last frame
    uint32_t    lastRequested;   ///< what the passes asked for on the last world pass
    uint32_t    lastAvailable;   ///< what they got
    uint32_t    passCount;
    uint32_t    targetCount;     ///< render targets alive
    uint32_t    targetBytes;     ///< their approximate video memory
    uint32_t    shadersCompiled; ///< compiles this session (cache misses)
    uint32_t    shadersCached;   ///< memory or disk cache hits this session
    const char* depthStatus;     ///< one line, owned by the service, valid until the next call
} WXL_GfxStatus;

/**
 * @brief The service table. Check structSize before reading a field a later version appended.
 */
typedef struct WXL_GraphicsExtendApi
{
    uint32_t structSize;
    uint32_t apiVersion;

    // --- passes ------------------------------------------------------------------------------------

    /**
     * @brief Adds a pass for the process lifetime.
     *
     * Before each world pass the service polls every pass's wants; when none wants anything the world
     * pass is left completely untouched. Otherwise it supplies the union of what they asked for,
     * runs each wanting pass's begin, and once the world is drawn runs each wanting pass's draw in
     * order. A pass whose NEED_DEPTH / NEED_NORMALS / NEED_HDR could not be met still runs: read
     * WXL_GfxFrame::available.
     * @return the pass id (non-zero), 0 when desc is invalid.
     */
    uint32_t(__cdecl* AddPass)(const WXL_GfxPassDesc* desc);

    /**
     * @brief The frame the passes last ran in, while its resources are valid (see WXL_GfxFrame).
     *
     * For work outside a draw callback, e.g. from OnEndScene. Returns 0 (and leaves *out untouched)
     * when no frame ran since the last world pass began or the device was lost.
     */
    int(__cdecl* CurrentFrame)(WXL_GfxFrame* out);

    /// Smoothed GPU milliseconds of a pass's draw; -1 until measured (or with profiling off).
    float(__cdecl* PassGpuMs)(uint32_t passId);

    /**
     * @brief Binds the light buffer the engine's rewritten materials add during the world pass.
     *
     * Only inside a begin callback. Binds texture on sampler 12 (clamped, bilinear) and writes rows
     * (this frame's view space -> the texture's space, as four float4 rows in dp4 form: c26..c29) and
     * params (strength, cap per channel, scene share, 0: c30) through the engine's constant cache, the
     * way src/game/GBuffer.hpp requires. The service unbinds the texture and zeroes c30 as soon as the
     * world is drawn, before any pass runs. One provider per frame: the last call wins.
     * @return non-zero when bound.
     */
    int(__cdecl* SetEngineLightBuffer)(void* texture, const float rows[16], const float params[4]);

    void(__cdecl* GetStatus)(WXL_GfxStatus* out);

    // --- render targets --------------------------------------------------------------------------

    /**
     * @brief Declares a render target texture (D3DPOOL_DEFAULT) the service keeps for you.
     *
     * Nothing is allocated until GetTarget. The service releases it before a device reset and
     * re-creates it on the next GetTarget, and re-creates a followed-size target when the world's
     * size changes; generation tells you when either happened.
     * @return a handle (non-zero), 0 when desc is invalid.
     */
    uint32_t(__cdecl* CreateTarget)(const WXL_GfxTargetDesc* desc);

    /// Creates the texture if needed and fills *out. 0 when the device refused it (logged once).
    int(__cdecl* GetTarget)(uint32_t handle, void* device, WXL_GfxTarget* out);

    /// Releases the texture and forgets the handle.
    void(__cdecl* DestroyTarget)(uint32_t handle);

    // --- textures ----------------------------------------------------------------------------------

    /// A shared WXL_GFX_TEX_* texture (IDirect3DBaseTexture9*, D3DPOOL_MANAGED), borrowed for the
    /// process; created on first use. Null when unavailable (or the blue noise is still baking).
    void*(__cdecl* Texture)(void* device, uint32_t which);

    /// Reads a DDS header; non-zero when the bytes hold a DDS file this reader takes, entirely.
    int(__cdecl* DdsParse)(const void* bytes, size_t size, WXL_GfxDdsInfo* out);

    /**
     * @brief Creates the texture (2D, cube or volume, every mip in the file) and uploads the data.
     * @param pool  D3DPOOL (D3DPOOL_MANAGED survives a device reset).
     * @param name  for the log.
     * @return IDirect3DBaseTexture9*, caller owns; null when refused (the reason is logged).
     */
    void*(__cdecl* DdsCreateTexture)(void* device, const void* bytes, size_t size, uint32_t pool,
                                     const char* name);

    /// ReadClientFile then DdsCreateTexture. Caller owns the result.
    void*(__cdecl* DdsLoadTexture)(void* device, const char* path, uint32_t pool);

    /**
     * @brief Decodes one surface (face or slice, mip level) to 8-bit BGRA -- A8R8G8B8 in memory.
     *
     * Every format above decodes with its real colours and alpha (block-compressed ones included);
     * float formats are clamped to 0..1. Writes width * height * 4 bytes to out.
     */
    int(__cdecl* DdsDecodeBgra)(const void* bytes, size_t size, uint32_t faceOrSlice, uint32_t level,
                                WXL_ByteSink* out, uint32_t* width, uint32_t* height);

    /// As DdsDecodeBgra, to one byte per texel: L8 / A8 / A8L8 their first channel, the rest Rec.601
    /// luminance.
    int(__cdecl* DdsDecodeLuminance)(const void* bytes, size_t size, uint32_t faceOrSlice,
                                     uint32_t level, WXL_ByteSink* out, uint32_t* width,
                                     uint32_t* height);

    // --- files ---------------------------------------------------------------------------------------

    /**
     * @brief Reads a whole client file: through the client's file system (archives and patches), then
     *        loose from the client folder, then loose from its Data\ folder.
     * @return non-zero when the file was read and written to out.
     */
    int(__cdecl* ReadClientFile)(const char* path, WXL_ByteSink* out);

    // --- BLS -----------------------------------------------------------------------------------------

    /// Permutations in a BLS file; -1 when the bytes are not one whole BLS file.
    int(__cdecl* BlsCount)(const void* bytes, size_t size);

    /// Fills *out with permutation index (code points into bytes). 0 when out of range or invalid.
    int(__cdecl* BlsPermutation)(const void* bytes, size_t size, uint32_t index,
                                 WXL_GfxBlsPermutation* out);

    /// Writes a BLS file holding count permutations (their code is copied, padded to four bytes).
    int(__cdecl* BlsWrite)(const WXL_GfxBlsPermutation* permutations, uint32_t count, WXL_ByteSink* out);

    // --- shaders ---------------------------------------------------------------------------------------

    /**
     * @brief Compiles HLSL to D3D bytecode with d3dcompiler_47.
     *
     * Results are cached in memory for the session and on disk (Extensions\wxl-graphics-extend\cache),
     * keyed by the source, every file it included, the defines, entry, target and flags: an unchanged
     * program never reaches the compiler twice, across extensions and across sessions. Errors and
     * warnings go to the log with the program's name. Any thread (the caches are locked).
     * @return non-zero when bytecode was written to out.
     */
    int(__cdecl* CompileShader)(const WXL_GfxShaderDesc* desc, WXL_ByteSink* out);

    /// CompileShader, then IDirect3DDevice9::CreatePixelShader. Caller owns; null on failure.
    void*(__cdecl* CreatePixelShader)(void* device, const WXL_GfxShaderDesc* desc);

    /// CompileShader, then IDirect3DDevice9::CreateVertexShader. Caller owns; null on failure.
    void*(__cdecl* CreateVertexShader)(void* device, const WXL_GfxShaderDesc* desc);

    /// A built-in include file's text ("wxl/gfx/depth.hlsli", ...), for an offline or own compile
    /// pipeline; NULL when there is none by that name. Static for the process.
    const char*(__cdecl* IncludeFile)(const char* name, size_t* size);

    /// Drops the memory cache (the disk cache is validated against the sources on every hit anyway).
    void(__cdecl* ClearShaderCache)(void);

    // --- full-screen passes ------------------------------------------------------------------------------

    /// The vs_3_0 pass-through vertex shader DrawFullscreen's quad needs (ps_3_0 cannot run without
    /// one). Borrowed for the process: shaders survive a device reset.
    void*(__cdecl* FullscreenVertexShader)(void* device);

    /**
     * @brief Sets a width x height viewport and draws one clip-space quad (D3DFVF_XYZW, UP draw).
     *
     * Binds FullscreenVertexShader, the FVF and the viewport; the pixel shader, samplers and render
     * states are the caller's. D3D9 puts pixel centres on integer coordinates: in the pixel shader
     * read the screen position from VPOS and sample at (vpos + 0.5) / size (wxl/gfx/common.hlsli:
     * WxlScreenUv). Stream 0 is unbound afterwards, as after any UP draw.
     */
    void(__cdecl* DrawFullscreen)(void* device, uint32_t width, uint32_t height);
} WXL_GraphicsExtendApi;

#ifdef __cplusplus
}
#endif

#endif // WXL_GRAPHICS_EXTEND_API_H
