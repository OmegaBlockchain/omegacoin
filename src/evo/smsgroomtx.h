// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_SMSGROOMTX_H
#define BITCOIN_EVO_SMSGROOMTX_H

#include <primitives/transaction.h>
#include <serialize.h>

#include <vector>

class CBlockIndex;
class CValidationState;

/** Room flags */
enum {
    SMSG_ROOM_OPEN      = (1U << 0),  // Anyone may join (vs invitation-only)
    SMSG_ROOM_MODERATED = (1U << 1),  // Room key holder may censor messages
};

/**
 * SMSG room creation special transaction payload.
 *
 * The room is identified by the txid of this transaction.
 * The room public key is used by SMSG to encrypt messages to the room.
 */
class CSmsgRoomTx
{
public:
    static constexpr auto SPECIALTX_TYPE = TRANSACTION_SMSG_ROOM;
    static constexpr uint8_t CURRENT_VERSION = 1;

    uint8_t nVersion{CURRENT_VERSION};
    uint32_t nFlags{0};
    std::vector<uint8_t> vchRoomPubKey;     // 33-byte compressed public key
    uint32_t nRetentionDays{31};            // Max message retention (1-31)
    uint32_t nMaxMembers{0};                // 0 = unlimited

    SERIALIZE_METHODS(CSmsgRoomTx, obj)
    {
        READWRITE(obj.nVersion, obj.nFlags, obj.vchRoomPubKey, obj.nRetentionDays, obj.nMaxMembers);
    }
};

bool CheckSmsgRoomTx(const CTransaction& tx, const CBlockIndex* pindexPrev, CValidationState& state);

#endif // BITCOIN_EVO_SMSGROOMTX_H
