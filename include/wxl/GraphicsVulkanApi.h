// wxl-graphics-extend's Vulkan side: when the client runs on DXVK, the Vulkan device behind the D3D9
// device, the world pass's targets as Vulkan images, and one compute block per frame recorded by any
// extension. Published by the wxl-graphics-extend extension as
// WXL_Api::GetInterface(WXL_GRAPHICS_VULKAN_API_NAME, WXL_GRAPHICS_VULKAN_API_VERSION).
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

#ifndef WXL_GRAPHICS_VULKAN_API_H
#define WXL_GRAPHICS_VULKAN_API_H

#include <stddef.h>
#include <stdint.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES   // functions come from the context's getDeviceProcAddr, never from a link
#endif
#include <vulkan/vulkan_core.h>   // deps/vulkan-headers/include: on every extension's include path

#include "wxl/GraphicsExtendApi.h"

// When. DXVK translates the client's D3D9 into Vulkan and exposes its device through an interop
// interface (ID3D9VkInteropDevice). With it, this service lets extensions run Vulkan compute on the
// same device, the same queue and the same images the client draws with -- no shader model 3 limit,
// real 3D textures, storage images and buffers. Without DXVK (native D3D9) Available() is 0 and
// every compute pass sits out; consumers that need Vulkan simply stay inert.
//
// The frame. Compute passes join the scheduler of GraphicsExtendApi.h: their wants are polled with
// the other passes' before the world pass (so the INTZ depth, the G-buffer and the HDR target are
// supplied for them too). Once the world is drawn, and before any D3D9 pass draws, the service
// flushes the D3D9 work (the world pass), records every wanting compute pass, in order, into one
// command buffer, and submits it on DXVK's own queue behind that work. One flush per frame, whatever
// the number of passes. The results are then ready for the D3D9 passes that follow (a D3D9 pass
// samples them through a D3D9 texture: see CopyToTexture).
//
// Images. Images the service creates for you (CreateImage) are always in VK_IMAGE_LAYOUT_GENERAL, so
// you only order your accesses with barriers (CmdBarrier), never transition. D3D9 textures you use in
// a compute pass (ImportTexture, and the frame's depth / normals / scene colour, imported for you)
// are moved to GENERAL when imported and back to DXVK's own layout at the end of the block; a D3D9
// texture read this way must hold its data on the GPU already: DEFAULT pool, filled by rendering,
// UpdateTexture or UpdateSurface. A MANAGED texture filled with LockRect is uploaded by DXVK only when
// a D3D9 draw first uses it, so a compute pass could read it stale.
//
// Threads. Render thread only, like the rest of the service. Record callbacks run on it and record
// Vulkan only: a D3D9 call that draws, copies or uploads from inside one reaches the GPU after the
// block, not before it.
//
// Capabilities. DXVK creates a Vulkan 1.3 device with the features it needs itself; GetCaps says
// which of them a compute pass may rely on, and the SPIR-V version its modules may declare.

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_GRAPHICS_VULKAN_API_NAME    "wxl.graphics-extend.vulkan"
#define WXL_GRAPHICS_VULKAN_API_VERSION 1

/// Frames the service keeps in flight: per-frame allocations (uniforms, staging, descriptor sets) are
/// reused after this many frames, once the GPU is done with them.
#define WXL_GFX_VK_FRAMES_IN_FLIGHT 3

/**
 * @brief The Vulkan device behind the client's D3D9 device.
 *
 * Valid while generation stays the same: a new D3D9 device (not a reset) brings a new Vulkan device,
 * and every object created on the old one must be recreated. The service destroys what it created
 * for you (images, pipelines, samplers) itself when that happens.
 */
typedef struct WXL_GfxVkContext
{
    uint32_t                  structSize;
    uint32_t                  generation;        ///< non-zero; changes with the Vulkan device
    VkInstance                instance;
    VkPhysicalDevice          physicalDevice;
    VkDevice                  device;
    VkQueue                   queue;             ///< DXVK's rendering queue; never submit to it yourself
    uint32_t                  queueFamily;
    uint32_t                  queueIndex;
    uint32_t                  apiVersion;        ///< the physical device's; the usable one is WXL_GfxVkCaps::apiVersion
    PFN_vkGetInstanceProcAddr getInstanceProcAddr;
    PFN_vkGetDeviceProcAddr   getDeviceProcAddr; ///< load device functions with it (include/wxl/gfx/Vk.hpp)
    VkPhysicalDeviceLimits    limits;
} WXL_GfxVkContext;

// --- capabilities ------------------------------------------------------------------------------------
// What a compute pass may use on DXVK's device (GetCaps). Vulkan cannot report the features a VkDevice
// was created with, so `enabled` is the hardware's support narrowed to what the running DXVK
// generation enables when supported (its src/dxvk/dxvk_device_info.cpp). `supported` is the hardware
// alone: what a DXVK that enabled more could offer. Test `enabled` before using anything below.

#define WXL_GFX_VK_CAP_SHADER_FLOAT16                  (1ull << 0)   ///< half arithmetic (DXC -enable-16bit-types); DXVK 3
#define WXL_GFX_VK_CAP_SHADER_INT8                     (1ull << 1)
#define WXL_GFX_VK_CAP_SHADER_INT16                    (1ull << 2)
#define WXL_GFX_VK_CAP_SHADER_INT64                    (1ull << 3)
#define WXL_GFX_VK_CAP_SHADER_FLOAT64                  (1ull << 4)
#define WXL_GFX_VK_CAP_STORAGE_BUFFER_16BIT            (1ull << 5)   ///< 16-bit members in storage buffers; DXVK 3
#define WXL_GFX_VK_CAP_UNIFORM_BUFFER_16BIT            (1ull << 6)   ///< 16-bit members in constant buffers; never on DXVK
#define WXL_GFX_VK_CAP_PUSH_CONSTANT_16BIT             (1ull << 7)
#define WXL_GFX_VK_CAP_STORAGE_BUFFER_8BIT             (1ull << 8)   ///< DXVK 3
#define WXL_GFX_VK_CAP_UNIFORM_BUFFER_8BIT             (1ull << 9)   ///< never on DXVK
#define WXL_GFX_VK_CAP_SCALAR_BLOCK_LAYOUT             (1ull << 10)  ///< DXC -fvk-use-scalar-layout; DXVK 3
#define WXL_GFX_VK_CAP_SUBGROUP_EXTENDED_TYPES         (1ull << 11)  ///< wave ops on 8/16/64-bit types; never on DXVK
#define WXL_GFX_VK_CAP_SUBGROUP_SIZE_CONTROL           (1ull << 12)  ///< DXVK 3
#define WXL_GFX_VK_CAP_COMPUTE_FULL_SUBGROUPS          (1ull << 13)  ///< DXVK 3
#define WXL_GFX_VK_CAP_SUBGROUP_UNIFORM_CONTROL_FLOW   (1ull << 14)  ///< DXVK 3
#define WXL_GFX_VK_CAP_SUBGROUP_ROTATE                 (1ull << 15)  ///< never on DXVK
#define WXL_GFX_VK_CAP_MAXIMAL_RECONVERGENCE           (1ull << 16)  ///< never on DXVK
#define WXL_GFX_VK_CAP_QUAD_CONTROL                    (1ull << 17)  ///< never on DXVK
#define WXL_GFX_VK_CAP_VULKAN_MEMORY_MODEL             (1ull << 18)
#define WXL_GFX_VK_CAP_VULKAN_MEMORY_MODEL_DEVICE_SCOPE (1ull << 19) ///< never on DXVK: no -fspv-use-vulkan-memory-model
#define WXL_GFX_VK_CAP_BUFFER_DEVICE_ADDRESS           (1ull << 20)
#define WXL_GFX_VK_CAP_DESCRIPTOR_INDEXING             (1ull << 21)  ///< runtime arrays, partially bound
#define WXL_GFX_VK_CAP_NON_UNIFORM_INDEXING            (1ull << 22)  ///< NonUniformResourceIndex on images and buffers; DXVK 3
#define WXL_GFX_VK_CAP_INLINE_UNIFORM_BLOCK            (1ull << 23)  ///< DXVK 3
#define WXL_GFX_VK_CAP_ROBUSTNESS2                     (1ull << 24)  ///< robustBufferAccess2 + nullDescriptor
#define WXL_GFX_VK_CAP_TIMELINE_SEMAPHORE              (1ull << 25)
#define WXL_GFX_VK_CAP_SYNCHRONIZATION2                (1ull << 26)
#define WXL_GFX_VK_CAP_DYNAMIC_RENDERING               (1ull << 27)
#define WXL_GFX_VK_CAP_MAINTENANCE4                    (1ull << 28)
#define WXL_GFX_VK_CAP_MAINTENANCE5                    (1ull << 29)
#define WXL_GFX_VK_CAP_MAINTENANCE6                    (1ull << 30)
#define WXL_GFX_VK_CAP_DEMOTE_TO_HELPER                (1ull << 31)
#define WXL_GFX_VK_CAP_ZERO_INIT_WORKGROUP_MEMORY      (1ull << 32)
#define WXL_GFX_VK_CAP_FLOAT_CONTROLS2                 (1ull << 33)  ///< DXVK 3
#define WXL_GFX_VK_CAP_INTEGER_DOT_PRODUCT             (1ull << 34)  ///< dot4add_*; never on DXVK
#define WXL_GFX_VK_CAP_STORAGE_READ_WITHOUT_FORMAT     (1ull << 35)  ///< Unknown-format storage reads; per format (FormatFeatures)
#define WXL_GFX_VK_CAP_STORAGE_WRITE_WITHOUT_FORMAT    (1ull << 36)  ///< Unknown-format storage writes; per format (FormatFeatures)
#define WXL_GFX_VK_CAP_STORAGE_ARRAY_DYNAMIC_INDEXING  (1ull << 37)  ///< dynamic indices into storage image / buffer arrays; DXVK 3
#define WXL_GFX_VK_CAP_IMAGE_INT64_ATOMICS             (1ull << 38)  ///< never on DXVK
#define WXL_GFX_VK_CAP_BUFFER_INT64_ATOMICS            (1ull << 39)  ///< never on DXVK
#define WXL_GFX_VK_CAP_ATOMIC_FLOAT32_ADD              (1ull << 40)  ///< float atomic add on buffers; never on DXVK
#define WXL_GFX_VK_CAP_UNIFIED_IMAGE_LAYOUTS           (1ull << 41)  ///< every DXVK image stays GENERAL; DXVK 3 unless disabled in dxvk.conf
#define WXL_GFX_VK_CAP_PUSH_DESCRIPTOR                 (1ull << 42)  ///< never on DXVK
#define WXL_GFX_VK_CAP_RAY_QUERY                       (1ull << 43)  ///< never on DXVK
#define WXL_GFX_VK_CAP_ACCELERATION_STRUCTURE          (1ull << 44)  ///< never on DXVK
#define WXL_GFX_VK_CAP_COOPERATIVE_MATRIX              (1ull << 45)  ///< never on DXVK
#define WXL_GFX_VK_CAP_COMPUTE_DERIVATIVES             (1ull << 46)  ///< ddx / Sample with implicit LOD in compute; never on DXVK

/**
 * @brief DXVK's device as a compute pass sees it: versions, features, subgroups.
 *
 * Valid while the context's generation stays the same. Subgroup (wave) operations need no feature:
 * every SM 6.0 Wave* / Quad* intrinsic whose VK_SUBGROUP_FEATURE_* bit is in subgroupOperations works
 * in compute on 32-bit types (8/16/64-bit types need WXL_GFX_VK_CAP_SUBGROUP_EXTENDED_TYPES).
 */
typedef struct WXL_GfxVkCaps
{
    uint32_t               structSize;
    uint32_t               apiVersion;         ///< usable on the device: the lower of DXVK's instance version (1.3) and the device's
    uint32_t               deviceApiVersion;   ///< the physical device's own, as vulkaninfo shows it
    uint32_t               spirvVersion;       ///< highest SPIR-V a module may declare (word 1: 0x00010600 is 1.6)
    uint32_t               dxvkGeneration;     ///< 3 for DXVK 3.x, 2 before: the table `enabled` comes from
    uint64_t               enabled;            ///< WXL_GFX_VK_CAP_*: usable on DXVK's device
    uint64_t               supported;          ///< WXL_GFX_VK_CAP_*: the hardware's, enabled or not
    uint32_t               subgroupSize;       ///< the default wave size of a compute dispatch
    uint32_t               minSubgroupSize;
    uint32_t               maxSubgroupSize;
    uint32_t               maxComputeWorkgroupSubgroups;
    VkShaderStageFlags     subgroupStages;
    VkSubgroupFeatureFlags subgroupOperations; ///< VK_SUBGROUP_FEATURE_*: which wave intrinsics map
    VkBool32               subgroupQuadOperationsInAllStages;
    VkShaderStageFlags     requiredSubgroupSizeStages;
    VkDriverId             driverId;
    char                   driverName[VK_MAX_DRIVER_NAME_SIZE];
    char                   driverInfo[VK_MAX_DRIVER_INFO_SIZE];
} WXL_GfxVkCaps;

/// An image as a compute pass uses it. For a service image, layout is always VK_IMAGE_LAYOUT_GENERAL.
typedef struct WXL_GfxVkImage
{
    VkImage            image;
    VkImageView        view;      ///< the whole image; for a depth-stencil format, its depth aspect
    VkFormat           format;
    VkImageType        type;
    VkExtent3D         extent;
    uint32_t           mipLevels;
    VkImageLayout      layout;    ///< the layout it is in inside the block (GENERAL)
    VkImageAspectFlags aspect;    ///< of view
} WXL_GfxVkImage;

typedef struct WXL_GfxVkImageDesc
{
    uint32_t          structSize;
    const char*       name;        ///< for the log and the panel; copied
    VkImageType       type;        ///< VK_IMAGE_TYPE_2D or VK_IMAGE_TYPE_3D
    VkFormat          format;
    uint32_t          width, height, depth;   ///< depth 1 for a 2D image
    uint32_t          mipLevels;   ///< 0 means 1
    VkImageUsageFlags usage;       ///< STORAGE and/or SAMPLED; TRANSFER_SRC | TRANSFER_DST are always added
} WXL_GfxVkImageDesc;

/// A range of a per-frame host-visible buffer, mapped: write through `mapped` during the record
/// callback, then use buffer + offset in the same frame's commands.
typedef struct WXL_GfxVkAlloc
{
    VkBuffer     buffer;
    VkDeviceSize offset;
    VkDeviceSize size;
    void*        mapped;
} WXL_GfxVkAlloc;

typedef struct WXL_GfxVkBinding
{
    uint32_t         binding;
    VkDescriptorType type;
    uint32_t         count;       ///< 0 means 1
} WXL_GfxVkBinding;

/**
 * @brief A compute pipeline: one SPIR-V module, one descriptor set (set 0) and optional push constants.
 *
 * The bindings describe set 0 for every stage the pipeline has (compute). SPIR-V is compiled offline
 * (DXC: -spirv -T cs_6_0 -fspv-target-env=vulkan1.3, or glslang) and embedded in the consumer. A
 * module newer than WXL_GfxVkCaps::spirvVersion is refused.
 */
typedef struct WXL_GfxVkPipelineDesc
{
    uint32_t                structSize;
    const char*             name;           ///< for the log; copied
    const uint32_t*         spirv;
    size_t                  spirvBytes;     ///< a multiple of 4
    const char*             entry;          ///< NULL = "main"
    const WXL_GfxVkBinding* bindings;
    uint32_t                bindingCount;
    uint32_t                pushConstantBytes;   ///< 0 for none; a multiple of 4, at most 128
} WXL_GfxVkPipelineDesc;

typedef struct WXL_GfxVkPipeline
{
    VkPipeline            pipeline;
    VkPipelineLayout      layout;
    VkDescriptorSetLayout setLayout;
} WXL_GfxVkPipeline;

#define WXL_GFX_VK_SAMPLER_POINT_CLAMP  0u
#define WXL_GFX_VK_SAMPLER_LINEAR_CLAMP 1u
#define WXL_GFX_VK_SAMPLER_POINT_WRAP   2u
#define WXL_GFX_VK_SAMPLER_LINEAR_WRAP  3u

/**
 * @brief What a compute pass records with.
 *
 * Valid for the duration of the record callback. The frame's shared targets arrive imported: their
 * `image` is VK_NULL_HANDLE when absent this frame (frame->available says the same).
 */
typedef struct WXL_GfxVkFrame
{
    uint32_t            structSize;
    VkCommandBuffer     cmd;            ///< recording; bind, dispatch, barrier into it. Never end or submit it
    uint32_t            slot;           ///< 0 .. WXL_GFX_VK_FRAMES_IN_FLIGHT - 1, for your own per-frame rings
    const WXL_GfxFrame* frame;          ///< the D3D9 frame: view, time, sizes, availability
    WXL_GfxVkImage      depth;          ///< the INTZ depth (sample the depth aspect: the hardware depth, 0..1)
    WXL_GfxVkImage      normals;        ///< the G-buffer normal target
    WXL_GfxVkImage      sceneColor;     ///< the HDR scene colour, when the world was drawn in HDR
    WXL_GfxVkImage      albedo;         ///< the G-buffer albedo target (WXL_GFX_NEED_ALBEDO)
} WXL_GfxVkFrame;

typedef void(__cdecl* WXL_GfxVkRecordFn)(void* user, const WXL_GfxVkFrame* frame);

typedef struct WXL_GfxVkPassDesc
{
    uint32_t          structSize;
    const char*       name;     ///< for the log and the panel; copied
    int32_t           order;    ///< lower records first (WXL_GFX_ORDER_*); equal orders keep add order
    WXL_GfxWantsFn    wants;    ///< required: WXL_GFX_NEED_* (GraphicsExtendApi.h), 0 to sit out
    WXL_GfxVkRecordFn record;   ///< required
    void*             user;
} WXL_GfxVkPassDesc;

typedef struct WXL_GfxVulkanApi
{
    uint32_t structSize;
    uint32_t apiVersion;

    /// Non-zero when the client's device is DXVK's and its interop answered. Known once the device
    /// exists (from the first world pass on); 0 before, and for good on native D3D9.
    int(__cdecl* Available)(void);

    /// Fills the context; 0 when not Available. Set out->structSize first.
    int(__cdecl* GetContext)(WXL_GfxVkContext* out);

    /// One line for a panel or a log: DXVK found or not, device, queue, the block's last GPU time.
    const char*(__cdecl* Status)(void);

    // --- compute passes ------------------------------------------------------------------------------

    /// Adds a compute pass for the process lifetime; its wants join the scheduler's. 0 when invalid.
    uint32_t(__cdecl* AddComputePass)(const WXL_GfxVkPassDesc* desc);

    /// Smoothed GPU milliseconds of a compute pass (Vulkan timestamps); -1 until measured.
    float(__cdecl* ComputePassGpuMs)(uint32_t passId);

    // --- inside a record callback ----------------------------------------------------------------------

    /**
     * @brief The Vulkan image behind a D3D9 texture or surface, moved to GENERAL for this block.
     *
     * Only inside a record callback. The same texture imported twice in one block returns the same
     * import. The view is the whole resource (depth aspect for a depth-stencil format) and lives
     * until the block's GPU work is done. 0 when the resource has no GPU image (SYSTEMMEM, SCRATCH).
     */
    int(__cdecl* ImportTexture)(void* d3dResource, WXL_GfxVkImage* out);

    /**
     * @brief Records a copy from a service image into a D3D9 texture, for a D3D9 pass to sample.
     *
     * Only inside a record callback. The texture is imported as needed; formats must be
     * copy-compatible (the D3D9 format's Vulkan equivalent in DXVK, e.g. A16B16G16R16F and
     * VK_FORMAT_R16G16B16A16_SFLOAT) and the extents equal (mip 0; a volume texture for a 3D image).
     * Put CmdBarrier between the writes and this copy.
     */
    int(__cdecl* CopyToTexture)(const WXL_GfxVkImage* source, void* d3dTexture);

    /// A global memory barrier: every earlier compute / transfer write made visible to every later
    /// compute / transfer read and write. Between two dispatches that depend on each other.
    void(__cdecl* CmdBarrier)(VkCommandBuffer cmd);

    /// Host-visible memory for this frame's uniform data (aligned to minUniformBufferOffsetAlignment).
    int(__cdecl* AllocUniform)(VkDeviceSize size, WXL_GfxVkAlloc* out);

    /// Host-visible memory for this frame's uploads (vkCmdCopyBufferToImage from it), 16-byte aligned.
    int(__cdecl* AllocStaging)(VkDeviceSize size, WXL_GfxVkAlloc* out);

    /// Uploads bytes into a whole service image (mip 0) through this frame's staging memory: tightly
    /// packed rows, the image's own texel size. Records the copy and the barrier after it.
    int(__cdecl* UploadImage)(const WXL_GfxVkImage* image, const void* data, size_t bytes);

    /// A descriptor set for a pipeline's set 0, from this frame's pool; VK_NULL_HANDLE when out.
    VkDescriptorSet(__cdecl* AllocDescriptorSet)(VkDescriptorSetLayout layout);

    // --- objects kept for you (destroyed with the Vulkan device) ---------------------------------------

    /// A service image in GENERAL. Create outside or inside a record callback (the first layout
    /// transition is recorded into the next block). 0 when refused.
    int(__cdecl* CreateImage)(const WXL_GfxVkImageDesc* desc, WXL_GfxVkImage* out);

    /// Destroys a service image once the GPU is done with it.
    void(__cdecl* DestroyImage)(const WXL_GfxVkImage* image);

    /// A compute pipeline; 0 when the SPIR-V or the layout is refused (the reason is logged).
    int(__cdecl* CreateComputePipeline)(const WXL_GfxVkPipelineDesc* desc, WXL_GfxVkPipeline* out);

    void(__cdecl* DestroyPipeline)(const WXL_GfxVkPipeline* pipeline);

    /// A shared sampler (WXL_GFX_VK_SAMPLER_*).
    VkSampler(__cdecl* Sampler)(uint32_t which);

    /// A shared WXL_GFX_TEX_* texture (GraphicsExtendApi.h) as a service image, uploaded through
    /// Vulkan so a compute pass reads it filled; VK_NULL_HANDLE image while unavailable (blue noise
    /// still baking). 0 when not Available.
    int(__cdecl* SharedTexture)(uint32_t which, WXL_GfxVkImage* out);

    // --- capabilities: present when structSize covers them (wxl::gfx::vk::GetCaps checks) -----------

    /// Fills the capabilities; 0 when not Available. Set out->structSize first: an older, shorter
    /// struct receives what fits.
    int(__cdecl* GetCaps)(WXL_GfxVkCaps* out);

    /// A format's VkFormatFeatureFlags2 for optimal tiling (VkFormatProperties3); 0 when not Available.
    /// VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT: a storage image of this format may be read
    /// through an Unknown-format declaration (DXC -fspv-use-unknown-image-format).
    VkFormatFeatureFlags2(__cdecl* FormatFeatures)(VkFormat format);
} WXL_GfxVulkanApi;

#ifdef __cplusplus
}
#endif

#endif // WXL_GRAPHICS_VULKAN_API_H
