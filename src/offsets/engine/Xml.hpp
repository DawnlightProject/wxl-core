// FrameXML's XML load path for the target client (335): the node walk, the one region-wide load entry
// every frame and region type reaches, and the texture calls an atlas member is applied through.
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

#include <cstdint>

namespace wxl::offsets::engine::xml
{
    // --- the parsed node ---------------------------------------------------------------------
    // Byte offsets into a parsed XML node, read off the child walk every LoadXML shares:
    // for (n = *(int*)(node + 0x08); n; n = *(int*)(n + 0x34)) SStrCmpI(*(char**)(n + 0x14), ...)
    // -- CSimpleFrame::LoadXML_Attributes at 0x00492F80, CSimpleFrame::LoadXML at 0x004936FE.
    constexpr uintptr_t kNodeFirstChild  = 0x08;
    constexpr uintptr_t kNodeName        = 0x14;
    constexpr uintptr_t kNodeNextSibling = 0x34;

    // XMLNode::GetAttributeByName, the lookup every attribute of every tag goes through. ecx = node,
    // the name on the stack; returns the attribute's value, or null when the node does not carry it.
    // Identified at 0x0048868A: 68 1C A8 9E 00 ("parentKey"), 8B CF (ecx = node), E8 -> 0x00814730.
    // Its body walks the node's attribute array -- count at +0x24, array at +0x28, stride 0x18, each
    // entry's name at +0x08 and value at +0x14 -- which is where the node offsets above were checked.
    constexpr uintptr_t kNodeGetAttributeByName = 0x00814730;
    using NodeGetAttributeByNameFn = const char*(__fastcall*)(void* node, void* edx, const char* name);

    // The client's own boolean-attribute reader; every "true"/"false" attribute in the XML load path
    // is put through it, so an attribute added from outside spells its booleans the same way.
    // __cdecl, one stack argument. 0x008154E0 in full: 55 / 8B EC / 56 / 8B 75 08 / 6A 00 /
    // E8 -> 0x00815400 / 83 C4 04 / 5E / 5D / C3 -- caller-cleaned, result in eax.
    constexpr uintptr_t kStringToBool = 0x008154E0;
    using StringToBoolFn = int(__cdecl*)(const char* value);

    // The client's own decimal reader, used for every numeric attribute in the XML load path.
    // __stdcall: 0x0076FB80 opens 55 / 8B EC / 83 EC 08 / 56 / 8B 75 08 and closes C2 04 00, so the
    // callee cleans its one argument. Returns in st0, and returns 0 for a null string (D9 EE, fldz).
    constexpr uintptr_t kStringToFloat = 0x0076FB80;
    using StringToFloatFn = float(__stdcall*)(const char* value);

    // XMLNode::GetChildByName: the same sibling walk, first child whose name matches, or null.
    constexpr uintptr_t kNodeGetChildByName = 0x008146F0;
    using NodeGetChildByNameFn = void*(__fastcall*)(void* node, void* edx, const char* name);

    // --- the one load entry every region and frame type reaches ------------------------------
    // CScriptRegion::LoadXML(node, status). ecx = the region subobject, node at [ebp+8], status at
    // [ebp+0xC]. Exactly three functions call it -- CSimpleTexture::LoadXML (site 0x00485FC7),
    // CSimpleFontString::LoadXML (0x00487484) and CSimpleFrame::LoadXML (0x0049341E) -- and every
    // other frame type's LoadXML (Button, StatusBar, EditBox, Model, ScrollFrame, Slider, HTML,
    // ColorSelect, MessageFrame, MessageScrollFrame, Cooldown, BlobFrame) reaches it by calling
    // CSimpleFrame::LoadXML. It is therefore the single seam covering all of them, and it is also
    // where the client's own parentKey reader lives, so anything added here runs where parentKey does.
    constexpr uintptr_t kScriptRegionLoadXML = 0x00488670;
    using RegionLoadXMLFn = void(__fastcall*)(void* region, void* edx, void* node, void* status);

    // Distance from the region subobject LoadXML is invoked on back to the FrameScript_Object base.
    // CScriptRegion::LoadXML takes the step outright at 0x004886AC: 8B 56 E0 (mov edx, [esi-0x20],
    // the base's vtable) then 83 C6 E0 (add esi, -0x20), and from there dispatches GetParent through
    // vtable slot +0x14 with ecx = esi. CSimpleTexture::LoadXML takes the same step for its name,
    // *(int*)(this - 0x20).
    constexpr int32_t kRegionToScriptObject = -0x20;

    // FrameScript_Object::GetOrRegisterLuaObjectRef: registers the object's Lua table if it has none,
    // then returns the LUA_REGISTRYINDEX reference to it. ecx = the FrameScript_Object base.
    // 0x00488380 in full: 56 / 8B F1 / 83 7E 04 00 / 75 07 / 6A 00 / E8 -> RegisterScriptObject /
    // 8B 46 08 / 5E / C3. That table is the one parentKey assigns into, so it is what Lua sees.
    constexpr uintptr_t kScriptObjectLuaRef = 0x00488380;
    using ScriptObjectLuaRefFn = int(__fastcall*)(void* object, void* edx);

    // --- <Texture> -----------------------------------------------------------------------------
    // CSimpleTexture::LoadXML(node, status). ecx = the region subobject, node at [ebp+8], status at
    // [ebp+0xC] -- 0x00485F40: 55 / 8B EC / 83 EC 54 / 53 / 8B 5D 08 / 56 / 57 / 89 4D FC.
    constexpr uintptr_t kSimpleTextureLoadXML = 0x00485F40;
    using TextureLoadXMLFn = void(__fastcall*)(void* region, void* edx, void* node, void* status);

    // CSimpleTexture::SetTexture(file, horizTile, vertTile, filterMode, imageMode). ecx = the
    // FrameScript_Object base, NOT the region subobject: it stores the texture handle at base + 0xD4,
    // which the <Texture> loader reaches as region[0x2D] (0x2D * 4 + 0x20 == 0xD4). Returns 0 when the
    // file could not be loaded, in which case the caller leaves the texture as it was. All 22 call
    // sites in the binary pass the filter mode from the global below and an image mode of 0.
    constexpr uintptr_t kSimpleTextureSetTexture = 0x004859E0;
    using TextureSetTextureFn = int(__fastcall*)(void* object, void* edx, const char* file,
                                                int horizTile, int vertTile, int filterMode,
                                                int imageMode);

    // CSimpleTexture::SetTexCoord: eight floats, UL.x UL.y LL.x LL.y UR.x UR.y LR.x LR.y, written to
    // base + 0x140 .. base + 0x15C in that order. ecx = the FrameScript_Object base, as above. The
    // <TexCoords> reader writes the same eight fields directly and invalidates nothing, so neither
    // does this: the corners are read when the region draws.
    constexpr uintptr_t kSimpleTextureSetTexCoord = 0x00481640;
    using TextureSetTexCoordFn = void(__fastcall*)(void* object, void* edx, const float* corners);

    // Default texture filter mode. Every SetTexture call site in the binary passes this global.
    constexpr uintptr_t kTextureDefaultFilterMode = 0x00AC0F00;

    // --- size, for useAtlasSize ------------------------------------------------------------------
    // Virtual slots CLayoutFrame::LoadXML calls with the x and y of a <Size>, on the region subobject:
    // (**(code**)(*this + 0x1C))(width) then (**(code**)(*this + 0x20))(height). Dispatched through
    // the vtable rather than calling CLayoutFrame::SetWidth / SetHeight directly, because
    // CSimpleTexture overrides that pair of slots.
    constexpr uintptr_t kLayoutFrameSetWidthSlot  = 0x1C;
    constexpr uintptr_t kLayoutFrameSetHeightSlot = 0x20;
    using LayoutFrameSetExtentFn = void(__fastcall*)(void* region, void* edx, float value);

    // The pixels-to-layout-units conversion those two slots expect. LoadXML_Dimensions (0x00815740)
    // computes NDCToDDCWidth(value / (GetAspectCompensation() * 1024)) for both x and y, and both
    // helpers are a single multiply by a global: NDCToDDCWidth (0x0047C070) returns
    // *(float*)0x00AC0CB4 * value, GetAspectCompensation (0x0047BFE0) returns *(float*)0x00AC0CBC.
    // The two globals are read directly rather than the two functions called, there being nothing in
    // either function but that multiply.
    constexpr uintptr_t kNdcToDdcWidthScale   = 0x00AC0CB4;
    constexpr uintptr_t kAspectCompensation   = 0x00AC0CBC;
    constexpr float     kLayoutReferenceWidth = 1024.0f;
}
