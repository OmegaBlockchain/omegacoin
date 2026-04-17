// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/merkle.h>
#include <fs.h>
#include <smsg/msganchor.h>
#include <test/util/setup_common.h>
#include <univalue.h>

#include <boost/test/unit_test.hpp>

#include <sstream>

namespace {

uint256 TestHash(const std::string &message)
{
    return smsg::MsgSHA256(message);
}

std::string ReadText(const fs::path &path)
{
    fsbridge::ifstream file(path);
    BOOST_REQUIRE(file.is_open());

    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

void WriteText(const fs::path &path, const std::string &text)
{
    fsbridge::ofstream file(path);
    BOOST_REQUIRE(file.is_open());
    file << text;
}

void RequireQueued(smsg::MsgAnchorManager &manager, const uint256 &hash,
                   const uint256 &prev, std::string &error)
{
    BOOST_REQUIRE(manager.QueueHash(hash, prev, &error) == smsg::QueueHashResult::QUEUED);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(msganchor_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(anchor_payload_roundtrip)
{
    const uint256 root = TestHash("anchor-root");
    const int64_t anchor_time = 1713370000;

    const std::vector<uint8_t> payload = smsg::BuildAnchorPayload(root, anchor_time);
    BOOST_CHECK_EQUAL(payload.size(), smsg::ANCHOR_PAYLOAD_SIZE);

    uint256 parsed_root;
    int64_t parsed_time = 0;
    BOOST_CHECK(smsg::ParseAnchorPayload(payload, parsed_root, parsed_time));
    BOOST_CHECK_EQUAL(parsed_root.GetHex(), root.GetHex());
    BOOST_CHECK_EQUAL(parsed_time, anchor_time);
}

BOOST_AUTO_TEST_CASE(prepared_abort_preserves_queue_order)
{
    smsg::MsgAnchorManager manager;
    const fs::path path = GetDataDir() / "msganchor_abort.json";
    std::string error;
    BOOST_REQUIRE(manager.Load(path, &error));

    const uint256 h1 = TestHash("message-1");
    const uint256 h2 = TestHash("message-2");
    const uint256 h3 = TestHash("message-3");
    const uint256 h4 = TestHash("message-4");

    RequireQueued(manager, h1, uint256(), error);
    RequireQueued(manager, h2, h1, error);
    RequireQueued(manager, h3, h2, error);

    std::vector<uint8_t> payload;
    uint256 root;
    int64_t anchor_time = 0;
    size_t count = 0;
    BOOST_REQUIRE(manager.PrepareCommit(payload, root, anchor_time, count, &error));
    BOOST_CHECK_EQUAL(count, 3U);

    RequireQueued(manager, h4, h3, error);
    BOOST_REQUIRE(manager.AbortPrepared(&error));
    BOOST_CHECK_EQUAL(manager.PendingCount(), 4U);

    const std::vector<uint256> expected{h1, h2, h3, h4};
    BOOST_CHECK_EQUAL(manager.PendingMerkleRoot().GetHex(), ComputeMerkleRoot(expected).GetHex());
}

BOOST_AUTO_TEST_CASE(load_requeues_prepared_before_pending)
{
    const fs::path path = GetDataDir() / "msganchor_reload.json";
    std::string error;

    {
        smsg::MsgAnchorManager writer;
        BOOST_REQUIRE(writer.Load(path, &error));

        const uint256 h1 = TestHash("reload-1");
        const uint256 h2 = TestHash("reload-2");
        const uint256 h3 = TestHash("reload-3");

        RequireQueued(writer, h1, uint256(), error);
        RequireQueued(writer, h2, h1, error);

        std::vector<uint8_t> payload;
        uint256 root;
        int64_t anchor_time = 0;
        size_t count = 0;
        BOOST_REQUIRE(writer.PrepareCommit(payload, root, anchor_time, count, &error));
        RequireQueued(writer, h3, h2, error);
    }

    smsg::MsgAnchorManager reader;
    BOOST_REQUIRE(reader.Load(path, &error));
    BOOST_CHECK_EQUAL(reader.PendingCount(), 3U);

    const std::vector<uint256> expected{
        TestHash("reload-1"),
        TestHash("reload-2"),
        TestHash("reload-3"),
    };
    BOOST_CHECK_EQUAL(reader.PendingMerkleRoot().GetHex(), ComputeMerkleRoot(expected).GetHex());
}

BOOST_AUTO_TEST_CASE(confirm_persists_revision_links_and_proof)
{
    const fs::path path = GetDataDir() / "msganchor_confirm.json";
    std::string error;
    const std::string txid = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const uint256 h1 = TestHash("version-1");
    const uint256 h2 = TestHash("version-2");
    uint256 root;

    {
        smsg::MsgAnchorManager manager;
        BOOST_REQUIRE(manager.Load(path, &error));

        RequireQueued(manager, h1, uint256(), error);
        RequireQueued(manager, h2, h1, error);

        std::vector<uint8_t> payload;
        int64_t anchor_time = 0;
        size_t count = 0;
        BOOST_REQUIRE(manager.PrepareCommit(payload, root, anchor_time, count, &error));
        BOOST_REQUIRE(manager.SubmitPrepared(txid, &error));
    }

    smsg::MsgAnchorManager reader;
    BOOST_REQUIRE(reader.Load(path, &error));

    std::vector<uint256> branch;
    uint32_t index = 0;
    BOOST_CHECK(!reader.GetProof(h2, branch, index));

    BOOST_REQUIRE(reader.MarkBatchConfirmed(txid, 1713371234, &error));
    BOOST_REQUIRE(reader.GetProof(h2, branch, index));
    BOOST_CHECK(smsg::VerifyMerkleBranch(h2, branch, index, root));

    UniValue doc;
    BOOST_REQUIRE(doc.read(ReadText(path)));
    BOOST_REQUIRE(doc["version"].isNum());
    BOOST_CHECK_EQUAL(doc["version"].get_int64(), smsg::ANCHOR_STATE_VERSION);
    BOOST_REQUIRE(doc["batches"].isArray());
    BOOST_REQUIRE_EQUAL(doc["batches"].size(), 1U);
    const UniValue &entries = doc["batches"][0]["entries"];
    BOOST_REQUIRE(entries.isArray());
    BOOST_REQUIRE_EQUAL(entries.size(), 2U);
    BOOST_CHECK_EQUAL(entries[1]["prev"].get_str(), h1.GetHex());
}

BOOST_AUTO_TEST_CASE(queue_hash_reports_typed_states)
{
    smsg::MsgAnchorManager manager;
    const fs::path path = GetDataDir() / "msganchor_queue_status.json";
    std::string error;
    BOOST_REQUIRE(manager.Load(path, &error));

    const uint256 hash = TestHash("status-message");
    const std::string txid = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

    BOOST_CHECK(manager.GetHashStatus(hash) == smsg::AnchorHashStatus::NOT_FOUND);
    BOOST_CHECK(manager.QueueHash(hash, uint256(), &error) == smsg::QueueHashResult::QUEUED);
    BOOST_CHECK(manager.GetHashStatus(hash) == smsg::AnchorHashStatus::PENDING);
    BOOST_CHECK(manager.QueueHash(hash, uint256(), &error) == smsg::QueueHashResult::PENDING);

    std::vector<uint8_t> payload;
    uint256 root;
    int64_t anchor_time = 0;
    size_t count = 0;
    BOOST_REQUIRE(manager.PrepareCommit(payload, root, anchor_time, count, &error));
    BOOST_CHECK(manager.GetHashStatus(hash) == smsg::AnchorHashStatus::PENDING);
    BOOST_CHECK(manager.QueueHash(hash, uint256(), &error) == smsg::QueueHashResult::PENDING);

    BOOST_REQUIRE(manager.SubmitPrepared(txid, &error));
    BOOST_CHECK(manager.GetHashStatus(hash) == smsg::AnchorHashStatus::SUBMITTED);
    BOOST_CHECK(manager.QueueHash(hash, uint256(), &error) == smsg::QueueHashResult::PENDING);

    BOOST_REQUIRE(manager.MarkBatchConfirmed(txid, 1713374444, &error));
    BOOST_CHECK(manager.GetHashStatus(hash) == smsg::AnchorHashStatus::CONFIRMED);
    BOOST_CHECK(manager.QueueHash(hash, uint256(), &error) == smsg::QueueHashResult::CONFIRMED);
}

BOOST_AUTO_TEST_CASE(queue_hash_reports_full_state)
{
    smsg::MsgAnchorManager manager;
    const fs::path path = GetDataDir() / "msganchor_queue_full.json";
    std::string error;
    BOOST_REQUIRE(manager.Load(path, &error));

    for (uint32_t i = 0; i < smsg::ANCHOR_BATCH_MAX; ++i) {
        RequireQueued(manager, TestHash(std::string("full-") + std::to_string(i)), uint256(), error);
    }

    BOOST_CHECK(manager.QueueHash(TestHash("full-overflow"), uint256(), &error) == smsg::QueueHashResult::FULL);
}

BOOST_AUTO_TEST_CASE(load_failure_preserves_existing_state)
{
    smsg::MsgAnchorManager manager;
    const fs::path path = GetDataDir() / "msganchor_parse_failure.json";
    std::string error;
    BOOST_REQUIRE(manager.Load(path, &error));
    RequireQueued(manager, TestHash("stable"), uint256(), error);
    BOOST_CHECK_EQUAL(manager.PendingCount(), 1U);

    WriteText(path, "{invalid-json");
    BOOST_CHECK(!manager.Load(path, &error));
    BOOST_CHECK(!error.empty());
    BOOST_CHECK_EQUAL(manager.PendingCount(), 1U);
}

BOOST_AUTO_TEST_CASE(empty_state_file_is_not_treated_as_fresh_start)
{
    smsg::MsgAnchorManager manager;
    const fs::path path = GetDataDir() / "msganchor_empty_state.json";
    std::string error;
    BOOST_REQUIRE(manager.Load(path, &error));
    RequireQueued(manager, TestHash("stable-empty"), uint256(), error);

    WriteText(path, "");
    BOOST_CHECK(!manager.Load(path, &error));
    BOOST_CHECK(!error.empty());
    BOOST_CHECK_EQUAL(manager.PendingCount(), 1U);
}

BOOST_AUTO_TEST_SUITE_END()
