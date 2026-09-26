// wxl-graphics-extend: DXVK's D3D9 interop interfaces, re-stated for ABI compatibility.
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

#pragma once

// The interfaces below are DXVK's (https://github.com/doitsujin/dxvk, zlib licence): their UUIDs and
// vtable order are re-stated from upstream src/d3d9/d3d9_interfaces.h so that this extension can talk
// to DXVK's d3d9.dll through QueryInterface without depending on DXVK's headers or build. Nothing
// else is DXVK's: the comments are ours and say only what this extension relies on. Every method is
// STDMETHODCALLTYPE on an IUnknown-derived struct, as COM requires; the order of the pure virtuals is
// the vtable, and must never change.
//
// CreateImage (ID3D9VkInteropDevice, last slot) exists only since DXVK 2.5: it is declared so the
// vtable is complete, and never called. The three interop interfaces are identical in 2.7.1 and 3.1.1.
// ID3D9VkExtInterface answers on DXVK's factory since 3.0, which is how the 3.x generation is told
// apart (caps::Query).

#include "wxl/GraphicsVulkanApi.h"   // VK_NO_PROTOTYPES, then vulkan_core.h

#include <windows.h>
#include <d3d9.h>

// __uuidof: MSVC reads the __declspec(uuid); MinGW emulates it through __CRT_UUID_DECL, which must
// follow the declaration at global scope (an explicit specialisation of __mingw_uuidof).
#if defined(_MSC_VER)
#define WXL_DXVK_UUID(str) __declspec(uuid(str))
#else
#define WXL_DXVK_UUID(str)
#endif

/// On the IDirect3D9 factory: the instance and the adapters' physical devices.
struct WXL_DXVK_UUID("3461a81b-ce41-485b-b6b5-fcf08ba6a6bd") ID3D9VkInteropInterface : public IUnknown
{
    virtual void STDMETHODCALLTYPE GetInstanceHandle(VkInstance* pInstance) = 0;
    virtual void STDMETHODCALLTYPE GetPhysicalDeviceHandle(UINT Adapter, VkPhysicalDevice* pPhysicalDevice) = 0;
};

#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(ID3D9VkInteropInterface, 0x3461a81b, 0xce41, 0x485b, 0xb6, 0xb5, 0xfc, 0xf0, 0x8b, 0xa6, 0xa6, 0xbd)
#endif

/// On a texture or a surface: the VkImage behind it.
struct WXL_DXVK_UUID("d56344f5-8d35-46fd-806d-94c351b472c1") ID3D9VkInteropTexture : public IUnknown
{
    /**
     * @brief The image handle, its layout and its creation info. Any pointer may be null.
     *
     * When pInfo is given, its sType must be VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, pNext null and
     * queueFamilyIndexCount the length of pQueueFamilyIndices (0 with null). pLayout receives the
     * image's default layout (GENERAL everywhere under VK_KHR_unified_image_layouts). DXVK may leave
     * an image elsewhere across a flush -- a render target of a suspended render pass, or in 3.x
     * any image it restores lazily -- so it is the layout after device::SettleD3D9 only.
     * @return S_OK; D3DERR_NOTFOUND for a resource without a GPU image (SYSTEMMEM, SCRATCH, a NULL
     *         format); D3DERR_INVALIDCALL for a pInfo that is not as described.
     */
    virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage* pHandle, VkImageLayout* pLayout, VkImageCreateInfo* pInfo) = 0;
};

#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(ID3D9VkInteropTexture, 0xd56344f5, 0x8d35, 0x46fd, 0x80, 0x6d, 0x94, 0xc3, 0x51, 0xb4, 0x72, 0xc1)
#endif

/// On the device: the Vulkan device, its queue, and the locks around a submission of our own.
struct WXL_DXVK_UUID("2eaa4b89-0107-4bdb-87f7-0f541c493ce0") ID3D9VkInteropDevice : public IUnknown
{
    virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance* pInstance, VkPhysicalDevice* pPhysDev, VkDevice* pDevice) = 0;

    /// DXVK's rendering queue, the one every D3D9 command runs on.
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue* pQueue, uint32_t* pQueueIndex, uint32_t* pQueueFamilyIndex) = 0;

    /// Recorded on DXVK's command stream. Whatever the layouts, DXVK first ends its render pass for
    /// good (not suspended), which returns the images it left elsewhere to their default layout.
    virtual void STDMETHODCALLTYPE TransitionTextureLayout(ID3D9VkInteropTexture* pTexture, const VkImageSubresourceRange* pSubresources,
                                                           VkImageLayout OldLayout, VkImageLayout NewLayout) = 0;

    /// Submits every pending D3D9 command (always a submission, even an empty one): required before
    /// a submission of ours that touches a D3D9 resource, so that ours queues behind the D3D9 work.
    /// DXVK may move an image to new memory at a flush, so handles are read after it, not before.
    virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;

    /// Takes the queue: DXVK submits nothing until ReleaseSubmissionQueue. No D3D9 call may be made,
    /// from any thread, while it is held.
    virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;

    virtual void STDMETHODCALLTYPE LockDevice() = 0;
    virtual void STDMETHODCALLTYPE UnlockDevice() = 0;

    virtual bool STDMETHODCALLTYPE WaitForResource(IDirect3DResource9* pResource, DWORD MapFlags) = 0;

    /// DXVK 2.5 and later only: declared for the vtable, never called (see the top of this file).
    virtual HRESULT STDMETHODCALLTYPE CreateImage(const void* pDesc, IDirect3DResource9** ppResult) = 0;
};

#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(ID3D9VkInteropDevice, 0x2eaa4b89, 0x0107, 0x4bdb, 0x87, 0xf7, 0x0f, 0x54, 0x1c, 0x49, 0x3c, 0xe0)
#endif

/// On the IDirect3D9 factory, DXVK 3.0 and later: only asked for, never called. A 2.x factory logs
/// the unknown query in DXVK's log (two warning lines per device).
struct WXL_DXVK_UUID("65b55086-e3e3-4c3e-b3a0-86815cce2c4c") ID3D9VkExtInterface : public IUnknown
{
    virtual void STDMETHODCALLTYPE UnlockAdditionalFormats() = 0;
};

#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(ID3D9VkExtInterface, 0x65b55086, 0xe3e3, 0x4c3e, 0xb3, 0xa0, 0x86, 0x81, 0x5c, 0xce, 0x2c, 0x4c)
#endif

#undef WXL_DXVK_UUID
