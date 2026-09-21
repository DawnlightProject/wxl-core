// NetClient detour: publish every inbound server message as OnPacketReceived, before the client
// dispatches it.
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

#include "config.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/events/Event.hpp"

#include "common/Log.hpp"
#include "offsets/game/Net.hpp"

#include <cstdint>
#include <cstring>

namespace
{
    namespace ev   = wxl::events;
    namespace noff = wxl::offsets::game::net;

    noff::ProcessMessageFn g_origProcessMessage = nullptr;

    /**
     * @brief Detours NetClient::ProcessMessage, emitting OnPacketReceived before the client's own
     *        dispatch, and skipping that dispatch when a subscriber claims the message.
     *
     * This is the only seam that sees every opcode. The engine function reads the u16 opcode and
     * then does `if (opcode < 0x51F && table[opcode]) table[opcode](...)` -- so an opcode at or above
     * the table's width is never dispatched and never reaches a registered handler, it merely resets
     * the message buffer. Anything a server and a patched client agree on outside the stock range is
     * reachable here and nowhere else.
     *
     * The cursor is advanced past the opcode for the emission and put back before the original runs:
     * a subscriber reads the payload from byte zero, and the stock handler still sees an untouched
     * message. Nothing else about the message is disturbed.
     *
     * The argument list matters more than it looks: the engine function takes THREE stack arguments
     * and ends in `ret 0xC`, so this detour must too, or it under-pops its caller's frame and the
     * crash lands in NETEVENTQUEUE::Poll with no trace of where it came from. See the note on
     * offsets::game::net::ProcessMessageFn. @p flag is unread by the body and forwarded verbatim.
     *
     * @param self  the NetClient (in practice the session's ClientConnection).
     * @param conn  the WowConnection the message arrived on -- not necessarily the active one.
     * @param msg   the inbound message, a CDataStore.
     * @param flag  the engine's third argument, passed straight through.
     */
    void __fastcall hkProcessMessage(void* self, void* edx, void* conn, void* msg, int flag)
    {
        if (!ev::Any(ev::Event::OnPacketReceived) || !msg)
        {
            g_origProcessMessage(self, edx, conn, msg, flag);
            return;
        }

        auto*          store     = static_cast<noff::DataStore*>(msg);
        const uint32_t savedRead = store->read;

        if (store->size < savedRead + 2)   // not even an opcode: leave it entirely to the client
        {
            g_origProcessMessage(self, edx, conn, msg, flag);
            return;
        }

        uint16_t opcode = 0;
        std::memcpy(&opcode, store->data - store->base + savedRead, sizeof opcode);
        store->read = savedRead + 2;

        bool                  handled = false;
        ev::PacketReceivedArgs a{ opcode, msg, conn, self, &handled };
        ev::Emit(ev::Event::OnPacketReceived, &a);

        store->read = savedRead;

        static bool logged = false;
        if (!logged) { logged = true; WLOG_INFO("net: inbound message stream active"); }

        if (!handled)
            g_origProcessMessage(self, edx, conn, msg, flag);
    }

    bool InstallNetPacket()
    {
        wxl::hook::Install("NetClientProcessMessage", noff::kProcessMessage, &hkProcessMessage,
                           &g_origProcessMessage);
        return true;
    }
}

WXL_REGISTER_FEATURE("netpacket", true, InstallNetPacket)
