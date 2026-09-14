// Reading a frame's XML node while it loads, and reaching the Lua object the client gave it.
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

#include "game/Binding.hpp"
#include "game/Script.hpp"
#include "offsets/engine/Xml.hpp"

/**
 * @brief The XML load path, as a detour sees it.
 *
 * The parser does not reject what it does not know: an attribute nobody reads is parsed, stored on
 * the node, and never looked at. So teaching the client a new attribute or element is adding a read,
 * never making the parser accept one -- attach after a type's LoadXML and read the node yourself.
 *
 * Every frame and region type funnels through one such entry, CScriptRegion::LoadXML, which is also
 * where the client's own parentKey lives. An object's Lua table is reachable from there: the
 * FrameScript_Object base sits a fixed distance behind the region pointer LoadXML is invoked on, and
 * carries a LUA_REGISTRYINDEX reference to the table Lua sees as the frame.
 */
namespace wxl::game::xml
{
    namespace off = wxl::offsets::engine::xml;

    /// A parsed XML node. The parser's own type; only the three fields below are known.
    using Node = void*;

    /// Signature of the load entry every region and frame type reaches, for a detour and trampoline.
    using RegionLoadXMLFn = off::RegionLoadXMLFn;

    /// Its address.
    constexpr uintptr_t kRegionLoadXML = off::kScriptRegionLoadXML;

    /// Signature and address of the <Texture> load entry.
    using TextureLoadXMLFn = off::TextureLoadXMLFn;
    constexpr uintptr_t kTextureLoadXML = off::kSimpleTextureLoadXML;

    /**
     * @brief Reads an attribute off a node.
     * @param node  The node.
     * @param name  Attribute name, matched without regard to case.
     * @return Its value, or null when the node does not carry the attribute. The string belongs to
     *         the parsed document and outlives the load, but not a UI reload -- copy anything kept.
     */
    inline const char* Attribute(Node node, const char* name)
    {
        if (!node || !name) return nullptr;
        return Native<off::NodeGetAttributeByNameFn>(off::kNodeGetAttributeByName)(node, nullptr, name);
    }

    /// Reads an attribute's text as a boolean, the way every stock boolean attribute is read.
    inline bool TextToBool(const char* value)
    {
        if (!value || !*value) return false;
        return Native<off::StringToBoolFn>(off::kStringToBool)(value) != 0;
    }

    /// Reads an attribute's text as a number, the way every stock numeric attribute is read.
    inline float TextToNumber(const char* value)
    {
        if (!value || !*value) return 0.0f;
        return Native<off::StringToFloatFn>(off::kStringToFloat)(value);
    }

    /**
     * @brief Reads a boolean attribute off a node.
     * @param node  The node.
     * @param name  Attribute name.
     * @return false when the attribute is absent or empty, so an omitted attribute is an off one.
     */
    inline bool AttributeIsTrue(Node node, const char* name)
    { return TextToBool(Attribute(node, name)); }

    /// The node's first child, or null.
    inline Node FirstChild(Node node)
    {
        if (!node) return nullptr;
        return *reinterpret_cast<Node*>(reinterpret_cast<uint8_t*>(node) + off::kNodeFirstChild);
    }

    /// The node after this one under the same parent, or null.
    inline Node NextSibling(Node node)
    {
        if (!node) return nullptr;
        return *reinterpret_cast<Node*>(reinterpret_cast<uint8_t*>(node) + off::kNodeNextSibling);
    }

    /// The node's element name.
    inline const char* NodeName(Node node)
    {
        if (!node) return nullptr;
        return *reinterpret_cast<const char**>(reinterpret_cast<uint8_t*>(node) + off::kNodeName);
    }

    /**
     * @brief The FrameScript_Object base behind the region pointer a LoadXML entry is invoked on.
     * @param region  The `this` a LoadXML detour received.
     *
     * LoadXML runs on a subobject that is not the start of the allocation; the base carrying the Lua
     * registration and the class vtable sits a fixed distance behind it. The client's own parentKey
     * reader takes exactly this step.
     */
    inline void* ObjectOf(void* region)
    {
        if (!region) return nullptr;
        return reinterpret_cast<uint8_t*>(region) + off::kRegionToScriptObject;
    }

    /**
     * @brief Pushes the Lua table an object is seen as, registering it if it has none yet.
     * @param state   Script state.
     * @param region  The `this` a LoadXML detour received.
     * @return false when there was nothing to push, in which case the stack is untouched.
     *
     * The registration is the same one a named frame gets: the table is created, given the class's
     * metatable, and held by a reference in the registry. Registering an unnamed region here keeps
     * it alive for the process, which is what an assignment into a parent's table would do anyway.
     */
    inline bool PushObject(void* state, void* region)
    {
        void* object = ObjectOf(region);
        if (!state || !object) return false;

        const int ref = Native<off::ScriptObjectLuaRefFn>(off::kScriptObjectLuaRef)(object, nullptr);
        if (ref == 0) return false;

        script::RawGetI(state, script::kRegistryIndex, ref);
        if (script::Type(state, -1) != script::kTypeTable)
        {
            script::SetTop(state, -2);
            return false;
        }
        return true;
    }

    /**
     * @brief Points a texture at a file.
     * @param region     The `this` a <Texture> LoadXML detour received.
     * @param file       Texture path, spelled the way the stock loader takes it: backslashes, no
     *                   extension.
     * @param horizTile  Repeat horizontally.
     * @param vertTile   Repeat vertically.
     * @return false when the file could not be loaded, in which case the texture is left as it was.
     */
    inline bool SetTexture(void* region, const char* file, bool horizTile, bool vertTile)
    {
        void* object = ObjectOf(region);
        if (!object || !file || !*file) return false;

        const int filter = *reinterpret_cast<const int*>(off::kTextureDefaultFilterMode);
        return Native<off::TextureSetTextureFn>(off::kSimpleTextureSetTexture)(
                   object, nullptr, file, horizTile ? 1 : 0, vertTile ? 1 : 0, filter, 0) != 0;
    }

    /**
     * @brief Sets a texture's four corners.
     * @param region  The `this` a <Texture> LoadXML detour received.
     * @param left    Left edge in texture coordinates.
     * @param right   Right edge.
     * @param top     Top edge.
     * @param bottom  Bottom edge.
     *
     * The engine stores all four corners rather than a rectangle; an axis-aligned rectangle is the
     * degenerate case written here.
     */
    inline void SetTexCoord(void* region, float left, float right, float top, float bottom)
    {
        void* object = ObjectOf(region);
        if (!object) return;

        const float corners[8] = { left, top, left, bottom, right, top, right, bottom };
        Native<off::TextureSetTexCoordFn>(off::kSimpleTextureSetTexCoord)(object, nullptr, corners);
    }

    /**
     * @brief Converts a pixel extent to the layout units a region's size is kept in.
     * @param pixels  Extent at the interface's reference width.
     *
     * The same conversion the <Size> reader applies to an <AbsDimension>, so a size set from here and
     * a size written in XML mean the same thing.
     */
    inline float PixelsToLayout(float pixels)
    {
        const float aspect = *reinterpret_cast<const float*>(off::kAspectCompensation);
        const float scale  = *reinterpret_cast<const float*>(off::kNdcToDdcWidthScale);
        if (aspect == 0.0f) return 0.0f;
        return scale * (pixels / (aspect * off::kLayoutReferenceWidth));
    }

    /**
     * @brief Sets a region's width, in layout units.
     * @param region  The `this` a LoadXML detour received.
     *
     * Dispatched through the same virtual slot the <Size> reader uses, so a type that overrides it --
     * CSimpleTexture does -- still gets its own.
     */
    inline void SetWidth(void* region, float width)
    {
        if (!region) return;

        uint8_t* vtable = *reinterpret_cast<uint8_t**>(region);
        auto fn = *reinterpret_cast<off::LayoutFrameSetExtentFn*>(vtable + off::kLayoutFrameSetWidthSlot);
        fn(region, nullptr, width);
    }

    /// Sets a region's height, in layout units. See SetWidth.
    inline void SetHeight(void* region, float height)
    {
        if (!region) return;

        uint8_t* vtable = *reinterpret_cast<uint8_t**>(region);
        auto fn = *reinterpret_cast<off::LayoutFrameSetExtentFn*>(vtable + off::kLayoutFrameSetHeightSlot);
        fn(region, nullptr, height);
    }
}
