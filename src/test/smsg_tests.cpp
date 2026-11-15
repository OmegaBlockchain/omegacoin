// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

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

BOOST_AUTO_TEST_SUITE_END()
