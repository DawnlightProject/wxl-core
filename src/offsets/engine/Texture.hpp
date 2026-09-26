// BLP texture loading: the loader, the BLP file object, and the device limits it reads.
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

#include <cstddef>
#include <cstdint>

// INTERNAL to the core. How a BLP becomes a texture (Wow.exe 3.3.5a 12340):
//  TextureCreate (Gx.hpp kTextureCreate) -> CreateBlpTexture (0x004B8BE0) -> CTexture async create
//  (0x004B8A50): opens the file, sizes a read object by SFile's file size, and queues the whole-file read
//  (SMemAlloc of that size, Texture.cpp:0x809) on the disk thread. The completion (0x004B7E80) runs the
//  loader (0x004B7BD0) on the main thread: CBLPFile::Source copies the 0x494-byte header from the buffer,
//  and CBLPFile::LockChain2 either points the mip table at buffer + offset (DXT, ARGB) or decodes palettized
//  levels into the boot-time scratch chain; GxTexUpdate then uploads (CGxDeviceD3d::ITexUpload blits each
//  level into the texture's lock). Only after that does the completion close the file and free the buffer.
namespace wxl::offsets::engine::texture
{
    // The loader's own CBLPFile object: 0x4B4 bytes (header copy at +4, source bytes at +0x498).
    constexpr size_t kBlpFileBytes = 0x4B4;

    // Initialises a CBLPFile on the stack the way the loader does (magic, type, empty header). __fastcall(this).
    constexpr uintptr_t kBlpFileInit = 0x004B58D0;
    using BlpFileInitFn = void*(__fastcall*)(void* self);

    // CBLPFile::Source(bytes): copies the header, keeps bytes as the source; 1 on a BLP2 header. __thiscall.
    constexpr uintptr_t kBlpFileSource = 0x006AE900;
    using BlpFileSourceFn = int(__thiscall*)(void* self, const void* bytes);

    // CBLPFile::LockChain2(name, pixelFormat, &mipTable, firstLevel, inPlace): fills a mip table. With a
    // non-null table and inPlace 0 it lays the chain out after the table and decodes into it. __thiscall.
    constexpr uintptr_t kBlpFileLockChain2 = 0x006AFFD0;
    using BlpFileLockChain2Fn = int(__thiscall*)(void* self, const char* name, int pixelFormat, void** table,
                                                 uint32_t firstLevel, int inPlace);

    // CBLPFile::Close(): frees what the object owns. __thiscall.
    constexpr uintptr_t kBlpFileClose = 0x006AE8B0;
    using BlpFileCloseFn = void(__thiscall*)(void* self);

    // PIXEL_FORMAT the loader decodes palettized files to.
    constexpr int kPixelArgb8888 = 2;

    // CGxDevice fields: the caps block starts at +0x214 (CGxDevice::Caps); its largest texture edge at +0x6C
    // drives the loader's RequestImageDimensions, and the base mip level (baseMip) sits at +0x350.
    constexpr size_t kDeviceMaxTextureEdge = 0x214 + 0x6C;
    constexpr size_t kDeviceBaseMip = 0x350;

    // AsyncFileReadWait's reentrancy counter (u32): non-zero while the main thread blocks on one read.
    constexpr uintptr_t kAsyncReadWaiting = 0x00B4A26C;
}
