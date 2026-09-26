// wxl-graphics-extend: what DXVK's device offers a compute pass -- versions, features, subgroups.
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

#include "Caps.hpp"
#include "Device.hpp"
#include "Formats.hpp"
#include "../core/Extension.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    namespace device = wxl::gfx::vulkan::device;
    namespace formats = wxl::gfx::vulkan::formats;

    // DXVK creates its instance for Vulkan 1.3 in 2.x and 3.x (DxvkVulkanApiVersion,
    // src/dxvk/dxvk_instance.h): a 1.4 device is still a 1.3 device through it.
    constexpr uint32_t kDxvkInstanceApi = VK_API_VERSION_1_3;

    // VK_KHR_unified_image_layouts is newer than deps/vulkan-headers (1.3.290); as in 1.4.350.
    constexpr const char*     kUnifiedLayoutsName = "VK_KHR_unified_image_layouts";
    constexpr VkStructureType kUnifiedLayoutsType = VkStructureType(1000527000);
    struct UnifiedImageLayoutsFeatures
    {
        VkStructureType sType;
        void*           pNext;
        VkBool32        unifiedImageLayouts;
        VkBool32        unifiedImageLayoutsVideo;
    };
    // Promoted from the NV extension with the same structure and sType.
    constexpr const char* kComputeDerivativesKhrName = "VK_KHR_compute_shader_derivatives";

    // What DXVK enables whenever the hardware has it: getFeatureList in src/dxvk/dxvk_device_info.cpp,
    // v2.7.1 for 2.x, v3.0 to v3.1.1 for 3.x (the same for these bits). A bit in neither table is
    // never enabled on DXVK's device.
    constexpr uint64_t kDxvk2 = WXL_GFX_VK_CAP_SHADER_INT8 | WXL_GFX_VK_CAP_SHADER_INT16 | WXL_GFX_VK_CAP_SHADER_INT64
                              | WXL_GFX_VK_CAP_SHADER_FLOAT64 | WXL_GFX_VK_CAP_PUSH_CONSTANT_16BIT
                              | WXL_GFX_VK_CAP_VULKAN_MEMORY_MODEL | WXL_GFX_VK_CAP_BUFFER_DEVICE_ADDRESS
                              | WXL_GFX_VK_CAP_DESCRIPTOR_INDEXING | WXL_GFX_VK_CAP_ROBUSTNESS2
                              | WXL_GFX_VK_CAP_TIMELINE_SEMAPHORE | WXL_GFX_VK_CAP_SYNCHRONIZATION2
                              | WXL_GFX_VK_CAP_DYNAMIC_RENDERING | WXL_GFX_VK_CAP_MAINTENANCE4
                              | WXL_GFX_VK_CAP_MAINTENANCE5 | WXL_GFX_VK_CAP_MAINTENANCE6
                              | WXL_GFX_VK_CAP_DEMOTE_TO_HELPER | WXL_GFX_VK_CAP_ZERO_INIT_WORKGROUP_MEMORY;
    constexpr uint64_t kDxvk3 = kDxvk2 | WXL_GFX_VK_CAP_SHADER_FLOAT16 | WXL_GFX_VK_CAP_STORAGE_BUFFER_16BIT
                              | WXL_GFX_VK_CAP_STORAGE_BUFFER_8BIT | WXL_GFX_VK_CAP_SCALAR_BLOCK_LAYOUT
                              | WXL_GFX_VK_CAP_SUBGROUP_SIZE_CONTROL | WXL_GFX_VK_CAP_COMPUTE_FULL_SUBGROUPS
                              | WXL_GFX_VK_CAP_SUBGROUP_UNIFORM_CONTROL_FLOW | WXL_GFX_VK_CAP_NON_UNIFORM_INDEXING
                              | WXL_GFX_VK_CAP_INLINE_UNIFORM_BLOCK | WXL_GFX_VK_CAP_FLOAT_CONTROLS2
                              | WXL_GFX_VK_CAP_STORAGE_ARRAY_DYNAMIC_INDEXING | WXL_GFX_VK_CAP_UNIFIED_IMAGE_LAYOUTS;

    // Vulkan 1.3 itself allows Unknown-format storage images, format by format (format features 2).
    constexpr uint64_t kCore13 = WXL_GFX_VK_CAP_STORAGE_READ_WITHOUT_FORMAT | WXL_GFX_VK_CAP_STORAGE_WRITE_WITHOUT_FORMAT;

    struct Word
    {
        uint64_t    bit;
        const char* word;
    };

    constexpr Word kCapWords[] = {
        { WXL_GFX_VK_CAP_SHADER_FLOAT16, "float16" },
        { WXL_GFX_VK_CAP_SHADER_INT8, "int8" },
        { WXL_GFX_VK_CAP_SHADER_INT16, "int16" },
        { WXL_GFX_VK_CAP_SHADER_INT64, "int64" },
        { WXL_GFX_VK_CAP_SHADER_FLOAT64, "float64" },
        { WXL_GFX_VK_CAP_STORAGE_BUFFER_16BIT, "storage-16bit" },
        { WXL_GFX_VK_CAP_UNIFORM_BUFFER_16BIT, "uniform-16bit" },
        { WXL_GFX_VK_CAP_PUSH_CONSTANT_16BIT, "push-16bit" },
        { WXL_GFX_VK_CAP_STORAGE_BUFFER_8BIT, "storage-8bit" },
        { WXL_GFX_VK_CAP_UNIFORM_BUFFER_8BIT, "uniform-8bit" },
        { WXL_GFX_VK_CAP_SCALAR_BLOCK_LAYOUT, "scalar-layout" },
        { WXL_GFX_VK_CAP_SUBGROUP_EXTENDED_TYPES, "subgroup-extended-types" },
        { WXL_GFX_VK_CAP_SUBGROUP_SIZE_CONTROL, "subgroup-size-control" },
        { WXL_GFX_VK_CAP_COMPUTE_FULL_SUBGROUPS, "full-subgroups" },
        { WXL_GFX_VK_CAP_SUBGROUP_UNIFORM_CONTROL_FLOW, "subgroup-uniform-control-flow" },
        { WXL_GFX_VK_CAP_SUBGROUP_ROTATE, "subgroup-rotate" },
        { WXL_GFX_VK_CAP_MAXIMAL_RECONVERGENCE, "maximal-reconvergence" },
        { WXL_GFX_VK_CAP_QUAD_CONTROL, "quad-control" },
        { WXL_GFX_VK_CAP_VULKAN_MEMORY_MODEL, "memory-model" },
        { WXL_GFX_VK_CAP_VULKAN_MEMORY_MODEL_DEVICE_SCOPE, "memory-model-device-scope" },
        { WXL_GFX_VK_CAP_BUFFER_DEVICE_ADDRESS, "buffer-device-address" },
        { WXL_GFX_VK_CAP_DESCRIPTOR_INDEXING, "descriptor-indexing" },
        { WXL_GFX_VK_CAP_NON_UNIFORM_INDEXING, "non-uniform-indexing" },
        { WXL_GFX_VK_CAP_INLINE_UNIFORM_BLOCK, "inline-uniform-block" },
        { WXL_GFX_VK_CAP_ROBUSTNESS2, "robustness2" },
        { WXL_GFX_VK_CAP_TIMELINE_SEMAPHORE, "timeline-semaphore" },
        { WXL_GFX_VK_CAP_SYNCHRONIZATION2, "synchronization2" },
        { WXL_GFX_VK_CAP_DYNAMIC_RENDERING, "dynamic-rendering" },
        { WXL_GFX_VK_CAP_MAINTENANCE4, "maintenance4" },
        { WXL_GFX_VK_CAP_MAINTENANCE5, "maintenance5" },
        { WXL_GFX_VK_CAP_MAINTENANCE6, "maintenance6" },
        { WXL_GFX_VK_CAP_DEMOTE_TO_HELPER, "demote-to-helper" },
        { WXL_GFX_VK_CAP_ZERO_INIT_WORKGROUP_MEMORY, "zero-init-groupshared" },
        { WXL_GFX_VK_CAP_FLOAT_CONTROLS2, "float-controls2" },
        { WXL_GFX_VK_CAP_INTEGER_DOT_PRODUCT, "integer-dot-product" },
        { WXL_GFX_VK_CAP_STORAGE_READ_WITHOUT_FORMAT, "storage-read-unformatted" },
        { WXL_GFX_VK_CAP_STORAGE_WRITE_WITHOUT_FORMAT, "storage-write-unformatted" },
        { WXL_GFX_VK_CAP_STORAGE_ARRAY_DYNAMIC_INDEXING, "storage-array-indexing" },
        { WXL_GFX_VK_CAP_IMAGE_INT64_ATOMICS, "image-int64-atomics" },
        { WXL_GFX_VK_CAP_BUFFER_INT64_ATOMICS, "buffer-int64-atomics" },
        { WXL_GFX_VK_CAP_ATOMIC_FLOAT32_ADD, "float32-atomic-add" },
        { WXL_GFX_VK_CAP_UNIFIED_IMAGE_LAYOUTS, "unified-image-layouts" },
        { WXL_GFX_VK_CAP_PUSH_DESCRIPTOR, "push-descriptor" },
        { WXL_GFX_VK_CAP_RAY_QUERY, "ray-query" },
        { WXL_GFX_VK_CAP_ACCELERATION_STRUCTURE, "acceleration-structure" },
        { WXL_GFX_VK_CAP_COOPERATIVE_MATRIX, "cooperative-matrix" },
        { WXL_GFX_VK_CAP_COMPUTE_DERIVATIVES, "compute-derivatives" },
    };

    constexpr Word kSubgroupWords[] = {
        { VK_SUBGROUP_FEATURE_BASIC_BIT, "basic" },
        { VK_SUBGROUP_FEATURE_VOTE_BIT, "vote" },
        { VK_SUBGROUP_FEATURE_ARITHMETIC_BIT, "arithmetic" },
        { VK_SUBGROUP_FEATURE_BALLOT_BIT, "ballot" },
        { VK_SUBGROUP_FEATURE_SHUFFLE_BIT, "shuffle" },
        { VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT, "shuffle-relative" },
        { VK_SUBGROUP_FEATURE_CLUSTERED_BIT, "clustered" },
        { VK_SUBGROUP_FEATURE_QUAD_BIT, "quad" },
        { VK_SUBGROUP_FEATURE_PARTITIONED_BIT_NV, "partitioned-nv" },
        { VK_SUBGROUP_FEATURE_ROTATE_BIT_KHR, "rotate" },
        { VK_SUBGROUP_FEATURE_ROTATE_CLUSTERED_BIT_KHR, "rotate-clustered" },
    };

    const char* Words(uint64_t bits, const Word* words, size_t count, char* buf, size_t cap)
    {
        if (!cap) return "";
        size_t n = 0;
        buf[0] = '\0';
        for (size_t i = 0; i < count; ++i)
        {
            if (!(bits & words[i].bit) || n + 1 >= cap) continue;
            const int wrote = std::snprintf(buf + n, cap - n, "%s%s", n ? " " : "", words[i].word);
            if (wrote > 0) n = std::min(cap - 1, n + size_t(wrote));
        }
        if (!buf[0]) std::snprintf(buf, cap, "none");
        return buf;
    }

    /// DXVK 3.0 and later answer ID3D9VkExtInterface on their factory; an interop without it is 2.x.
    uint32_t Generation(void* d3dDevice)
    {
        auto* dev = static_cast<IDirect3DDevice9*>(d3dDevice);
        IDirect3D9* factory = nullptr;
        if (!dev || FAILED(dev->GetDirect3D(&factory)) || !factory) return 2;
        IUnknown* ext = nullptr;
        const bool three = SUCCEEDED(factory->QueryInterface(__uuidof(ID3D9VkExtInterface), reinterpret_cast<void**>(&ext))) && ext;
        if (ext) ext->Release();
        factory->Release();
        return three ? 3u : 2u;
    }

    /// The highest SPIR-V a Vulkan version accepts, as word 1 of a module spells it.
    uint32_t SpirvFor(uint32_t api)
    {
        const uint32_t minor = VK_API_VERSION_MAJOR(api) > 1 ? 99u : VK_API_VERSION_MINOR(api);
        if (minor >= 3) return 0x00010600u;
        if (minor == 2) return 0x00010500u;
        if (minor == 1) return 0x00010300u;
        return 0x00010000u;
    }

    VkFormatFeatureFlags2 FormatFeaturesOn(const device::Context& c, VkFormat format)
    {
        VkFormatProperties3 p3{};
        p3.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;
        VkFormatProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
        p2.pNext = &p3;
        c.fn.vkGetPhysicalDeviceFormatProperties2(c.physical, format, &p2);
        return p3.optimalTilingFeatures;
    }
}

namespace wxl::gfx::vulkan::caps
{
    void Query(const device::Context& c, void* d3dDevice, WXL_GfxVkCaps& out)
    {
        out = WXL_GfxVkCaps{};
        out.structSize = sizeof out;

        uint32_t count = 0;
        c.fn.vkEnumerateDeviceExtensionProperties(c.physical, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> exts(count);
        if (count) c.fn.vkEnumerateDeviceExtensionProperties(c.physical, nullptr, &count, exts.data());
        exts.resize(count);
        auto has = [&](const char* name) {
            for (const VkExtensionProperties& e : exts)
                if (std::strcmp(e.extensionName, name) == 0) return true;
            return false;
        };

        // The core blocks always (DXVK needs 1.3); an extension's block only when it is advertised.
        VkPhysicalDeviceFeatures2 f{};
        f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        VkPhysicalDeviceVulkan11Features f11{};
        f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        VkPhysicalDeviceVulkan12Features f12{};
        f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceVulkan13Features f13{};
        f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceRobustness2FeaturesEXT robust{};
        robust.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
        VkPhysicalDeviceMaintenance5FeaturesKHR m5{};
        m5.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR;
        VkPhysicalDeviceMaintenance6FeaturesKHR m6{};
        m6.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR;
        VkPhysicalDeviceShaderFloatControls2FeaturesKHR fc2{};
        fc2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT_CONTROLS_2_FEATURES_KHR;
        VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR ucf{};
        ucf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_FEATURES_KHR;
        VkPhysicalDeviceShaderSubgroupRotateFeaturesKHR rotate{};
        rotate.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_ROTATE_FEATURES_KHR;
        VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR reconverge{};
        reconverge.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR;
        VkPhysicalDeviceShaderQuadControlFeaturesKHR quad{};
        quad.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_QUAD_CONTROL_FEATURES_KHR;
        VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT imageI64{};
        imageI64.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT;
        VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomicFloat{};
        atomicFloat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;
        VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{};
        rayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR accel{};
        accel.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{};
        coop.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
        VkPhysicalDeviceComputeShaderDerivativesFeaturesNV derivatives{};
        derivatives.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_NV;
        UnifiedImageLayoutsFeatures unified{};
        unified.sType = kUnifiedLayoutsType;

        VkBaseOutStructure* last = reinterpret_cast<VkBaseOutStructure*>(&f);
        auto chain = [&](void* s) {
            auto* b = static_cast<VkBaseOutStructure*>(s);
            last->pNext = b;
            last = b;
        };
        chain(&f11);
        chain(&f12);
        chain(&f13);
        if (has(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)) chain(&robust);
        if (has(VK_KHR_MAINTENANCE_5_EXTENSION_NAME)) chain(&m5);
        if (has(VK_KHR_MAINTENANCE_6_EXTENSION_NAME)) chain(&m6);
        if (has(VK_KHR_SHADER_FLOAT_CONTROLS_2_EXTENSION_NAME)) chain(&fc2);
        if (has(VK_KHR_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_EXTENSION_NAME)) chain(&ucf);
        if (has(VK_KHR_SHADER_SUBGROUP_ROTATE_EXTENSION_NAME)) chain(&rotate);
        if (has(VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME)) chain(&reconverge);
        if (has(VK_KHR_SHADER_QUAD_CONTROL_EXTENSION_NAME)) chain(&quad);
        if (has(VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME)) chain(&imageI64);
        if (has(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME)) chain(&atomicFloat);
        if (has(VK_KHR_RAY_QUERY_EXTENSION_NAME)) chain(&rayQuery);
        if (has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) chain(&accel);
        if (has(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME)) chain(&coop);
        if (has(kComputeDerivativesKhrName) || has(VK_NV_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME)) chain(&derivatives);
        if (has(kUnifiedLayoutsName)) chain(&unified);
        c.fn.vkGetPhysicalDeviceFeatures2(c.physical, &f);

        VkPhysicalDeviceProperties2 p{};
        p.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        VkPhysicalDeviceVulkan11Properties p11{};
        p11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
        VkPhysicalDeviceVulkan12Properties p12{};
        p12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
        VkPhysicalDeviceVulkan13Properties p13{};
        p13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES;
        p.pNext = &p11;
        p11.pNext = &p12;
        p12.pNext = &p13;
        c.fn.vkGetPhysicalDeviceProperties2(c.physical, &p);

        const uint32_t deviceApi = p.properties.apiVersion;
        const uint32_t usable = std::min(VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(deviceApi), VK_API_VERSION_MINOR(deviceApi), 0),
                                         kDxvkInstanceApi);
        const bool core13 = usable >= VK_API_VERSION_1_3;
        const VkPhysicalDeviceFeatures& core = f.features;

        uint64_t s = 0;
        auto set = [&s](uint64_t bit, bool on) { if (on) s |= bit; };
        set(WXL_GFX_VK_CAP_SHADER_FLOAT16, f12.shaderFloat16);
        set(WXL_GFX_VK_CAP_SHADER_INT8, f12.shaderInt8);
        set(WXL_GFX_VK_CAP_SHADER_INT16, core.shaderInt16);
        set(WXL_GFX_VK_CAP_SHADER_INT64, core.shaderInt64);
        set(WXL_GFX_VK_CAP_SHADER_FLOAT64, core.shaderFloat64);
        set(WXL_GFX_VK_CAP_STORAGE_BUFFER_16BIT, f11.storageBuffer16BitAccess);
        set(WXL_GFX_VK_CAP_UNIFORM_BUFFER_16BIT, f11.uniformAndStorageBuffer16BitAccess);
        set(WXL_GFX_VK_CAP_PUSH_CONSTANT_16BIT, f11.storagePushConstant16);
        set(WXL_GFX_VK_CAP_STORAGE_BUFFER_8BIT, f12.storageBuffer8BitAccess);
        set(WXL_GFX_VK_CAP_UNIFORM_BUFFER_8BIT, f12.uniformAndStorageBuffer8BitAccess);
        set(WXL_GFX_VK_CAP_SCALAR_BLOCK_LAYOUT, f12.scalarBlockLayout);
        set(WXL_GFX_VK_CAP_SUBGROUP_EXTENDED_TYPES, f12.shaderSubgroupExtendedTypes);
        set(WXL_GFX_VK_CAP_SUBGROUP_SIZE_CONTROL, f13.subgroupSizeControl);
        set(WXL_GFX_VK_CAP_COMPUTE_FULL_SUBGROUPS, f13.computeFullSubgroups);
        set(WXL_GFX_VK_CAP_SUBGROUP_UNIFORM_CONTROL_FLOW, ucf.shaderSubgroupUniformControlFlow);
        set(WXL_GFX_VK_CAP_SUBGROUP_ROTATE, rotate.shaderSubgroupRotate);
        set(WXL_GFX_VK_CAP_MAXIMAL_RECONVERGENCE, reconverge.shaderMaximalReconvergence);
        set(WXL_GFX_VK_CAP_QUAD_CONTROL, quad.shaderQuadControl);
        set(WXL_GFX_VK_CAP_VULKAN_MEMORY_MODEL, f12.vulkanMemoryModel);
        set(WXL_GFX_VK_CAP_VULKAN_MEMORY_MODEL_DEVICE_SCOPE, f12.vulkanMemoryModelDeviceScope);
        set(WXL_GFX_VK_CAP_BUFFER_DEVICE_ADDRESS, f12.bufferDeviceAddress);
        set(WXL_GFX_VK_CAP_DESCRIPTOR_INDEXING, f12.runtimeDescriptorArray && f12.descriptorBindingPartiallyBound);
        set(WXL_GFX_VK_CAP_NON_UNIFORM_INDEXING, f12.shaderSampledImageArrayNonUniformIndexing
                                                 && f12.shaderStorageImageArrayNonUniformIndexing
                                                 && f12.shaderStorageBufferArrayNonUniformIndexing);
        set(WXL_GFX_VK_CAP_INLINE_UNIFORM_BLOCK, f13.inlineUniformBlock);
        set(WXL_GFX_VK_CAP_ROBUSTNESS2, robust.robustBufferAccess2 && robust.nullDescriptor);
        set(WXL_GFX_VK_CAP_TIMELINE_SEMAPHORE, f12.timelineSemaphore);
        set(WXL_GFX_VK_CAP_SYNCHRONIZATION2, f13.synchronization2);
        set(WXL_GFX_VK_CAP_DYNAMIC_RENDERING, f13.dynamicRendering);
        set(WXL_GFX_VK_CAP_MAINTENANCE4, f13.maintenance4);
        set(WXL_GFX_VK_CAP_MAINTENANCE5, m5.maintenance5);
        set(WXL_GFX_VK_CAP_MAINTENANCE6, m6.maintenance6);
        set(WXL_GFX_VK_CAP_DEMOTE_TO_HELPER, f13.shaderDemoteToHelperInvocation);
        set(WXL_GFX_VK_CAP_ZERO_INIT_WORKGROUP_MEMORY, f13.shaderZeroInitializeWorkgroupMemory);
        set(WXL_GFX_VK_CAP_FLOAT_CONTROLS2, fc2.shaderFloatControls2);
        set(WXL_GFX_VK_CAP_INTEGER_DOT_PRODUCT, f13.shaderIntegerDotProduct);
        set(WXL_GFX_VK_CAP_STORAGE_READ_WITHOUT_FORMAT, core.shaderStorageImageReadWithoutFormat || core13);
        set(WXL_GFX_VK_CAP_STORAGE_WRITE_WITHOUT_FORMAT, core.shaderStorageImageWriteWithoutFormat || core13);
        set(WXL_GFX_VK_CAP_STORAGE_ARRAY_DYNAMIC_INDEXING, core.shaderStorageImageArrayDynamicIndexing
                                                           && core.shaderStorageBufferArrayDynamicIndexing);
        set(WXL_GFX_VK_CAP_IMAGE_INT64_ATOMICS, imageI64.shaderImageInt64Atomics);
        set(WXL_GFX_VK_CAP_BUFFER_INT64_ATOMICS, f12.shaderBufferInt64Atomics);
        set(WXL_GFX_VK_CAP_ATOMIC_FLOAT32_ADD, atomicFloat.shaderBufferFloat32AtomicAdd);
        set(WXL_GFX_VK_CAP_UNIFIED_IMAGE_LAYOUTS, unified.unifiedImageLayouts);
        set(WXL_GFX_VK_CAP_PUSH_DESCRIPTOR, has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME));
        set(WXL_GFX_VK_CAP_RAY_QUERY, rayQuery.rayQuery);
        set(WXL_GFX_VK_CAP_ACCELERATION_STRUCTURE, accel.accelerationStructure);
        set(WXL_GFX_VK_CAP_COOPERATIVE_MATRIX, coop.cooperativeMatrix);
        set(WXL_GFX_VK_CAP_COMPUTE_DERIVATIVES, derivatives.computeDerivativeGroupQuads || derivatives.computeDerivativeGroupLinear);

        out.dxvkGeneration = Generation(d3dDevice);
        uint64_t e = s & (out.dxvkGeneration >= 3 ? kDxvk3 : kDxvk2);
        // DXVK drops 16-bit push constants on a device without 16-bit integers.
        if (!(s & WXL_GFX_VK_CAP_SHADER_INT16)) e &= ~WXL_GFX_VK_CAP_PUSH_CONSTANT_16BIT;
        if (core13) e |= s & kCore13;

        out.apiVersion                        = usable;
        out.deviceApiVersion                  = deviceApi;
        out.spirvVersion                      = SpirvFor(usable);
        out.enabled                           = e;
        out.supported                         = s;
        out.subgroupSize                      = p11.subgroupSize;
        out.minSubgroupSize                   = p13.minSubgroupSize;
        out.maxSubgroupSize                   = p13.maxSubgroupSize;
        out.maxComputeWorkgroupSubgroups      = p13.maxComputeWorkgroupSubgroups;
        out.subgroupStages                    = p11.subgroupSupportedStages;
        out.subgroupOperations                = p11.subgroupSupportedOperations;
        out.subgroupQuadOperationsInAllStages = p11.subgroupQuadOperationsInAllStages;
        out.requiredSubgroupSizeStages        = p13.requiredSubgroupSizeStages;
        out.driverId                          = p12.driverID;
        std::memcpy(out.driverName, p12.driverName, sizeof out.driverName);
        std::memcpy(out.driverInfo, p12.driverInfo, sizeof out.driverInfo);
        out.driverName[sizeof out.driverName - 1] = '\0';
        out.driverInfo[sizeof out.driverInfo - 1] = '\0';
    }

    void Log(const device::Context& c)
    {
        const WXL_GfxVkCaps& k = c.caps;
        char usable[16], dev[24], instance[16], words[640];
        GFX_LOG_INFO("vulkan: Vulkan %s usable (device %s through DXVK %u.x's %s instance), SPIR-V up to %u.%u; driver %s %s",
                     Version(k.apiVersion, false, usable, sizeof usable), Version(k.deviceApiVersion, true, dev, sizeof dev),
                     k.dxvkGeneration, Version(kDxvkInstanceApi, false, instance, sizeof instance),
                     (k.spirvVersion >> 16) & 0xFF, (k.spirvVersion >> 8) & 0xFF, k.driverName, k.driverInfo);
        GFX_LOG_INFO("vulkan: subgroups: %u lanes (%u..%u, at most %u per workgroup), %s in compute; size control %s, full subgroups %s; operations: %s",
                     k.subgroupSize, k.minSubgroupSize, k.maxSubgroupSize, k.maxComputeWorkgroupSubgroups,
                     (k.subgroupStages & VK_SHADER_STAGE_COMPUTE_BIT) ? "available" : "NOT available",
                     (k.enabled & WXL_GFX_VK_CAP_SUBGROUP_SIZE_CONTROL) ? "on" : "off",
                     (k.enabled & WXL_GFX_VK_CAP_COMPUTE_FULL_SUBGROUPS) ? "on" : "off",
                     SubgroupNames(k.subgroupOperations, words, sizeof words));
        GFX_LOG_INFO("vulkan: enabled on DXVK's device: %s", CapNames(k.enabled, words, sizeof words));
        GFX_LOG_INFO("vulkan: on the hardware, not enabled by DXVK: %s", CapNames(k.supported & ~k.enabled, words, sizeof words));

        // The formats compute passes store into, and whether an Unknown-format declaration may read / write them.
        static const VkFormat kFormats[] = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16_SFLOAT,
                                             VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R32G32B32A32_SFLOAT };
        size_t n = 0;
        words[0] = '\0';
        for (VkFormat fmt : kFormats)
        {
            const VkFormatFeatureFlags2 ff = FormatFeaturesOn(c, fmt);
            char name[24];
            const char* rw = !(ff & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT) ? "no storage"
                           : (ff & VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT)
                             ? ((ff & VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT) ? "rw" : "r")
                             : ((ff & VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT) ? "w" : "typed only");
            const int wrote = std::snprintf(words + n, sizeof words - n, "%s%s %s", n ? ", " : "", formats::Name(fmt, name, sizeof name), rw);
            if (wrote > 0) n = std::min(sizeof words - 1, n + size_t(wrote));
        }
        GFX_LOG_INFO("vulkan: storage images declared Unknown: %s", words);
    }

    VkFormatFeatureFlags2 FormatFeatures(VkFormat format)
    {
        return device::Available() ? FormatFeaturesOn(device::Ctx(), format) : 0;
    }

    const char* CapNames(uint64_t bits, char* buf, size_t cap)
    {
        return Words(bits, kCapWords, sizeof kCapWords / sizeof kCapWords[0], buf, cap);
    }

    const char* SubgroupNames(VkSubgroupFeatureFlags ops, char* buf, size_t cap)
    {
        return Words(ops, kSubgroupWords, sizeof kSubgroupWords / sizeof kSubgroupWords[0], buf, cap);
    }

    const char* Version(uint32_t v, bool patch, char* buf, size_t cap)
    {
        if (patch) std::snprintf(buf, cap, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
        else       std::snprintf(buf, cap, "%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v));
        return buf;
    }
}
