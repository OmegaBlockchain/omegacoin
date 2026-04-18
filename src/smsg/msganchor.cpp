// Copyright (c) 2018-2024 The Documentchain developers
// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <smsg/msganchor.h>

#include <chainparams.h>
#include <consensus/merkle.h>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <logging.h>
#include <node/context.h>
#include <node/transaction.h>
#include <script/script.h>
#include <txmempool.h>
#include <util/system.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <univalue.h>
#include <validation.h>

#ifdef ENABLE_WALLET
#include <wallet/wallet.h>
#endif

#include <algorithm>
#include <ctime>
#include <cstring>
#include <sstream>
#include <utility>

namespace smsg {

MsgAnchorManager g_msgAnchor;

// --- hash helpers -----------------------------------------------------------

uint256 MsgSHA256(const std::string &msg)
{
    uint256 result;
    CSHA256().Write(reinterpret_cast<const uint8_t *>(msg.data()), msg.size())
             .Finalize(result.begin());
    return result;
}

std::vector<uint8_t> BuildAnchorPayload(const uint256 &root, int64_t anchor_time)
{
    std::vector<uint8_t> payload(ANCHOR_PAYLOAD_SIZE);
    memcpy(payload.data(), ANCHOR_MAGIC, sizeof(ANCHOR_MAGIC));
    memcpy(payload.data() + sizeof(ANCHOR_MAGIC), root.begin(), 32);
    WriteLE64(payload.data() + sizeof(ANCHOR_MAGIC) + 32, static_cast<uint64_t>(anchor_time));
    return payload;
}

bool ParseAnchorPayload(const std::vector<unsigned char> &payload,
                        uint256 &root_out, int64_t &anchor_time_out)
{
    if (payload.size() != ANCHOR_PAYLOAD_SIZE) {
        return false;
    }
    if (memcmp(payload.data(), ANCHOR_MAGIC, sizeof(ANCHOR_MAGIC)) != 0) {
        return false;
    }

    memcpy(root_out.begin(), payload.data() + sizeof(ANCHOR_MAGIC), 32);
    anchor_time_out = static_cast<int64_t>(
        ReadLE64(payload.data() + sizeof(ANCHOR_MAGIC) + 32));
    return true;
}

// Compute Merkle branch using double-SHA256 pairwise (Bitcoin standard).
std::vector<uint256> ComputeMerkleBranch(std::vector<uint256> hashes, uint32_t index)
{
    std::vector<uint256> branch;
    while (hashes.size() > 1) {
        if (hashes.size() & 1) {
            hashes.push_back(hashes.back());
        }
        branch.push_back(hashes[index ^ 1]);

        const size_t n_half = hashes.size() / 2;
        for (size_t i = 0; i < n_half; ++i) {
            hashes[i] = Hash(hashes[i * 2].begin(),     hashes[i * 2].end(),
                             hashes[i * 2 + 1].begin(), hashes[i * 2 + 1].end());
        }
        hashes.resize(n_half);
        index >>= 1;
    }
    return branch;
}

bool VerifyMerkleBranch(const uint256 &leaf, const std::vector<uint256> &branch,
                        uint32_t index, const uint256 &root)
{
    uint256 cur = leaf;
    for (const auto &sibling : branch) {
        if (index & 1) {
            cur = Hash(sibling.begin(), sibling.end(), cur.begin(), cur.end());
        } else {
            cur = Hash(cur.begin(), cur.end(), sibling.begin(), sibling.end());
        }
        index >>= 1;
    }
    return cur == root;
}

// --- persistence ------------------------------------------------------------

static void SetError(std::string *error_out, const std::string &message)
{
    if (error_out) {
        *error_out = message;
    }
}

static bool ReadFile(const fs::path &path, std::string &data_out, std::string *error_out)
{
    data_out.clear();

    try {
        if (!fs::exists(path)) {
            return true;
        }
        if (fs::is_directory(path)) {
            SetError(error_out, strprintf("MsgAnchor: %s is a directory, not a file.", path.string()));
            return false;
        }
    } catch (const fs::filesystem_error &e) {
        SetError(error_out,
            strprintf("MsgAnchor: failed to inspect %s: %s",
                      path.string(), fsbridge::get_filesystem_error_message(e)));
        return false;
    }

    fsbridge::ifstream f(path);
    if (!f.is_open()) {
        SetError(error_out, strprintf("MsgAnchor: failed to open %s", path.string()));
        return false;
    }

    std::ostringstream ss;
    ss << f.rdbuf();
    if (!f.good() && !f.eof()) {
        SetError(error_out, strprintf("MsgAnchor: failed to read %s", path.string()));
        return false;
    }

    data_out = ss.str();
    if (data_out.empty()) {
        SetError(error_out, strprintf("MsgAnchor: %s is empty.", path.string()));
        return false;
    }

    return true;
}

static void LogStateError(const std::string *error_out, const std::string &fallback_message)
{
    LogPrintf("%s\n", error_out && !error_out->empty() ? *error_out : fallback_message);
}

static int64_t GetConfirmedBlockTime(const uint256 &block_hash)
{
    if (block_hash.IsNull()) {
        return 0;
    }

    LOCK(cs_main);
    const CBlockIndex *block_index = LookupBlockIndex(block_hash);
    return block_index ? block_index->GetBlockTime() : 0;
}

static bool ExtractAnchorPayloadFromScript(const CScript &script, uint256 &root_out,
                                           int64_t &anchor_time_out)
{
    CScript::const_iterator pc = script.begin();
    opcodetype opcode;
    std::vector<unsigned char> payload;

    if (!script.GetOp(pc, opcode) || opcode != OP_RETURN) {
        return false;
    }
    if (!script.GetOp(pc, opcode, payload)) {
        return false;
    }
    if (pc != script.end()) {
        return false;
    }

    return ParseAnchorPayload(payload, root_out, anchor_time_out);
}

static bool TransactionHasAnchorPayload(const CTransaction &tx, const uint256 &expected_root,
                                        int64_t expected_anchor_time)
{
    for (const auto &txout : tx.vout) {
        uint256 root;
        int64_t anchor_time = 0;
        if (!ExtractAnchorPayloadFromScript(txout.scriptPubKey, root, anchor_time)) {
            continue;
        }
        if (root != expected_root) {
            continue;
        }
        if (expected_anchor_time != 0 && anchor_time != expected_anchor_time) {
            continue;
        }
        return true;
    }

    return false;
}

#ifdef ENABLE_WALLET
static void RecordAnchorTransaction(CWallet * const pwallet, const CTransactionRef &tx)
{
    if (!tx || !pwallet || pwallet->GetWalletTx(tx->GetHash())) {
        return;
    }
    pwallet->CommitTransaction(tx, {}, {});
}
#endif

AnchorTxLookupResult LookupAnchorTransaction(const std::string &txid_hex,
                                             const uint256 &expected_root,
                                             int64_t expected_anchor_time,
                                             const CTxMemPool *mempool)
{
    AnchorTxLookupResult result;
    if (txid_hex.size() != 64 || !IsHex(txid_hex)) {
        return result;
    }

    const uint256 txid = uint256S(txid_hex);

#ifdef ENABLE_WALLET
    for (const auto &wallet : GetWallets()) {
        CTransactionRef wallet_tx;
        int wallet_depth = 0;
        uint256 wallet_block_hash;
        int64_t wallet_tx_time = 0;

        {
            LOCK(wallet->cs_wallet);
            const CWalletTx *wtx = wallet->GetWalletTx(txid);
            if (!wtx) {
                continue;
            }
            wallet_tx = wtx->tx;
            wallet_depth = wtx->GetDepthInMainChain();
            wallet_block_hash = wtx->m_confirm.hashBlock;
            wallet_tx_time = wtx->GetTxTime();
        }

        if (!wallet_tx || !TransactionHasAnchorPayload(*wallet_tx, expected_root, expected_anchor_time)) {
            continue;
        }

        result.found = true;
        result.confirmed = wallet_depth > 0;
        if (result.confirmed) {
            result.confirmed_time = GetConfirmedBlockTime(wallet_block_hash);
            if (result.confirmed_time == 0) {
                result.confirmed_time = wallet_tx_time;
            }
        }
        return result;
    }
#endif

    uint256 hash_block;
    CTransactionRef tx = GetTransaction(/* block_index */ nullptr, mempool, txid,
                                        Params().GetConsensus(), hash_block);
    if (!tx || !TransactionHasAnchorPayload(*tx, expected_root, expected_anchor_time)) {
        return result;
    }

    result.found = true;
    result.confirmed = !hash_block.IsNull();
    if (result.confirmed) {
        result.confirmed_time = GetConfirmedBlockTime(hash_block);
    }
    return result;
}

static bool ParseHashString(const UniValue &value, const std::string &field_name,
                            uint256 &out, std::string *error_out, bool allow_missing = false)
{
    if (value.isNull()) {
        if (allow_missing) {
            out = uint256();
            return true;
        }
        SetError(error_out, strprintf("MsgAnchor: missing %s field.", field_name));
        return false;
    }
    if (!value.isStr()) {
        SetError(error_out, strprintf("MsgAnchor: %s must be a hex string.", field_name));
        return false;
    }

    const std::string &hex = value.get_str();
    if (allow_missing && hex.empty()) {
        out = uint256();
        return true;
    }
    if (hex.size() != 64 || !IsHex(hex)) {
        SetError(error_out, strprintf("MsgAnchor: %s must be 64 hex chars.", field_name));
        return false;
    }

    out = uint256S(hex);
    return true;
}

static bool ParseInt64Field(const UniValue &value, const std::string &field_name,
                            int64_t &out, std::string *error_out, int64_t default_value = 0)
{
    if (value.isNull()) {
        out = default_value;
        return true;
    }
    if (!value.isNum()) {
        SetError(error_out, strprintf("MsgAnchor: %s must be a number.", field_name));
        return false;
    }

    out = value.get_int64();
    return true;
}

static bool ParseStringField(const UniValue &value, const std::string &field_name,
                             std::string &out, std::string *error_out, bool allow_missing = false)
{
    if (value.isNull()) {
        if (allow_missing) {
            out.clear();
            return true;
        }
        SetError(error_out, strprintf("MsgAnchor: missing %s field.", field_name));
        return false;
    }
    if (!value.isStr()) {
        SetError(error_out, strprintf("MsgAnchor: %s must be a string.", field_name));
        return false;
    }

    out = value.get_str();
    return true;
}

static UniValue SerializeEntry(const AnchorEntry &entry)
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("hash", entry.hash.GetHex());
    obj.pushKV("prev", entry.prev.GetHex());
    obj.pushKV("time", entry.nTime);
    return obj;
}

static UniValue SerializeEntries(const std::vector<AnchorEntry> &entries)
{
    UniValue arr(UniValue::VARR);
    for (const auto &entry : entries) {
        arr.push_back(SerializeEntry(entry));
    }
    return arr;
}

static bool ParseEntry(const UniValue &value, AnchorEntry &entry, std::string *error_out)
{
    if (!value.isObject()) {
        SetError(error_out, "MsgAnchor: entry must be an object.");
        return false;
    }

    if (!ParseHashString(find_value(value, "hash"), "hash", entry.hash, error_out)) {
        return false;
    }
    if (!ParseHashString(find_value(value, "prev"), "prev", entry.prev, error_out, /* allow_missing */ true)) {
        return false;
    }
    if (!ParseInt64Field(find_value(value, "time"), "time", entry.nTime, error_out)) {
        return false;
    }

    return true;
}

static bool ParseEntryList(const UniValue &value, std::vector<AnchorEntry> &entries_out,
                           std::string *error_out)
{
    if (value.isNull()) {
        entries_out.clear();
        return true;
    }
    if (!value.isArray()) {
        SetError(error_out, "MsgAnchor: entries must be an array.");
        return false;
    }

    std::vector<AnchorEntry> entries;
    entries.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        AnchorEntry entry;
        if (!ParseEntry(value[i], entry, error_out)) {
            return false;
        }
        entries.push_back(entry);
    }

    entries_out = std::move(entries);
    return true;
}

static bool ParseLegacyHashes(const UniValue &value, std::vector<AnchorEntry> &entries_out,
                              std::string *error_out)
{
    if (value.isNull()) {
        entries_out.clear();
        return true;
    }
    if (!value.isArray()) {
        SetError(error_out, "MsgAnchor: hashes must be an array.");
        return false;
    }

    std::vector<AnchorEntry> entries;
    entries.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        AnchorEntry entry;
        if (!ParseHashString(value[i], "hashes[]", entry.hash, error_out)) {
            return false;
        }
        entries.push_back(entry);
    }

    entries_out = std::move(entries);
    return true;
}

static bool ParseBatch(const UniValue &value, AnchorBatch &batch, std::string *error_out)
{
    if (!value.isObject()) {
        SetError(error_out, "MsgAnchor: batch must be an object.");
        return false;
    }

    if (!ParseHashString(find_value(value, "root"), "root", batch.merkle_root, error_out)) {
        return false;
    }
    if (!ParseStringField(find_value(value, "txid"), "txid", batch.txid, error_out)) {
        return false;
    }
    if (!ParseInt64Field(find_value(value, "anchor_time"), "anchor_time", batch.nAnchorTime,
                         error_out, /* default_value */ 0)) {
        return false;
    }
    if (!ParseInt64Field(find_value(value, "submitted"), "submitted", batch.nSubmitted,
                         error_out, /* default_value */ 0)) {
        return false;
    }
    if (!ParseInt64Field(find_value(value, "committed"), "committed", batch.nCommitted,
                         error_out, /* default_value */ 0)) {
        return false;
    }

    if (value.exists("entries")) {
        if (!ParseEntryList(find_value(value, "entries"), batch.entries, error_out)) {
            return false;
        }
    } else {
        if (!ParseLegacyHashes(find_value(value, "hashes"), batch.entries, error_out)) {
            return false;
        }
    }

    return true;
}

static bool ParsePrepared(const UniValue &value, PreparedAnchorBatch &batch, std::string *error_out)
{
    if (!value.isObject()) {
        SetError(error_out, "MsgAnchor: prepared batch must be an object.");
        return false;
    }

    if (!ParseHashString(find_value(value, "root"), "root", batch.merkle_root, error_out)) {
        return false;
    }
    if (!ParseInt64Field(find_value(value, "anchor_time"), "anchor_time", batch.nAnchorTime,
                         error_out, /* default_value */ 0)) {
        return false;
    }

    if (value.exists("entries")) {
        if (!ParseEntryList(find_value(value, "entries"), batch.entries, error_out)) {
            return false;
        }
    } else {
        if (!ParseLegacyHashes(find_value(value, "hashes"), batch.entries, error_out)) {
            return false;
        }
    }

    return true;
}

bool MsgAnchorManager::Load(const fs::path &path, std::string *error_out)
{
    LOCK(cs_anchor);

    std::string data;
    if (!ReadFile(path, data, error_out)) {
        LogStateError(error_out, "MsgAnchor: failed to read anchor state.");
        return false;
    }
    std::vector<AnchorEntry> pending;
    std::optional<PreparedAnchorBatch> prepared;
    std::vector<AnchorBatch> batches;

    if (!data.empty()) {
        UniValue root;
        if (!root.read(data)) {
            SetError(error_out, strprintf("MsgAnchor: failed to parse %s", path.string()));
            LogStateError(error_out, "MsgAnchor: parse failure");
            return false;
        }
        if (!root.isObject()) {
            SetError(error_out, strprintf("MsgAnchor: %s must contain a JSON object", path.string()));
            LogStateError(error_out, "MsgAnchor: invalid root object");
            return false;
        }

        const UniValue &j_version = find_value(root, "version");
        if (!j_version.isNull()) {
            int64_t version = 0;
            if (!ParseInt64Field(j_version, "version", version, error_out)) {
                LogStateError(error_out, "MsgAnchor: invalid version");
                return false;
            }
            if (version != ANCHOR_STATE_VERSION) {
                SetError(error_out, strprintf("MsgAnchor: unsupported state version %d.", version));
                LogStateError(error_out, "MsgAnchor: unsupported version");
                return false;
            }
        }

        if (!ParseEntryList(find_value(root, "pending"), pending, error_out)) {
            LogStateError(error_out, "MsgAnchor: invalid pending state");
            return false;
        }

        const UniValue &j_prepared = find_value(root, "prepared");
        if (!j_prepared.isNull()) {
            PreparedAnchorBatch parsed_prepared;
            if (!ParsePrepared(j_prepared, parsed_prepared, error_out)) {
                LogStateError(error_out, "MsgAnchor: invalid prepared state");
                return false;
            }
            prepared = std::move(parsed_prepared);
        }

        const UniValue &j_batches = find_value(root, "batches");
        if (!j_batches.isNull()) {
            if (!j_batches.isArray()) {
                SetError(error_out, "MsgAnchor: batches must be an array.");
                LogStateError(error_out, "MsgAnchor: invalid batches");
                return false;
            }
            batches.reserve(j_batches.size());
            for (size_t i = 0; i < j_batches.size(); ++i) {
                AnchorBatch batch;
                if (!ParseBatch(j_batches[i], batch, error_out)) {
                    LogStateError(error_out, "MsgAnchor: invalid batch entry");
                    return false;
                }
                batches.push_back(std::move(batch));
            }
        }
    }

    if (prepared) {
        std::vector<AnchorEntry> restored;
        restored.reserve(prepared->entries.size() + pending.size());
        restored.insert(restored.end(), prepared->entries.begin(), prepared->entries.end());
        restored.insert(restored.end(), pending.begin(), pending.end());
        pending.swap(restored);
    }

    std::unordered_set<uint256, StaticSaltedHasher> all_hashes;
    std::unordered_set<uint256, StaticSaltedHasher> pending_hashes;
    std::unordered_set<uint256, StaticSaltedHasher> prepared_hashes;
    std::unordered_map<uint256, BatchEntryLocation, StaticSaltedHasher> batch_index;
    std::unordered_map<std::string, size_t> txid_index;
    if (!BuildIndexes(pending, std::vector<AnchorEntry>{}, batches,
                      all_hashes, pending_hashes, prepared_hashes, batch_index,
                      txid_index, error_out)) {
        LogStateError(error_out, "MsgAnchor: invalid anchor index state");
        return false;
    }

    m_path = path;
    m_pending = std::move(pending);
    m_prepared.reset();
    m_batches = std::move(batches);
    m_all_hashes = std::move(all_hashes);
    m_pending_hashes = std::move(pending_hashes);
    m_prepared_hashes = std::move(prepared_hashes);
    m_batch_index = std::move(batch_index);
    m_txid_index = std::move(txid_index);
    m_loaded = true;
    return true;
}

bool MsgAnchorManager::SaveLocked(std::string *error_out) const
{
    UniValue j_batches(UniValue::VARR);
    for (const auto &batch : m_batches) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("root", batch.merkle_root.GetHex());
        obj.pushKV("txid", batch.txid);
        obj.pushKV("anchor_time", batch.nAnchorTime);
        obj.pushKV("submitted", batch.nSubmitted);
        obj.pushKV("committed", batch.nCommitted);
        obj.pushKV("entries", SerializeEntries(batch.entries));
        j_batches.push_back(obj);
    }

    UniValue doc(UniValue::VOBJ);
    doc.pushKV("version", ANCHOR_STATE_VERSION);
    doc.pushKV("pending", SerializeEntries(m_pending));
    if (m_prepared) {
        UniValue prepared(UniValue::VOBJ);
        prepared.pushKV("root", m_prepared->merkle_root.GetHex());
        prepared.pushKV("anchor_time", m_prepared->nAnchorTime);
        prepared.pushKV("entries", SerializeEntries(m_prepared->entries));
        doc.pushKV("prepared", prepared);
    }
    doc.pushKV("batches", j_batches);

    fs::path tmp = fs::path(m_path.string() + ".tmp");
    fsbridge::ofstream f;
    f.open(tmp);
    if (!f.is_open()) {
        SetError(error_out, strprintf("MsgAnchor: cannot write %s", tmp.string()));
        LogPrintf("%s\n", error_out ? *error_out : "MsgAnchor: open failed");
        return false;
    }

    f << doc.write(2);
    if (!f.good()) {
        f.close();
        fs::remove(tmp);
        SetError(error_out, strprintf("MsgAnchor: write failed for %s", tmp.string()));
        LogPrintf("%s\n", error_out ? *error_out : "MsgAnchor: write failed");
        return false;
    }
    f.close();

    if (!RenameOver(tmp, m_path)) {
        fs::remove(tmp);
        SetError(error_out, strprintf("MsgAnchor: failed to replace %s", m_path.string()));
        LogPrintf("%s\n", error_out ? *error_out : "MsgAnchor: rename failed");
        return false;
    }

    return true;
}

bool MsgAnchorManager::Save(std::string *error_out) const
{
    LOCK(cs_anchor);
    return SaveLocked(error_out);
}

#ifdef ENABLE_WALLET
static bool RestorePreparedBatch(const std::string &message, AnchorCommitResult &result_out,
                                 std::string *error_out)
{
    std::string abort_error;
    if (!g_msgAnchor.AbortPrepared(&abort_error)) {
        result_out.error = AnchorCommitError::DATABASE;
        SetError(error_out,
                 abort_error.empty()
                     ? "Anchor commit failed and the prepared batch could not be restored."
                     : abort_error);
        return false;
    }

    SetError(error_out, message);
    return false;
}

bool CommitPendingAnchorBatch(CWallet *pwallet, NodeContext &node, bool wait_callback,
                              AnchorCommitResult &result_out, std::string *error_out)
{
    result_out = AnchorCommitResult{};
    if (error_out) {
        error_out->clear();
    }

    if (!pwallet) {
        result_out.error = AnchorCommitError::WALLET_UNAVAILABLE;
        SetError(error_out, "MsgAnchor: wallet not available.");
        return false;
    }
    if (!node.connman || !node.mempool) {
        result_out.error = AnchorCommitError::BROADCAST;
        SetError(error_out, "MsgAnchor: transaction relay is not available.");
        return false;
    }
    if (!g_msgAnchor.IsLoaded()) {
        result_out.error = AnchorCommitError::NOT_INITIALIZED;
        SetError(error_out, "MsgAnchor: anchor state is not initialized.");
        return false;
    }

    std::vector<uint8_t> payload;
    uint256 root;
    int64_t anchor_time = 0;
    size_t pending_count = 0;
    if (!g_msgAnchor.PrepareCommit(payload, root, anchor_time, pending_count, error_out)) {
        result_out.error = AnchorCommitError::PREPARE;
        return false;
    }

    result_out.merkle_root = root;
    result_out.count = pending_count;

    CScript op_return_script = CScript() << OP_RETURN << payload;
    std::vector<CRecipient> recipients = {{op_return_script, 0, false}};

    CTransactionRef new_tx;
    {
        pwallet->BlockUntilSyncedToCurrentChain();
        LOCK(pwallet->cs_wallet);

        CCoinControl coin_control;
        coin_control.fRequireAllInputs = false;

        CAmount fee = 0;
        int change_pos = -1;
        bilingual_str fail_reason;
        if (!pwallet->CreateTransaction(recipients, new_tx, fee, change_pos, fail_reason, coin_control)) {
            result_out.error = AnchorCommitError::CREATE_TRANSACTION;
            return RestorePreparedBatch(fail_reason.original, result_out, error_out);
        }
    }

    result_out.txid = new_tx->GetHash().GetHex();

    std::string broadcast_error;
    const TransactionError broadcast_result = BroadcastTransaction(
        node, new_tx, broadcast_error, pwallet->m_default_max_tx_fee,
        /* relay */ true, wait_callback, /* bypass_limits */ false);
    result_out.tx_error = broadcast_result;

    const AnchorTxLookupResult lookup = LookupAnchorTransaction(
        result_out.txid, root, anchor_time, node.mempool.get());
    if (broadcast_result != TransactionError::OK && !lookup.found) {
        result_out.error = AnchorCommitError::BROADCAST;
        return RestorePreparedBatch(
            !broadcast_error.empty() ? broadcast_error : "MsgAnchor: failed to broadcast anchor transaction.",
            result_out, error_out);
    }

    RecordAnchorTransaction(pwallet, new_tx);

    if (!g_msgAnchor.SubmitPrepared(result_out.txid, error_out)) {
        result_out.error = AnchorCommitError::DATABASE;
        return false;
    }

    if (lookup.confirmed) {
        result_out.confirmed = true;
        result_out.confirmed_time = lookup.confirmed_time != 0
            ? lookup.confirmed_time
            : static_cast<int64_t>(time(nullptr));
        if (!g_msgAnchor.MarkBatchConfirmed(result_out.txid, result_out.confirmed_time, error_out)) {
            result_out.error = AnchorCommitError::DATABASE;
            return false;
        }
    }

    return true;
}
#endif

// --- public API -------------------------------------------------------------

std::vector<uint256> MsgAnchorManager::EntryHashes(const std::vector<AnchorEntry> &entries)
{
    std::vector<uint256> hashes;
    hashes.reserve(entries.size());
    for (const auto &entry : entries) {
        hashes.push_back(entry.hash);
    }
    return hashes;
}

uint256 MsgAnchorManager::ComputeEntriesMerkleRoot(const std::vector<AnchorEntry> &entries)
{
    if (entries.empty()) {
        return uint256();
    }
    return ComputeMerkleRoot(EntryHashes(entries));
}

void MsgAnchorManager::IndexHashes(const std::vector<AnchorEntry> &entries,
                                   std::unordered_set<uint256, StaticSaltedHasher> &target)
{
    for (const auto &entry : entries) {
        target.insert(entry.hash);
    }
}

bool MsgAnchorManager::BuildIndexes(
    const std::vector<AnchorEntry> &pending,
    const std::vector<AnchorEntry> &prepared,
    const std::vector<AnchorBatch> &batches,
    std::unordered_set<uint256, StaticSaltedHasher> &all_hashes_out,
    std::unordered_set<uint256, StaticSaltedHasher> &pending_hashes_out,
    std::unordered_set<uint256, StaticSaltedHasher> &prepared_hashes_out,
    std::unordered_map<uint256, BatchEntryLocation, StaticSaltedHasher> &batch_index_out,
    std::unordered_map<std::string, size_t> &txid_index_out,
    std::string *error_out)
{
    all_hashes_out.clear();
    pending_hashes_out.clear();
    prepared_hashes_out.clear();
    batch_index_out.clear();
    txid_index_out.clear();

    auto add_hash = [&](const uint256 &hash, const std::string &location) {
        if (!all_hashes_out.insert(hash).second) {
            SetError(error_out, strprintf("MsgAnchor: duplicate hash %s found in %s.",
                                          hash.GetHex(), location));
            return false;
        }
        return true;
    };

    pending_hashes_out.reserve(pending.size());
    for (const auto &entry : pending) {
        if (!add_hash(entry.hash, "pending state")) {
            return false;
        }
        pending_hashes_out.insert(entry.hash);
    }

    prepared_hashes_out.reserve(prepared.size());
    for (const auto &entry : prepared) {
        if (!add_hash(entry.hash, "prepared state")) {
            return false;
        }
        prepared_hashes_out.insert(entry.hash);
    }

    for (size_t batch_idx = 0; batch_idx < batches.size(); ++batch_idx) {
        const auto &batch = batches[batch_idx];
        if (batch.txid.empty()) {
            SetError(error_out, strprintf("MsgAnchor: batch %u has an empty txid.",
                                          static_cast<unsigned int>(batch_idx)));
            return false;
        }
        if (!txid_index_out.emplace(batch.txid, batch_idx).second) {
            SetError(error_out, strprintf("MsgAnchor: duplicate batch txid %s found in batch %u.",
                                          batch.txid, static_cast<unsigned int>(batch_idx)));
            return false;
        }
        for (size_t entry_idx = 0; entry_idx < batch.entries.size(); ++entry_idx) {
            const auto &entry = batch.entries[entry_idx];
            if (!add_hash(entry.hash, strprintf("batch %u", static_cast<unsigned int>(batch_idx)))) {
                return false;
            }
            batch_index_out.emplace(entry.hash, BatchEntryLocation{
                batch_idx,
                static_cast<uint32_t>(entry_idx),
            });
        }
    }

    return true;
}

QueueHashResult MsgAnchorManager::QueueHash(const uint256 &hash, const uint256 &prev,
                                            std::string *error_out)
{
    LOCK(cs_anchor);
    if (error_out) {
        error_out->clear();
    }

    if (m_all_hashes.count(hash) != 0) {
        if (m_pending_hashes.count(hash) != 0 || m_prepared_hashes.count(hash) != 0) {
            return QueueHashResult::PENDING;
        }
        const auto batch_it = m_batch_index.find(hash);
        if (batch_it == m_batch_index.end()) {
            SetError(error_out, strprintf("MsgAnchor: indexed hash %s is missing a batch entry.",
                                          hash.GetHex()));
            return QueueHashResult::QUEUE_ERROR;
        }
        return m_batches[batch_it->second.batch_index].nCommitted != 0
            ? QueueHashResult::CONFIRMED
            : QueueHashResult::PENDING;
    }
    if (m_pending.size() >= ANCHOR_BATCH_MAX) {
        return QueueHashResult::FULL;
    }

    AnchorEntry entry;
    entry.hash = hash;
    entry.prev = prev;
    entry.nTime = static_cast<int64_t>(time(nullptr));
    m_pending.push_back(entry);
    m_all_hashes.insert(hash);
    m_pending_hashes.insert(hash);
    if (!SaveLocked(error_out)) {
        m_pending.pop_back();
        m_pending_hashes.erase(hash);
        m_all_hashes.erase(hash);
        return QueueHashResult::QUEUE_ERROR;
    }

    return QueueHashResult::QUEUED;
}

bool MsgAnchorManager::GetBatchInfo(const uint256 &hash, AnchorBatchInfo &info_out) const
{
    LOCK(cs_anchor);
    const auto batch_it = m_batch_index.find(hash);
    if (batch_it == m_batch_index.end()) {
        return false;
    }

    const AnchorBatch &batch = m_batches[batch_it->second.batch_index];
    info_out.merkle_root = batch.merkle_root;
    info_out.txid = batch.txid;
    info_out.nAnchorTime = batch.nAnchorTime;
    info_out.nSubmitted = batch.nSubmitted;
    info_out.nCommitted = batch.nCommitted;
    return true;
}

AnchorHashStatus MsgAnchorManager::GetHashStatus(const uint256 &hash,
                                                 AnchorBatchInfo *info_out) const
{
    LOCK(cs_anchor);

    if (m_pending_hashes.count(hash) != 0 || m_prepared_hashes.count(hash) != 0) {
        return AnchorHashStatus::PENDING;
    }

    const auto batch_it = m_batch_index.find(hash);
    if (batch_it == m_batch_index.end()) {
        return AnchorHashStatus::NOT_FOUND;
    }

    const AnchorBatch &batch = m_batches[batch_it->second.batch_index];
    if (info_out) {
        info_out->merkle_root = batch.merkle_root;
        info_out->txid = batch.txid;
        info_out->nAnchorTime = batch.nAnchorTime;
        info_out->nSubmitted = batch.nSubmitted;
        info_out->nCommitted = batch.nCommitted;
    }
    return batch.nCommitted != 0 ? AnchorHashStatus::CONFIRMED : AnchorHashStatus::SUBMITTED;
}

bool MsgAnchorManager::GetProof(const uint256 &hash, std::vector<uint256> &branch_out,
                                uint32_t &index_out) const
{
    LOCK(cs_anchor);
    const auto batch_it = m_batch_index.find(hash);
    if (batch_it == m_batch_index.end()) {
        return false;
    }

    const BatchEntryLocation &location = batch_it->second;
    const AnchorBatch &batch = m_batches[location.batch_index];
    if (batch.nCommitted == 0 || batch.entries.empty() || location.entry_index >= batch.entries.size()) {
        return false;
    }

    index_out = location.entry_index;
    branch_out = ComputeMerkleBranch(EntryHashes(batch.entries), index_out);
    return true;
}

bool MsgAnchorManager::PrepareCommit(std::vector<uint8_t> &payload_out, uint256 &root_out,
                                     int64_t &anchor_time_out, size_t &count_out,
                                     std::string *error_out)
{
    LOCK(cs_anchor);

    if (m_prepared) {
        SetError(error_out, "MsgAnchor: another anchor commit is already prepared.");
        return false;
    }
    if (m_pending.empty()) {
        SetError(error_out, "MsgAnchor: no pending hashes to commit.");
        return false;
    }

    std::unordered_set<uint256, StaticSaltedHasher> pending_hashes = m_pending_hashes;
    PreparedAnchorBatch prepared;
    prepared.entries.swap(m_pending);
    prepared.merkle_root = ComputeEntriesMerkleRoot(prepared.entries);
    prepared.nAnchorTime = static_cast<int64_t>(time(nullptr));

    root_out = prepared.merkle_root;
    anchor_time_out = prepared.nAnchorTime;
    count_out = prepared.entries.size();
    payload_out = BuildAnchorPayload(root_out, anchor_time_out);

    m_prepared = std::move(prepared);
    m_pending_hashes.clear();
    m_prepared_hashes = std::move(pending_hashes);
    if (!SaveLocked(error_out)) {
        m_pending.swap(m_prepared->entries);
        m_pending_hashes = std::move(m_prepared_hashes);
        m_prepared_hashes.clear();
        m_prepared.reset();
        return false;
    }

    return true;
}

bool MsgAnchorManager::SubmitPrepared(const std::string &txid, std::string *error_out)
{
    LOCK(cs_anchor);

    if (!m_prepared) {
        SetError(error_out, "MsgAnchor: no prepared batch to submit.");
        return false;
    }
    if (txid.empty()) {
        SetError(error_out, "MsgAnchor: submitted txid must not be empty.");
        return false;
    }
    if (m_txid_index.count(txid) != 0) {
        SetError(error_out, strprintf("MsgAnchor: batch txid %s already exists.", txid));
        return false;
    }

    AnchorBatch batch;
    batch.merkle_root = m_prepared->merkle_root;
    batch.txid = txid;
    batch.entries = m_prepared->entries;
    batch.nAnchorTime = m_prepared->nAnchorTime;
    batch.nSubmitted = static_cast<int64_t>(time(nullptr));

    const size_t batch_index = m_batches.size();
    m_batches.push_back(batch);
    m_txid_index.emplace(txid, batch_index);
    for (size_t i = 0; i < batch.entries.size(); ++i) {
        m_batch_index.emplace(batch.entries[i].hash, BatchEntryLocation{
            batch_index,
            static_cast<uint32_t>(i),
        });
    }
    m_prepared_hashes.clear();
    m_prepared.reset();
    if (!SaveLocked(error_out)) {
        m_prepared = PreparedAnchorBatch{batch.merkle_root, batch.entries, batch.nAnchorTime};
        IndexHashes(batch.entries, m_prepared_hashes);
        for (const auto &entry : batch.entries) {
            m_batch_index.erase(entry.hash);
        }
        m_txid_index.erase(txid);
        m_batches.pop_back();
        return false;
    }

    return true;
}

bool MsgAnchorManager::AbortPrepared(std::string *error_out)
{
    LOCK(cs_anchor);

    if (!m_prepared) {
        SetError(error_out, "MsgAnchor: no prepared batch to abort.");
        return false;
    }

    const std::vector<AnchorEntry> original_pending = m_pending;
    const std::optional<PreparedAnchorBatch> prepared = m_prepared;
    const std::unordered_set<uint256, StaticSaltedHasher> original_pending_hashes = m_pending_hashes;
    const std::unordered_set<uint256, StaticSaltedHasher> original_prepared_hashes = m_prepared_hashes;

    std::vector<AnchorEntry> restored;
    restored.reserve(m_prepared->entries.size() + m_pending.size());
    restored.insert(restored.end(), m_prepared->entries.begin(), m_prepared->entries.end());
    restored.insert(restored.end(), m_pending.begin(), m_pending.end());
    m_pending = std::move(restored);
    m_pending_hashes.insert(m_prepared_hashes.begin(), m_prepared_hashes.end());
    m_prepared_hashes.clear();
    m_prepared.reset();

    if (!SaveLocked(error_out)) {
        m_pending = original_pending;
        m_prepared = prepared;
        m_pending_hashes = original_pending_hashes;
        m_prepared_hashes = original_prepared_hashes;
        return false;
    }

    return true;
}

bool MsgAnchorManager::MarkBatchConfirmed(const std::string &txid, int64_t confirmed_time,
                                          std::string *error_out)
{
    LOCK(cs_anchor);

    const auto it = m_txid_index.find(txid);
    if (it == m_txid_index.end()) {
        SetError(error_out, strprintf("MsgAnchor: batch for txid %s not found.", txid));
        return false;
    }

    AnchorBatch &batch = m_batches[it->second];
    if (batch.nCommitted != 0) {
        return true;
    }

    batch.nCommitted = confirmed_time > 0 ? confirmed_time : static_cast<int64_t>(time(nullptr));
    if (!SaveLocked(error_out)) {
        batch.nCommitted = 0;
        return false;
    }
    return true;
}

size_t MsgAnchorManager::PendingCount() const
{
    LOCK(cs_anchor);
    return m_pending.size();
}

uint256 MsgAnchorManager::PendingMerkleRoot() const
{
    LOCK(cs_anchor);
    return ComputeEntriesMerkleRoot(m_pending);
}

int64_t MsgAnchorManager::OldestPendingTime() const
{
    LOCK(cs_anchor);
    return m_pending.empty() ? 0 : m_pending.front().nTime;
}

bool MsgAnchorManager::HasPreparedBatch() const
{
    LOCK(cs_anchor);
    return m_prepared.has_value();
}

bool MsgAnchorManager::IsLoaded() const
{
    LOCK(cs_anchor);
    return m_loaded;
}

} // namespace smsg
