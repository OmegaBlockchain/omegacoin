// Copyright (c) 2021-2023 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/validation.h>
#include <evo/smsgroomtx.h>
#include <evo/mnhftx.h>
#include <evo/specialtx.h>
#include <evo/specialtxman.h>
#include <primitives/transaction.h>
#include <uint256.h>
#include <util/string.h>
#include <util/strencodings.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>
#include <test/util/setup_common.h>

#include <cstdint>
#include <vector>


bool VerifyMNHFTx(const CTransaction& tx, CValidationState& state)
{
    MNHFTxPayload mnhfTx;
    if (!GetTxPayload(tx, mnhfTx)) {
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-mnhf-payload");
    }

    if (mnhfTx.nVersion == 0 || mnhfTx.nVersion > MNHFTxPayload::CURRENT_VERSION) {
        return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "bad-mnhf-version");
    }

    return true;
}

static CMutableTransaction CreateMNHFTx(const uint256& mnhfTxHash, const CBLSSignature& cblSig, const uint16_t& versionBit)
{
    MNHFTxPayload extraPayload;
    extraPayload.nVersion = 1;
    extraPayload.signal.nVersion = versionBit;
    extraPayload.signal.quorumHash = mnhfTxHash;
    extraPayload.signal.sig = cblSig;

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_MNHF_SIGNAL;
    SetTxPayload(tx, extraPayload);

    return tx;
}

static CMutableTransaction CreateSmsgRoomTx()
{
    CKey key;
    key.MakeNewKey(true);

    CSmsgRoomTx roomTx;
    const CPubKey pubkey = key.GetPubKey();
    roomTx.nFlags = SMSG_ROOM_OPEN;
    roomTx.vchRoomPubKey.assign(pubkey.begin(), pubkey.end());
    roomTx.nRetentionDays = 7;
    roomTx.nMaxMembers = 32;

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_SMSG_ROOM;
    SetTxPayload(tx, roomTx);

    return tx;
}

BOOST_FIXTURE_TEST_SUITE(specialtx_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(verify_mnhf_specialtx_tests)
{
    int count = 10;
    uint16_t ver = 2;

    std::vector<CBLSSignature> vec_sigs;
    std::vector<CBLSPublicKey> vec_pks;
    std::vector<CBLSSecretKey> vec_sks;

    CBLSSecretKey sk;
    uint256 hash = GetRandHash();
    for (int i = 0; i < count; i++) {
        sk.MakeNewKey();
        vec_pks.push_back(sk.GetPublicKey());
        vec_sks.push_back(sk);
    }

    CBLSSecretKey ag_sk = CBLSSecretKey::AggregateInsecure(vec_sks);
    CBLSPublicKey ag_pk = CBLSPublicKey::AggregateInsecure(vec_pks);

    BOOST_CHECK(ag_sk.IsValid());
    BOOST_CHECK(ag_pk.IsValid());

    uint256 verHash = uint256S(ToString(ver));
    auto sig = ag_sk.Sign(verHash);
    BOOST_CHECK(sig.VerifyInsecure(ag_pk, verHash));

    const CMutableTransaction tx = CreateMNHFTx(hash, sig, ver);
    CValidationState state;
    BOOST_CHECK(VerifyMNHFTx(CTransaction(tx), state));
}

BOOST_AUTO_TEST_CASE(smsg_room_activation_boundary_tests)
{
    const auto& consensus = Params().GetConsensus();
    BOOST_CHECK_EQUAL(consensus.nForkEnforcementHeight, 3190000);
    BOOST_CHECK_EQUAL(consensus.nSmsgRoomHeight, 3200000);

    const CTransaction tx{CreateSmsgRoomTx()};
    CCoinsView view_dummy;
    CCoinsViewCache view(&view_dummy);

    CBlockIndex prev_before_activation;
    prev_before_activation.nHeight = consensus.nSmsgRoomHeight - 2;

    CValidationState state_before;
    {
        LOCK(cs_main);
        BOOST_CHECK(!CheckSpecialTx(tx, &prev_before_activation, state_before, view, false));
    }
    BOOST_CHECK(state_before.IsInvalid());
    BOOST_CHECK_EQUAL(state_before.GetRejectReason(), "bad-tx-smsgroom-not-active");

    CBlockIndex prev_at_activation;
    prev_at_activation.nHeight = consensus.nSmsgRoomHeight - 1;

    CValidationState state_at;
    {
        LOCK(cs_main);
        BOOST_CHECK(CheckSpecialTx(tx, &prev_at_activation, state_at, view, false));
    }
    BOOST_CHECK(state_at.IsValid());
}

BOOST_AUTO_TEST_SUITE_END()
