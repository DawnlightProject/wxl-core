// wxl-host: BLP2 texture images in the form the client's texture loader reads them.
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

// Every rule here is read from the client (addresses in Wow.exe 3.3.5a 12340):
//  - GetTextureFormats (0x004B5FE0) turns the header's preferred format and alpha depth into the pixel
//    format the loader decodes to and the texture format it creates;
//  - CBLPFile::LockChain2 (0x006AFFD0) walks the levels in place for DXT and ARGB files, and decodes
//    palettized ones level by level through CBLPFile::DecompPal (0x006AF810);
//  - DecompPalFastPath (0x006AE990) and DecompPalARGB8888 (0x006AE9E0) are the two ARGB8888 decoders, with
//    the alpha tables s_oneBitAlphaLookup (0x00AD90C0) and s_eightBitAlphaLookup (0x00AD90B0).

#include "host/Blp.hpp"

#include <algorithm>
#include <cstring>

namespace wxl::hostd::blp
{
    namespace
    {
        constexpr uint8_t kOneBitAlpha[2] = { 0x00, 0xFF };
        constexpr uint8_t kFourBitAlpha[16] = { 0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238, 255 };

        // PIXEL_FORMAT and EGxTexFormat values the loader uses.
        constexpr uint32_t kPixelDxt1 = 0, kPixelDxt3 = 1, kPixelArgb8888 = 2, kPixelDxt5 = 7, kPixelUnspecified = 8;

        uint32_t U32(const uint8_t* p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }

        void PutU32(uint8_t* p, uint32_t v)
        {
            std::memcpy(p, &v, 4);
        }

        uint32_t Dim(uint32_t v, uint32_t level)
        {
            const uint32_t d = level < 32 ? v >> level : 0;
            return d ? d : 1;
        }

        /// The loader's ARGB8888 decode leaves the texture in ARGB8888 only for these (GetTextureFormats).
        bool UploadsArgb8888(uint8_t preferred, uint8_t alphaDepth)
        {
            if (preferred == kPixelArgb8888) return true;
            return preferred == kPixelUnspecified && alphaDepth != 0 && alphaDepth != 1 && alphaDepth != 4;
        }

        /// Bytes of alpha a palettized level carries after its indices.
        uint64_t AlphaBytes(uint8_t alphaDepth, uint64_t pixels)
        {
            switch (alphaDepth)
            {
            case 1: return (pixels + 7) / 8;
            case 4: return (pixels + 1) / 2;
            case 8: return pixels;
            default: return 0;
            }
        }

        /// One level, exactly as DecompPalFastPath (alpha depth 8) or DecompPalARGB8888 (the others) writes it.
        void DecodeLevel(const uint8_t* palette, uint8_t alphaDepth, const uint8_t* src, uint32_t pixels, uint8_t* dst)
        {
            const uint8_t* alpha = src + pixels;
            if (alphaDepth == 8)
            {
                for (uint32_t i = 0; i < pixels; ++i)
                {
                    std::memcpy(dst + i * 4, palette + src[i] * 4, 4);
                    dst[i * 4 + 3] = alpha[i];
                }
                return;
            }
            for (uint32_t i = 0; i < pixels; ++i)
            {
                std::memcpy(dst + i * 4, palette + src[i] * 4, 4);
                dst[i * 4 + 3] = 0xFF;
            }
            if (alphaDepth == 1)
            {
                const uint32_t whole = pixels >> 3;
                for (uint32_t k = 0; k < whole; ++k)
                    for (uint32_t bit = 0; bit < 8; ++bit)
                        dst[(k * 8 + bit) * 4 + 3] = kOneBitAlpha[(alpha[k] >> bit) & 1];
                const uint32_t rest = pixels & 7;
                for (uint32_t bit = 0; bit < rest; ++bit)
                    dst[(whole * 8 + bit) * 4 + 3] = kOneBitAlpha[(alpha[whole] >> bit) & 1];
            }
            else if (alphaDepth == 4)
            {
                const uint32_t pairs = pixels >> 1;
                for (uint32_t k = 0; k < pairs; ++k)
                {
                    dst[(k * 2) * 4 + 3] = kFourBitAlpha[alpha[k] & 0xF];
                    dst[(k * 2 + 1) * 4 + 3] = kFourBitAlpha[alpha[k] >> 4];
                }
                if (pixels & 1) dst[(pairs * 2) * 4 + 3] = kFourBitAlpha[alpha[pairs] & 0xF];
            }
        }
    }

    uint32_t LevelCount(uint32_t width, uint32_t height)
    {
        uint32_t count = 1;
        if (width == height * 6) width /= 6;
        while (width > 1 || height > 1)
        {
            ++count;
            width = std::max(1u, width >> 1);
            height = std::max(1u, height >> 1);
        }
        return count;
    }

    bool IsBlp2(const uint8_t* file, size_t size)
    {
        return file && size >= kHeaderBytes && U32(file + field::kMagic) == kMagicBlp2 && U32(file + field::kType) == 1;
    }

    bool GpuLevels(const uint8_t* image, size_t size, std::vector<Level>& out)
    {
        out.clear();
        if (!IsBlp2(image, size)) return false;
        const uint8_t compression = image[field::kCompression];
        const uint8_t alphaDepth = image[field::kAlphaDepth];
        const uint8_t preferred = image[field::kPreferredFormat];
        const uint32_t width = U32(image + field::kWidth), height = U32(image + field::kHeight);
        if (!width || !height || width == height * 6) return false;   // cube faces upload one by one

        uint32_t blockBytes = 0;
        if (compression == kCompressionDxt)
        {
            if (preferred == kPixelDxt1) blockBytes = 8;
            else if (preferred == kPixelDxt3 || preferred == kPixelDxt5) blockBytes = 16;
            else return false;   // decompressed on upload
        }
        else if (compression != kCompressionArgb || !UploadsArgb8888(preferred, alphaDepth))
        {
            return false;
        }

        // ITexWHDStartEnd (0x006A5EF0): a DXT chain stops once its smaller side reaches 4; no GPU level is smaller.
        uint32_t uploaded = kMaxLevels;
        if (blockBytes)
        {
            uploaded = 1;
            if (std::min(width, height) >= 5)
                for (uint32_t side = std::min(width, height); side > 4; side >>= 1) ++uploaded;
        }
        for (uint32_t i = 0; i < kMaxLevels && i < uploaded; ++i)
        {
            const uint32_t offset = U32(image + field::kOffsets + i * 4);
            const uint32_t fileBytes = U32(image + field::kSizes + i * 4);
            if (!fileBytes) break;
            const uint64_t w = Dim(width, i), h = Dim(height, i);
            const uint64_t bytes = blockBytes ? ((w + 3) / 4) * ((h + 3) / 4) * blockBytes : w * h * 4;
            if (uint64_t(offset) + bytes > size) break;
            out.push_back(Level{ offset, uint32_t(bytes) });
        }
        return !out.empty();
    }

    bool Decode(const uint8_t* file, size_t size, uint32_t maxEdge, std::vector<uint8_t>& out)
    {
        out.clear();
        if (!IsBlp2(file, size) || file[field::kCompression] != kCompressionPalette) return false;
        const uint8_t alphaDepth = file[field::kAlphaDepth];
        const uint8_t preferred = file[field::kPreferredFormat];
        // GetTextureFormats: the loader decodes to ARGB8888 for these preferred formats only.
        if (preferred != kPixelArgb8888 && preferred != 3 && preferred != 4 && preferred != 5 && preferred != kPixelUnspecified)
            return false;
        // A file without mips leaves the rest of the loader's scratch chain in place: left to the client.
        if (!(file[field::kHasMips] & 0xF)) return false;
        const uint32_t width = U32(file + field::kWidth), height = U32(file + field::kHeight);
        if (!width || !height || width > 0x8000 || height > 0x8000) return false;
        // RequestImageDimensions skips levels of a texture the device cannot hold; the in-place walk would not.
        if (!maxEdge || width > maxEdge || height > maxEdge) return false;

        const uint32_t levels = LevelCount(width, height);
        if (levels > kMaxLevels) return false;
        uint64_t total = kHeaderBytes;
        for (uint32_t i = 0; i < levels; ++i)
        {
            const uint32_t offset = U32(file + field::kOffsets + i * 4);
            const uint32_t fileBytes = U32(file + field::kSizes + i * 4);
            if (!fileBytes) return false;   // CBLPFile::Lock2 fails on an empty level: so does the client
            const uint64_t pixels = uint64_t(Dim(width, i)) * Dim(height, i);
            if (uint64_t(offset) + pixels + AlphaBytes(alphaDepth, pixels) > size) return false;
            total += pixels * 4;
        }
        if (total > 0x7FFFFFFF) return false;

        out.resize(size_t(total));
        std::memcpy(out.data(), file, kHeaderBytes);
        out[field::kCompression] = kCompressionArgb;
        std::memset(out.data() + field::kOffsets, 0, kMaxLevels * 8);
        const uint8_t* palette = file + field::kPalette;
        uint32_t at = kHeaderBytes;
        for (uint32_t i = 0; i < levels; ++i)
        {
            const uint32_t pixels = Dim(width, i) * Dim(height, i);
            PutU32(out.data() + field::kOffsets + i * 4, at);
            PutU32(out.data() + field::kSizes + i * 4, pixels * 4);
            DecodeLevel(palette, alphaDepth, file + U32(file + field::kOffsets + i * 4), pixels, out.data() + at);
            at += pixels * 4;
        }
        return true;
    }
}
