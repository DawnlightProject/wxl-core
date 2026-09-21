// Realm-connection bindings: reading an inbound message, and driving the client's own connect /
// disconnect the way the realm-select screen does.
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
#include <cstring>

#include "game/Binding.hpp"
#include "game/DataStore.hpp"
#include "offsets/game/Net.hpp"

/**
 * @brief The one realm connection the session owns, and the CDataStore a message arrives in.
 *
 * Two unrelated jobs live here because they are two halves of the same seam: OnPacketReceived hands
 * a subscriber a CDataStore and a connection, and everything a subscriber can usefully do with
 * either is below.
 *
 * Everything in this namespace is main-thread only. OnPacketReceived is published from
 * NetClient::ProcessMessage, which NETEVENTQUEUE::Poll drains on the game thread, so a handler is
 * already on the right thread -- but see Redial's warning about doing it from inside that call.
 */
namespace wxl::game::net
{
    namespace noff = wxl::offsets::game::net;

    /// The session's ClientConnection, or null before the realm connection exists.
    inline void* Connection()
    {
        return *reinterpret_cast<void**>(noff::kCurrentConnection);
    }

    namespace detail
    {
        template <class T>
        inline T* Field(void* conn, size_t offset)
        {
            return reinterpret_cast<T*>(static_cast<uint8_t*>(conn) + offset);
        }
    }

    /// The logged-in account name, as the connection holds it. Null when there is no connection.
    inline const char* AccountName(void* conn)
    {
        return conn ? detail::Field<const char>(conn, noff::kOffAccountName) : nullptr;
    }

    /// The live session key (40 bytes), as the connection holds it. Null when there is no connection.
    inline const uint8_t* SessionKey(void* conn)
    {
        return conn ? detail::Field<const uint8_t>(conn, noff::kOffSessionKey) : nullptr;
    }

    /// The connection's net state; compare against wxl::offsets::game::net's kState* values.
    inline uint32_t NetState(void* conn)
    {
        return conn ? *detail::Field<uint32_t>(conn, noff::kOffNetState) : noff::kStateUninitialized;
    }

    /// True while the realm socket is up.
    inline bool Connected(void* conn)
    {
        return conn && *detail::Field<uint32_t>(conn, noff::kOffConnected) != 0;
    }

    /// The realm id the connection will announce in CMSG_AUTH_SESSION.
    inline uint32_t RealmId(void* conn)
    {
        return conn ? *detail::Field<uint32_t>(conn, noff::kOffRealmId) : 0;
    }

    /**
     * @brief Sets the realm the next CMSG_AUTH_SESSION announces.
     *
     * A worldserver compares this against its own configured realm id and refuses the session when
     * they differ, so it has to be right before a connect to a realm other than the one the glue
     * screen picked.
     */
    inline void SetSelectedRealm(void* conn, uint32_t region, uint32_t battlegroup, uint32_t realmId)
    {
        if (!conn) return;
        Native<noff::SetSelectedRealmFn>(noff::kSetSelectedRealm)(conn, nullptr, region, battlegroup,
                                                                  realmId);
    }

    /// The connection's current region / battlegroup, for a caller that only wants to change the realm.
    inline uint32_t Region(void* conn)      { return conn ? *detail::Field<uint32_t>(conn, noff::kOffRegion) : 0; }
    inline uint32_t Battlegroup(void* conn) { return conn ? *detail::Field<uint32_t>(conn, noff::kOffBattlegroup) : 0; }

    /**
     * @brief Drops the realm connection without telling anything above the transport, and re-dials.
     *
     * This is the realm-select screen's own sequence (ClientServices::ConnectToSelectedServer ends in
     * exactly NetClient::Connect on this object), with one correction in front of it.
     *
     * The correction: NetClient::Disconnect branches on the net state. In NS_CONNECTED -- which is
     * every in-world client -- it closes the socket with the response object still attached, so
     * WCDisconnected fires and the main thread walks
     * ClientConnection::HandleDisconnect -> NetClient::HandleDisconnect -> CGlueMgr::NetDisconnectHandler,
     * i.e. straight to the disconnected glue screen. In any other state it detaches the response
     * FIRST and tears down silently, leaving a fresh WowConnection and NS_INITIALIZED behind -- which
     * is also the only state Connect accepts. Writing the state before the call is therefore not a
     * trick to dodge a check, it selects the teardown that does not notify.
     *
     * Everything the reconnect needs is already resident and untouched by the teardown: the account
     * name and session key live in the login-data block the logon server wrote at realm-join time, so
     * the client's stock SMSG_AUTH_CHALLENGE handler answers with a valid CMSG_AUTH_SESSION and
     * re-arms RC4 off the same key on the first send.
     *
     * @warning Do NOT call this from inside an OnPacketReceived handler. The silent teardown runs
     *          NETEVENTQUEUE::Clear, which frees the very node list NETEVENTQUEUE::Poll is iterating
     *          to deliver that packet. Record the request and redial from a frame tick.
     *
     * @param conn     the connection, from Connection().
     * @param address  "host" or "host:port"; the client defaults the port to 9090 when it is absent.
     * @return false when there is no connection to redial.
     */
    inline bool Redial(void* conn, const char* address)
    {
        if (!conn || !address || !address[0]) return false;

        *detail::Field<uint32_t>(conn, noff::kOffNetState) = noff::kStateInitialized;
        Native<noff::ConnectionVoidFn>(noff::kClientConnectionDisconnect)(conn, nullptr);
        Native<noff::NetClientConnectFn>(noff::kNetClientConnect)(conn, nullptr, address);
        return true;
    }

    // --- sending a message -----------------------------------------------------------------------
    // ClientConnection::SendCharEnumOpcode replayed: a CDataStore on the stack, the opcode, the
    // payload, NetClient::Send. The store starts in write mode and is put back to read 0 at the end,
    // because that is what Send takes as "finished" -- it is not a cursor rewind, it is the flag.

    /**
     * @brief Sends a message already built, under @p opcode.
     *
     * The opcode is NOT written by the caller: it has to be the first dword of the store, so a
     * payload built separately would have to be rebuilt to put it in front. Instead the payload is
     * appended to a message this function opens. See the OutMessage overload below for the shape a
     * caller actually uses.
     *
     * @return false when there is no connection, or the socket is down.
     */
    inline bool SendRaw(OutMessage& message)
    {
        void* conn = Connection();
        if (!Connected(conn)) return false;

        message.Finish();
        Native<noff::NetClientSendFn>(noff::kNetClientSend)(conn, nullptr, message.Raw());
        return true;
    }

    /**
     * @brief Builds a message under @p opcode, lets @p fill write its payload, and sends it.
     *
     * The opcode goes in first because a CMSG header is a size and then a four-byte opcode, and the
     * store holds everything after the size. @p fill receives the message with the opcode already
     * written.
     *
     * @code
     *   net::Send(kCmsgEmpowerRelease, [&](OutMessage& m) { m.PutU32(spellId); });
     * @endcode
     */
    template <class Fill>
    inline bool Send(uint32_t opcode, Fill&& fill)
    {
        OutMessage message;
        message.PutU32(opcode);
        fill(message);
        return SendRaw(message);
    }

    /**
     * @brief Sends @p count little-endian dwords under @p opcode.
     *
     * Kept because most Dawnlight messages are exactly this. Anything else uses the Send above.
     */
    inline bool SendDwords(uint32_t opcode, const uint32_t* values, size_t count)
    {
        return Send(opcode, [&](OutMessage& m) {
            for (size_t i = 0; i < count; ++i)
                m.PutU32(values[i]);
        });
    }

    /// One-dword convenience, which is the shape of every Dawnlight CMSG so far.
    inline bool Send(uint32_t opcode, uint32_t value)
    {
        return SendDwords(opcode, &value, 1);
    }

    // --- reading an inbound message --------------------------------------------------------------
    // The message is a CDataStore. OnPacketReceived positions its read cursor past the opcode before
    // publishing and puts it back afterwards, so a subscriber reads the payload from byte zero and
    // the client's own handler still sees an untouched message.

    /// Bytes still unread in the message.
    inline uint32_t Remaining(const void* packet)
    {
        if (!packet) return 0;
        const auto* ds = static_cast<const noff::DataStore*>(packet);
        return ds->size > ds->read ? ds->size - ds->read : 0;
    }

    /**
     * @brief Copies the next @p count bytes out of the message and advances the cursor.
     * @return false, leaving the cursor where it was, when the message is too short.
     */
    inline bool Read(void* packet, void* out, uint32_t count)
    {
        if (!packet || Remaining(packet) < count) return false;
        auto* ds = static_cast<noff::DataStore*>(packet);
        std::memcpy(out, ds->data - ds->base + ds->read, count);
        ds->read += count;
        return true;
    }

    inline bool ReadU8(void* packet, uint8_t& out)   { return Read(packet, &out, 1); }
    inline bool ReadU16(void* packet, uint16_t& out) { return Read(packet, &out, 2); }
    inline bool ReadU32(void* packet, uint32_t& out) { return Read(packet, &out, 4); }

    /**
     * @brief Copies a NUL-terminated string out of the message and advances past its terminator.
     *
     * @param out  receives the text, always NUL-terminated within @p cap.
     * @return false, leaving the cursor where it was, when no terminator is found in what remains or
     *         the text does not fit in @p cap.
     */
    inline bool ReadCString(void* packet, char* out, size_t cap)
    {
        if (!packet || !out || cap == 0) return false;
        auto*          ds        = static_cast<noff::DataStore*>(packet);
        const uint8_t* cursor    = ds->data - ds->base + ds->read;
        const uint32_t remaining = Remaining(packet);

        uint32_t length = 0;
        while (length < remaining && cursor[length] != 0) ++length;
        if (length >= remaining) return false;      // no terminator inside the message
        if (length + 1 > cap) return false;

        std::memcpy(out, cursor, length);
        out[length] = '\0';
        ds->read += length + 1;
        return true;
    }
}
