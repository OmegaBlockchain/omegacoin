// Copyright (c) 2018-2024 The Documentchain developers
// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef OMEGA_SMSG_MSGANCHOR_H
#define OMEGA_SMSG_MSGANCHOR_H

#include <fs.h>
#include <saltedhasher.h>
#include <sync.h>
#include <uint256.h>
#include <util/error.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class CTxMemPool;
class CWallet;
struct NodeContext;

namespace smsg {

// OP_RETURN magic prefix: "OMA\x01"
static const uint8_t ANCHOR_MAGIC[4] = {0x4F, 0x4D, 0x41, 0x01};

static const uint32_t ANCHOR_BATCH_MAX = 1000;
static const uint32_t ANCHOR_BATCH_MIN = 100;
static const int64_t ANCHOR_AUTO_COMMIT_INTERVAL = 10 * 60;
static const int64_t ANCHOR_STATE_VERSION = 1;
static const size_t ANCHOR_TIMESTAMP_SIZE = 8;
static const size_t ANCHOR_PAYLOAD_SIZE = sizeof(ANCHOR_MAGIC) + 32 + ANCHOR_TIMESTAMP_SIZE;

struct AnchorEntry {
    uint256 hash;
    uint256 prev;   // zero if no predecessor (revision tracking)
    int64_t nTime = 0;
};

struct AnchorBatchInfo {
    uint256     merkle_root;
    std::string txid;
    int64_t     nAnchorTime = 0;
    int64_t     nSubmitted = 0;
    int64_t     nCommitted = 0;
};

struct AnchorBatch {
    uint256                  merkle_root;
    std::string              txid;
    std::vector<AnchorEntry> entries;       // ordered leaf set, preserves revisions
    int64_t                  nAnchorTime = 0;
    int64_t                  nSubmitted = 0;
    int64_t                  nCommitted = 0; // unix timestamp; 0 = not yet confirmed
};

struct PreparedAnchorBatch {
    uint256                  merkle_root;
    std::vector<AnchorEntry> entries;
    int64_t                  nAnchorTime = 0;
};

enum class AnchorHashStatus {
    NOT_FOUND,
    PENDING,
    SUBMITTED,
    CONFIRMED,
};

enum class QueueHashResult {
    QUEUED,
    PENDING,
    CONFIRMED,
    FULL,
    QUEUE_ERROR,
};

struct AnchorTxLookupResult {
    bool found = false;
    bool confirmed = false;
    int64_t confirmed_time = 0;
};

enum class AnchorCommitError {
    NONE,
    WALLET_UNAVAILABLE,
    NOT_INITIALIZED,
    PREPARE,
    CREATE_TRANSACTION,
    BROADCAST,
    DATABASE,
};

struct AnchorCommitResult {
    std::string txid;
    uint256 merkle_root;
    size_t count = 0;
    bool confirmed = false;
    int64_t confirmed_time = 0;
    AnchorCommitError error = AnchorCommitError::NONE;
    TransactionError tx_error = TransactionError::OK;
};

class MsgAnchorManager
{
public:
    mutable CCriticalSection cs_anchor;

    bool Load(const fs::path &path, std::string *error_out = nullptr);
    bool Save(std::string *error_out = nullptr) const;

    // Queue hash with optional prev (revision link).
    QueueHashResult QueueHash(const uint256 &hash, const uint256 &prev,
                              std::string *error_out = nullptr);

    // Retrieve batch metadata for a queued hash, whether confirmed or not.
    bool GetBatchInfo(const uint256 &hash, AnchorBatchInfo &info_out) const;

    // Classify a hash as absent, pending/prepared, submitted, or confirmed.
    AnchorHashStatus GetHashStatus(const uint256 &hash,
                                   AnchorBatchInfo *info_out = nullptr) const;

    // Get Merkle branch for confirmed hash. branch = sibling hashes bottom-up.
    bool GetProof(const uint256 &hash, std::vector<uint256> &branch_out, uint32_t &index_out) const;

    // Move the current pending batch into a prepared state and build its payload.
    bool PrepareCommit(std::vector<uint8_t> &payload_out, uint256 &root_out,
                       int64_t &anchor_time_out, size_t &count_out,
                       std::string *error_out = nullptr);

    // Persist the prepared batch as submitted after a tx has been accepted.
    bool SubmitPrepared(const std::string &txid, std::string *error_out = nullptr);

    // Restore the prepared batch to the front of the pending queue.
    bool AbortPrepared(std::string *error_out = nullptr);

    // Mark a submitted batch as confirmed.
    bool MarkBatchConfirmed(const std::string &txid, int64_t confirmed_time,
                            std::string *error_out = nullptr);

    size_t  PendingCount() const;
    uint256 PendingMerkleRoot() const;
    int64_t OldestPendingTime() const;
    bool    HasPreparedBatch() const;
    bool    IsLoaded() const;

private:
    struct BatchEntryLocation {
        size_t batch_index = 0;
        uint32_t entry_index = 0;
    };

    bool SaveLocked(std::string *error_out) const;
    static std::vector<uint256> EntryHashes(const std::vector<AnchorEntry> &entries);
    static uint256 ComputeEntriesMerkleRoot(const std::vector<AnchorEntry> &entries);
    static void IndexHashes(const std::vector<AnchorEntry> &entries,
                            std::unordered_set<uint256, StaticSaltedHasher> &target);
    static bool BuildIndexes(const std::vector<AnchorEntry> &pending,
                             const std::vector<AnchorEntry> &prepared,
                             const std::vector<AnchorBatch> &batches,
                             std::unordered_set<uint256, StaticSaltedHasher> &all_hashes_out,
                             std::unordered_set<uint256, StaticSaltedHasher> &pending_hashes_out,
                             std::unordered_set<uint256, StaticSaltedHasher> &prepared_hashes_out,
                             std::unordered_map<uint256, BatchEntryLocation, StaticSaltedHasher> &batch_index_out,
                             std::unordered_map<std::string, size_t> &txid_index_out,
                             std::string *error_out);

    fs::path                           m_path;
    std::vector<AnchorEntry>           m_pending;
    std::optional<PreparedAnchorBatch> m_prepared;
    std::vector<AnchorBatch>           m_batches;
    std::unordered_set<uint256, StaticSaltedHasher> m_all_hashes;
    std::unordered_set<uint256, StaticSaltedHasher> m_pending_hashes;
    std::unordered_set<uint256, StaticSaltedHasher> m_prepared_hashes;
    std::unordered_map<uint256, BatchEntryLocation, StaticSaltedHasher> m_batch_index;
    std::unordered_map<std::string, size_t> m_txid_index;
    bool                               m_loaded = false;
};

extern MsgAnchorManager g_msgAnchor;

// Single SHA-256 of raw message bytes (not double-SHA256).
uint256 MsgSHA256(const std::string &msg);

// Build and parse the OP_RETURN payload used for anchoring.
std::vector<uint8_t> BuildAnchorPayload(const uint256 &root, int64_t anchor_time);
bool ParseAnchorPayload(const std::vector<unsigned char> &payload,
                        uint256 &root_out, int64_t &anchor_time_out);

// Lookup an anchor transaction in wallet state, mempool, or the active chain.
AnchorTxLookupResult LookupAnchorTransaction(const std::string &txid_hex,
                                             const uint256 &expected_root,
                                             int64_t expected_anchor_time,
                                             const CTxMemPool *mempool = nullptr);

#ifdef ENABLE_WALLET
bool CommitPendingAnchorBatch(CWallet *pwallet, NodeContext &node, bool wait_callback,
                              AnchorCommitResult &result_out, std::string *error_out = nullptr);
#endif

// Compute Merkle branch (sibling hashes bottom-up) for leaf at index.
// Uses the same double-SHA256 pairwise algorithm as the Bitcoin block merkle tree.
std::vector<uint256> ComputeMerkleBranch(std::vector<uint256> hashes, uint32_t index);

// Verify branch: the leaf is the single-SHA256 message hash from MsgSHA256,
// while the internal tree edges are still combined with Bitcoin-style double-SHA256 Hash().
bool VerifyMerkleBranch(const uint256 &leaf, const std::vector<uint256> &branch,
                        uint32_t index, const uint256 &root);

} // namespace smsg

#endif // OMEGA_SMSG_MSGANCHOR_H
