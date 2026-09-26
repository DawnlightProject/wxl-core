// wxl-graphics-extend: what the Vulkan side knows about image formats -- texel sizes, aspects, names.
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

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#include <cstddef>
#include <cstdint>

// Header-only and Windows-free, so a host test can exercise it. Only uncompressed formats are
// known: an upload (UploadImage) or a copy (CopyToTexture) of anything else is refused.
namespace wxl::gfx::vulkan::formats
{
    /// Bytes per texel of an uncompressed colour format; 0 for a format the service does not handle.
    constexpr uint32_t TexelBytes(VkFormat f)
    {
        switch (f)
        {
        case VK_FORMAT_R4G4_UNORM_PACK8:
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_USCALED: case VK_FORMAT_R8_SSCALED:
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_SRGB:
            return 1;
        case VK_FORMAT_R4G4B4A4_UNORM_PACK16: case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
        case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16:
        case VK_FORMAT_R5G5B5A1_UNORM_PACK16: case VK_FORMAT_B5G5R5A1_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_USCALED: case VK_FORMAT_R8G8_SSCALED:
        case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8_SRGB:
        case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_USCALED: case VK_FORMAT_R16_SSCALED:
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16_SFLOAT:
            return 2;
        case VK_FORMAT_R8G8B8_UNORM: case VK_FORMAT_R8G8B8_SNORM: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R8G8B8_SRGB:
        case VK_FORMAT_B8G8R8_UNORM: case VK_FORMAT_B8G8R8_SNORM: case VK_FORMAT_B8G8R8_UINT: case VK_FORMAT_B8G8R8_SINT: case VK_FORMAT_B8G8R8_SRGB:
            return 3;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_USCALED: case VK_FORMAT_R8G8B8A8_SSCALED:
        case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SNORM: case VK_FORMAT_B8G8R8A8_USCALED: case VK_FORMAT_B8G8R8A8_SSCALED:
        case VK_FORMAT_B8G8R8A8_UINT: case VK_FORMAT_B8G8R8A8_SINT: case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SNORM_PACK32: case VK_FORMAT_A8B8G8R8_USCALED_PACK32:
        case VK_FORMAT_A8B8G8R8_SSCALED_PACK32: case VK_FORMAT_A8B8G8R8_UINT_PACK32: case VK_FORMAT_A8B8G8R8_SINT_PACK32:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_SNORM_PACK32: case VK_FORMAT_A2R10G10B10_USCALED_PACK32:
        case VK_FORMAT_A2R10G10B10_SSCALED_PACK32: case VK_FORMAT_A2R10G10B10_UINT_PACK32: case VK_FORMAT_A2R10G10B10_SINT_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2B10G10R10_SNORM_PACK32: case VK_FORMAT_A2B10G10R10_USCALED_PACK32:
        case VK_FORMAT_A2B10G10R10_SSCALED_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2B10G10R10_SINT_PACK32:
        case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_USCALED: case VK_FORMAT_R16G16_SSCALED:
        case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
            return 4;
        case VK_FORMAT_R16G16B16_UNORM: case VK_FORMAT_R16G16B16_SNORM: case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16_SINT:
        case VK_FORMAT_R16G16B16_SFLOAT:
            return 6;
        case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_USCALED:
        case VK_FORMAT_R16G16B16A16_SSCALED: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R64_UINT: case VK_FORMAT_R64_SINT: case VK_FORMAT_R64_SFLOAT:
            return 8;
        case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32_SFLOAT:
            return 12;
        case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_R64G64_UINT: case VK_FORMAT_R64G64_SINT: case VK_FORMAT_R64G64_SFLOAT:
            return 16;
        case VK_FORMAT_R64G64B64_UINT: case VK_FORMAT_R64G64B64_SINT: case VK_FORMAT_R64G64B64_SFLOAT:
            return 24;
        case VK_FORMAT_R64G64B64A64_UINT: case VK_FORMAT_R64G64B64A64_SINT: case VK_FORMAT_R64G64B64A64_SFLOAT:
            return 32;
        default:
            return 0;
        }
    }

    constexpr bool HasDepth(VkFormat f)
    {
        switch (f)
        {
        case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return true;
        default:
            return false;
        }
    }

    constexpr bool HasStencil(VkFormat f)
    {
        switch (f)
        {
        case VK_FORMAT_S8_UINT: case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return true;
        default:
            return false;
        }
    }

    /// Every aspect the format has: a layout transition must name them all (a combined depth-stencil
    /// image moves both aspects together without separateDepthStencilLayouts).
    constexpr VkImageAspectFlags BarrierAspect(VkFormat f)
    {
        VkImageAspectFlags a = 0;
        if (HasDepth(f))   a |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (HasStencil(f)) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
        return a ? a : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);
    }

    /// The one aspect a view samples: depth for a depth format (the stencil is not what a pass
    /// reads), stencil for a stencil-only one, colour otherwise.
    constexpr VkImageAspectFlags ViewAspect(VkFormat f)
    {
        if (HasDepth(f))   return VK_IMAGE_ASPECT_DEPTH_BIT;
        if (HasStencil(f)) return VK_IMAGE_ASPECT_STENCIL_BIT;
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }

    /// For the log and the panel: the common ones by name, the rest as "VkFormat(n)" through buf.
    inline const char* Name(VkFormat f, char* buf, size_t cap)
    {
        switch (f)
        {
        case VK_FORMAT_R8_UNORM: return "R8_UNORM";
        case VK_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
        case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
        case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM";
        case VK_FORMAT_R16_UNORM: return "R16_UNORM";
        case VK_FORMAT_R16_SFLOAT: return "R16_SFLOAT";
        case VK_FORMAT_R16G16_UNORM: return "R16G16_UNORM";
        case VK_FORMAT_R16G16_SFLOAT: return "R16G16_SFLOAT";
        case VK_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
        case VK_FORMAT_R32_UINT: return "R32_UINT";
        case VK_FORMAT_R32_SFLOAT: return "R32_SFLOAT";
        case VK_FORMAT_R32G32_SFLOAT: return "R32G32_SFLOAT";
        case VK_FORMAT_R32G32B32A32_SFLOAT: return "R32G32B32A32_SFLOAT";
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "B10G11R11_UFLOAT";
        case VK_FORMAT_D16_UNORM: return "D16_UNORM";
        case VK_FORMAT_D32_SFLOAT: return "D32_SFLOAT";
        case VK_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32_SFLOAT_S8_UINT";
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return "BC1_RGBA_UNORM";
        case VK_FORMAT_BC2_UNORM_BLOCK: return "BC2_UNORM";
        case VK_FORMAT_BC3_UNORM_BLOCK: return "BC3_UNORM";
        case VK_FORMAT_UNDEFINED: return "UNDEFINED";
        default:
            break;
        }
        // Spelled out by hand: no snprintf here keeps the header free of <cstdio> for the host test.
        unsigned v = static_cast<unsigned>(f);
        char digits[12];
        size_t n = 0;
        do { digits[n++] = char('0' + v % 10); v /= 10; } while (v && n < sizeof digits);
        const char* prefix = "VkFormat(";
        size_t i = 0;
        while (prefix[i] && i + 1 < cap) { buf[i] = prefix[i]; ++i; }
        while (n && i + 1 < cap) buf[i++] = digits[--n];
        if (i + 1 < cap) buf[i++] = ')';
        buf[i < cap ? i : cap - 1] = '\0';
        return buf;
    }
}
