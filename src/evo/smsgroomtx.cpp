// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/smsgroomtx.h>
#include <evo/specialtx.h>

#include <consensus/validation.h>
#include <pubkey.h>

bool CheckSmsgRoomTx(const CTransaction& tx, const CBlockIndex* pindexPrev, CValidationState& state)
{
    CSmsgRoomTx roomTx;
    if (!GetTxPayload(tx, roomTx)) {
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-smsgroom-payload");
    }

    if (roomTx.nVersion == 0 || roomTx.nVersion > CSmsgRoomTx::CURRENT_VERSION) {
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-smsgroom-version");
    }

    // Room public key must be a valid compressed public key
    if (roomTx.vchRoomPubKey.size() != CPubKey::COMPRESSED_SIZE) {
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-smsgroom-pubkey-size");
    }
    CPubKey roomPubKey(roomTx.vchRoomPubKey);
    if (!roomPubKey.IsFullyValid()) {
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-smsgroom-pubkey-invalid");
    }

    // Retention must be 1-31 days
    if (roomTx.nRetentionDays < 1 || roomTx.nRetentionDays > 31) {
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-smsgroom-retention");
    }

    // Only defined flag bits may be set
    if (roomTx.nFlags & ~(SMSG_ROOM_OPEN | SMSG_ROOM_MODERATED)) {
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-smsgroom-flags");
    }

    return true;
}
