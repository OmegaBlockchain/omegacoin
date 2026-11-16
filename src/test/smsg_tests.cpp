// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/hmac_sha256.h>
#include <smsg/db.h>
#include <smsg/smessage.h>
#include <test/util/setup_common.h>
#include <timedata.h>

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> MakeMessage(uint32_t nRealPayload, uint32_t nHeaderPayload, int64_t timestamp)
{
    std::vector<uint8_t> v(smsg::SMSG_HDR_LEN + nRealPayload, 0);
    smsg::SecureMessage *psmsg = (smsg::SecureMessage*)v.data();
    psmsg->version[0] = 2;
    psmsg->version[1] = 1;
    psmsg->timestamp = timestamp;
    psmsg->nPayload = nHeaderPayload;
    return v;
}

void MakeKey(uint8_t *chKey, const char *prefix, uint64_t timestamp, uint8_t fill)
{
    memcpy(chKey, prefix, 2);
    for (int i = 0; i < 8; ++i) {
        chKey[2 + i] = (uint8_t)(timestamp >> (8 * (7 - i)));
    }
    memset(chKey + 10, fill, 20);
}

uint64_t KeyTime(const uint8_t *chKey)
{
    uint64_t t = 0;
    for (int i = 0; i < 8; ++i) {
        t = (t << 8) | chKey[2 + i];
    }
    return t;
}

// The free-message PoW HMAC exactly as Validate()/SetHash() compute it.
void PowHash(std::vector<uint8_t> &v, uint32_t nonce, uint8_t out[32])
{
    smsg::SecureMessage *psmsg = (smsg::SecureMessage*)v.data();
    memcpy(psmsg->nonce, &nonce, 4);
    uint8_t civ[32];
    for (int i = 0; i < 32; i += 4) {
        memcpy(civ + i, &nonce, 4);
    }
    CHMAC_SHA256 ctx(civ, 32);
    ctx.Write(v.data() + 4, smsg::SMSG_HDR_LEN - 4);
    ctx.Write(v.data() + smsg::SMSG_HDR_LEN, v.size() - smsg::SMSG_HDR_LEN);
    ctx.Finalize(out);
}

// Grinds a nonce that meets only the old ~16.2-bit test, then stamps the checksum.
bool GrindLegacyOnly(std::vector<uint8_t> &v)
{
    uint8_t h[32];
    for (uint32_t n = 0; n < 50000000; ++n) {
        PowHash(v, n, h);
        const uint8_t low = h[29] & 7;
        if (h[31] == 0 && h[30] == 0 && low != 0 && low != 7) {
            memcpy(v.data(), h, 4);
            return true;
        }
    }
    return false;
}

struct SmsgDbTestingSetup : public BasicTestingSetup {
    ~SmsgDbTestingSetup()
    {
        LOCK(smsg::cs_smsgDB);
        delete smsg::smsgDB;
        smsg::smsgDB = nullptr;
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(smsg_tests, SmsgDbTestingSetup)

// SEC-01: the header length field is read by HashMsg/GetMsgID, so a message
// whose header disagrees with the supplied buffer must be refused before any
// of them runs.
BOOST_AUTO_TEST_CASE(validate_rejects_header_length_mismatch)
{
    const int64_t now = GetAdjustedTime();
    const uint32_t nReal = 64;

    // The length field sits in the last four header bytes.
    {
        std::vector<uint8_t> v = MakeMessage(nReal, 0x04030201, now);
        BOOST_CHECK_EQUAL(smsg::SMSG_HDR_LEN, 104U);
        BOOST_CHECK(v[100] == 0x01 && v[101] == 0x02 && v[102] == 0x03 && v[103] == 0x04);
    }

    for (uint32_t nHeader : {nReal + 1, nReal + 64, nReal - 1, 0U, 2000000000U, 0xffffffffU}) {
        std::vector<uint8_t> v = MakeMessage(nReal, nHeader, now);
        BOOST_CHECK_EQUAL(smsgModule.Validate(v.data(), v.data() + smsg::SMSG_HDR_LEN, nReal), (int)smsg::SMSG_GENERAL_ERROR);
    }

    // A consistent header gets past the length gate and is judged on its
    // content instead (here: a timestamp far in the future).
    std::vector<uint8_t> v = MakeMessage(nReal, nReal, now + 100000);
    BOOST_CHECK_EQUAL(smsgModule.Validate(v.data(), v.data() + smsg::SMSG_HDR_LEN, nReal), (int)smsg::SMSG_TIME_IN_FUTURE);

    // Zero-length payloads stay rejected.
    std::vector<uint8_t> empty = MakeMessage(0, 0, now);
    BOOST_CHECK(smsgModule.Validate(empty.data(), empty.data() + smsg::SMSG_HDR_LEN, 0) != (int)smsg::SMSG_NO_ERROR);
}

// SEC-05: trollboxlist walks the "tb" rows newest-first with PrevSmesg.
BOOST_AUTO_TEST_CASE(prevsmesg_walks_prefix_newest_first)
{
    LOCK(smsg::cs_smsgDB);
    smsg::SecMsgDB db;
    BOOST_REQUIRE(db.Open("cw"));

    uint8_t chKey[30];
    smsg::SecMsgStored stored;
    stored.timeReceived = 1;
    stored.status = 0;
    stored.folderId = 0;
    stored.vchMessage.assign(smsg::SMSG_HDR_LEN + 8, 0x5a);

    // Empty prefix range, with and without neighbouring prefixes.
    {
        std::unique_ptr<leveldb::Iterator> it(db.pdb->NewIterator(leveldb::ReadOptions()));
        BOOST_CHECK(!db.PrevSmesg(it.get(), "tb", chKey, stored));
    }

    // Neighbours on both sides must never be returned.
    for (const char *prefix : {"im", "ta", "tc", "zz"}) {
        MakeKey(chKey, prefix, 250, 0x11);
        BOOST_REQUIRE(db.WriteSmesg(chKey, stored));
    }
    {
        std::unique_ptr<leveldb::Iterator> it(db.pdb->NewIterator(leveldb::ReadOptions()));
        BOOST_CHECK(!db.PrevSmesg(it.get(), "tb", chKey, stored));
    }

    // Timestamps that differ in a high byte check the big-endian ordering.
    const std::vector<uint64_t> vTimes = {100, 200, 300, 70000, 1794758100ULL};
    for (uint64_t t : vTimes) {
        stored.timeReceived = (int64_t)t;
        MakeKey(chKey, "tb", t, 0x22);
        BOOST_REQUIRE(db.WriteSmesg(chKey, stored));
    }
    // Two rows sharing a timestamp are both returned.
    stored.timeReceived = 300;
    MakeKey(chKey, "tb", 300, 0x33);
    BOOST_REQUIRE(db.WriteSmesg(chKey, stored));

    // A record that cannot be deserialised is skipped, not fatal.
    MakeKey(chKey, "tb", 250, 0x44);
    BOOST_REQUIRE(db.pdb->Put(leveldb::WriteOptions(), leveldb::Slice((const char*)chKey, 30), "x").ok());
    // A key of the wrong size inside the prefix range ends the walk.
    BOOST_REQUIRE(db.pdb->Put(leveldb::WriteOptions(), "tb", "x").ok());

    std::vector<uint64_t> vSeen;
    {
        std::unique_ptr<leveldb::Iterator> it(db.pdb->NewIterator(leveldb::ReadOptions()));
        smsg::SecMsgStored out;
        while (db.PrevSmesg(it.get(), "tb", chKey, out)) {
            BOOST_CHECK_EQUAL((uint64_t)out.timeReceived, KeyTime(chKey));
            BOOST_CHECK_EQUAL(out.vchMessage.size(), smsg::SMSG_HDR_LEN + 8);
            vSeen.push_back(KeyTime(chKey));
            BOOST_REQUIRE(vSeen.size() <= 16);
        }
    }
    const std::vector<uint64_t> vExpect = {1794758100ULL, 70000, 300, 300, 200, 100};
    BOOST_CHECK_EQUAL_COLLECTIONS(vSeen.begin(), vSeen.end(), vExpect.begin(), vExpect.end());

    // Forward and backward walks see the same rows.
    size_t nForward = 0;
    {
        std::unique_ptr<leveldb::Iterator> it(db.pdb->NewIterator(leveldb::ReadOptions()));
        smsg::SecMsgStored out;
        // The 2-byte "tb" key sorts first and stops NextSmesg, so start after it.
        it->Seek(std::string("tb") + std::string(1, '\0'));
        if (it->Valid()) it->Prev();
        while (db.NextSmesg(it.get(), "tb", chKey, out)) {
            nForward++;
            BOOST_REQUIRE(nForward <= 16);
        }
    }
    BOOST_CHECK_EQUAL(nForward, vExpect.size());

    // Prefixes that cannot be bounded are refused rather than misread.
    {
        std::unique_ptr<leveldb::Iterator> it(db.pdb->NewIterator(leveldb::ReadOptions()));
        BOOST_CHECK(!db.PrevSmesg(it.get(), "t", chKey, stored));
        BOOST_CHECK(!db.PrevSmesg(it.get(), std::string("t\xff"), chKey, stored));
    }
}

// BUG-02: SendData() announces a bucket to a peer only when timeChanged >=
// the peer's lastMatched, which is GetTime(). timeChanged used to be
// GetAdjustedTime(), so a node with a negative peer time offset never (offset
// <= -2) or only sometimes (offset -1) announced its own or relayed messages.
// The global time offset cannot be set here without breaking timedata_tests,
// so the clock is passed in explicitly.
BOOST_AUTO_TEST_CASE(bucket_change_time_uses_local_clock)
{
    const int64_t now = 1800000000;
    SetMockTime(now);

    const uint8_t sample1[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const uint8_t sample2[8] = {9, 10, 11, 12, 13, 14, 15, 16};
    smsg::SecMsgBucket bkt;
    bkt.setTokens.insert(smsg::SecMsgToken(now - 60, sample1, 8, 0, 2));

    bkt.hashBucket(now - 10);
    BOOST_CHECK_EQUAL(bkt.timeChanged, now - 10);
    BOOST_CHECK_EQUAL(bkt.nActive, 1U);

    // Same content: the change time is kept.
    bkt.hashBucket(now + 50);
    BOOST_CHECK_EQUAL(bkt.timeChanged, now - 10);

    // New content through the default overload: stamped with GetTime(), the
    // clock SendData() uses for lastMatched, not the adjusted time.
    bkt.setTokens.insert(smsg::SecMsgToken(now - 30, sample2, 8, 0, 2));
    bkt.hashBucket();
    BOOST_CHECK_EQUAL(bkt.timeChanged, GetTime());
    BOOST_CHECK_EQUAL(bkt.nActive, 2U);

    // The SendData() condition: a change in the same second as the last
    // send cycle (lastMatched == timeChanged) must still be announced.
    const int64_t lastMatched = GetTime();
    BOOST_CHECK(!(bkt.timeChanged < lastMatched));

    SetMockTime(0);
}

// SEC-05: free messages from SMSG_POW_V2_TIME on need the full 19 bits;
// SetHash() always produces them, so its messages are valid everywhere.
BOOST_AUTO_TEST_CASE(free_message_pow_is_19_bits_after_activation)
{
    const uint32_t nReal = 64;
    const int64_t act = smsg::SMSG_POW_V2_TIME;

    SetMockTime(act - 600);
    std::vector<uint8_t> before = MakeMessage(nReal, nReal, act - 1200);
    BOOST_REQUIRE(GrindLegacyOnly(before));
    BOOST_CHECK_EQUAL(smsgModule.Validate(before.data(), before.data() + smsg::SMSG_HDR_LEN, nReal), (int)smsg::SMSG_NO_ERROR);

    SetMockTime(act + 600);
    std::vector<uint8_t> after = MakeMessage(nReal, nReal, act + 60);
    BOOST_REQUIRE(GrindLegacyOnly(after));
    BOOST_CHECK_EQUAL(smsgModule.Validate(after.data(), after.data() + smsg::SMSG_HDR_LEN, nReal), (int)smsg::SMSG_INVALID_HASH);

    std::vector<uint8_t> fresh = MakeMessage(nReal, nReal, act + 60);
    const bool was_enabled = smsg::fSecMsgEnabled;
    smsg::fSecMsgEnabled = true; // SetHash() stops when SMSG is disabled
    BOOST_CHECK_EQUAL(smsgModule.SetHash(fresh.data(), fresh.data() + smsg::SMSG_HDR_LEN, nReal), (int)smsg::SMSG_NO_ERROR);
    smsg::fSecMsgEnabled = was_enabled;
    BOOST_CHECK_EQUAL(smsgModule.Validate(fresh.data(), fresh.data() + smsg::SMSG_HDR_LEN, nReal), (int)smsg::SMSG_NO_ERROR);
    uint32_t nonce;
    memcpy(&nonce, ((smsg::SecureMessage*)fresh.data())->nonce, 4);
    uint8_t h[32];
    PowHash(fresh, nonce, h);
    BOOST_CHECK(h[31] == 0 && h[30] == 0 && (h[29] & 7) == 0);

    SetMockTime(0);
}

// A topic (version 4) message is free: nonce is its PoW counter, not a
// lifetime. It used to be stored with nonce[0] days (0..255).
BOOST_AUTO_TEST_CASE(topic_message_lives_two_days)
{
    const int64_t now = 1800000000;
    SetMockTime(now);
    std::vector<uint8_t> v = MakeMessage(80, 80, now - 60);
    smsg::SecureMessage *psmsg = (smsg::SecureMessage*)v.data();
    psmsg->version[0] = smsg::SMSG_VERSION_TOPIC;
    psmsg->version[1] = 0;
    psmsg->nonce[0] = 200;
    for (size_t i = smsg::SMSG_HDR_LEN; i < v.size(); ++i) {
        v[i] = (uint8_t)i;
    }

    const int64_t bucketTime = (now - 60) - ((now - 60) % smsg::SMSG_BUCKET_LEN);
    {
        LOCK(smsgModule.cs_smsg);
        BOOST_REQUIRE_EQUAL(smsgModule.Store(v.data(), v.data() + smsg::SMSG_HDR_LEN, 80, false), (int)smsg::SMSG_NO_ERROR);
        const auto &tokens = smsgModule.buckets[bucketTime].setTokens;
        BOOST_REQUIRE_EQUAL(tokens.size(), 1U);
        BOOST_CHECK_EQUAL((int)tokens.begin()->ttl, 2);
        smsgModule.buckets.erase(bucketTime);
    }
    SetMockTime(0);
}

// SEC-18: the token sample is the start of the message hash, so a message that
// only copies another's timestamp and first 8 payload bytes no longer blocks it.
BOOST_AUTO_TEST_CASE(token_sample_covers_whole_message)
{
    const int64_t now = 1800000000;
    SetMockTime(now);
    std::vector<uint8_t> real = MakeMessage(80, 80, now - 120);
    for (size_t i = smsg::SMSG_HDR_LEN; i < real.size(); ++i) {
        real[i] = (uint8_t)i;
    }
    std::vector<uint8_t> junk = real;
    junk.back() ^= 0xff; // same timestamp, same first 8 payload bytes

    const int64_t bucketTime = (now - 120) - ((now - 120) % smsg::SMSG_BUCKET_LEN);
    {
        LOCK(smsgModule.cs_smsg);
        BOOST_REQUIRE_EQUAL(smsgModule.Store(junk.data(), junk.data() + smsg::SMSG_HDR_LEN, 80, false), (int)smsg::SMSG_NO_ERROR);
        BOOST_CHECK_EQUAL(smsgModule.Store(real.data(), real.data() + smsg::SMSG_HDR_LEN, 80, false), (int)smsg::SMSG_NO_ERROR);
        // The very same message is still recognised as a duplicate.
        BOOST_CHECK(smsgModule.Store(real.data(), real.data() + smsg::SMSG_HDR_LEN, 80, false) != (int)smsg::SMSG_NO_ERROR);

        const auto &tokens = smsgModule.buckets[bucketTime].setTokens;
        BOOST_REQUIRE_EQUAL(tokens.size(), 2U);
        const std::vector<uint8_t> id = smsgModule.GetMsgID((const smsg::SecureMessage*)real.data(), real.data() + smsg::SMSG_HDR_LEN);
        bool found = false;
        for (const auto &t : tokens) {
            found |= memcmp(t.sample, id.data() + 8, 8) == 0;
        }
        BOOST_CHECK(found);
        smsgModule.buckets.erase(bucketTime);
    }
    SetMockTime(0);
}

BOOST_AUTO_TEST_SUITE_END()
