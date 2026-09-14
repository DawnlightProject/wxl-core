// Reading a script call's arguments and answering it.
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

#include "game/Binding.hpp"
#include "offsets/engine/Lua.hpp"

/**
 * @brief The arguments a script call carries, and what it hands back.
 *
 * Indices are one-based. A method invoked as frame:Method(a, b) puts the frame at 1 and its first
 * argument at 2 -- read the frame with wxl::game::glue::MethodSelf() rather than from the stack.
 *
 * A return value is pushed, and the count of pushes returned from the function.
 */
namespace wxl::game::script
{
    namespace off = wxl::offsets::engine::lua;

    /// A script function, as the method tables and the global registry hold it.
    using Function = off::LuaCFunction;

    // The engine checks every script callback against the bounds of its own image before invoking it,
    // and a function compiled into an extension lies outside them by construction -- the call is
    // refused with a fatal "Invalid function pointer", which is what makes this seam the gate any
    // script function added from outside has to pass.
    //
    // Detour it and return, without calling the original, for the pointers you registered yourself.
    // Anything else must still reach the original: the check is doing its job for the calls it was
    // written for, and widening it wholesale would give that up for all of them.

    /// Entry the engine validates a script callback through.
    constexpr uintptr_t kValidateCallbackSeam = off::kValidateFunctionPointer;

    /// Its signature, for a detour and the matching trampoline.
    using ValidateCallbackFn = off::ValidateFunctionPointerFn;

    /**
     * @brief Counts the values passed to the call.
     * @param state  Script state the call arrived on.
     * @return How many, including the frame for a method call.
     */
    inline int ArgCount(void* state)
    { return Native<off::LuaGetTopFn>(off::kLuaGetTop)(state); }

    /**
     * @brief Reports whether an argument is a number, or a string convertible to one.
     * @param state  Script state.
     * @param index  One-based argument index.
     */
    inline bool IsNumber(void* state, int index)
    { return Native<off::LuaIsNumberFn>(off::kLuaIsNumber)(state, index) != 0; }

    /**
     * @brief Reports whether an argument is a string, or a number convertible to one.
     * @param state  Script state.
     * @param index  One-based argument index.
     */
    inline bool IsString(void* state, int index)
    { return Native<off::LuaIsStringFn>(off::kLuaIsString)(state, index) != 0; }

    /**
     * @brief Reads an argument as a number.
     * @param state  Script state.
     * @param index  One-based argument index.
     * @return The value, or 0 when it is not convertible.
     */
    inline double ToNumber(void* state, int index)
    { return Native<off::LuaToNumberFn>(off::kLuaToNumber)(state, index); }

    /**
     * @brief Reads an argument as a string.
     * @param state  Script state.
     * @param index  One-based argument index.
     * @return The text, or null when it is not convertible. It belongs to the script state and is only
     *         valid until the call returns -- copy anything kept.
     */
    inline const char* ToString(void* state, int index)
    { return Native<off::LuaToStringFn>(off::kLuaToString)(state, index, nullptr); }

    /**
     * @brief Pushes a number as a return value.
     * @param state  Script state.
     * @param value  Value to return.
     */
    inline void PushNumber(void* state, double value)
    { Native<off::LuaPushNumberFn>(off::kLuaPushNumber)(state, value); }

    /**
     * @brief Pushes a string as a return value.
     * @param state  Script state.
     * @param value  Text to return; it is copied.
     */
    inline void PushString(void* state, const char* value)
    { Native<off::LuaPushStringFn>(off::kLuaPushString)(state, value); }

    /**
     * @brief Pushes a boolean as a return value.
     * @param state  Script state.
     * @param value  Value to return.
     */
    inline void PushBoolean(void* state, bool value)
    { Native<off::LuaPushBooleanFn>(off::kLuaPushBoolean)(state, value ? 1 : 0); }

    /// The pseudo-indices and the two type tags a table walk distinguishes.
    constexpr int kRegistryIndex = off::kRegistryIndex;
    constexpr int kGlobalsIndex  = off::kGlobalsIndex;
    constexpr int kTypeNil       = off::kTypeNil;
    constexpr int kTypeTable     = off::kTypeTable;

    /**
     * @brief The index of the top stack slot, which is also how many values sit on the stack.
     *
     * ArgCount is the same call read as a question about a script call's arguments; this is the same
     * question asked by code that pushes scratch values and has to put the stack back.
     */
    inline int StackTop(void* state)
    { return Native<off::LuaGetTopFn>(off::kLuaGetTop)(state); }

    /**
     * @brief The type tag of a stack slot.
     * @param state  Script state.
     * @param index  Stack index; negative counts from the top.
     */
    inline int Type(void* state, int index)
    { return Native<off::LuaTypeFn>(off::kLuaType)(state, index); }

    /**
     * @brief Sets the stack top, dropping or nil-filling to reach it.
     * @param state  Script state.
     * @param index  New top; negative counts from the current top.
     *
     * A C function that pushes scratch values restores the top it was entered with rather than
     * counting pops: one early return that forgets a pop leaves the stack wrong for every later call
     * on that state.
     */
    inline void SetTop(void* state, int index)
    { Native<off::LuaSetTopFn>(off::kLuaSetTop)(state, index); }

    /// Pushes nil.
    inline void PushNil(void* state)
    { Native<off::LuaPushNilFn>(off::kLuaPushNil)(state); }

    /// Pushes a copy of the value at @p index.
    inline void PushValue(void* state, int index)
    { Native<off::LuaPushValueFn>(off::kLuaPushValue)(state, index); }

    /**
     * @brief Raw table read: pops the key at the top, pushes t[key].
     * @param state       Script state.
     * @param tableIndex  Stack index of the table.
     *
     * Raw, so no __index runs. A frame's Lua table carries a metatable whose __index is its method
     * table, and a raw read is how you ask what the object itself holds rather than what its class
     * would answer.
     */
    inline void RawGet(void* state, int tableIndex)
    { Native<off::LuaRawGetFn>(off::kLuaRawGet)(state, tableIndex); }

    /// Raw indexed read: pushes t[n]. This is how a registry reference is resolved to its value.
    inline void RawGetI(void* state, int tableIndex, int n)
    { Native<off::LuaRawGetIFn>(off::kLuaRawGetI)(state, tableIndex, n); }

    /// Raw table write: pops the value then the key, and sets t[key] = value. No __newindex runs.
    inline void RawSet(void* state, int tableIndex)
    { Native<off::LuaRawSetFn>(off::kLuaRawSet)(state, tableIndex); }

    /**
     * @brief Advances a table walk: pops a key, pushes the next key and its value.
     * @return 0 at the end of the table, having pushed nothing.
     *
     * Start it by pushing nil as the first key.
     */
    inline int Next(void* state, int tableIndex)
    { return Native<off::LuaNextFn>(off::kLuaNext)(state, tableIndex); }

    /**
     * @brief Pushes the value of a global.
     * @param state  Script state.
     * @param name   Global's name.
     *
     * Raw, matching how the engine itself reads the global table when it looks a script object up by
     * name (FrameScript_Object::RegisterScriptObject). Pushes nil when there is no such global.
     */
    inline void PushGlobal(void* state, const char* name)
    {
        PushString(state, name);
        RawGet(state, kGlobalsIndex);
    }

    /**
     * @brief Registers a global script function.
     * @param name      Name it is called by.
     * @param function  Function invoked.
     *
     * For a method on a frame type, see wxl::game::glue::AddMethods instead.
     */
    inline void Register(const char* name, Function function)
    { Native<off::FrameScriptRegisterFunctionFn>(off::kFrameScriptRegisterFunction)(name, function); }

    /**
     * @brief The active script context.
     * @return The Lua state FrameScript runs on, or null before script initialization and while the
     *         client is between contexts.
     *
     * A context is torn down and rebuilt across a UI reload, so a caller that registered anything
     * into one compares the pointer rather than assuming its own registration survived.
     */
    inline void* Context()
    { return Native<off::FrameScriptGetContextFn>(off::kFrameScriptGetContext)(); }

    /**
     * @brief Compiles and runs a chunk of script source on a context.
     * @param source  Chunk text, NUL terminated.
     * @param state   Script state to run it on; Context() is what a caller outside a script call has.
     *
     * Nothing is returned: the engine's entry reports a compile or runtime failure through the
     * client's own script error path, not to the caller.
     */
    inline void Execute(const char* source, void* state)
    { Native<off::FrameScriptExecuteFn>(off::kFrameScriptExecute)(source, state); }
}
