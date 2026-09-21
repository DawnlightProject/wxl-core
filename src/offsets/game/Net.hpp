// NetClient / RealmConnection / ClientConnection: the realm connection, its message dispatch, and
// the CDataStore an inbound message is read out of.
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
#include <cstddef>

#include "offsets/game/DataStore.hpp"

// INTERNAL to the core. Modules never include this; they use wxl::game::net / wxl::events.
//
// The class chain is NetClient -> RealmConnection -> ClientConnection, and ClientServices owns one
// instance of the last for the whole session -- glue screens and world alike. Every address and
// field below was read out of the Ghidra export of the 12340 client
// (_sources/data_uncompress/ghidra_export/08_decompiled.c); the reasoning is in the comments,
// because the names alone are misleading in at least two places.
namespace wxl::offsets::game::net
{
    // --- the connection object -------------------------------------------------------------------

    /// ClientServices::m_currentConnection: the one ClientConnection the session owns, or null before
    /// the realm connection exists. Read directly rather than through ClientServices::Connection
    /// (0x006B0970), which calls a no-return fatal-error path on null instead of answering.
    constexpr uintptr_t kCurrentConnection = 0x00C79CF4;

    // Login data block, written wholesale by NetClient::SetLoginData (0x00631F70) when the logon
    // server answers the realm join. It is 0x14C dwords starting at this+4 and it SURVIVES a
    // disconnect -- nothing in the teardown path clears it. That is what makes a plain reconnect
    // possible without re-authenticating against the logon server.
    constexpr size_t kOffAccountName     = 0x004; ///< char[0x500], NUL-terminated
    constexpr size_t kOffLoginServerId   = 0x504; ///< u32, echoed in CMSG_AUTH_SESSION
    constexpr size_t kOffSessionKey      = 0x508; ///< u8[40], the live SRP6 session key
    constexpr size_t kOffLoginServerType = 0x530; ///< u32, echoed in CMSG_AUTH_SESSION
    constexpr size_t kSessionKeySize     = 40;

    /// NetClient::m_netState. NS_UNINITIALIZED 0, NS_INITIALIZED 2, NS_CONNECTING 4, NS_CONNECTED 5,
    /// NS_DISCONNECTING 6. Both Connect and ConnectInternal refuse (SErrAppDisplayFatal) unless it
    /// reads exactly 2.
    constexpr size_t kOffNetState = 0x534;
    enum : uint32_t
    {
        kStateUninitialized = 0,
        kStateInitialized   = 2,
        kStateConnecting    = 4,
        kStateConnected     = 5,
        kStateDisconnecting = 6,
    };

    /// NetClient::m_suspended: set by the SMSG_SUSPEND_COMMS handler; while it is set NetClient::Send
    /// queues into the held-message list instead of writing to the socket.
    constexpr size_t kOffSuspended = 0x538;

    /// The message-handler function table and its matching user-pointer table, both
    /// 0x51F entries of 4 bytes, both zeroed by NetClient::Initialize. See kMaxHandlerOpcode.
    constexpr size_t kOffHandlerTable      = 0x053C;
    constexpr size_t kOffHandlerParamTable = 0x19B8;

    /// The active WowConnection, and the pending one a SMSG_REDIRECT_CLIENT dials.
    constexpr size_t kOffActiveConnection   = 0x2E38;
    constexpr size_t kOffRedirectConnection = 0x2E3C;

    // RealmConnection::SetSelectedRealm writes these three, and RealmConnection::HandleAuthChallenge
    // puts them into CMSG_AUTH_SESSION. The realm id is the field a worldserver compares against its
    // own configured realm id before it will accept the session at all.
    constexpr size_t kOffRegion      = 0x2F30;
    constexpr size_t kOffBattlegroup = 0x2F34;
    constexpr size_t kOffRealmId     = 0x2F38;

    /// ClientConnection: nonzero while the realm socket is up (set in HandleConnect, cleared in
    /// HandleDisconnect and in Disconnect).
    constexpr size_t kOffConnected = 0x2F3C;

    // --- entries ---------------------------------------------------------------------------------

    /// NetClient::ProcessMessage(WowConnection* conn, CDataStore* msg, int flag). The single point
    /// every inbound message reaches on the MAIN thread, called from NetClient::HandleData
    /// (0x00632460), itself drained by NETEVENTQUEUE::Poll. It reads the u16 opcode off the
    /// CDataStore and dispatches through the handler table; see kMaxHandlerOpcode for the rest.
    ///
    /// THREE stack arguments and `ret 0xC` -- read off the instruction stream, because the Ghidra
    /// export gets this one wrong. 08_decompiled.c declares it with two, and a detour that believes
    /// that emits `ret 8`, under-pops the caller's frame by four, and HandleData's epilogue
    /// (`pop esi` BEFORE `mov esp, ebp`, at 0x006324FA) then restores ESI from the `push 0` slot.
    /// Control returns to NETEVENTQUEUE::Poll with ESI = 0 and Poll faults on its next instruction,
    /// `mov ecx, [esi]` at 0x0063359D -- a null dereference three frames away from the mistake.
    /// Verified at all three call sites (0x006324C9, 0x00714AFB, 0x00716A78); the third argument is
    /// 0 from HandleData and 1 from the other two, is never read by the body, and is forwarded
    /// verbatim rather than assumed.
    constexpr uintptr_t kProcessMessage = 0x00631FE0;
    using ProcessMessageFn = void(__fastcall*)(void* self, void* edx, void* conn, void* msg, int flag);

    /// The dispatcher's bound: `if (opcode < 0x51F && table[opcode])`. It is exact -- the handler
    /// table is 0x51F entries wide -- so an opcode at or above it can never be dispatched, and
    /// NetClient::SetMessageHandler (which does NOT bounds-check) would silently write into the
    /// neighbouring user-pointer table. An opcode the dispatcher will not take falls through to
    /// CDataStore::Reset (0x004010E0) and is discarded without a disconnect.
    constexpr uint32_t kMaxHandlerOpcode = 0x51F;

    /// NetClient::Connect(const char* address). Parses "host" or "host:port" (default port 9090),
    /// clears encryption, and dials. Requires m_netState == NS_INITIALIZED.
    constexpr uintptr_t kNetClientConnect = 0x006323C0;
    using NetClientConnectFn = void(__fastcall*)(void* self, void* edx, const char* address);

    /// NetClient::Disconnect(). Two behaviours, and the difference is the whole game:
    ///   m_netState == NS_CONNECTED -> "graceful": state becomes NS_DISCONNECTING and the socket is
    ///       closed with its response object STILL ATTACHED, so WowConnection::WCDisconnected fires,
    ///       queues event 0x1A, and the main thread runs ClientConnection::HandleDisconnect ->
    ///       NetClient::HandleDisconnect -> CGlueMgr::NetDisconnectHandler. That is the glue screen.
    ///   any other state -> "silent": the response is detached FIRST, the socket closed, the event
    ///       queue cleared, a fresh WowConnection allocated, and the state left at NS_INITIALIZED.
    ///       Nothing above the transport is told.
    constexpr uintptr_t kNetClientDisconnect = 0x00631EA0;

    /// ClientConnection::Disconnect(): NetClient::Disconnect, clear the connected flag, and
    /// WardenClient_Destroy -- the counterpart of the WardenClient_Initialize that
    /// ClientConnection::HandleConnect runs on the next connect.
    constexpr uintptr_t kClientConnectionDisconnect = 0x006B0940;
    using ConnectionVoidFn = void(__fastcall*)(void* self, void* edx);

    /// RealmConnection::SetSelectedRealm(region, battlegroup, realmId).
    constexpr uintptr_t kSetSelectedRealm = 0x00464490;
    using SetSelectedRealmFn = void(__fastcall*)(void* self, void* edx, uint32_t region,
                                                 uint32_t battlegroup, uint32_t realmId);

    // --- sending a message -----------------------------------------------------------------------
    // ClientConnection::SendCharEnumOpcode (0x00464A40) is the whole outbound path in twenty
    // instructions: build a CDataStore on the stack, write the opcode, hand it to NetClient::Send.
    // The store and its writers live in offsets/game/DataStore.hpp; only the send entry is here.

    /// NetClient::Send(CDataStore*). Prepends the size, encrypts the header, writes to the socket --
    /// or queues, while m_suspended is set. It requires m_read == 0: the store is built in write mode
    /// with m_read == -1, and putting it back to 0 is what marks the message finished.
    constexpr uintptr_t kNetClientSend = 0x00632B50;
    using NetClientSendFn = int(__fastcall*)(void* self, void* edx, void* msg);

    // --- CDataStore ------------------------------------------------------------------------------
    // Declared once, in offsets/game/DataStore.hpp. Aliased here because an inbound message IS one
    // and every reader in game/Net.hpp names it.

    using DataStore = wxl::offsets::game::datastore::DataStore;
}
