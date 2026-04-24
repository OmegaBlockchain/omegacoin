// Copyright (c) 2017-2024 The Particl Core developers
// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/*
Notes:
    Running with -debug could leave to and from address hashes and public keys in the log.

    Wallet Locked
        A copy of each incoming message is stored in bucket files ending in _wl.dat
        wl (wallet locked) bucket files are deleted if they expire, like normal buckets
        When the wallet is unlocked all the messages in wl files are scanned.

    Address Whitelist
        Owned Addresses are stored in addresses vector
        Saved to smsg.ini
        Modify options using the smsglocalkeys rpc command or edit the smsg.ini file (with client closed)

    TODO:
        For buckets older than current, only need to store no. messages and hash in memory

*/

#include <smsg/smessage.h>

#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <map>
#include <stdexcept>
#include <errno.h>
#include <limits>
#include <compat/byteswap.h>

#include <boost/algorithm/string/replace.hpp>
#include <boost/chrono.hpp>
#include <boost/thread/thread.hpp>


#include <secp256k1.h>
#include <secp256k1_ecdh.h>
#include <secp256k1_recovery.h>

#include <crypto/hmac_sha256.h>
#include <crypto/sha512.h>

#include <policy/policy.h>
#include <support/allocators/secure.h>
#include <consensus/validation.h>
#include <ui_interface.h>
#include <validation.h>
#include <validationinterface.h>
#include <wallet/ismine.h>

#include <sync.h>
#include <random.h>
#include <chain.h>
#include <netmessagemaker.h>
#include <fs.h>

#ifdef ENABLE_WALLET
#include <wallet/coincontrol.h>
#include <wallet/wallet.h>
#include <interfaces/chain.h>
#endif

#include <base58.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <clientversion.h>

#include <xxhash/xxhash.h>

#include <smsg/crypter.h>
#include <smsg/db.h>
#include <smsg/msganchor.h>
#include <util/system.h>
#include <thread>

#if ENABLE_ZMQ
#include <zmq/zmqnotificationinterface.h>
#endif

extern CConnman* g_connman;

extern void Misbehaving(NodeId nodeid, int howmuch, const std::string& message="");
extern CCriticalSection cs_main;

smsg::CSMSG smsgModule;

namespace smsg {
std::atomic<bool> fSecMsgEnabled{false};

boost::thread_group threadGroupSmsg;

boost::signals2::signal<void (SecMsgStored &inboxHdr)> NotifySecMsgInboxChanged;
boost::signals2::signal<void (SecMsgStored &outboxHdr)> NotifySecMsgOutboxChanged;
boost::signals2::signal<void ()> NotifySecMsgWalletUnlocked;
boost::signals2::signal<void (SecMsgStored &trollboxHdr)> NotifySecMsgTrollboxChanged;


secp256k1_context *secp256k1_context_smsg = nullptr;

static void ResetSmsgRuntimeState(CSMSG& module, bool stop_threads)
{
    fSecMsgEnabled = false;

#ifdef ENABLE_WALLET
    if (module.m_handler_unload) {
        module.m_handler_unload->disconnect();
        module.m_handler_unload.reset();
    }
    if (module.m_handler_status) {
        module.m_handler_status->disconnect();
        module.m_handler_status.reset();
    }
    if (module.m_handler_unlock_start) {
        module.m_handler_unlock_start->disconnect();
        module.m_handler_unlock_start.reset();
    }
#endif

    if (g_connman) {
        g_connman->SetLocalServices(ServiceFlags(g_connman->GetLocalServices() & ~NODE_SMSG));
    }

    if (stop_threads) {
        threadGroupSmsg.interrupt_all();
        threadGroupSmsg.join_all();
    }

    if (smsgDB) {
        LOCK(cs_smsgDB);
        delete smsgDB;
        smsgDB = nullptr;
    }

    module.UnloadAllWallets();
    module.pwallet.reset();

    module.keyStore.Clear();
    module.trollboxAddress = CKeyID();
    module.nLastProcessedPurged = 0;

    {
        LOCK(module.cs_smsg);
        module.addresses.clear();
        module.buckets.clear();
        module.setPurged.clear();
        module.setPurgedTimestamps.clear();
    }

    if (secp256k1_context_smsg) {
        secp256k1_context_destroy(secp256k1_context_smsg);
        secp256k1_context_smsg = nullptr;
    }
}

uint32_t SMSGGetSecondsInDay()
{
    static bool fIsRegTest = Params().NetworkIDString() == "regtest";
    return fIsRegTest ? 600 : SMSG_SECONDS_IN_DAY;
}

std::string SecMsgToken::ToString() const
{
    return strprintf("%d-%08x", timestamp, *((uint64_t*)sample));
}

void SecMsgBucket::hashBucket()
{
    void *state = XXH32_init(1);

    int64_t now = GetAdjustedTime();

    nActive = 0;
    nLeastTTL = 0;
    for (auto it = setTokens.begin(); it != setTokens.end(); ++it) {
        if (it->timestamp + it->ttl * SMSGGetSecondsInDay() < now) {
            continue;
        }

        XXH32_update(state, it->sample, 8);
        if (it->ttl > 0 && (nLeastTTL == 0 || it->ttl < nLeastTTL)) {
            nLeastTTL = it->ttl;
        }
        nActive++;
    }

    uint32_t hash_new = XXH32_digest(state);

    if (hash != hash_new) {
        LogPrint(BCLog::SMSG, "Bucket hash updated from %u to %u.\n", hash, hash_new);

        hash = hash_new;
        timeChanged = GetAdjustedTime();
    }

    LogPrint(BCLog::SMSG, "Hashed %u messages, hash %u\n", nActive, hash_new);
    return;
};

size_t SecMsgBucket::CountActive()
{
    size_t nMessages = 0;

    int64_t now = GetAdjustedTime();
    for (auto it = setTokens.begin(); it != setTokens.end(); ++it) {
        if (it->timestamp + it->ttl * SMSGGetSecondsInDay() < now) {
            continue;
        }
        nMessages++;
    }

    return nMessages;
};

static int64_t GetAnchorAutoCommitInterval()
{
    return Params().NetworkIDString() == "regtest" ? 30 : ANCHOR_AUTO_COMMIT_INTERVAL;
}

static void MaybeAutoCommitAnchors(const int64_t now)
{
#ifdef ENABLE_WALLET
    if (!g_msgAnchor.IsLoaded() || g_msgAnchor.HasPreparedBatch()) {
        return;
    }

    const size_t pending_count = g_msgAnchor.PendingCount();
    if (pending_count == 0) {
        return;
    }

    const bool reached_size = pending_count >= ANCHOR_BATCH_MIN;
    const int64_t oldest_pending_time = g_msgAnchor.OldestPendingTime();
    const bool reached_time = oldest_pending_time != 0
        && oldest_pending_time + GetAnchorAutoCommitInterval() <= now;
    if (!reached_size && !reached_time) {
        return;
    }

    std::shared_ptr<CWallet> wallet = smsgModule.pwallet;
    if (!wallet) {
        LogPrint(BCLog::SMSG, "MsgAnchor: skipping auto-commit, no wallet is attached.\n");
        return;
    }
    if (wallet->IsLocked()) {
        LogPrint(BCLog::SMSG, "MsgAnchor: skipping auto-commit, wallet is locked.\n");
        return;
    }
    if (!smsgModule.m_node) {
        LogPrint(BCLog::SMSG, "MsgAnchor: skipping auto-commit, node context is unavailable.\n");
        return;
    }

    AnchorCommitResult commit_result;
    std::string error;
    if (!CommitPendingAnchorBatch(wallet.get(), *smsgModule.m_node,
                                  /* wait_callback */ false, commit_result, &error)) {
        LogPrintf("MsgAnchor auto-commit failed: %s\n",
                  error.empty() ? "unknown anchor auto-commit failure" : error);
        return;
    }

    LogPrintf("MsgAnchor auto-commit submitted tx %s for root %s with %u hashes.\n",
              commit_result.txid, commit_result.merkle_root.GetHex(),
              static_cast<unsigned int>(commit_result.count));
#else
    (void)now;
#endif
}

void ThreadSecureMsg(const CBlockIndex* pindex)
{
    // Bucket management thread

    uint32_t nLoop = 0;
    std::vector<std::pair<int64_t, NodeId> > vTimedOutLocks;
    while (fSecMsgEnabled) {
        nLoop++;
        int64_t now = GetAdjustedTime();

        if (LogAcceptCategory(BCLog::SMSG) && nLoop % SMSG_THREAD_LOG_GAP == 0) { // log every SMSG_THREAD_LOG_GAP instance, is useful source of timestamps
            LogPrintf("SecureMsgThread %d \n", now);
        }

        vTimedOutLocks.resize(0);
        int64_t cutoffTime = now - SMSG_RETENTION;
        {
            LOCK(smsgModule.cs_smsg);
            for (std::map<int64_t, SecMsgBucket>::iterator it(smsgModule.buckets.begin()); it != smsgModule.buckets.end(); ) {
                bool fErase = it->first < cutoffTime;

                if (!fErase
                    && it->first + it->second.nLeastTTL * SMSGGetSecondsInDay() < now) {
                    it->second.hashBucket();

                    // TODO: periodically prune files
                    if (it->second.nActive < 1) {
                        fErase = true;
                    }
                }

                if (fErase) {
                    LogPrint(BCLog::SMSG, "Removing bucket %d \n", it->first);

                    std::string fileName = std::to_string(it->first);

                    fs::path fullPath = GetDataDir() / "smsgstore" / (fileName + "_01.dat");
                    if (fs::exists(fullPath)) {
                        try { fs::remove(fullPath);
                        } catch (const fs::filesystem_error &ex) {
                            LogPrintf("Error removing bucket file %s.\n", ex.what());
                        }
                    } else {
                        LogPrintf("Path %s does not exist \n", fullPath.string());
                    }

                    // Look for a wl file, it stores incoming messages when wallet is locked
                    fullPath = GetDataDir() / "smsgstore" / (fileName + "_01_wl.dat");
                    if (fs::exists(fullPath)) {
                        try { fs::remove(fullPath);
                        } catch (const fs::filesystem_error &ex) {
                            LogPrintf("Error removing wallet locked file %s.\n", ex.what());
                        }
                    }

                    smsgModule.buckets.erase(it++);
                } else {
                    if (it->second.nLockCount > 0) { // Tick down nLockCount, to eventually expire if peer never sends data
                        it->second.nLockCount--;

                        if (it->second.nLockCount == 0) { // lock timed out
                            vTimedOutLocks.push_back(std::make_pair(it->first, it->second.nLockPeerId)); // g_connman->cs_vNodes

                            it->second.nLockPeerId = 0;
                        }
                    }

                    ++it;
                }
            }

            if (smsgModule.nLastProcessedPurged + SMSGGetSecondsInDay() < now) {
                if (smsgModule.BuildPurgedSets() != 0) {
                    LogPrintf("ThreadSecureMsg: BuildPurgedSets failed, keeping previous purge state.\n");
                }
            }
        } // cs_smsg

        for (std::vector<std::pair<int64_t, NodeId> >::iterator it(vTimedOutLocks.begin()); it != vTimedOutLocks.end(); it++) {
            NodeId nPeerId = it->second;
            uint32_t fExists = 0;

            LogPrint(BCLog::SMSG, "Lock on bucket %d for peer %d timed out.\n", it->first, nPeerId);

            // Look through the nodes for the peer that locked this bucket

            {
                LOCK(g_connman->cs_vNodes);
                for (auto *pnode : g_connman->vNodes) {
                    if (pnode->GetId() != nPeerId) {
                        continue;
                    }

                    fExists = 1; //found in g_connman->vNodes

                    LOCK(pnode->smsgData.cs_smsg_net);
                    int64_t ignoreUntil = GetTime() + SMSG_TIME_IGNORE;
                    pnode->smsgData.ignoreUntil = ignoreUntil;

                    // Alert peer that they are being ignored
                    std::vector<uint8_t> vchData;
                    vchData.resize(8);
                    memcpy(&vchData[0], &ignoreUntil, 8);
                    g_connman->PushMessage(pnode,
                        CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgIgnore", vchData));

                    LogPrint(BCLog::SMSG, "This node will ignore peer %d until %d.\n", nPeerId, ignoreUntil);
                    break;
                }
            } // g_connman->cs_vNodes

            LogPrint(BCLog::SMSG, "smsg-thread: ignoring - looked peer %d, status on search %u\n", nPeerId, fExists);
        }

        MaybeAutoCommitAnchors(now);

        try {
            boost::this_thread::sleep_for(boost::chrono::seconds(SMSG_THREAD_DELAY));
        } catch (const boost::thread_interrupted &) {
            break;
        }
    }
    return;
};

void ThreadSecureMsgPow(const CBlockIndex* pindex)
{
    // Proof of work thread

    int rv;
    std::vector<uint8_t> vchKey;
    SecMsgStored smsgStored;

    std::string sPrefix("qm");
    uint8_t chKey[30];

    while (fSecMsgEnabled) {
        // Sleep at end, then fSecMsgEnabled is tested on wake

        SecMsgDB dbOutbox;
        leveldb::Iterator *it;
        bool dbOpened = false;
        {
            LOCK(cs_smsgDB);
            dbOpened = dbOutbox.Open("cr+");
            if (dbOpened) {
                // fifo (smallest key first)
                it = dbOutbox.pdb->NewIterator(leveldb::ReadOptions());
            }
        }
        if (!dbOpened) {
            LogPrintf("%s: Failed to open outbox DB, sleeping before retry.\n", __func__);
            UninterruptibleSleep(std::chrono::milliseconds{2000});
            continue;
        }
        // Break up lock, SecureMsgSetHash will take long

        for (;;) {
            if (!fSecMsgEnabled) {
                break;
            }
            {
                LOCK(cs_smsgDB);
                if (!dbOutbox.NextSmesg(it, sPrefix, chKey, smsgStored))
                    break;
            }

            uint8_t *pHeader = &smsgStored.vchMessage[0];
            uint8_t *pPayload = &smsgStored.vchMessage[SMSG_HDR_LEN];
            SecureMessage *psmsg = (SecureMessage*) pHeader;

            const int64_t FUND_TXN_TIMEOUT = 3600 * 48;
            int64_t now = GetTime();

            if (psmsg->version[0] == 3) {
                uint256 txid;
                uint160 msgId;
                if (0 != smsgModule.HashMsg(*psmsg, pPayload, psmsg->nPayload - psmsg->GetPaidTailSize(), msgId)
                    || !GetFundingTxid(pPayload, psmsg->nPayload, txid)) {
                    LogPrintf("%s: Get msgID or Txn Hash failed.\n", __func__);
                    LOCK(cs_smsgDB);
                    dbOutbox.EraseSmesg(chKey);
                    continue;
                }

                // Zero txid: crashed after qm* write but before funding tx was committed — recover.
                if (txid.IsNull()) {
                    txid = smsgModule.FindFundingTx(msgId);
                    if (txid.IsNull()) {
                        // No wallet tx found for this msgId — re-fund the message.
                        SecureMessage smsgFund;
                        memcpy(smsgFund.data(), pHeader, SMSG_HDR_LEN);
                        smsgFund.pPayload = new uint8_t[psmsg->nPayload];
                        memcpy(smsgFund.pPayload, pPayload, psmsg->nPayload);
                        smsgFund.nPayload = psmsg->nPayload;

                        std::string sFundErr;
                        if (smsgModule.FundMsg(smsgFund, sFundErr, false, nullptr, &msgId) != SMSG_NO_ERROR) {
                            LogPrintf("%s: recovery re-fund failed for msgId %s: %s\n", __func__, msgId.ToString(), sFundErr);
                            if (now > psmsg->timestamp + FUND_TXN_TIMEOUT) {
                                LogPrintf("%s: Timeout, dropping unfunded msgId %s.\n", __func__, msgId.ToString());
                                LOCK(cs_smsgDB);
                                dbOutbox.EraseSmesg(chKey);
                            }
                            continue;
                        }
                        GetFundingTxid(smsgFund.pPayload, smsgFund.nPayload, txid);
                        // Copy full funded payload (txid + blinding key for blinded) into stored record.
                        memcpy(pPayload, smsgFund.pPayload, psmsg->nPayload);
                    } else {
                        // Wallet tx exists — stamp recovered txid into the stored record tail.
                        memcpy(pPayload + psmsg->nPayload - 32, txid.begin(), 32);
                    }
                    {
                        LOCK(cs_smsgDB);
                        dbOutbox.WriteSmesg(chKey, smsgStored);
                    }
                    LogPrintf("%s: recovery: stamped funding txid %s for msgId %s.\n", __func__, txid.ToString(), msgId.ToString());
                    continue;
                }

                CTransactionRef txOut;
                uint256 hashBlock;
                {
                    LOCK(cs_main);
                    txOut = GetTransaction(nullptr, nullptr, txid, Params().GetConsensus(), hashBlock);
                }

                int blockDepth = -1;
                if (!hashBlock.IsNull()) {
                    CBlockIndex *pindex = LookupBlockIndex(hashBlock);
                    if (pindex && ::ChainActive().Contains(pindex)) {
                        blockDepth = ::ChainActive().Height() - pindex->nHeight + 1;
                    }
                }

                if (blockDepth > 0) {
                    LogPrintf("Found txn %s at depth %d\n", txid.ToString(), blockDepth);
                } else {
                    // Failure
                    if (now > psmsg->timestamp + FUND_TXN_TIMEOUT) {
                        LogPrintf("%s: Funding txn timeout, dropping message %s\n", __func__, msgId.ToString());
                        LOCK(cs_smsgDB);
                        dbOutbox.EraseSmesg(chKey);
                    }
                    continue;
                }
            } else {
                // Do proof of work
                rv = smsgModule.SetHash(pHeader, pPayload, psmsg->nPayload);
                if (rv == SMSG_SHUTDOWN_DETECTED) {
                    break; // leave message in db, if terminated due to shutdown
                }
                if (rv != 0) {
                    LogPrintf("SecMsgPow: Could not get proof of work hash, leaving in queue for retry.\n");
                    continue;
                }
            }


            // Add to message store before removing from queue; on failure leave in queue for retry
            {
                LOCK(smsgModule.cs_smsg);
                if (smsgModule.Store(pHeader, pPayload, psmsg->nPayload, true) != 0) {
                    LogPrintf("SecMsgPow: Could not place message in buckets, leaving in queue.\n");
                    continue;
                }
            }

            // Remove message from queue only after durable store
            {
                LOCK(cs_smsgDB);
                dbOutbox.EraseSmesg(chKey);
            }

            // Test if message was sent to self
            {
                LOCK(smsgModule.cs_smsg);
                smsgModule.ScanMessage(pHeader, pPayload, psmsg->nPayload, true);
            }
        }

        delete it;

        // Shutdown thread waits 5 seconds, this should be less
        UninterruptibleSleep(std::chrono::milliseconds{2000});
    };
    return;
};

void AddOptions()
{
    gArgs.AddArg("-smsg", "Enable secure messaging. (default: true)", ArgsManager::ALLOW_ANY, OptionsCategory::SMSG);
    gArgs.AddArg("-smsgscanchain", "Scan the block chain for public key addresses on startup. (default: false)", ArgsManager::ALLOW_ANY, OptionsCategory::SMSG);
    gArgs.AddArg("-smsgscanincoming", "Scan incoming blocks for public key addresses. (default: false)", ArgsManager::ALLOW_ANY, OptionsCategory::SMSG);
    gArgs.AddArg("-smsgnotify=<cmd>", "Execute command when a message is received. (%s in cmd is replaced by receiving address)", ArgsManager::ALLOW_ANY, OptionsCategory::SMSG);
    gArgs.AddArg("-smsgsaddnewkeys", "Scan for incoming messages on new wallet keys. (default: false)", ArgsManager::ALLOW_ANY, OptionsCategory::SMSG);

    return;
};

const char *GetString(size_t errorCode)
{
    switch(errorCode)
    {
        case SMSG_UNKNOWN_VERSION:                      return "Unknown version";
        case SMSG_INVALID_ADDRESS:                      return "Invalid address";
        case SMSG_INVALID_ADDRESS_FROM:                 return "Invalid address from";
        case SMSG_INVALID_ADDRESS_TO:                   return "Invalid address to";
        case SMSG_INVALID_PUBKEY:                       return "Invalid public key";
        case SMSG_PUBKEY_MISMATCH:                      return "Public key does not match address";
        case SMSG_PUBKEY_EXISTS:                        return "Public key exists in database";
        case SMSG_PUBKEY_NOT_EXISTS:                    return "Public key not in database";
        case SMSG_KEY_EXISTS:                           return "Key exists in database";
        case SMSG_KEY_NOT_EXISTS:                       return "Key not in database";
        case SMSG_UNKNOWN_KEY:                          return "Unknown key";
        case SMSG_UNKNOWN_KEY_FROM:                     return "Unknown private key for from address";
        case SMSG_ALLOCATE_FAILED:                      return "Allocate failed";
        case SMSG_MAC_MISMATCH:                         return "MAC mismatch";
        case SMSG_WALLET_UNSET:                         return "Wallet unset";
        case SMSG_WALLET_NO_PUBKEY:                     return "Pubkey not found in wallet";
        case SMSG_WALLET_NO_KEY:                        return "Key not found in wallet";
        case SMSG_WALLET_LOCKED:                        return "Wallet is locked";
        case SMSG_DISABLED:                             return "SMSG is disabled";
        case SMSG_UNKNOWN_MESSAGE:                      return "Unknown Message";
        case SMSG_PAYLOAD_OVER_SIZE:                    return "Payload too large";
        case SMSG_TIME_IN_FUTURE:                       return "Timestamp is in the future";
        case SMSG_TIME_EXPIRED:                         return "Time to live expired";
        case SMSG_INVALID_HASH:                         return "Invalid hash";
        case SMSG_CHECKSUM_MISMATCH:                    return "Checksum mismatch";
        case SMSG_SHUTDOWN_DETECTED:                    return "Shutdown detected";
        case SMSG_MESSAGE_TOO_LONG:                     return "Message is too long";
        case SMSG_COMPRESS_FAILED:                      return "Compression failed";
        case SMSG_ENCRYPT_FAILED:                       return "Encryption failed";
        case SMSG_FUND_FAILED:                          return "Fund message failed";
        default:
            return "Unknown error";
    };
    return "No Error";
};

static void NotifyUnload(CSMSG *ps)
{
    LogPrintf("SMSG NotifyUnload\n");
    ps->Disable();
};

int CSMSG::BuildBucketSet()
{
    /*
        Build the bucket set by scanning the files in the smsgstore dir.
        buckets should be empty
    */

    LogPrint(BCLog::SMSG, "%s\n", __func__);

    int64_t  now            = GetAdjustedTime();
    uint32_t nFiles         = 0;
    uint32_t nMessages      = 0;

    fs::path pathSmsgDir = GetDataDir() / "smsgstore";
    fs::directory_iterator itend;

    if (!fs::exists(pathSmsgDir)
        || !fs::is_directory(pathSmsgDir)) {
        LogPrintf("Message store directory does not exist.\n");
        return SMSG_NO_ERROR; // not an error
    }

    for (fs::directory_iterator itd(pathSmsgDir); itd != itend; ++itd) {
        if (!fs::is_regular_file(itd->status())) {
            continue;
        }

        std::string fileType = itd->path().extension().string();

        if (fileType.compare(".dat") != 0) {
            continue;
        }

        nFiles++;
        std::string fileName = itd->path().filename().string();

        LogPrint(BCLog::SMSG, "Processing file: %s.\n", fileName);

        // TODO files must be split if > 2GB
        // time_noFile.dat
        size_t sep = fileName.find_first_of("_");
        if (sep == std::string::npos) {
            continue;
        }

        std::string stime = fileName.substr(0, sep);
        int64_t fileTime;
        if (!ParseInt64(stime, &fileTime)) {
            LogPrintf("%s: ParseInt64 failed %s.\n", __func__, stime);
            continue;
        }

        if (fileTime < now - SMSG_RETENTION) {
            LogPrintf("Dropping file %s, expired.\n", fileName);
            try {
                fs::remove(itd->path());
            } catch (const fs::filesystem_error &ex) {
                LogPrintf("Error removing bucket file %s, %s.\n", fileName, ex.what());
            }
            continue;
        }

        if (omega::endsWith(fileName, "_wl.dat")) {
            LogPrint(BCLog::SMSG, "Skipping wallet locked file: %s.\n", fileName);
            continue;
        }

        size_t nTokenSetSize = 0;
        SecureMessage smsg;
        {
            LOCK(cs_smsg);

            SecMsgBucket &bucket = buckets[fileTime];
            std::set<SecMsgToken> &tokenSet = bucket.setTokens;

            FILE *fp;
            if (!(fp = fopen(itd->path().string().c_str(), "rb"))) {
                LogPrintf("Error opening file: %s\n", strerror(errno));
                continue;
            }

            for (;;) {
                long int ofs = ftell(fp);
                SecMsgToken token;
                token.offset = ofs;
                errno = 0;
                if (fread(smsg.data(), sizeof(uint8_t), SMSG_HDR_LEN, fp) != (size_t)SMSG_HDR_LEN) {
                    if (errno != 0) {
                        LogPrintf("fread header failed: %s\n", strerror(errno));
                    } else {
                        //LogPrintf("End of file.\n");
                    }
                    break;
                }
                token.timestamp = smsg.timestamp;

                uint32_t nDaysToLive = smsg.version[0] == 0 && smsg.version[1] == 0 ? 0  // Purged message header
                    : smsg.version[0] < 3 ? 2 : smsg.nonce[0];

                token.ttl = nDaysToLive;
                if (nDaysToLive > 0 && (bucket.nLeastTTL == 0 || nDaysToLive < bucket.nLeastTTL)) {
                    bucket.nLeastTTL = nDaysToLive;
                }

                if (smsg.nPayload < 8) {
                    if (fseek(fp, smsg.nPayload, SEEK_CUR) != 0) {
                        LogPrintf("fseek failed: %s.\n", strerror(errno));
                        break;
                    }
                    continue;
                }

                if (fread(token.sample, sizeof(uint8_t), 8, fp) != 8) {
                    LogPrintf("fread failed: %s\n", strerror(errno));
                    break;
                }

                if (fseek(fp, smsg.nPayload-8, SEEK_CUR) != 0) {
                    LogPrintf("fseek failed: %s.\n", strerror(errno));
                    break;
                }

                tokenSet.insert(token);
            }

            fclose(fp);
            buckets[fileTime].hashBucket();
            nTokenSetSize = tokenSet.size();
        } // cs_smsg

        nMessages += nTokenSetSize;
        LogPrint(BCLog::SMSG, "Bucket %d contains %u messages.\n", fileTime, nTokenSetSize);
    }

    LogPrintf("Processed %u files, loaded %u buckets containing %u messages.\n", nFiles, buckets.size(), nMessages);
    return SMSG_NO_ERROR;
};

int CSMSG::BuildPurgedSets()
{
    LogPrint(BCLog::SMSG, "%s\n", __func__);
    LOCK2(cs_smsg, cs_smsgDB);

    SecMsgDB db;
    if (!db.Open("cr+")) {
        return SMSG_GENERAL_ERROR;
    }

    std::set<SecMsgPurged> tmpPurged;
    std::set<int64_t> tmpPurgedTimestamps;
    int64_t now = GetTime();
    size_t nPurged = 0;
    std::string sPrefix("pm");
    uint8_t chKey[30];
    SecMsgPurged purged;
    leveldb::Iterator *it = db.pdb->NewIterator(leveldb::ReadOptions());
    while (db.NextPurged(it, sPrefix, chKey, purged)) {
        if (purged.timepurged + 31 * SMSGGetSecondsInDay() < now) {
            db.ErasePurged(chKey);
            continue;
        }
        tmpPurged.insert(purged);
        tmpPurgedTimestamps.insert(purged.timestamp);
        nPurged++;
    }
    delete it;

    setPurged = std::move(tmpPurged);
    setPurgedTimestamps = std::move(tmpPurgedTimestamps);

    LogPrint(BCLog::SMSG, "Loaded %u purged tokens from database.\n", nPurged);

    nLastProcessedPurged = now;

    return SMSG_NO_ERROR;
};

/*
SecureMsgAddWalletAddresses
    Enumerates the AddressBook, filters out anon outputs and checks the "real addresses"
    Adds these to the vector addresses to be used for decryption

    Returns 0 on success
*/

int CSMSG::AddWalletAddresses()
{
    LogPrint(BCLog::SMSG, "%s\n", __func__);

#ifdef ENABLE_WALLET
    if (!pwallet) {
        return errorN(SMSG_WALLET_UNSET, "No wallet.");
    }

    if (!gArgs.GetBoolArg("-smsgsaddnewkeys", false)) {
        LogPrint(BCLog::SMSG, "%s smsgsaddnewkeys option is disabled.\n", __func__);
        return SMSG_GENERAL_ERROR;
    }

    uint32_t nAdded = 0;

    LOCK(pwallet->cs_wallet);
    for (const auto &entry : pwallet->mapAddressBook) { // PAIRTYPE(CTxDestination, CAddressBookData)
        if (!pwallet->IsMine(entry.first)) {
            continue;
        }

        // TODO: skip addresses for stealth transactions
        const PKHash *pkHash = std::get_if<PKHash>(&entry.first);
        if (!pkHash) {
            continue;
        }
        CKeyID keyID(*pkHash);

        if (m_cachedOutboxAddr.IsNull())
            m_cachedOutboxAddr = keyID;

        bool fExists = 0;
        for (std::vector<SecMsgAddress>::iterator it = addresses.begin(); it != addresses.end(); ++it) {
            if (keyID != it->address) {
                continue;
            }
            fExists = 1;
            break;
        }

        if (fExists) {
            continue;
        }

        bool recvEnabled    = 1;
        bool recvAnon       = 1;

        addresses.push_back(SecMsgAddress(keyID, recvEnabled, recvAnon));
        nAdded++;
    }

    LogPrint(BCLog::SMSG, "Added %u addresses to whitelist.\n", nAdded);
#endif
    return SMSG_NO_ERROR;
};

int CSMSG::LoadKeyStore()
{
    LOCK(cs_smsgDB);

    SecMsgDB db;
    if (!db.Open("cr+")) {
        return SMSG_GENERAL_ERROR;
    }

    size_t nKeys = 0;
    std::string sPrefix("sk");
    CKeyID idk;
    SecMsgKey key;
    leveldb::Iterator *it = db.pdb->NewIterator(leveldb::ReadOptions());
    while (db.NextPrivKey(it, sPrefix, idk, key)) {
        if (!(key.nFlags & (SMK_RECEIVE_ON | SMK_CONTACT_ONLY))) {
            continue;
        }
        keyStore.AddKey(idk, key);
        nKeys++;
    }
    delete it;

    LogPrint(BCLog::SMSG, "Loaded %u keys from database.\n", nKeys);
    return SMSG_NO_ERROR;
};

int CSMSG::ReadIni()
{
    if (!fSecMsgEnabled) {
        return SMSG_DISABLED;
    }

    LogPrint(BCLog::SMSG, "%s\n", __func__);

    fs::path fullpath = GetDataDir() / "smsg.ini";

    FILE *fp;
    errno = 0;
    if (!(fp = fopen(fullpath.string().c_str(), "r"))) {
        return errorN(SMSG_GENERAL_ERROR, "%s: Error opening file: %s", __func__, strerror(errno));
    }

    char cLine[512];
    char *pName, *pValue;

    char cAddress[64];
    int addrRecv, addrRecvAnon;

    while (fgets(cLine, 512, fp))  {
        cLine[strcspn(cLine, "\n")] = '\0';
        cLine[strcspn(cLine, "\r")] = '\0';
        cLine[511] = '\0'; // for safety

        // Check that line contains a name value pair and is not a comment, or section header
        if (cLine[0] == '#' || cLine[0] == '[' || strcspn(cLine, "=") < 1) {
            continue;
        }

        if (!(pName = strtok(cLine, "="))
            || !(pValue = strtok(nullptr, "="))) {
            continue;
        }

        if (strcmp(pName, "newAddressRecv") == 0) {
            options.fNewAddressRecv = (strcmp(pValue, "true") == 0) ? true : false;
        } else
        if (strcmp(pName, "newAddressAnon") == 0) {
            options.fNewAddressAnon = (strcmp(pValue, "true") == 0) ? true : false;
        } else
        if (strcmp(pName, "scanIncoming") == 0) {
            options.fScanIncoming = (strcmp(pValue, "true") == 0) ? true : false;
        } else
        if (strcmp(pName, "key") == 0) {
            int rv = sscanf(pValue, "%63[^|]|%d|%d", cAddress, &addrRecv, &addrRecvAnon);
            if (rv == 3) {
                CTxDestination dest = DecodeDestination(std::string(cAddress));
                const PKHash *pkHash = std::get_if<PKHash>(&dest);
                CKeyID k = pkHash ? CKeyID(*pkHash) : CKeyID();

                if (k.IsNull()) {
                    LogPrintf("Could not parse key line %s, rv %d.\n", pValue, rv);
                } else {
                    addresses.push_back(SecMsgAddress(k, addrRecv, addrRecvAnon));
                }
            } else {
                LogPrintf("Could not parse key line %s, rv %d.\n", pValue, rv);
            }
        } else {
            LogPrintf("Unknown setting name: '%s'.\n", pName);
        }
    }

    fclose(fp);
    LogPrintf("Loaded %u addresses.\n", addresses.size());

    LoadTopicSubs();

    return SMSG_NO_ERROR;
};

int CSMSG::WriteIni()
{
    if (!fSecMsgEnabled) {
        return SMSG_DISABLED;
    }

    LogPrint(BCLog::SMSG, "%s\n", __func__);

    std::vector<SecMsgAddress> addrSnapshot;
    SecMsgOptions optSnapshot;
    {
        LOCK(cs_smsg);
        addrSnapshot = addresses;
        optSnapshot = options;
    }

    fs::path fullpath = GetDataDir() / "smsg.ini~";

    FILE *fp;
    errno = 0;
    if (!(fp = fopen(fullpath.string().c_str(), "w"))) {
        return errorN(SMSG_GENERAL_ERROR, "%s: Error opening file: %s", __func__, strerror(errno));
    }

    if (fwrite("[Options]\n", sizeof(char), 10, fp) != 10) {
        LogPrintf("fwrite error: %s\n", strerror(errno));
        fclose(fp);
        return SMSG_GENERAL_ERROR;
    }

    if (fprintf(fp, "newAddressRecv=%s\n", optSnapshot.fNewAddressRecv ? "true" : "false") < 0
        || fprintf(fp, "newAddressAnon=%s\n", optSnapshot.fNewAddressAnon ? "true" : "false") < 0
        || fprintf(fp, "scanIncoming=%s\n", optSnapshot.fScanIncoming ? "true" : "false") < 0) {
        LogPrintf("fprintf error: %s\n", strerror(errno));
        fclose(fp);
        return SMSG_GENERAL_ERROR;
    }

    if (fwrite("\n[Keys]\n", sizeof(char), 8, fp) != 8) {
        LogPrintf("fwrite error: %s\n", strerror(errno));
        fclose(fp);
        return SMSG_GENERAL_ERROR;
    }

    for (const auto &addr : addrSnapshot) {
        if (addr.address.IsNull()) {
            LogPrintf("%s: Error saving address - invalid.\n", __func__);
            continue;
        }
        std::string sAddr = EncodeDestination(PKHash(addr.address));
        if (fprintf(fp, "key=%s|%d|%d\n", sAddr.c_str(), addr.fReceiveEnabled, addr.fReceiveAnon) < 0) {
            LogPrintf("fprintf error: %s\n", strerror(errno));
            fclose(fp);
            return SMSG_GENERAL_ERROR;
        }
    }

    if (fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
        LogPrintf("%s: flush/sync error: %s\n", __func__, strerror(errno));
        fclose(fp);
        return SMSG_GENERAL_ERROR;
    }
    fclose(fp);

    try {
        fs::path finalpath = GetDataDir() / "smsg.ini";
        fs::rename(fullpath, finalpath);
    } catch (const fs::filesystem_error &ex) {
        LogPrintf("Error renaming file %s, %s.\n", fullpath.string(), ex.what());
        return SMSG_GENERAL_ERROR;
    }
    return SMSG_NO_ERROR;
};

bool CSMSG::Start(std::shared_ptr<CWallet> pwalletIn, bool fDontStart, bool fScanChain)
{
    if (fDontStart) {
        LogPrintf("Secure messaging not started.\n");
        return false;
    }

    LogPrintf("SMSG debug: Start() called, wallet=%s\n", pwalletIn ? pwalletIn->GetName() : "null");

    try {

    if (pwallet) {
        error("%s: pwallet is already set.", __func__);
        ResetSmsgRuntimeState(*this, /*stop_threads=*/false);
        return false;
    }
    pwallet = pwalletIn;
    LogPrintf("SMSG debug: pwallet assigned.\n");

#ifdef ENABLE_WALLET
    if (pwallet) {
        LogPrintf("SMSG debug: attaching wallet notification handlers.\n");
        m_handler_unload = interfaces::MakeHandler(pwallet->NotifyUnload.connect(boost::bind(&NotifyUnload, this)));
        // When the wallet is unlocked, scan any messages that arrived while locked.
        m_handler_status = interfaces::MakeHandler(pwallet->NotifyStatusChanged.connect(
            [this](CWallet *w) {
                if (fSecMsgEnabled && w && !w->IsLocked()) {
                    try {
                        WalletUnlocked();
                    } catch (const std::exception &e) {
                        LogPrintf("%s: WalletUnlocked threw: %s\n", __func__, e.what());
                    } catch (...) {
                        LogPrintf("%s: WalletUnlocked threw unknown exception.\n", __func__);
                    }
                }
            }));
        LogPrintf("SMSG debug: wallet handlers attached.\n");
    }
#endif

    LogPrintf("SMSG debug: calling ReadIni().\n");
    if (ReadIni() != 0) {
        LogPrintf("Failed to read smsg.ini\n");
    }
    LogPrintf("SMSG debug: ReadIni() done, addresses.size()=%zu.\n", addresses.size());

    if (addresses.size() < 1) {
        LogPrintf("No address keys loaded.\n");
        LogPrintf("SMSG debug: calling AddWalletAddresses().\n");
        if (AddWalletAddresses() != 0) {
            LogPrintf("Failed to load addresses from wallet.\n");
        } else {
            LogPrintf("Loaded addresses from wallet.\n");
        }
        LogPrintf("SMSG debug: AddWalletAddresses() done, addresses.size()=%zu.\n", addresses.size());
    } else {
        LogPrintf("Loaded addresses from SMSG.ini\n");
    }

    LogPrintf("SMSG debug: calling LoadKeyStore().\n");
    if (LoadKeyStore() != 0) {
        error("%s: LoadKeyStore failed.", __func__);
        ResetSmsgRuntimeState(*this, /*stop_threads=*/false);
        return false;
    }
    LogPrintf("SMSG debug: LoadKeyStore() done.\n");

    // Import Trollbox keypair — public chat, all nodes share this key
    LogPrintf("SMSG debug: importing Trollbox keypair.\n");
    {
        std::vector<uint8_t> vchPrivKey = ParseHex(TROLLBOX_PRIVKEY_HEX);
        CKey trollboxKey;
        trollboxKey.Set(vchPrivKey.begin(), vchPrivKey.end(), true);
        if (trollboxKey.IsValid()) {
            trollboxAddress = trollboxKey.GetPubKey().GetID();
            if (!keyStore.HaveKey(trollboxAddress)) {
                SecMsgKey smsgKey;
                smsgKey.key = trollboxKey;
                smsgKey.sLabel = "Trollbox";
                smsgKey.nFlags = SMK_RECEIVE_ON | SMK_RECEIVE_ANON;
                keyStore.AddKey(trollboxAddress, smsgKey);
                LogPrintf("SMSG debug: writing Trollbox key to DB.\n");
                LOCK(cs_smsgDB);
                SecMsgDB db;
                if (db.Open("cr+")) {
                    db.WriteKey(trollboxAddress, smsgKey);
                }
                LogPrintf("SMSG debug: Trollbox key written to DB.\n");
            }
            LogPrintf("Trollbox address: %s\n", EncodeDestination(PKHash(trollboxAddress)));
        } else {
            LogPrintf("Warning: Trollbox private key is invalid.\n");
        }
    }
    LogPrintf("SMSG debug: Trollbox import done.\n");

    LogPrintf("SMSG debug: creating secp256k1 context.\n");
    if (secp256k1_context_smsg) {
        error("%s: secp256k1_context_smsg already exists.", __func__);
        ResetSmsgRuntimeState(*this, /*stop_threads=*/false);
        return false;
    }

    if (!(secp256k1_context_smsg = secp256k1_context_create(SECP256K1_CONTEXT_SIGN))) {
        error("%s: secp256k1_context_create failed.", __func__);
        ResetSmsgRuntimeState(*this, /*stop_threads=*/false);
        return false;
    }

    {
        // Pass in a random blinding seed to the secp256k1 context.
        std::vector<uint8_t, secure_allocator<uint8_t>> vseed(32);
        GetRandBytes(vseed.data(), 32);
        bool ret = secp256k1_context_randomize(secp256k1_context_smsg, vseed.data());
        assert(ret);
    }
    LogPrintf("SMSG debug: secp256k1 context created.\n");

    if (fScanChain) {
        LogPrintf("SMSG debug: calling ScanBlockChain().\n");
        ScanBlockChain();
        LogPrintf("SMSG debug: ScanBlockChain() done.\n");
    }

    LogPrintf("SMSG debug: calling BuildBucketSet().\n");
    if (BuildBucketSet() != 0) {
        error("%s: Could not load bucket sets, secure messaging disabled.", __func__);
        ResetSmsgRuntimeState(*this, /*stop_threads=*/false);
        return false;
    }
    LogPrintf("SMSG debug: BuildBucketSet() done.\n");

    LogPrintf("SMSG debug: calling BuildPurgedSets().\n");
    if (BuildPurgedSets() != 0) {
        error("%s: Could not load purged sets, secure messaging disabled.", __func__);
        ResetSmsgRuntimeState(*this, /*stop_threads=*/false);
        return false;
    }
    LogPrintf("SMSG debug: BuildPurgedSets() done.\n");

    LogPrintf("SMSG debug: loading anchor state.\n");
    {
        std::string anchor_error;
        if (!g_msgAnchor.Load(GetDataDir() / "smsganchor.json", &anchor_error)) {
            LogPrintf("smsg: anchor state unavailable, anchoring disabled for this session: %s\n",
                      anchor_error.empty() ? "load failed" : anchor_error);
        }
    }
    LogPrintf("SMSG debug: anchor state loaded.\n");

    if (g_connman) {
        g_connman->SetLocalServices(ServiceFlags(g_connman->GetLocalServices() | NODE_SMSG));
        LogPrintf("SMSG debug: NODE_SMSG service flag set.\n");
    } else {
        LogPrintf("SMSG debug: g_connman is null, NODE_SMSG service flag not set.\n");
    }

    const CBlockIndex* pindex = ::ChainActive().Tip();
    LogPrintf("SMSG debug: launching smsg threads, chain tip=%s.\n", pindex ? pindex->GetBlockHash().ToString() : "null");

    fSecMsgEnabled = true;
    threadGroupSmsg.create_thread([pindex]() { TraceThread("smsg", [pindex]() { ThreadSecureMsg(pindex); }); });
    threadGroupSmsg.create_thread([pindex]() { TraceThread("smsg-pow", [pindex]() { ThreadSecureMsgPow(pindex); }); });

    LogPrintf("SMSG debug: Start() completed successfully.\n");
    return true;

    } catch (const std::exception &e) {
        LogPrintf("SMSG debug: EXCEPTION in Start(): %s\n", e.what());
        ResetSmsgRuntimeState(*this, /*stop_threads=*/false);
        return false;
    } catch (...) {
        LogPrintf("SMSG debug: UNKNOWN EXCEPTION in Start()\n");
        ResetSmsgRuntimeState(*this, /*stop_threads=*/false);
        return false;
    }
};

bool CSMSG::StartDelayed(std::shared_ptr<CWallet> pwalletIn, bool fDontStart, bool fScanChain, int nDelaySeconds)
{
    if (fDontStart) {
        LogPrintf("Secure messaging not started.\n");
        return false;
    }
    if (nDelaySeconds <= 0) {
        return Start(pwalletIn, false, fScanChain);
    }
    LogPrintf("Secure messaging will start in %d seconds.\n", nDelaySeconds);
    threadGroupSmsg.create_thread([this, pwalletIn, fScanChain, nDelaySeconds]() {
        try {
            boost::this_thread::sleep_for(boost::chrono::seconds(nDelaySeconds));
        } catch (const boost::thread_interrupted &) {
            LogPrintf("Secure messaging startup cancelled.\n");
            return;
        }
        Start(pwalletIn, false, fScanChain);
    });
    return true;
};

bool CSMSG::StartOnUnlock(std::shared_ptr<CWallet> pwalletIn, bool fScanChain, int nDelaySeconds)
{
    if (!pwalletIn) {
        LogPrintf("SMSG StartOnUnlock: no wallet, cannot wait for unlock.\n");
        return false;
    }

    LogPrintf("Secure messaging will start %d seconds after wallet is unlocked.\n", nDelaySeconds);

    m_handler_unlock_start = interfaces::MakeHandler(pwalletIn->NotifyStatusChanged.connect(
        [this, pwalletIn, fScanChain, nDelaySeconds](CWallet *w) {
            if (fSecMsgEnabled || !w || w->IsLocked())
                return;
            LogPrintf("Wallet unlocked — scheduling secure messaging start in %d seconds.\n", nDelaySeconds);
            StartDelayed(pwalletIn, false, fScanChain, nDelaySeconds);
        }));

    return true;
};

bool CSMSG::Shutdown()
{
    // Clean up unlock listener even if SMSG never fully started
    if (m_handler_unlock_start) {
        m_handler_unlock_start->disconnect();
        m_handler_unlock_start.reset();
    }

    const bool was_enabled = fSecMsgEnabled;
    const bool had_partial_state = pwallet || secp256k1_context_smsg || smsgDB ||
                                   m_handler_unload || m_handler_status;

    if (was_enabled) {
        LogPrintf("Stopping secure messaging.\n");

        if (WriteIni() != 0) {
            LogPrintf("Failed to save smsg.ini\n");
        }
    }

    ResetSmsgRuntimeState(*this, /*stop_threads=*/true);
    return was_enabled || had_partial_state;
};

bool CSMSG::Enable(std::shared_ptr<CWallet> pwallet)
{
    // Start secure messaging at runtime
    if (fSecMsgEnabled) {
        LogPrintf("SecureMsgEnable: secure messaging is already enabled.\n");
        return false;
    }

    {
        LOCK(cs_smsg);
        addresses.clear(); // should be empty already
        buckets.clear();   // should be empty already
    } // cs_smsg released — Start() acquires it internally via BuildBucketSet()

    if (!Start(pwallet, false, false)) {
        return error("%s: SecureMsgStart failed.\n", __func__);
    }

    if (pwallet) {
        LoadWallet(pwallet);
    }

    // Ping all connected peers to initiate the SMSG handshake.
    // Filter by peer's advertised services (nServices), not by our own
    // stale per-connection local services which predate this enable call.
    {
        LOCK(g_connman->cs_vNodes);
        for(auto *pnode : g_connman->vNodes) {
            if (!(pnode->nServices & NODE_SMSG)) {
                continue;
            }
            g_connman->PushMessage(pnode,
                CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgPing"));
            g_connman->PushMessage(pnode,
                CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgPong")); // Send pong as have missed initial ping sent by peer when it connected
        };
    } // g_connman->cs_vNodes

    LogPrintf("Secure messaging enabled.\n");
    return true;
};

bool CSMSG::Disable()
{
    // Stop secure messaging at runtime
    if (!fSecMsgEnabled) {
        return error("%s: Secure messaging is already disabled.", __func__);
    }

    // Shutdown() interrupts and joins worker threads; must not hold cs_smsg
    // here as workers acquire cs_smsg and join_all() would deadlock.
    if (!Shutdown()) {
        return error("%s: SecureMsgShutdown failed.\n", __func__);
    }

    {
        LOCK(cs_smsg);
        std::map<int64_t, SecMsgBucket>::iterator it;
        for (it = buckets.begin(); it != buckets.end(); ++it) {
            it->second.setTokens.clear();
        }
        buckets.clear();
        addresses.clear();
        m_cachedOutboxAddr = CKeyID();
    } // cs_smsg

    // Tell each smsg enabled peer that this node is disabling
    {
        LOCK(g_connman->cs_vNodes);
        for (auto *pnode : g_connman->vNodes) {
            if (!pnode->smsgData.fEnabled) {
                continue;
            }
            LOCK(pnode->smsgData.cs_smsg_net);
            g_connman->PushMessage(pnode,
                CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgDisabled"));
            pnode->smsgData.fEnabled = false;
        }
    } // g_connman->cs_vNodes

    LogPrintf("Secure messaging disabled.\n");
    return true;
};

bool CSMSG::UnloadAllWallets()
{
#ifdef ENABLE_WALLET
    for (auto it = m_wallet_unload_handlers.begin(); it != m_wallet_unload_handlers.end(); ++it) {
        it->second->disconnect();
    }
    m_wallet_unload_handlers.clear();
    pactive_wallet.reset();
    m_vpwallets.clear();
#endif
    return true;
};

bool CSMSG::LoadWallet(std::shared_ptr<CWallet> pwallet_in)
{
#ifdef ENABLE_WALLET
    std::vector<std::shared_ptr<CWallet>>::iterator i = std::find(m_vpwallets.begin(), m_vpwallets.end(), pwallet_in);
    if (i != m_vpwallets.end()) return true;
    m_wallet_unload_handlers[pwallet_in.get()] = interfaces::MakeHandler(pwallet_in->NotifyUnload.connect(std::bind(&NotifyUnload, this)));
    m_vpwallets.push_back(pwallet_in);
#endif
    return true;
};

bool CSMSG::WalletUnloaded(CWallet *pwallet_removed)
{
    LOCK(cs_smsg);
    bool removed = false;
#ifdef ENABLE_WALLET
    if (pwallet_removed && pactive_wallet.get() == pwallet_removed) {
        SetActiveWallet(nullptr);
    }
    for (size_t i = 0; i < m_vpwallets.size(); ++i) {
        if (m_vpwallets[i].get() != pwallet_removed) {
            continue;
        }
        m_vpwallets.erase(m_vpwallets.begin() + i);
        removed = true;
        break;
    }
    auto it = m_wallet_unload_handlers.find(pwallet_removed);
    if (it != m_wallet_unload_handlers.end()) {
        it->second->disconnect();
        m_wallet_unload_handlers.erase(it);
    }
#endif
    return removed;
};

bool CSMSG::SetActiveWallet(std::shared_ptr<CWallet> pwallet_in)
{
#ifdef ENABLE_WALLET
    LOCK(cs_smsg);
    pactive_wallet.reset();

    if (pwallet_in) {
        pactive_wallet = pwallet_in;
        LoadWallet(pwallet_in);
        LogPrintf("Secure messaging using active wallet %s.\n", pactive_wallet->GetName());
    } else {
        LogPrintf("Secure messaging unset active wallet.\n");
    }
    return true;
#endif
    return false;
};

std::string CSMSG::GetWalletName()
{
#ifdef ENABLE_WALLET
    return pactive_wallet ? pactive_wallet->GetName() : "Not set.";
#endif
    return "Wallet Disabled.";
};

std::string CSMSG::LookupLabel(PKHash &hash)
{
#ifdef ENABLE_WALLET
    // Check pwallet first (it may not be in m_vpwallets if LoadWallet wasn't called)
    if (pwallet) {
        LOCK(pwallet->cs_wallet);
        auto mi(pwallet->mapAddressBook.find(hash));
        if (mi != pwallet->mapAddressBook.end()) {
            return mi->second.name;
        }
    }
    for (const auto &pw : m_vpwallets) {
        if (pw == pwallet) continue; // already checked above
        LOCK(pw->cs_wallet);
        auto mi(pw->mapAddressBook.find(hash));
        if (mi != pw->mapAddressBook.end()) {
            return mi->second.name;
        }
    }
#endif
    return "";
};

void CSMSG::GetNodesStats(int node_id, UniValue &result)
{
    LOCK(g_connman->cs_vNodes);
    for (auto *pnode : g_connman->vNodes) {
        if (node_id > -1 && node_id != pnode->GetId()) {
            continue;
        }
        LOCK(pnode->smsgData.cs_smsg_net);
        if (!pnode->smsgData.fEnabled) {
            continue;
        }
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("id", pnode->GetId());
        obj.pushKV("address", pnode->GetAddrName());
        obj.pushKV("version", (int) pnode->smsgData.m_version);
        obj.pushKV("ignoreuntil", pnode->smsgData.ignoreUntil);
        obj.pushKV("misbehaving", (int) pnode->smsgData.misbehaving);
        obj.pushKV("numwantsent", (int) pnode->smsgData.m_num_want_sent);
        obj.pushKV("receivecounter", (int) pnode->smsgData.m_receive_counter);
        obj.pushKV("ignoredcounter", (int) pnode->smsgData.m_ignored_counter);
        obj.pushKV("num_pending_inv", (int) pnode->smsgData.m_buckets.size());
        obj.pushKV("num_shown_buckets", (int) pnode->smsgData.m_buckets_last_shown.size());
        if (node_id > -1) {
            UniValue pending_inv_buckets(UniValue::VARR);
            for (auto it = pnode->smsgData.m_buckets.begin(); it != pnode->smsgData.m_buckets.end(); ++it) {
                UniValue bucket(UniValue::VOBJ);
                obj.pushKV("active", (int) it->second.m_active);
                obj.pushKV("hash", std::to_string(it->second.m_hash));
                pending_inv_buckets.push_back(bucket);
            }
            obj.pushKV("pending_inv_buckets", pending_inv_buckets);
            UniValue shown_buckets(UniValue::VARR);
            for (auto it = pnode->smsgData.m_buckets_last_shown.begin(); it != pnode->smsgData.m_buckets_last_shown.end(); ++it) {
                UniValue bucket(UniValue::VOBJ);
                obj.pushKV("time", it->first);
                obj.pushKV("last_shown", it->second);
                shown_buckets.push_back(bucket);
            }
            obj.pushKV("shown_buckets", shown_buckets);
        }

        result.push_back(obj);
    }
};

void CSMSG::ClearBanned()
{
    LOCK(g_connman->cs_vNodes);
    for (auto *pnode : g_connman->vNodes) {
        LOCK(pnode->smsgData.cs_smsg_net);
        if (!pnode->smsgData.fEnabled) {
            continue;
        }
        pnode->smsgData.ignoreUntil = 0;
        pnode->smsgData.misbehaving = 0;
    }
};

int CSMSG::ReceiveData(CNode *pfrom, const std::string &strCommand, CDataStream &vRecv)
{
    /*
        Called from ProcessMessage
        Runs in ThreadMessageHandler2
    */

    /*
        returns SecureMessageCodes

        TODO:
        Explain better and make use of better terminology such as
        Node A <-> Node B <-> Node C

        Commands
        + smsgInv =
            (1) received inventory of other node.
                (1.1) sanity checks
            (2) loop through buckets
                (2.1) sanity checks
                (2.2) check if bucket is locked to node C, if so continue but don't match. TODO: handle this properly, add critical section, lock on write. On read: nothing changes = no lock
                    (2.2.3) If our bucket is not locked to another node then add hash to buffer to be requested..
            (3) send smsgShow with list of hashes to request.

        + smsgShow =
            (1) received a list of requested bucket hashes which the other party does not have.
            (2) respond with smsgHave - contains all the message hashes within the requested buckets.
        + smsgHave =
            (1) A list of all the message hashes which a node has in response to smsgShow.
        + smsgWant =
            (1) A list of the message hashes that a node does not have and wants to retrieve from the node which sent smsgHave
        + smsgMsg =
            (1) In response to
        + smsgPing = ping request
        + smsgPong = pong response
        + smsgMatch =
            Obsolete, it used tell a node up to which time their messages were synced in response to smsg, but this is overhead because we know exactly when we sent them
    */

    if (LogAcceptCategory(BCLog::SMSG)) {
        LogPrintf("%s: %s %s.\n", __func__, pfrom->GetAddrName(), strCommand);
    }

    if (::ChainstateActive().IsInitialBlockDownload()) { // Wait until chain synced
        if (strCommand == "smsgPing") {
            pfrom->smsgData.lastSeen = -1; // Mark node as requiring a response once chain is synced
        }
        return SMSG_NO_ERROR;
    }

    if (!fSecMsgEnabled) {
        if (strCommand == "smsgPing") { // ignore smsgPing
            return SMSG_NO_ERROR;
        }
        return SMSG_UNKNOWN_MESSAGE;
    }

    if (pfrom->nVersion < MIN_SMSG_PROTO_VERSION) {
        LogPrint(BCLog::SMSG, "Peer %d version %d too low for SMSG (minimum %d).\n", pfrom->GetId(), pfrom->nVersion, MIN_SMSG_PROTO_VERSION);
        return SMSG_NO_ERROR;
    }

    if (strCommand == "smsgInv") {
        std::vector<uint8_t> vchData;
        vRecv >> vchData;

        if (vchData.size() < 4) {
            Misbehaving(pfrom->GetId(), 1);
            return SMSG_GENERAL_ERROR; // Not enough data received to be a valid smsgInv
        }

        int64_t now = GetAdjustedTime();

        {
            LOCK(pfrom->smsgData.cs_smsg_net);

            if (now < pfrom->smsgData.ignoreUntil) {
                LogPrint(BCLog::SMSG, "Node is ignoring peer %d until %d.\n", pfrom->GetId(), pfrom->smsgData.ignoreUntil);
                return SMSG_GENERAL_ERROR;
            }
        }

        uint32_t nBuckets = buckets.size();
        uint32_t nLocked = 0;           // no. of locked buckets on this node
        uint32_t nInvBuckets;           // no. of bucket headers sent by peer in smsgInv
        memcpy(&nInvBuckets, &vchData[0], 4);
        LogPrint(BCLog::SMSG, "Remote node sent %d bucket headers, this has %d.\n", nInvBuckets, nBuckets);


        // Check no of buckets:
        if (nInvBuckets > (SMSG_RETENTION / SMSG_BUCKET_LEN) + 1) { // +1 for some leeway
            LogPrintf("Peer sent more bucket headers than possible %u, %u.\n", nInvBuckets, (SMSG_RETENTION / SMSG_BUCKET_LEN));
            Misbehaving(pfrom->GetId(), 1);
            return SMSG_GENERAL_ERROR;
        }

        if (vchData.size() < 4 + nInvBuckets * 16) {
            LogPrintf("Remote node did not send enough data.\n");
            Misbehaving(pfrom->GetId(), 1);
            return SMSG_GENERAL_ERROR;
        }

        std::vector<uint8_t> vchDataOut;
        vchDataOut.reserve(4 + 8 * nInvBuckets); // Reserve max possible size
        vchDataOut.resize(4);
        uint32_t nShowBuckets = 0;

        uint8_t *p = &vchData[4];
        for (uint32_t i = 0; i < nInvBuckets; ++i) {
            int64_t time;
            uint32_t ncontent, hash;
            memcpy(&time, p, 8);
            memcpy(&ncontent, p+8, 4);
            memcpy(&hash, p+12, 4);

            p += 16;

            // Check time valid:
            if (time % SMSG_BUCKET_LEN) {
                LogPrint(BCLog::SMSG, "Not a valid bucket time %d.\n", time);
                Misbehaving(pfrom->GetId(), 1);
                continue;
            }
            if (time < now - SMSG_RETENTION) {
                LogPrint(BCLog::SMSG, "Not interested in peer bucket %d, has expired.\n", time);

                if (time < now - SMSG_RETENTION - SMSG_TIME_LEEWAY) {
                    Misbehaving(pfrom->GetId(), 1);
                }
                continue;
            }
            if (time > now + SMSG_TIME_LEEWAY) {
                LogPrint(BCLog::SMSG, "Not interested in peer bucket %d, in the future.\n", time);
                Misbehaving(pfrom->GetId(), 1);
                continue;
            }

            if (ncontent < 1) {
                LogPrint(BCLog::SMSG, "Peer sent empty bucket, ignore %d %u %u.\n", time, ncontent, hash);
                continue;
            }

            {
                LOCK(cs_smsg);
                const auto it_lb = buckets.find(time);

                if (LogAcceptCategory(BCLog::SMSG)) {
                    LogPrintf("Peer bucket %d %u %u.\n", time, ncontent, hash);
                    if (it_lb != buckets.end()) {
                        LogPrintf("This bucket %d %u %u.\n", time, it_lb->second.setTokens.size(), it_lb->second.hash);
                    }
                }

                if (it_lb != buckets.end() && it_lb->second.nLockCount > 0) {
                    LogPrint(BCLog::SMSG, "Bucket is locked %u, waiting for peer %u to send data.\n", it_lb->second.nLockCount, it_lb->second.nLockPeerId);
                    nLocked++;
                    continue;
                }

                // If this node has more than the peer node, peer node will pull from this
                //  if then peer node has more this node will pull from peer
                uint32_t nLocalActive = (it_lb != buckets.end()) ? it_lb->second.nActive : 0;
                uint32_t nLocalHash = (it_lb != buckets.end()) ? it_lb->second.hash : 0;
                if (it_lb == buckets.end()
                    || nLocalActive < ncontent
                    || (nLocalActive == ncontent
                        && nLocalHash != hash)) { // if same amount in buckets check hash
                    LogPrint(BCLog::SMSG, "Requesting contents of bucket %d.\n", time);

                    uint32_t sz = vchDataOut.size();
                    vchDataOut.resize(sz + 8);
                    memcpy(&vchDataOut[sz], &time, 8);

                    nShowBuckets++;
                }
            } // cs_smsg
        }

        // TODO: should include hash?
        memcpy(&vchDataOut[0], &nShowBuckets, 4);
        if (vchDataOut.size() > 4) {
            g_connman->PushMessage(pfrom,
                CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgShow", vchDataOut));
        } else
        if (nLocked < 1) { // Don't report buckets as matched if any are locked
            // Peer has no buckets we want, don't send them again until something changes
            //  peer will still request buckets from this node if needed (< ncontent)
            vchDataOut.resize(8);
            memcpy(&vchDataOut[0], &now, 8);
            g_connman->PushMessage(pfrom,
                CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgMatch", vchDataOut));
            LogPrint(BCLog::SMSG, "Sending smsgMatch, no locked buckets, time = %d.\n", now);
        } else
        if (nLocked >= 1) {
            LogPrint(BCLog::SMSG, "%u buckets were locked, time = %d.\n", nLocked, now);
        }
    } else
    if (strCommand == "smsgShow") {
        std::vector<uint8_t> vchData;
        vRecv >> vchData;

        if (vchData.size() < 4) {
            return SMSG_GENERAL_ERROR;
        }

        uint32_t nBuckets;
        memcpy(&nBuckets, &vchData[0], 4);

        if (nBuckets > (SMSG_RETENTION / SMSG_BUCKET_LEN) + 1 ||
            vchData.size() < 4 + (size_t)nBuckets * 8) {
            return SMSG_GENERAL_ERROR;
        }

        LogPrint(BCLog::SMSG, "smsgShow: peer wants to see content of %u buckets.\n", nBuckets);

        std::map<int64_t, SecMsgBucket>::iterator itb;
        std::set<SecMsgToken>::iterator it;

        std::vector<uint8_t> vchDataOut;
        vchDataOut.reserve(8 + 16 * 16384); // max tokens per bucket
        int64_t time;
        uint8_t *pIn = &vchData[4];
        for (uint32_t i = 0; i < nBuckets; ++i, pIn += 8) {
            memcpy(&time, pIn, 8);

            {
                LOCK(cs_smsg);
                itb = buckets.find(time);
                if (itb == buckets.end()) {
                    LogPrint(BCLog::SMSG, "Don't have bucket %d.\n", time);
                    continue;
                }

                std::set<SecMsgToken> &tokenSet = itb->second.setTokens;

                try { vchDataOut.resize(8 + 16 * tokenSet.size());
                } catch (std::exception &e) {
                    LogPrintf("vchDataOut.resize %u threw: %s.\n", 8 + 16 * tokenSet.size(), e.what());
                    continue;
                }
                memcpy(&vchDataOut[0], &time, 8);

                int64_t now = GetAdjustedTime();
                size_t nMessages = 0;
                uint8_t *p = &vchDataOut[8];
                for (it = tokenSet.begin(); it != tokenSet.end(); ++it) {
                    if (it->timestamp + it->ttl * SMSGGetSecondsInDay() < now) {
                        continue;
                    }
                    memcpy(p, &it->timestamp, 8);
                    memcpy(p+8, &it->sample, 8);

                    p += 16;
                    nMessages++;
                }
                if (nMessages != tokenSet.size()) {
                    try { vchDataOut.resize(8 + 16 * nMessages);
                    } catch (std::exception &e) {
                        LogPrintf("vchDataOut.resize %u threw: %s.\n", 8 + 16 * nMessages, e.what());
                        continue;
                    }
                }
            }
            g_connman->PushMessage(pfrom,
                CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgHave", vchDataOut));
        }
    } else
    if (strCommand == "smsgHave") {
        // Peer has these messages in bucket
        std::vector<uint8_t> vchData;
        vRecv >> vchData;

        if (vchData.size() < 8) {
            return SMSG_GENERAL_ERROR;
        }

        int n = std::min((int)((vchData.size() - 8) / 16), 16384);

        int64_t time;
        memcpy(&time, &vchData[0], 8);

        // Check time valid:
        int64_t now = GetAdjustedTime();
        if (time < now - SMSG_RETENTION) {
            LogPrint(BCLog::SMSG, "Not interested in peer bucket %d, has expired.\n", time);
            return SMSG_GENERAL_ERROR;
        }
        if (time > now + SMSG_TIME_LEEWAY) {
            LogPrint(BCLog::SMSG, "Not interested in peer bucket %d, in the future.\n", time);
            Misbehaving(pfrom->GetId(), 1);
            return SMSG_GENERAL_ERROR;
        }

        std::vector<uint8_t> vchDataOut;

        {
            LOCK(cs_smsg);
            auto it_bkt = buckets.find(time);
            if (it_bkt != buckets.end() && it_bkt->second.nLockCount > 0) {
                LogPrint(BCLog::SMSG, "Bucket %d lock count %u, waiting for message data from peer %u.\n", time, it_bkt->second.nLockCount, it_bkt->second.nLockPeerId);
                return SMSG_GENERAL_ERROR;
            }

            LogPrint(BCLog::SMSG, "Sifting through bucket %d.\n", time);

            vchDataOut.resize(8);
            memcpy(&vchDataOut[0], &vchData[0], 8);

            static const std::set<SecMsgToken> s_empty_tokens;
            const std::set<SecMsgToken> &tokenSet = (it_bkt != buckets.end()) ? it_bkt->second.setTokens : s_empty_tokens;
            std::set<SecMsgToken>::iterator it;
            SecMsgToken token;
            SecMsgPurged purgedToken;
            uint8_t *p = &vchData[8];

            for (int i = 0; i < n; ++i, p += 16) {
                memcpy(&token.timestamp, p, 8);
                memcpy(&token.sample, p+8, 8);

                if (setPurgedTimestamps.find(token.timestamp) != setPurgedTimestamps.end()) {
                    memcpy(&purgedToken.timestamp, p, 8);
                    memcpy(&purgedToken.sample, p+8, 8);
                    if (setPurged.find(purgedToken) != setPurged.end()) {
                        continue;
                    }
                }

                it = tokenSet.find(token);
                if (it == tokenSet.end()) {
                    int nd = vchDataOut.size();
                    try {
                        vchDataOut.resize(nd + 16);
                    } catch (std::exception &e) {
                        LogPrintf("vchDataOut.resize %d threw: %s.\n", nd + 16, e.what());
                        continue;
                    }

                    memcpy(&vchDataOut[nd], p, 16);
                }
            }
        } // cs_smsg

        if (vchDataOut.size() > 8) {
            uint32_t n_messages = (vchDataOut.size() - 8) / 16;
            if (LogAcceptCategory(BCLog::SMSG)) {
                LogPrintf("Asking peer for %u messages.\n", n_messages);
                LogPrintf("Locking bucket %u for peer %d.\n", time, pfrom->GetId());
            }
            {
                LOCK(cs_smsg);
                buckets[time].nLockCount   = 3; // lock this bucket for at most 3 * SMSG_THREAD_DELAY seconds, unset when peer sends smsgMsg
                buckets[time].nLockPeerId  = pfrom->GetId();
            }
            LOCK(pfrom->smsgData.cs_smsg_net);
            pfrom->smsgData.m_num_want_sent += n_messages;
            g_connman->PushMessage(pfrom,
                CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgWant", vchDataOut));
        }
    } else
    if (strCommand == "smsgWant") {
        std::vector<uint8_t> vchData;
        vRecv >> vchData;

        if (vchData.size() < 8)
            return SMSG_GENERAL_ERROR;

        std::vector<uint8_t> vchOne, vchBunch;

        vchBunch.resize(4 + 8); // nMessages + bucketTime

        int n = std::min((int)((vchData.size() - 8) / 16), 16384);

        int64_t time;
        uint32_t nBunch = 0;
        memcpy(&time, &vchData[0], 8);

        {
            LOCK(cs_smsg);
            auto itb = buckets.find(time);
            if (itb == buckets.end()) {
                LogPrint(BCLog::SMSG, "Don't have bucket %d.\n", time);
                return SMSG_GENERAL_ERROR;
            }

            std::set<SecMsgToken> &tokenSet = itb->second.setTokens;
            std::set<SecMsgToken>::iterator it;
            SecMsgToken token;
            uint8_t *p = &vchData[8];
            for (int i = 0; i < n; ++i) {
                memcpy(&token.timestamp, p, 8);
                memcpy(&token.sample, p+8, 8);

                it = tokenSet.find(token);
                if (it == tokenSet.end()) {
                    LogPrint(BCLog::SMSG, "Don't have wanted message %d.\n", token.timestamp);
                } else {
                    //LogPrintf("Have message at %d.\n", it->offset); // DEBUG
                    token.offset = it->offset;

                    // Place in vchOne so if SecureMsgRetrieve fails it won't corrupt vchBunch
                    if (Retrieve(token, vchOne) == SMSG_NO_ERROR) {
                        nBunch++;
                        vchBunch.insert(vchBunch.end(), vchOne.begin(), vchOne.end()); // append
                    } else {
                        LogPrintf("SecureMsgRetrieve failed %d.\n", token.timestamp);
                    }

                    if (nBunch >= 500
                        || vchBunch.size() >= 96000) {
                        LogPrint(BCLog::SMSG, "Break bunch %u, %u.\n", nBunch, vchBunch.size());
                        break; // end here, peer will send more want messages if needed.
                    }
                }
                p += 16;
            }
        } // cs_smsg

        if (nBunch > 0) {
            LogPrint(BCLog::SMSG, "Sending block of %u messages for bucket %d.\n", nBunch, time);

            memcpy(&vchBunch[0], &nBunch, 4);
            memcpy(&vchBunch[4], &time, 8);
            g_connman->PushMessage(pfrom,
                CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgMsg", vchBunch));
        }
    } else
    if (strCommand == "smsgMsg") {
        std::vector<uint8_t> vchData;
        vRecv >> vchData;

        LogPrint(BCLog::SMSG, "smsgMsg vchData.size() %u.\n", vchData.size());

        Receive(pfrom, vchData);
    } else
    if (strCommand == "smsgMatch") {
        /*
        Basically all this code has to go..
        For now we can use it to punish nodes running the older version, not that it's really need because the overhead is small.
        TODO: remove this code.
        */
        std::vector<uint8_t> vchData;
        vRecv >> vchData;

        if (vchData.size() < 8) {
            LogPrintf("smsgMatch, not enough data %u.\n", vchData.size());
            Misbehaving(pfrom->GetId(), 1);
            return SMSG_GENERAL_ERROR;
        }

        int64_t time;
        memcpy(&time, &vchData[0], 8);

        int64_t now = GetAdjustedTime();
        if (time > now + SMSG_TIME_LEEWAY) {
            LogPrintf("Warning: Peer buckets matched in the future: %d.\nEither this node or the peer node has the incorrect time set.\n", time);
            LogPrint(BCLog::SMSG, "Peer match time set to now.\n");
            time = now;
        }
        /*
        {
            LOCK(pfrom->smsgData.cs_smsg_net);
            pfrom->smsgData.lastMatched = time;
        }*/
        LogPrint(BCLog::SMSG, "Peer buckets matched in smsgWant at %d.\n", time);
    } else
    if (strCommand == "smsgPing") {
        // smsgPing is the initial message, send reply
        g_connman->PushMessage(pfrom,
            CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgPong"));
    } else
    if (strCommand == "smsgPong") {
        LogPrint(BCLog::SMSG, "Peer replied, secure messaging enabled.\n");

        {
            LOCK(pfrom->smsgData.cs_smsg_net);
            pfrom->smsgData.fEnabled = true;
        }
    } else
    if (strCommand == "smsgDisabled") {
        LogPrint(BCLog::SMSG, "Peer %d has disabled secure messaging.\n", pfrom->GetId());

        {
            LOCK(pfrom->smsgData.cs_smsg_net);
            pfrom->smsgData.fEnabled = false;
        }
    } else
    if (strCommand == "smsgIgnore") {
        // Peer is reporting that it will ignore this node until time.
        //  Ignore peer too
        std::vector<uint8_t> vchData;
        vRecv >> vchData;

        if (vchData.size() < 8) {
            LogPrintf("smsgIgnore, not enough data %u.\n", vchData.size());
            Misbehaving(pfrom->GetId(), 1);
            return SMSG_GENERAL_ERROR;
        }

        int64_t time;
        memcpy(&time, &vchData[0], 8);

        {
            int64_t now = GetAdjustedTime();
            int64_t clampedTime = std::min(time, now + (int64_t)SMSG_TIME_IGNORE * 10);
            if (clampedTime < now) clampedTime = now;
            LOCK(pfrom->smsgData.cs_smsg_net);
            pfrom->smsgData.ignoreUntil = clampedTime;
        }

        LogPrint(BCLog::SMSG, "Peer %d is ignoring this node until %d, ignore peer too.\n", pfrom->GetId(), time);
    } else {
        return SMSG_UNKNOWN_MESSAGE;
    }

    return SMSG_NO_ERROR;
};

bool CSMSG::SendData(CNode *pto, bool fSendTrickle)
{
    /*
        Called from ProcessMessage
        Runs in ThreadMessageHandler2
    */

    if (::ChainstateActive().IsInitialBlockDownload()) { // Wait until chain synced
        return true;
    }

    LOCK(pto->smsgData.cs_smsg_net);

    //LogPrintf("SecureMsgSendData() %s.\n", pto->GetAddrName());
    int64_t now = GetTime();

    if (pto->smsgData.lastSeen <= 0) {
        // First contact
        LogPrint(BCLog::SMSG, "%s: New node %s, peer id %u.\n", __func__, pto->GetAddrName(), pto->GetId());
        // Send smsgPing once, do nothing until receive 1st smsgPong (then set fEnabled)
        g_connman->PushMessage(pto,
            CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgPing"));

        // Send smsgPong message if received smsgPing from peer while syncing chain
        if (pto->smsgData.lastSeen < 0) {
            g_connman->PushMessage(pto,
                CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgPong"));
        }

        pto->smsgData.lastSeen = GetTime();
        return true;
    } else
    if (!pto->smsgData.fEnabled
        || now - pto->smsgData.lastSeen < SMSG_SEND_DELAY
        || now < pto->smsgData.ignoreUntil) {
        return true;
    }

    {
        LOCK(cs_smsg);
        std::map<int64_t, SecMsgBucket>::iterator it;

        uint32_t nBuckets = buckets.size();
        if (nBuckets > 0) { // no need to send keep alive pkts, coin messages already do that
            std::vector<uint8_t> vchData;
            // should reserve?
            vchData.reserve(4 + nBuckets*16); // timestamp + size + hash

            uint32_t nBucketsShown = 0;
            vchData.resize(4);

            /*
            Get time before loop and after looping through messages set nLastMatched to time before loop.
            This prevents scenario where:
                Loop()
                    message = locked and  thus skipped
                   message become free and nTimeChanged is updated
                End loop

                nLastMatched = GetTime()
                => bucket that became free in loop is now skipped :/

            Scenario 2:
                Same as one but time is updated before

                    bucket nTimeChanged is updated but not unlocked yet
                    now = GetTime()
                    Loop of buckets skips message

                But this is nanoseconds, very unlikely.

             */

            for (it = buckets.begin(); it != buckets.end(); ++it) {
                SecMsgBucket &bkt = it->second;

                uint32_t nMessages = bkt.nActive;

                if (bkt.timeChanged < pto->smsgData.lastMatched     // peer was last sent all buckets at time of lastMatched. It should have this bucket
                    || nMessages < 1) {                             // this bucket is empty
                    continue;
                }

                uint32_t hash = bkt.hash;

                if (LogAcceptCategory(BCLog::SMSG)) {
                    LogPrintf("Preparing bucket with hash %d for transfer to node %u. timeChanged=%d > lastMatched=%d\n", hash, pto->GetId(), bkt.timeChanged, pto->smsgData.lastMatched);
                }

                size_t sz = vchData.size();
                try { vchData.resize(vchData.size() + 16); } catch (std::exception& e) {
                    LogPrintf("vchData.resize %u threw: %s.\n", vchData.size() + 16, e.what());
                    continue;
                }

                uint8_t *p = &vchData[sz];
                memcpy(p, &it->first, 8);
                memcpy(p+8, &nMessages, 4);
                memcpy(p+12, &hash, 4);

                nBucketsShown++;
                //if (fDebug)
                //    LogPrintf("Sending bucket %d, size %d \n", it->first, it->second.size());
            }

            if (vchData.size() > 4) {
                memcpy(&vchData[0], &nBucketsShown, 4);
                LogPrint(BCLog::SMSG, "Sending %d bucket headers.\n", nBucketsShown);

                g_connman->PushMessage(pto,
                    CNetMsgMaker(INIT_PROTO_VERSION).Make("smsgInv", vchData));
            }
        }
    } // cs_smsg

    pto->smsgData.lastSeen = now;
    pto->smsgData.lastMatched = now; //bug fix smsg 3

    return true;
};

static int InsertAddress(CKeyID &hashKey, CPubKey &pubKey, SecMsgDB &addrpkdb)
{
    /*
    Insert key hash and public key to addressdb.

    (+) Called when receiving a message, it will automatically add the public key of the sender to our database so we can reply.

    Should have LOCK(cs_smsg) where db is opened

    returns SecureMessageCodes
    */

    if (addrpkdb.ExistsPK(hashKey)) {
        //LogPrintf("DB already contains public key for address.\n");
        CPubKey cpkCheck;
        if (!addrpkdb.ReadPK(hashKey, cpkCheck)) {
            LogPrintf("addrpkdb.Read failed.\n");
        } else {
            if (cpkCheck != pubKey) {
                LogPrintf("DB already contains existing public key that does not match .\n");
            }
        }
        return SMSG_PUBKEY_EXISTS;
    }

    if (!addrpkdb.WritePK(hashKey, pubKey)) {
        return errorN(SMSG_GENERAL_ERROR, "%s: Write pair failed.", __func__);
    }

    return SMSG_NO_ERROR;
};

static int InsertAddress(CKeyID &hashKey, CPubKey &pubKey)
{
    LOCK(cs_smsgDB);
    SecMsgDB addrpkdb;

    if (!addrpkdb.Open("cr+")) {
        return SMSG_GENERAL_ERROR;
    }

    return InsertAddress(hashKey, pubKey, addrpkdb);
};

static bool ScanBlock(CSMSG &smsg, const CBlock &block, SecMsgDB &addrpkdb,
    uint32_t &nTransactions, uint32_t &nElements, uint32_t &nPubkeys, uint32_t &nDuplicates)
{
    AssertLockHeld(cs_smsgDB);

    std::string reason;

    // Scan inputs of standard transactions for public keys
    for (const auto &tx : block.vtx) {
        if (tx->IsCoinBase()) {
            continue;
        }

        for (const auto &txin : tx->vin) {
            // Extract public key from scriptSig (P2PKH: <sig> <pubkey>)
            CScript::const_iterator pc = txin.scriptSig.begin();
            std::vector<uint8_t> vchSig, vchPubKey;
            opcodetype opcode;

            // Skip signature
            if (!txin.scriptSig.GetOp(pc, opcode, vchSig)) {
                continue;
            }
            // Get public key
            if (!txin.scriptSig.GetOp(pc, opcode, vchPubKey)) {
                continue;
            }
            if (vchPubKey.size() != 33 && vchPubKey.size() != 65) {
                continue;
            }

            CPubKey pubKey(vchPubKey);

            if (!pubKey.IsValid()
                || !pubKey.IsCompressed()) {
                continue;
            }

            CKeyID addrKey = pubKey.GetID();
            switch (InsertAddress(addrKey, pubKey, addrpkdb)) {
                case SMSG_NO_ERROR: nPubkeys++; break;          // added key
                case SMSG_PUBKEY_EXISTS: nDuplicates++; break;  // duplicate key
            }
        }

        nTransactions++;

        if (nTransactions % 10000 == 0) { // for ScanChainForPublicKeys
            LogPrintf("Scanning transaction no. %u.\n", nTransactions);
        }
    }
    return true;
};


bool CSMSG::ScanBlock(const CBlock &block)
{
    // - scan block for public key addresses

    if (!options.fScanIncoming) {
        return true;
    }

    LogPrint(BCLog::SMSG, "%s.\n", __func__);

    uint32_t nTransactions  = 0;
    uint32_t nElements      = 0;
    uint32_t nPubkeys       = 0;
    uint32_t nDuplicates    = 0;

    {
        LOCK(cs_smsgDB);

        SecMsgDB addrpkdb;
        if (!addrpkdb.Open("cw")
            || !addrpkdb.TxnBegin()) {
            return false;
        }

        smsg::ScanBlock(*this, block, addrpkdb,
            nTransactions, nElements, nPubkeys, nDuplicates);

        addrpkdb.TxnCommit();
    } // cs_smsgDB

    LogPrint(BCLog::SMSG, "Found %u transactions, %u elements, %u new public keys, %u duplicates.\n", nTransactions, nElements, nPubkeys, nDuplicates);

    return true;
};

/** Extract pubkeys from a single block's scriptSig inputs (no DB access). */
static void ExtractPubkeysFromBlock(const CBlock &block,
    std::vector<std::pair<CKeyID, CPubKey>> &vResults)
{
    for (const auto &tx : block.vtx) {
        if (tx->IsCoinBase())
            continue;
        for (const auto &txin : tx->vin) {
            CScript::const_iterator pc = txin.scriptSig.begin();
            std::vector<uint8_t> vchSig, vchPubKey;
            opcodetype opcode;

            if (!txin.scriptSig.GetOp(pc, opcode, vchSig))
                continue;
            if (!txin.scriptSig.GetOp(pc, opcode, vchPubKey))
                continue;
            if (vchPubKey.size() != 33 && vchPubKey.size() != 65)
                continue;

            CPubKey pubKey(vchPubKey);
            if (!pubKey.IsValid() || !pubKey.IsCompressed())
                continue;

            vResults.emplace_back(pubKey.GetID(), pubKey);
        }
    }
}

bool CSMSG::ScanChainForPublicKeys(const std::vector<FlatFilePos> &vBlockPos, uint32_t &nScannedOut)
{
    LogPrintf("Scanning block chain for public keys.\n");
    int64_t nStart = GetTimeMillis();

    nScannedOut = 0;
    const uint32_t nBlocks = vBlockPos.size();
    if (nBlocks == 0)
        return true;

    // Use at most half the available cores.
    int nThreads = std::max(1, GetNumCores() / 2);
    if ((uint32_t)nThreads > nBlocks)
        nThreads = nBlocks;

    LogPrintf("SMSG: Scanning %u blocks using %d threads.\n", nBlocks, nThreads);

    // Per-thread results: vector of extracted (CKeyID, CPubKey) pairs.
    std::vector<std::vector<std::pair<CKeyID, CPubKey>>> vThreadResults(nThreads);
    const Consensus::Params &consensus = Params().GetConsensus();

    // Per-thread last-processed block index (exclusive upper bound within chunk).
    std::vector<uint32_t> vThreadProcessed(nThreads, 0);

    // Shared progress counter for all threads.
    std::atomic<uint32_t> nBlocksProcessed{0};
    m_fScanAbort.store(false, std::memory_order_release);

    // Worker: read assigned blocks and extract pubkeys.
    auto worker = [&](int threadIdx) {
        uint32_t nPerThread = nBlocks / nThreads;
        uint32_t nFrom = threadIdx * nPerThread;
        uint32_t nTo = (threadIdx == nThreads - 1) ? nBlocks : nFrom + nPerThread;

        auto &results = vThreadResults[threadIdx];
        results.reserve((nTo - nFrom) * 2); // rough estimate

        uint32_t nLocal = 0;
        for (uint32_t i = nFrom; i < nTo; i++) {
            if (m_fScanAbort.load(std::memory_order_relaxed))
                break;

            CBlock block;
            if (!ReadBlockFromDisk(block, vBlockPos[i], consensus)) {
                LogPrintf("SMSG: ReadBlockFromDisk failed at pos %s.\n",
                          vBlockPos[i].ToString());
                continue;
            }
            ExtractPubkeysFromBlock(block, results);
            nLocal++;

            uint32_t nDone = nBlocksProcessed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (nDone % 500000 == 0) {
                LogPrintf("SMSG: Scan progress %u / %u blocks (%.1f%%).\n",
                          nDone, nBlocks, 100.0 * nDone / nBlocks);
            }
        }
        vThreadProcessed[threadIdx] = nLocal;
    };

    // Launch workers.
    std::vector<std::thread> vThreads;
    vThreads.reserve(nThreads - 1);
    for (int i = 1; i < nThreads; i++)
        vThreads.emplace_back(worker, i);

    // Use the current thread for chunk 0.
    worker(0);

    for (auto &t : vThreads)
        t.join();

    bool fAborted = m_fScanAbort.load(std::memory_order_acquire);

    // Compute the contiguous block count from block 0.
    // Each thread owns a sequential chunk; count fully completed chunks
    // plus the partial count of the first incomplete chunk.
    uint32_t nContiguous = 0;
    for (int i = 0; i < nThreads; i++) {
        uint32_t nPerThread = nBlocks / nThreads;
        uint32_t nChunkSize = (i == nThreads - 1) ? (nBlocks - i * nPerThread) : nPerThread;
        nContiguous += vThreadProcessed[i];
        if (vThreadProcessed[i] < nChunkSize)
            break; // this chunk was incomplete; stop here
    }
    nScannedOut = nContiguous;

    // Write all extracted pubkeys to DB (even on abort — they are valid).
    // Do NOT use TxnBegin()/TxnCommit() here: ExistsPK with an active batch
    // calls ScanBatch() which iterates the entire WriteBatch linearly — O(N²)
    // total for N pubkeys, making a 3M-block scan take many minutes.
    // Instead, write in chunks via pdb->Write() directly; ExistsPK with no
    // active batch does a fast O(log N) LevelDB Get.
    uint32_t nPubkeys    = 0;
    uint32_t nDuplicates = 0;

    {
        LOCK(cs_smsgDB);

        SecMsgDB addrpkdb;
        if (!addrpkdb.Open("cw")) {
            return false;
        }

        const uint32_t CHUNK_SIZE = 1000;
        leveldb::WriteBatch batch;
        uint32_t nChunkCount = 0;

        auto flushChunk = [&]() -> bool {
            if (nChunkCount == 0) return true;
            leveldb::WriteOptions wo;
            wo.sync = false;
            leveldb::Status s = addrpkdb.pdb->Write(wo, &batch);
            batch.Clear();
            nChunkCount = 0;
            if (!s.ok()) {
                LogPrintf("SMSG: DB chunk write failed: %s\n", s.ToString());
                return false;
            }
            return true;
        };

        for (auto &results : vThreadResults) {
            for (auto &[hashKey, pubKey] : results) {
                if (addrpkdb.ExistsPK(hashKey)) {
                    nDuplicates++;
                    continue;
                }
                // Build key/value in the same layout as WritePK.
                CDataStream ssKey(SER_DISK, CLIENT_VERSION);
                ssKey.reserve(sizeof(hashKey) + 2);
                ssKey << 'p' << 'k' << hashKey;
                CDataStream ssValue(SER_DISK, CLIENT_VERSION);
                ssValue << pubKey;
                batch.Put(ssKey.str(), ssValue.str());
                nPubkeys++;
                if (++nChunkCount >= CHUNK_SIZE) {
                    if (!flushChunk()) return false;
                }
            }
        }
        if (!flushChunk()) return false;
    } // cs_smsgDB

    if (fAborted) {
        LogPrintf("SMSG: Chain scan aborted by user after %u / %u blocks, %u public keys saved.\n",
                  nContiguous, nBlocks, nPubkeys);
    } else {
        LogPrintf("Scanned %u blocks, found %u public keys, %u duplicates.\n",
                  nBlocks, nPubkeys, nDuplicates);
        LogPrintf("Took %d ms\n", GetTimeMillis() - nStart);
    }

    return true;
};

bool CSMSG::ScanBlockChain()
{
    std::vector<FlatFilePos> vBlockPos;
    int nStartHeight = 0;

    // Hold cs_main only long enough to collect block positions.
    {
        LOCK(cs_main);
        CBlockIndex *pindexScan = nullptr;

        // Resume from last scanned height if available.
        {
            LOCK(cs_smsgDB);
            SecMsgDB db;
            if (db.Open("r")) {
                int nLastHeight = -1;
                if (db.ReadScanHeight(nLastHeight) && nLastHeight >= 0) {
                    int nResumeHeight = nLastHeight + 1;
                    if (nResumeHeight <= ::ChainActive().Height()) {
                        pindexScan = ::ChainActive()[nResumeHeight];
                        LogPrintf("SMSG: Resuming chain scan from height %d.\n", nResumeHeight);
                    } else {
                        LogPrintf("SMSG: Chain scan already up to date at height %d.\n", nLastHeight);
                        return true;
                    }
                }
            }
        }

        if (!pindexScan) {
            pindexScan = ::ChainActive().Genesis();
            if (pindexScan == nullptr) {
                return error("%s: pindexGenesisBlock not set.", __func__);
            }
            LogPrintf("SMSG: Starting chain scan from genesis.\n");
        }

        nStartHeight = pindexScan->nHeight;

        // Collect block file positions while cs_main is held.
        CBlockIndex *pindex = pindexScan;
        while (pindex) {
            vBlockPos.push_back(pindex->GetBlockPos());
            pindex = ::ChainActive().Next(pindex);
        }
    } // cs_main released — worker threads can proceed without blocking the node.

    uint32_t nScanned = 0;
    try {
        if (!ScanChainForPublicKeys(vBlockPos, nScanned)) {
            return false;
        }
    } catch (std::exception &e) {
        return error("%s: threw: %s.", __func__, e.what());
    }

    // Store the last scanned height (start + contiguous blocks processed - 1).
    if (nScanned > 0) {
        int nSaveHeight = nStartHeight + (int)nScanned - 1;
        LOCK(cs_smsgDB);
        SecMsgDB db;
        if (db.Open("cw")) {
            db.WriteScanHeight(nSaveHeight);
            LogPrintf("SMSG: Saved scan height %d.\n", nSaveHeight);
        }
    }

    return !m_fScanAbort.load(std::memory_order_acquire);
}

bool CSMSG::ScanBuckets()
{
    LogPrint(BCLog::SMSG, "%s\n", __func__);

    if (!fSecMsgEnabled) {
        return error("%s: SMSG is disabled.\n", __func__);
    }

#ifdef ENABLE_WALLET
    if (pwallet && pwallet->IsLocked()
        && addresses.size() > 0) {
        return error("%s: Wallet is locked.\n", __func__);
    }
#endif

    int64_t  mStart         = GetTimeMillis();
    int64_t  now            = GetTime();
    uint32_t nFiles         = 0;
    uint32_t nMessages      = 0;
    uint32_t nFoundMessages = 0;

    fs::path pathSmsgDir = GetDataDir() / "smsgstore";
    fs::directory_iterator itend;

    if (!fs::exists(pathSmsgDir)
        || !fs::is_directory(pathSmsgDir)) {
        LogPrintf("Message store directory does not exist.\n");
        return true; // not an error
    }

    SecureMessage smsg;
    std::vector<uint8_t> vchData;

    for (fs::directory_iterator itd(pathSmsgDir); itd != itend; ++itd) {
        if (!fs::is_regular_file(itd->status())) {
            continue;
        }

        std::string fileType = itd->path().extension().string();

        if (fileType.compare(".dat") != 0) {
            continue;
        }

        std::string fileName = itd->path().filename().string();

        LogPrint(BCLog::SMSG, "Processing file: %s.\n", fileName);
        nFiles++;

        // TODO files must be split if > 2GB
        // time_noFile.dat
        size_t sep = fileName.find_first_of("_");
        if (sep == std::string::npos) {
            continue;
        }

        std::string stime = fileName.substr(0, sep);
        int64_t fileTime;
        if (!ParseInt64(stime, &fileTime)) {
            LogPrintf("%s: ParseInt64 failed %s.\n", __func__, stime);
            continue;
        }

        if (fileTime < now - SMSG_RETENTION) {
            LogPrintf("Dropping file %s, expired.\n", fileName);
            try {
                fs::remove(itd->path());
            } catch (const fs::filesystem_error &ex) {
                LogPrintf("Error removing bucket file %s, %s.\n", fileName, ex.what());
            }
            continue;
        }

        if (omega::endsWith(fileName, "_wl.dat")) {
            // ScanBuckets must be run with unlocked wallet (if any receiving keys are wallet keys), remove any redundant _wl files
            LogPrint(BCLog::SMSG, "Removing wallet locked file: %s.\n", fileName);
            try { fs::remove(itd->path());
            } catch (const fs::filesystem_error &ex) {
                LogPrintf("Error removing wallet locked file %s.\n", ex.what());
            }
            continue;
        }

        {
            LOCK(cs_smsg);
            FILE *fp;
            errno = 0;
            if (!(fp = fopen(itd->path().string().c_str(), "rb"))) {
                LogPrintf("Error opening file: %s\n", strerror(errno));
                continue;
            }

            for (;;) {
                errno = 0;
                if (fread(smsg.data(), sizeof(uint8_t), SMSG_HDR_LEN, fp) != (size_t)SMSG_HDR_LEN) {
                    if (errno != 0) {
                        LogPrintf("fread header failed: %s\n", strerror(errno));
                    } else {
                        //LogPrintf("End of file.\n");
                    }
                    break;
                }

                try { vchData.resize(smsg.nPayload); } catch (std::exception &e) {
                    LogPrintf("SecureMsgWalletUnlocked(): Could not resize vchData, %u, %s\n", smsg.nPayload, e.what());
                    fclose(fp);
                    return false;
                }

                if (fread(&vchData[0], sizeof(uint8_t), smsg.nPayload, fp) != smsg.nPayload) {
                    LogPrintf("fread data failed: %s\n", strerror(errno));
                    break;
                }

                // Don't report to gui,
                int rv = ScanMessage(smsg.data(), &vchData[0], smsg.nPayload, false);

                if (rv == SMSG_NO_ERROR) {
                    nFoundMessages++;
                } else {
                    // SecureMsgScanMessage failed
                }

                nMessages++;
            }

            fclose(fp);
        } // cs_smsg
    }

    LogPrintf("Processed %u files, scanned %u messages, received %u messages.\n", nFiles, nMessages, nFoundMessages);
    LogPrintf("Took %d ms\n", GetTimeMillis() - mStart);

    return true;
}

int CSMSG::ManageLocalKey(CKeyID &keyId, ChangeType mode)
{
    // TODO: default recv and recvAnon
    {
        LOCK(cs_smsg);

        std::vector<SecMsgAddress>::iterator itFound = addresses.end();
        for (std::vector<SecMsgAddress>::iterator it = addresses.begin(); it != addresses.end(); ++it) {
            if (keyId != it->address) {
                continue;
            }
            itFound = it;
            break;
        }

        switch(mode)
        {
            case CT_NEW:
                if (itFound == addresses.end()) {
                    addresses.push_back(SecMsgAddress(keyId, options.fNewAddressRecv, options.fNewAddressAnon));
                } else {
                    LogPrint(BCLog::SMSG, "%s: Already have address: %s.\n", __func__, EncodeDestination(PKHash(keyId)));
                    return SMSG_KEY_EXISTS;
                }
                break;
            case CT_DELETED:
                if (itFound != addresses.end()) {
                    addresses.erase(itFound);
                } else {
                    return SMSG_KEY_NOT_EXISTS;
                }
                break;
            default:
                break;
        };
    } // cs_smsg

    // Persist immediately so keys survive disable/enable cycles and crashes.
    int iniRv = WriteIni();
    if (iniRv != SMSG_NO_ERROR) {
        return iniRv;
    }

    return SMSG_NO_ERROR;
};

int CSMSG::WalletUnlocked()
{
#ifdef ENABLE_WALLET
    /*
    When the wallet is unlocked, scan messages received while wallet was locked.
    */
    if (!fSecMsgEnabled || !pwallet) {
        return SMSG_WALLET_UNSET;
    }

    LogPrintf("SecureMsgWalletUnlocked()\n");

    if (pwallet->IsLocked()) {
        return errorN(SMSG_WALLET_LOCKED, "%s: Wallet is locked.", __func__);
    }

    int64_t  now            = GetTime();
    uint32_t nFiles         = 0;
    uint32_t nMessages      = 0;
    uint32_t nFoundMessages = 0;

    fs::path pathSmsgDir = GetDataDir() / "smsgstore";
    fs::directory_iterator itend;

    if (!fs::exists(pathSmsgDir)
        || !fs::is_directory(pathSmsgDir)) {
        LogPrintf("Message store directory does not exist.\n");
        return SMSG_NO_ERROR; // not an error
    }

    SecureMessage smsg;
    std::vector<uint8_t> vchData;

    for (fs::directory_iterator itd(pathSmsgDir); itd != itend; ++itd) {
        if (!fs::is_regular_file(itd->status())) {
            continue;
        }

        std::string fileName = itd->path().filename().string();

        if (!omega::endsWith(fileName, "_wl.dat")) {
            continue;
        }

        LogPrint(BCLog::SMSG, "Processing file: %s.\n", fileName);

        nFiles++;

        // TODO files must be split if > 2GB
        // time_noFile_wl.dat
        size_t sep = fileName.find_first_of("_");
        if (sep == std::string::npos) {
            continue;
        }

        std::string stime = fileName.substr(0, sep);
        int64_t fileTime;
        if (!ParseInt64(stime, &fileTime)) {
            LogPrintf("%s: ParseInt64 failed %s.\n", __func__, stime);
            continue;
        }

        if (fileTime < now - SMSG_RETENTION) {
            LogPrintf("Dropping wallet locked file %s, expired.\n", fileName);
            try {
                fs::remove(itd->path());
            } catch (const fs::filesystem_error &ex) {
                return errorN(SMSG_GENERAL_ERROR, "%s: Could not remove file %s - %s.", __func__, fileName, ex.what());
            }
            continue;
        }

        {
            LOCK(cs_smsg);
            FILE *fp;
            errno = 0;
            if (!(fp = fopen(itd->path().string().c_str(), "rb"))) {
                LogPrintf("Error opening file: %s\n", strerror(errno));
                continue;
            }

            bool fFileFullyRead = false;
            for (;;) {
                errno = 0;
                if (fread(smsg.data(), sizeof(uint8_t), SMSG_HDR_LEN, fp) != (size_t)SMSG_HDR_LEN) {
                    if (errno != 0) {
                        LogPrintf("fread header failed: %s\n", strerror(errno));
                    } else {
                        fFileFullyRead = true; // clean EOF
                    }
                    break;
                }

                if (smsg.nPayload == 0) {
                    LogPrintf("%s: Zero-length payload in %s, skipping.\n", __func__, fileName);
                    continue;
                }

                try { vchData.resize(smsg.nPayload); } catch (std::exception &e) {
                    LogPrintf("%s: Could not resize vchData, %u, %s\n", __func__, smsg.nPayload, e.what());
                    fclose(fp);
                    return SMSG_GENERAL_ERROR;
                }

                if (fread(&vchData[0], sizeof(uint8_t), smsg.nPayload, fp) != smsg.nPayload) {
                    LogPrintf("fread data failed: %s\n", strerror(errno));
                    break;
                }

                // Don't report to gui,
                int rv = ScanMessage(smsg.data(), &vchData[0], smsg.nPayload, false);

                if (rv == 0) {
                    nFoundMessages++;
                } else
                if (rv != 0) {
                    // SecureMsgScanMessage failed
                }

                nMessages++;
            }

            fclose(fp);

            if (fFileFullyRead) {
                try {
                    fs::remove(itd->path());
                } catch (const fs::filesystem_error &ex) {
                    return errorN(SMSG_GENERAL_ERROR, "%s: Could not remove file %s - %s.", __func__, fileName, ex.what());
                }
            } else {
                LogPrintf("%s: Keeping %s due to read error.\n", __func__, fileName);
            }
        } // cs_smsg
    };

    LogPrintf("Processed %u files, scanned %u messages, received %u messages.\n", nFiles, nMessages, nFoundMessages);

    // Notify gui
    NotifySecMsgWalletUnlocked();
#endif
    return SMSG_NO_ERROR;
};

int CSMSG::WalletKeyChanged(CKeyID &keyId, const std::string &sLabel, ChangeType mode)
{
    /*
    SecureMsgWalletKeyChanged():
    When a key changes in the wallet, this function should be called to update the addresses vector.

    mode:
        CT_NEW : a new key was added
        CT_DELETED : delete an existing key from vector.
    */

    if (!fSecMsgEnabled) {
        return SMSG_DISABLED;
    }

    LogPrintf("%s\n", __func__);

    if (!gArgs.GetBoolArg("-smsgsaddnewkeys", false)) {
        LogPrint(BCLog::SMSG, "%s smsgsaddnewkeys option is disabled.\n", __func__);
        return SMSG_GENERAL_ERROR;
    }

    return ManageLocalKey(keyId, mode);
};

int CSMSG::ScanMessage(const uint8_t *pHeader, const uint8_t *pPayload, uint32_t nPayload, bool reportToGui)
{
    LogPrint(BCLog::SMSG, "%s\n", __func__);
    /*
    Check if message belongs to this node.
    If so add to inbox db.

    if !reportToGui don't fire NotifySecMsgInboxChanged
     - loads messages received when wallet locked in bulk.

    returns SecureMessageCodes
    */

    bool fOwnMessage = false;
    MessageData msg; // placeholder
    CKeyID addressTo;
    const SecureMessage* psmsg_scan = (const SecureMessage*)pHeader;
    bool fIsTopic = (psmsg_scan->version[0] == SMSG_VERSION_TOPIC);
    LOCK(smsgModule.keyStore.cs_KeyStore);
    for (auto &p : smsgModule.keyStore.mapKeys) {
        auto &address = p.first;
        auto &key = p.second;

        if (!(key.nFlags & SMK_RECEIVE_ON)) {
            continue;
        }

        if (!(key.nFlags & SMK_RECEIVE_ANON)) {
            // Have to do full decrypt to see address from
            if (Decrypt(false, key.key, address, pHeader, pPayload, nPayload, msg) == 0) {
                if (LogAcceptCategory(BCLog::SMSG)) {
                    LogPrintf("Decrypted message with %s.\n", EncodeDestination(PKHash(addressTo)));
                }
                if (msg.sFromAddress.compare("anon") != 0) {
                    fOwnMessage = true;
                }
                addressTo = address;
                break;
            }
        } else {
            // For topic messages use full decrypt so sTopic is populated in one pass
            if (Decrypt(!fIsTopic, key.key, address, pHeader, pPayload, nPayload, msg) == 0) {
                if (LogAcceptCategory(BCLog::SMSG)) {
                    LogPrintf("Decrypted message with %s.\n", EncodeDestination(PKHash(addressTo)));
                }
                fOwnMessage = true;
                addressTo = address;
                break;
            }
        }
    }

    if (!fOwnMessage) {
#ifdef ENABLE_WALLET
        if (!pwallet) {
            LogPrint(BCLog::SMSG, "%s: Wallet is not set.\n", __func__);
            return SMSG_NO_ERROR;
        }

        std::vector<SecMsgAddress> addrSnapshot;
        {
            LOCK(cs_smsg);
            addrSnapshot = addresses;
        }

        LOCK(pwallet->cs_wallet);
        if (pwallet->IsLocked()) {
            LogPrint(BCLog::SMSG, "%s: Wallet is locked, storing message to scan later.\n", __func__);

            if (addrSnapshot.size() > 0) { // Only save unscanned if there are addresses
                int rv;
                if ((rv = StoreUnscanned(pHeader, pPayload, nPayload)) != 0) {
                    return SMSG_GENERAL_ERROR;
                }
            }

            return SMSG_WALLET_LOCKED;
        }

        for (const auto &addr : addrSnapshot) {
            if (!addr.fReceiveEnabled) {
                continue;
            }

            addressTo = addr.address;

            CKey keyDest;
            auto *spk = pwallet->GetLegacyScriptPubKeyMan();
            if (!spk || !spk->GetKey(addressTo, keyDest)) {
                continue;
            }

            if (!addr.fReceiveAnon) {
                // Have to do full decrypt to see address from
                if (Decrypt(false, keyDest, addressTo, pHeader, pPayload, nPayload, msg) == 0) {
                    if (LogAcceptCategory(BCLog::SMSG)) {
                        LogPrintf("Decrypted message with %s.\n", EncodeDestination(PKHash(addressTo)));
                    }
                    if (msg.sFromAddress.compare("anon") != 0) {
                        fOwnMessage = true;
                    }
                    break;
                }
            } else {
                // For topic messages use full decrypt so sTopic is populated in one pass
                if (Decrypt(!fIsTopic, keyDest, addressTo, pHeader, pPayload, nPayload, msg) == 0) {
                    if (LogAcceptCategory(BCLog::SMSG)) {
                        LogPrintf("Decrypted message with %s.\n", EncodeDestination(PKHash(addressTo)));
                    }
                    fOwnMessage = true;
                    break;
                }
            }
        }
#endif
    }

    if (fOwnMessage) {
        // Save to inbox (or trollbox if addressed to the trollbox channel)
        SecureMessage *psmsg = (SecureMessage*) pHeader;

        uint160 hash;
        HashMsg(*psmsg, pPayload, nPayload - psmsg->GetPaidTailSize(), hash);

        bool fTrollbox = (addressTo == trollboxAddress);
        std::string sPrefix(fTrollbox ? "tb" : "im");
        uint8_t chKey[30];
        int64_t timestamp_be = bswap_64(psmsg->timestamp);
        memcpy(&chKey[0], sPrefix.data(), 2);
        memcpy(&chKey[2], &timestamp_be, 8);
        memcpy(&chKey[10], hash.begin(), 20);

        SecMsgStored smsgInbox;
        smsgInbox.timeReceived  = GetTime();
        smsgInbox.status        = (SMSG_MASK_UNREAD) & 0xFF;
        smsgInbox.addrTo        = addressTo;

        try { smsgInbox.vchMessage.resize(SMSG_HDR_LEN + nPayload); } catch (std::exception &e) {
            return errorN(SMSG_ALLOCATE_FAILED, "%s: Could not resize vchData, %u, %s.", __func__, SMSG_HDR_LEN + nPayload, e.what());
        }
        memcpy(&smsgInbox.vchMessage[0], pHeader, SMSG_HDR_LEN);
        memcpy(&smsgInbox.vchMessage[SMSG_HDR_LEN], pPayload, nPayload);

        bool fExisted = false;
        bool fNotifyGui = false;
        bool fNotifyZmq = false;
        {
            LOCK(cs_smsgDB);
            SecMsgDB dbInbox;

            if (dbInbox.Open("cw")) {
                if (dbInbox.ExistsSmesg(chKey)) {
                    fExisted = true;
                    LogPrint(BCLog::SMSG, "Message already exists in %s db.\n", fTrollbox ? "trollbox" : "inbox");
                } else {
                    dbInbox.WriteSmesg(chKey, smsgInbox);
                    if (reportToGui) {
                        fNotifyGui = true;
                    }
                    fNotifyZmq = true;
                    LogPrintf("SecureMsg saved to %s, received with %s.\n",
                        fTrollbox ? "trollbox" : "inbox", EncodeDestination(PKHash(addressTo)));
                }
            }
        } // cs_smsgDB

        if (fNotifyGui) {
            if (fTrollbox) {
                NotifySecMsgTrollboxChanged(smsgInbox);
            } else {
                NotifySecMsgInboxChanged(smsgInbox);
            }
        }
#if ENABLE_ZMQ
        if (fNotifyZmq && g_zmq_notification_interface) {
            g_zmq_notification_interface->NotifySmsgReceived(smsgInbox.vchMessage);
        }
#endif

        // For topic messages: if the full topic was recovered, write the index entry
        if (!fExisted && !fTrollbox && psmsg->version[0] == SMSG_VERSION_TOPIC) {
            if (msg.sTopic.empty()) {
                // Test-only decrypt was used; do a full decrypt to recover sTopic
                MessageData topicMsg;
                Decrypt(false, addressTo, pHeader, pPayload, nPayload, topicMsg);
                msg.sTopic = topicMsg.sTopic;
                msg.parentMsgId = topicMsg.parentMsgId;
                msg.nRetentionDays = topicMsg.nRetentionDays;
            }
            if (!msg.sTopic.empty()) {
                LOCK(cs_smsgDB);
                SecMsgDB dbIdx;
                if (dbIdx.Open("cw")) {
                    dbIdx.WriteTopicIndex(msg.sTopic, psmsg->timestamp, hash,
                                          msg.parentMsgId, msg.nRetentionDays);
                }
            }
        }

        if (!fExisted && !fTrollbox) {
            // notify an external script when a message comes in
            std::string strCmd = gArgs.GetArg("-smsgnotify", "");

            //TODO: Format message
            if (!strCmd.empty()) {
                boost::replace_all(strCmd, "%s", EncodeDestination(PKHash(addressTo)));
                threadGroupSmsg.create_thread([s = strCmd]{ runCommand(s); });
            }

            // TODO: add NewSecureMessage signal to CMainSignals
            // GetMainSignals().NewSecureMessage(psmsg, hash);
        }
    }

    return SMSG_NO_ERROR;
};

int CSMSG::GetLocalKey(const CKeyID &ckid, CPubKey &cpkOut)
{
    LogPrint(BCLog::SMSG, "%s\n", __func__);

    if (keyStore.GetPubKey(ckid, cpkOut)) {
        return SMSG_NO_ERROR;
    }

#ifdef ENABLE_WALLET
    if (!pwallet) {
        return errorN(SMSG_WALLET_UNSET, "%s: Wallet disabled.", __func__);
    }

    auto *spk_pub = pwallet->GetLegacyScriptPubKeyMan();
    if (!spk_pub || !spk_pub->GetPubKey(ckid, cpkOut)) {
        return SMSG_WALLET_NO_PUBKEY;
    }

    if (!cpkOut.IsValid()
        || !cpkOut.IsCompressed()) {
        return errorN(SMSG_INVALID_PUBKEY, "%s: Public key is invalid %s.", __func__, HexStr(cpkOut));
    }
    return SMSG_NO_ERROR;
#endif

    return SMSG_WALLET_NO_PUBKEY;
};

int CSMSG::GetLocalPublicKey(const std::string &strAddress, std::string &strPublicKey)
{
    // returns SecureMessageCodes

    CTxDestination dest = DecodeDestination(strAddress);
    const PKHash *pkHash = std::get_if<PKHash>(&dest);
    if (!pkHash) {
        return SMSG_INVALID_ADDRESS;
    }
    CKeyID keyID(*pkHash);

    int rv;
    CPubKey pubKey;
    if ((rv = GetLocalKey(keyID, pubKey)) != 0) {
        return rv;
    }

    strPublicKey = EncodeBase58(pubKey.begin(), pubKey.end());
    return SMSG_NO_ERROR;
};

int CSMSG::GetStoredKey(const CKeyID &ckid, CPubKey &cpkOut)
{
    /* returns SecureMessageCodes
    */
    LogPrint(BCLog::SMSG, "%s\n", __func__);

    {
        LOCK(cs_smsgDB);
        SecMsgDB addrpkdb;

        if (!addrpkdb.Open("r")) {
            return SMSG_GENERAL_ERROR;
        }

        if (!addrpkdb.ReadPK(ckid, cpkOut)) {
            //LogPrintf("addrpkdb.Read failed: %s.\n", coinAddress.ToString());
            return SMSG_PUBKEY_NOT_EXISTS;
        }
    } // cs_smsgDB

    return SMSG_NO_ERROR;
};

int CSMSG::AddAddress(std::string &address, std::string &publicKey)
{
    /*
    Add address and matching public key to the database
    address and publicKey are in base58

    returns SecureMessageCodes
    */

    CTxDestination dest = DecodeDestination(address);
    const PKHash *pkHash = std::get_if<PKHash>(&dest);
    if (!pkHash) {
        return errorN(SMSG_INVALID_ADDRESS, "%s - Address is not valid: %s.", __func__, address);
    }
    CKeyID idk(*pkHash);

    std::vector<uint8_t> vchTest;

    if (IsHex(publicKey)) {
       vchTest = ParseHex(publicKey);
    } else {
        if (!DecodeBase58(publicKey, vchTest, 65)) {
            return errorN(SMSG_INVALID_PUBKEY, "%s - DecodeBase58 failed.", __func__);
        }
    }

    CPubKey pubKey(vchTest);
    if (!pubKey.IsValid()) {
        return errorN(SMSG_INVALID_PUBKEY, "%s - Invalid PubKey.", __func__);
    }

    // Check that public key matches address hash
    CKeyID keyIDT = pubKey.GetID();
    if (idk != keyIDT) {
        return errorN(SMSG_PUBKEY_MISMATCH, "%s - Public key does not hash to address %s.", __func__, address);
    }

    return InsertAddress(idk, pubKey);
};

int CSMSG::AddContact(std::string &address, std::string &publicKey, const std::string &label)
{
    /*
    Add address and public key as a named contact.
    Stores pubkey in addrpkdb (for encryption) and a SecMsgKey contact record
    (SMK_CONTACT_ONLY, no private key) so the contact appears in the keys list.
    */

    CTxDestination dest = DecodeDestination(address);
    const PKHash *pkHash = std::get_if<PKHash>(&dest);
    if (!pkHash) {
        return errorN(SMSG_INVALID_ADDRESS, "%s - Address is not valid: %s.", __func__, address);
    }
    CKeyID idk(*pkHash);

    std::vector<uint8_t> vchTest;
    if (IsHex(publicKey)) {
        vchTest = ParseHex(publicKey);
    } else {
        if (!DecodeBase58(publicKey, vchTest, 65)) {
            return errorN(SMSG_INVALID_PUBKEY, "%s - DecodeBase58 failed.", __func__);
        }
    }

    CPubKey pubKey(vchTest);
    if (!pubKey.IsValid()) {
        return errorN(SMSG_INVALID_PUBKEY, "%s - Invalid PubKey.", __func__);
    }

    CKeyID keyIDT = pubKey.GetID();
    if (idk != keyIDT) {
        return errorN(SMSG_PUBKEY_MISMATCH, "%s - Public key does not hash to address %s.", __func__, address);
    }

    // Store pubkey in address-pubkey DB (for message encryption)
    int rv = InsertAddress(idk, pubKey);
    if (rv != SMSG_NO_ERROR && rv != SMSG_PUBKEY_EXISTS) {
        return rv;
    }

    // Store a contact record in the key DB so it appears in the keys list
    SecMsgKey key;
    key.sLabel = label;
    key.nFlags = SMK_CONTACT_ONLY;
    key.pubkey = pubKey; // store pubkey in memory (not serialized, but useful for in-session use)
    // key.key is intentionally left as invalid CKey (no private key for contacts)

    {
        LOCK(cs_smsgDB);
        SecMsgDB db;
        if (!db.Open("cr+")) {
            return SMSG_GENERAL_ERROR;
        }
        if (!db.WriteKey(idk, key)) {
            return errorN(SMSG_GENERAL_ERROR, "%s - WriteKey failed.", __func__);
        }
    }

    keyStore.AddKey(idk, key);
    return SMSG_NO_ERROR;
};

int CSMSG::AddLocalAddress(const std::string &sAddress)
{
#ifdef ENABLE_WALLET
    LogPrintf("%s: %s\n", __func__, sAddress);

    if (!pwallet) {
        return errorN(SMSG_WALLET_UNSET, "%s: Wallet disabled.", __func__);
    }

    CTxDestination dest = DecodeDestination(sAddress);
    const PKHash *pkHash = std::get_if<PKHash>(&dest);
    if (!pkHash) {
        return errorN(SMSG_INVALID_ADDRESS, "%s - Address is not valid: %s.", __func__, sAddress);
    }
    CKeyID idk(*pkHash);

    auto *spk_have = pwallet->GetLegacyScriptPubKeyMan();
    if (!spk_have || !spk_have->HaveKey(idk)) {
        return errorN(SMSG_WALLET_NO_KEY, "%s: Key to %s not found in wallet.", __func__, sAddress);
    }

    int rv = ManageLocalKey(idk, CT_NEW);
    if (rv == SMSG_NO_ERROR) {
        pwallet->SetAddressBook(PKHash(idk), "", "smsg");
    }
    return rv;
#else
    return SMSG_WALLET_UNSET;
#endif
};

int CSMSG::ImportPrivkey(const CKey &vchSecret, const std::string &sLabel)
{
    SecMsgKey key;
    key.key = vchSecret;
    key.sLabel = sLabel;
    CKeyID idk = key.key.GetPubKey().GetID();
    key.nFlags |= SMK_RECEIVE_ON;
    key.nFlags |= SMK_RECEIVE_ANON;

    LOCK(cs_smsgDB);

    SecMsgDB db;
    if (!db.Open("cr+")) {
        return SMSG_GENERAL_ERROR;
    }

    if (!db.WriteKey(idk, key)) {
        return errorN(SMSG_GENERAL_ERROR, "%s - WriteKey failed.", __func__);
    }

    keyStore.AddKey(idk, key);

    return SMSG_NO_ERROR;
};

bool CSMSG::SetWalletAddressOption(const CKeyID &idk, std::string sOption, bool fValue)
{
    std::vector<smsg::SecMsgAddress>::iterator it;
    for (it = addresses.begin(); it != addresses.end(); ++it) {
        if (idk != it->address) {
            continue;
        }
        break;
    }

    if (it == addresses.end()) {
        return false;
    }

    if (sOption == "anon") {
        it->fReceiveAnon = fValue;
    } else
    if (sOption == "receive") {
        it->fReceiveEnabled = fValue;
    } else {
        return error("%s: Unknown option %s.\n", __func__, sOption);
    }

    return true;
};

bool CSMSG::SetSmsgAddressOption(const CKeyID &idk, std::string sOption, bool fValue)
{
    LOCK(cs_smsgDB);

    SecMsgDB db;
    if (!db.Open("cr+")) {
        return error("%s: Failed to open db.\n", __func__);
    }

    SecMsgKey key;
    if (!db.ReadKey(idk, key)) {
        return false;
    }

    if (sOption == "anon") {
        if (fValue) {
            key.nFlags |= SMK_RECEIVE_ANON;
        } else {
            key.nFlags &= ~SMK_RECEIVE_ANON;
        }
    } else
    if (sOption == "receive") {
        if (fValue) {
            key.nFlags |= SMK_RECEIVE_ON;
        } else {
            key.nFlags &= ~SMK_RECEIVE_ON;
        }
    } else {
        return error("%s: Unknown option %s.\n", __func__, sOption);
    }

    if (!db.WriteKey(idk, key)) {
        return false;
    }

    if (key.nFlags & SMK_RECEIVE_ON) {
        keyStore.AddKey(idk, key);
    } else {
        keyStore.EraseKey(idk);
    }

    return true;
};

int CSMSG::ReadSmsgKey(const CKeyID &idk, CKey &key)
{
    LOCK(cs_smsgDB);

    SecMsgDB db;
    if (!db.Open("cr+")) {
        return SMSG_GENERAL_ERROR;
    }

    SecMsgKey smk;
    if (!db.ReadKey(idk, smk)) {
        return SMSG_KEY_NOT_EXISTS;
    }

    key = smk.key;

    return SMSG_NO_ERROR;
};

int CSMSG::DumpPrivkey(const CKeyID &idk, CKey &key_out)
{
    LOCK(cs_smsgDB);

    SecMsgDB db;
    if (!db.Open("cr+")) {
        return SMSG_GENERAL_ERROR;
    }

    SecMsgKey smk;
    if (!db.ReadKey(idk, smk)) {
        return SMSG_KEY_NOT_EXISTS;
    }

    key_out = smk.key;
    return SMSG_NO_ERROR;
};

int CSMSG::RemoveAddress(const std::string &addr)
{
    CTxDestination dest = DecodeDestination(addr);
    if (!IsValidDestination(dest)) {
        return SMSG_INVALID_ADDRESS;
    }

    const PKHash *pkhash = std::get_if<PKHash>(&dest);
    if (!pkhash) {
        return SMSG_INVALID_ADDRESS;
    }

    CKeyID idk = ToKeyID(*pkhash);

    {
        LOCK(cs_smsg);
        auto it = addresses.begin();
        for (; it != addresses.end(); ++it) {
            if (it->address == idk) {
                break;
            }
        }
        if (it != addresses.end()) {
            addresses.erase(it);
        }
    }

    {
        LOCK(cs_smsgDB);
        SecMsgDB db;
        if (!db.Open("cr+")) {
            return SMSG_GENERAL_ERROR;
        }
        db.ErasePK(idk);
    }

    return SMSG_NO_ERROR;
};

int CSMSG::RemovePrivkey(const std::string &addr)
{
    CTxDestination dest = DecodeDestination(addr);
    if (!IsValidDestination(dest)) {
        return SMSG_INVALID_ADDRESS;
    }

    const PKHash *pkhash = std::get_if<PKHash>(&dest);
    if (!pkhash) {
        return SMSG_INVALID_ADDRESS;
    }

    CKeyID idk = ToKeyID(*pkhash);

    keyStore.EraseKey(idk);

    {
        LOCK(cs_smsgDB);
        SecMsgDB db;
        if (!db.Open("cr+")) {
            return SMSG_GENERAL_ERROR;
        }
        db.EraseKey(idk);
    }

    return SMSG_NO_ERROR;
};

int CSMSG::Retrieve(const SecMsgToken &token, std::vector<uint8_t> &vchData)
{
    LogPrint(BCLog::SMSG, "%s: %d.\n", __func__, token.timestamp);
    AssertLockHeld(cs_smsg);

    fs::path pathSmsgDir = GetDataDir() / "smsgstore";

    int64_t bucket = token.timestamp - (token.timestamp % SMSG_BUCKET_LEN);
    std::string fileName = std::to_string(bucket) + "_01.dat";
    fs::path fullpath = pathSmsgDir / fileName;

    FILE *fp;
    errno = 0;
    if (!(fp = fopen(fullpath.string().c_str(), "rb"))) {
        return errorN(SMSG_GENERAL_ERROR, "%s - Can't open file: %s\nPath %s.", __func__, strerror(errno), fullpath.string());
    }

    errno = 0;
    if (fseek(fp, token.offset, SEEK_SET) != 0) {
        fclose(fp);
        return errorN(SMSG_GENERAL_ERROR, "%s - fseek, strerror: %s.", __func__, strerror(errno));
    }

    SecureMessage smsg;
    errno = 0;
    if (fread(smsg.data(), sizeof(uint8_t), SMSG_HDR_LEN, fp) != (size_t)SMSG_HDR_LEN) {
        fclose(fp);
        return errorN(SMSG_GENERAL_ERROR, "%s - read header failed, strerror: %s.", __func__, strerror(errno));
    }

    try {vchData.resize(SMSG_HDR_LEN + smsg.nPayload);} catch (std::exception &e) {
        fclose(fp);
        return errorN(SMSG_ALLOCATE_FAILED, "%s - Could not resize vchData, %u, %s.", __func__, SMSG_HDR_LEN + smsg.nPayload, e.what());
    }

    memcpy(vchData.data(), smsg.data(), SMSG_HDR_LEN);
    errno = 0;
    if (fread(&vchData[SMSG_HDR_LEN], sizeof(uint8_t), smsg.nPayload, fp) != smsg.nPayload) {
        fclose(fp);
        return errorN(SMSG_GENERAL_ERROR, "%s - fread data failed: %s. Wanted %u bytes.", __func__, strerror(errno), smsg.nPayload);
    }

    fclose(fp);
    return SMSG_NO_ERROR;
};

int CSMSG::Remove(const SecMsgToken &token)
{
    LogPrint(BCLog::SMSG, "%s: %d.\n", __func__, token.timestamp);
    AssertLockHeld(cs_smsg);

    fs::path pathSmsgDir = GetDataDir() / "smsgstore";

    int64_t bucket = token.timestamp - (token.timestamp % SMSG_BUCKET_LEN);
    std::string fileName = std::to_string(bucket) + "_01.dat";
    fs::path fullpath = pathSmsgDir / fileName;

    FILE *fp;
    errno = 0;
    if (!(fp = fopen(fullpath.string().c_str(), "rb+"))) {
        return errorN(SMSG_GENERAL_ERROR, "%s - Can't open file: %s\nPath %s.", __func__, strerror(errno), fullpath.string());
    }

    errno = 0;
    if (fseek(fp, token.offset, SEEK_SET) != 0) {
        fclose(fp);
        return errorN(SMSG_GENERAL_ERROR, "%s - fseek, strerror: %s.", __func__, strerror(errno));
    }

    SecureMessage smsg;
    errno = 0;
    if (fread(smsg.data(), sizeof(uint8_t), SMSG_HDR_LEN, fp) != (size_t)SMSG_HDR_LEN) {
        fclose(fp);
        return errorN(SMSG_GENERAL_ERROR, "%s - read header failed, strerror: %s.", __func__, strerror(errno));
    }

    uint8_t z = 0;
    if (0 != fseek(fp, token.offset + 4, SEEK_SET)
        || 2 != fwrite(&z, 1, 2, fp)) {
        fclose(fp);
        return errorN(SMSG_GENERAL_ERROR, "%s - zero version strerror: %s.", __func__, strerror(errno));
    }

    if (fseek(fp, token.offset + SMSG_HDR_LEN + 8, SEEK_SET) != 0) {
        fclose(fp);
        return errorN(SMSG_GENERAL_ERROR, "%s - fseek, strerror: %s.", __func__, strerror(errno));
    }

    size_t zlen = smsg.nPayload - 8;
    if (smsg.nPayload <= 8 ||  zlen != fwrite(&z, 1, zlen, fp)) {
        fclose(fp);
        return errorN(SMSG_GENERAL_ERROR, "%s - fwrite, zlen %d, strerror: %s.", __func__, zlen, strerror(errno));
    }

    fclose(fp);
    return SMSG_NO_ERROR;
};

int CSMSG::Receive(CNode *pfrom, std::vector<uint8_t> &vchData)
{
    LogPrint(BCLog::SMSG, "%s\n", __func__);

    if (vchData.size() < 12) { // nBunch4 + timestamp8
        return errorN(SMSG_GENERAL_ERROR, "%s - Not enough data.", __func__);
    }

    uint32_t nBunch;
    int64_t bktTime;

    memcpy(&nBunch, &vchData[0], 4);
    memcpy(&bktTime, &vchData[4], 8);

    // Check bktTime ()
    //  Bucket may not exist yet - will be created when messages are added
    int64_t now = GetAdjustedTime();
    if (bktTime > now + SMSG_TIME_LEEWAY) {
        LogPrint(BCLog::SMSG, "bktTime > now.\n");
        // misbehave?
        return SMSG_GENERAL_ERROR;
    } else
    if (bktTime < now - SMSG_RETENTION) {
        LogPrint(BCLog::SMSG, "bktTime < now - SMSG_RETENTION.\n");
        // misbehave?
        return SMSG_GENERAL_ERROR;
    }

    std::map<int64_t, SecMsgBucket>::iterator itb;

    if (nBunch == 0 || nBunch > 500) {
        LogPrintf("Error: Invalid no. messages received in bunch %u, for bucket %d.\n", nBunch, bktTime);
        Misbehaving(pfrom->GetId(), 1);

        {
            LOCK(cs_smsg);
            // Release lock on bucket if it exists
            itb = buckets.find(bktTime);
            if (itb != buckets.end())
                itb->second.nLockCount = 0;
        } // cs_smsg
        return SMSG_GENERAL_ERROR;
    }

    {
        LOCK(pfrom->smsgData.cs_smsg_net);
        if (nBunch > pfrom->smsgData.m_num_want_sent) {
            Misbehaving(pfrom->GetId(), 1);
            return SMSG_GENERAL_ERROR;
        }
    }

    uint32_t n = 12;

    for (uint32_t i = 0; i < nBunch; ++i) {
        if (vchData.size() - n < SMSG_HDR_LEN) {
            LogPrintf("Error: not enough data sent, n = %u.\n", n);
            break;
        }

        SecureMessage *psmsg = (SecureMessage*) &vchData[n];

        if (psmsg->nPayload > vchData.size() - n - SMSG_HDR_LEN) {
            LogPrintf("Error: nPayload %u exceeds available data in smsgMsg.\n", psmsg->nPayload);
            Misbehaving(pfrom->GetId(), 10);
            break;
        }

        int rv;
        if ((rv = Validate(&vchData[n], &vchData[n + SMSG_HDR_LEN], psmsg->nPayload)) != 0) {
            // Message dropped
            if (rv == SMSG_INVALID_HASH) { // Invalid proof of work
                Misbehaving(pfrom->GetId(), 10);
            } else
            if (rv == SMSG_FUND_FAILED) { // Bad funding tx
                Misbehaving(pfrom->GetId(), 10);
            } else {
                Misbehaving(pfrom->GetId(), 1);
            }
            n += SMSG_HDR_LEN + psmsg->nPayload;
            continue;
        }

        // Topic routing: for version-4 messages, check FNV-1a hash in nonce[0..3]
        if (psmsg->version[0] == SMSG_VERSION_TOPIC) {
            uint32_t topicHash;
            memcpy(&topicHash, psmsg->nonce, 4);
            LOCK(cs_smsgSubs);
            if (!IsSubscribedTopicHash(topicHash)) {
                n += SMSG_HDR_LEN + psmsg->nPayload;
                continue;
            }
        }

        {
            LOCK(cs_smsg);
            // Store message, but don't hash bucket
            if (Store(&vchData[n], &vchData[n + SMSG_HDR_LEN], psmsg->nPayload, false) != 0) {
                // Message dropped
                break;
            }

            if (ScanMessage(&vchData[n], &vchData[n + SMSG_HDR_LEN], psmsg->nPayload, true) != 0) {
                // message recipient is not this node (or failed)
            }
        } // cs_smsg

        n += SMSG_HDR_LEN + psmsg->nPayload;
    }

    {
        LOCK(cs_smsg);
        // If messages have been added, bucket must exist now
        itb = buckets.find(bktTime);
        if (itb == buckets.end()) {
            LogPrint(BCLog::SMSG, "Don't have bucket %d.\n", bktTime);
            return SMSG_GENERAL_ERROR;
        }

        itb->second.nLockCount  = 0; // This node has received data from peer, release lock
        itb->second.nLockPeerId = 0;
        itb->second.hashBucket();
    } // cs_smsg

    {
        LOCK(pfrom->smsgData.cs_smsg_net);
        if (nBunch <= pfrom->smsgData.m_num_want_sent) {
            pfrom->smsgData.m_num_want_sent -= nBunch;
        } else {
            pfrom->smsgData.m_num_want_sent = 0;
        }
    }

    return SMSG_NO_ERROR;
};

int CSMSG::CheckPurged(const SecureMessage *psmsg, const uint8_t *pPayload)
{
    if (setPurgedTimestamps.find(psmsg->timestamp) != setPurgedTimestamps.end()) {
        return SMSG_NO_ERROR;
    }

    std::vector<uint8_t> vMsgId = GetMsgID(psmsg, pPayload);

    uint8_t chKey[30];
    chKey[0] = 'p';
    chKey[1] = 'm';
    memcpy(chKey+2, vMsgId.data(), 28);

    LOCK2(cs_smsg, cs_smsgDB);

    SecMsgDB db;
    if (!db.Open("cr+")) {
        return SMSG_GENERAL_ERROR;
    }

    SecMsgPurged purged;
    if (db.ReadPurged(chKey, purged)) {
        LogPrint(BCLog::SMSG, "%s Found purged %s\n", __func__, HexStr(vMsgId));

        // Add sample to purged token
        memcpy(purged.sample, pPayload, 8);
        setPurged.insert(purged);

        return SMSG_PURGED_MSG;
    }

    return SMSG_NO_ERROR;
};

int CSMSG::StoreUnscanned(const uint8_t *pHeader, const uint8_t *pPayload, uint32_t nPayload)
{
    /*
    When the wallet is locked a copy of each received message is stored to be scanned later if wallet is unlocked
    */

    LogPrint(BCLog::SMSG, "%s\n", __func__);

    if (!pHeader
        || !pPayload) {
        return errorN(SMSG_GENERAL_ERROR, "%s - Null pointer to header or payload.", __func__);
    }

    SecureMessage *psmsg = (SecureMessage*) pHeader;

    if (SMSG_NO_ERROR != CheckPurged(psmsg, pPayload)) {
        return errorN(SMSG_PURGED_MSG, "%s: Purged message.", __func__);
    }

    fs::path pathSmsgDir;
    try {
        pathSmsgDir = GetDataDir() / "smsgstore";
        fs::create_directory(pathSmsgDir);
    } catch (const fs::filesystem_error &ex) {
        return errorN(SMSG_GENERAL_ERROR, "%s - Failed to create directory %s - %s.", __func__, pathSmsgDir.string(), ex.what());
    }

    int64_t now = GetAdjustedTime();
    if (psmsg->timestamp > now + SMSG_TIME_LEEWAY) {
        return errorN(SMSG_GENERAL_ERROR, "%s: Message > now.", __func__);
    }
    if (psmsg->timestamp < now - SMSG_RETENTION) {
        return errorN(SMSG_GENERAL_ERROR, "%s: Message < SMSG_RETENTION.", __func__);
    }

    int64_t bucket = psmsg->timestamp - (psmsg->timestamp % SMSG_BUCKET_LEN);

    std::string fileName = std::to_string(bucket) + "_01_wl.dat";
    fs::path fullpath = pathSmsgDir / fileName;

    FILE *fp;
    errno = 0;
    if (!(fp = fopen(fullpath.string().c_str(), "ab"))) {
        return errorN(SMSG_GENERAL_ERROR, "%s - Can't open file, strerror: %s.", __func__, strerror(errno));
    }

    if (fwrite(pHeader, sizeof(uint8_t), SMSG_HDR_LEN, fp) != (size_t)SMSG_HDR_LEN
        || fwrite(pPayload, sizeof(uint8_t), nPayload, fp) != nPayload) {
        fclose(fp);
        return errorN(SMSG_GENERAL_ERROR, "%s - fwrite failed, strerror: %s.", __func__, strerror(errno));
    }

    fclose(fp);
    return SMSG_NO_ERROR;
};


int CSMSG::Store(const uint8_t *pHeader, const uint8_t *pPayload, uint32_t nPayload, bool fHashBucket)
{
    LogPrint(BCLog::SMSG, "%s\n", __func__);
    AssertLockHeld(cs_smsg);

    if (!pHeader || !pPayload) {
        return errorN(SMSG_GENERAL_ERROR, "Null pointer to header or payload.");
    }

    SecureMessage *psmsg = (SecureMessage*) pHeader;

    if (SMSG_NO_ERROR != CheckPurged(psmsg, pPayload)) {
        return errorN(SMSG_PURGED_MSG, "%s: Purged message.", __func__);
    }

    long int ofs;
    fs::path pathSmsgDir;
    try {
        pathSmsgDir = GetDataDir() / "smsgstore";
        fs::create_directory(pathSmsgDir);
    } catch (const fs::filesystem_error &ex) {
        return errorN(SMSG_GENERAL_ERROR, "Failed to create directory %s - %s.", pathSmsgDir.string(), ex.what());
    }

    int64_t now = GetAdjustedTime();
    if (psmsg->timestamp > now + SMSG_TIME_LEEWAY) {
        return errorN(SMSG_GENERAL_ERROR, "%s: Message > now.", __func__);
    }
    if (psmsg->timestamp < now - SMSG_RETENTION) {
        return errorN(SMSG_GENERAL_ERROR, "%s: Message < SMSG_RETENTION.", __func__);
    }

    int64_t bucketTime = psmsg->timestamp - (psmsg->timestamp % SMSG_BUCKET_LEN);

    uint32_t nDaysToLive = psmsg->version[0] < 3 ? 2 : psmsg->nonce[0];
    SecMsgToken token(psmsg->timestamp, pPayload, nPayload, 0, nDaysToLive);

    SecMsgBucket &bucket = buckets[bucketTime];
    std::set<SecMsgToken> &tokenSet = bucket.setTokens;
    std::set<SecMsgToken>::iterator it;
    it = tokenSet.find(token);
    if (it != tokenSet.end()) {
        LogPrint(BCLog::SMSG, "Already have message.\n");

        if (LogAcceptCategory(BCLog::SMSG)) {
            LogPrintf("bucketTime: %d\n", bucketTime);
            LogPrintf("Message token: %s, nPayload %u\n", token.ToString(), nPayload);
        }
        return SMSG_GENERAL_ERROR;
    }

    std::string fileName = std::to_string(bucketTime) + "_01.dat";
    fs::path fullpath = pathSmsgDir / fileName;

    FILE *fp;
    errno = 0;
    if (!(fp = fopen(fullpath.string().c_str(), "ab"))) {
        return errorN(SMSG_GENERAL_ERROR, "fopen failed: %s.", strerror(errno));
    }

    // On windows ftell will always return 0 after fopen(ab), call fseek to set.
    errno = 0;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return errorN(SMSG_GENERAL_ERROR, "fseek failed: %s.", strerror(errno));
    }

    ofs = ftell(fp);

    if (fwrite(pHeader,  sizeof(uint8_t), SMSG_HDR_LEN, fp) != (size_t)SMSG_HDR_LEN
        || fwrite(pPayload, sizeof(uint8_t),     nPayload, fp) != nPayload) {
        fclose(fp);
        return errorN(SMSG_GENERAL_ERROR, "fwrite failed: %s.", strerror(errno));
    }

    fclose(fp);

    token.offset = ofs;

    tokenSet.insert(token);

    if (nDaysToLive > 0 && (bucket.nLeastTTL == 0 || nDaysToLive < bucket.nLeastTTL)) {
        bucket.nLeastTTL = nDaysToLive;
    }

    if (fHashBucket) {
        bucket.hashBucket();
    }

    LogPrint(BCLog::SMSG, "SecureMsg added to bucket %d.\n", bucketTime);

    return SMSG_NO_ERROR;
};

int CSMSG::Store(const SecureMessage &smsg, bool fHashBucket)
{
    return Store(smsg.data(), smsg.pPayload, smsg.nPayload, fHashBucket);
};

int CSMSG::Purge(std::vector<uint8_t> &vMsgId, std::string &sError)
{
    LogPrint(BCLog::SMSG, "%s %s\n", __func__, HexStr(vMsgId));

    LOCK(cs_smsg);
    LOCK(cs_smsgDB);
    SecMsgDB db;
    if (!db.Open("cw")) {
        return SMSG_GENERAL_ERROR;
    }
    db.TxnBegin();
    int64_t now = GetTime();
    int64_t msgtime;
    memcpy(&msgtime, vMsgId.data(), 8);
    msgtime = bswap_64(msgtime);
    SecMsgPurged purged(msgtime, now);

    uint8_t chKey[30];
    chKey[0] = 'i';
    chKey[1] = 'm';
    memcpy(chKey+2, vMsgId.data(), 28);
    db.EraseSmesg(chKey);

    // Find in buckets
    int64_t bucketTime = msgtime - (msgtime % SMSG_BUCKET_LEN);

    SecMsgBucket &bucket = buckets[bucketTime];
    std::set<SecMsgToken> &tokenSet = bucket.setTokens;

    std::vector<uint8_t> vchOne;
    // setTokens is ordered by (timestamp, sample): use lower_bound to skip to candidates.
    SecMsgToken probe;
    probe.timestamp = msgtime;
    memset(probe.sample, 0, 8);
    for (auto it = tokenSet.lower_bound(probe); it != tokenSet.end() && it->timestamp == msgtime; ++it) {
        if (Retrieve(*it, vchOne) != SMSG_NO_ERROR) {
            LogPrintf("%s: Retrieve failed, msgid: %s\n", __func__, HexStr(vMsgId));
            continue;
        }

        const SecureMessage *psmsg = (SecureMessage*) vchOne.data();
        if (GetMsgID(psmsg, vchOne.data() + SMSG_HDR_LEN) != vMsgId) {
            continue;
        }

        if (Remove(*it) != SMSG_NO_ERROR) {
            LogPrintf("%s: Remove failed, msgid: %s\n", __func__, HexStr(vMsgId));
            break;
        }
        memcpy(purged.sample, vchOne.data() + SMSG_HDR_LEN, 8);
        it->ttl = 0;
        LogPrint(BCLog::SMSG, "Purged message %s in bucket %d\n", it->ToString(), bucketTime);
        memcpy(purged.sample, it->sample, 8);

        break;
    }

    chKey[0] = 'p';
    db.WritePurged(chKey, purged);
    db.TxnCommit();

    setPurged.insert(purged);
    setPurgedTimestamps.insert(purged.timestamp); // So network sync can prefilter on timestamp before checking for purged msgid in db

    return SMSG_NO_ERROR;
};

int CSMSG::Validate(const uint8_t *pHeader, const uint8_t *pPayload, uint32_t nPayload)
{
    // return SecureMessageCodes
    SecureMessage *psmsg = (SecureMessage*) pHeader;

    if (psmsg->IsPaidVersion()) {
        if (nPayload > SMSG_MAX_MSG_BYTES_PAID)
            return SMSG_PAYLOAD_OVER_SIZE;
    } else
    if (psmsg->IsTopicVersion()) {
        if (nPayload > SMSG_MAX_MSG_WORST_TOPIC)
            return SMSG_PAYLOAD_OVER_SIZE;
    } else
    if (nPayload > SMSG_MAX_MSG_WORST) {
        return SMSG_PAYLOAD_OVER_SIZE;
    }

    if (nPayload == 0) {
        return errorN(SMSG_GENERAL_ERROR, "%s: Zero-length payload.", __func__);
    }

    int64_t now = GetAdjustedTime();
    if (psmsg->timestamp > now + SMSG_TIME_LEEWAY) {
        LogPrint(BCLog::SMSG, "Time in future %d.\n", psmsg->timestamp);
        return SMSG_TIME_IN_FUTURE;
    }

    if (psmsg->version[0] == 3) {
        if (Params().GetConsensus().nPaidSmsgTime > now) {
            LogPrintf("%s: Paid SMSG not yet active on mainnet.\n", __func__);
            return SMSG_GENERAL_ERROR;
        }

        size_t nDaysRetention = psmsg->nonce[0];
        int64_t ttl = SMSGGetSecondsInDay() * nDaysRetention;
        if (ttl < SMSGGetSecondsInDay() || ttl > SMSG_MAX_PAID_TTL) {
            LogPrint(BCLog::SMSG, "TTL out of range %d.\n", ttl);
            return SMSG_GENERAL_ERROR;
        }
        if (now > psmsg->timestamp + ttl) {
            LogPrint(BCLog::SMSG, "Time expired %d, ttl %d.\n", psmsg->timestamp, ttl);
            return SMSG_TIME_EXPIRED;
        }

        size_t nMsgBytes = SMSG_HDR_LEN + psmsg->nPayload;
        int64_t nExpectFee = ((nMsgFeePerKPerDay * nMsgBytes) / 1000) * nDaysRetention;

        uint256 txid;
        uint160 msgId;
        uint32_t nTailSize = psmsg->GetPaidTailSize();
        if (nPayload < SMSG_PL_HDR_LEN + nTailSize) {
            return SMSG_GENERAL_ERROR;
        }
        if (0 != HashMsg(*psmsg, pPayload, psmsg->nPayload - nTailSize, msgId)
            || !GetFundingTxid(pPayload, psmsg->nPayload, txid)) {
            LogPrintf("%s: Get msgID or Txn Hash failed.\n", __func__);
            return SMSG_GENERAL_ERROR;
        }

        CTransactionRef txOut;
        uint256 hashBlock;
        {
            LOCK(cs_main);
            txOut = GetTransaction(nullptr, nullptr, txid, Params().GetConsensus(), hashBlock);
            if (!txOut || hashBlock.IsNull()) {
                return errorN(SMSG_GENERAL_ERROR, "%s: Transaction %s not found for message %s.\n", __func__, txid.ToString(), msgId.ToString());
            }

            if (txOut->IsCoinBase()) {
                return errorN(SMSG_GENERAL_ERROR, "%s: Transaction %s for message %s, is coinbase.\n", __func__, txid.ToString(), msgId.ToString());
            }

            int blockDepth = -1;
            CBlockIndex *pindex = LookupBlockIndex(hashBlock);
            if (pindex && ::ChainActive().Contains(pindex)) {
                blockDepth = ::ChainActive().Height() - pindex->nHeight + 1;
            }

            if (blockDepth < 1) {
                return errorN(SMSG_GENERAL_ERROR, "%s: Transaction %s for message %s, low depth %d.\n", __func__, txid.ToString(), msgId.ToString(), blockDepth);
            }

            bool fFound = false;
            // Find OP_RETURN output with SMSG funding data.
            // Two formats are supported:
            //   Legacy:  'F' <20-byte msgId> <4-byte fee>   (plaintext)
            //   Blinded: 'B' <20-byte commitment>           (confidential)
            static const uint8_t DO_FUND_MSG = 'F';
            static const uint8_t DO_FUND_MSG_BLIND = 'B';
            for (const auto &v : txOut->vout) {
                if (v.scriptPubKey.size() < 23 || v.scriptPubKey[0] != OP_RETURN) {
                    continue;
                }
                const std::vector<uint8_t> vData(v.scriptPubKey.begin() + 2, v.scriptPubKey.end());
                if (vData.size() < 21) {
                    continue;
                }

                if (vData[0] == DO_FUND_MSG_BLIND) {
                    // Blinded format: 'B' <20-byte commitment>
                    // Retrieve blinding key from the message payload
                    std::vector<uint8_t> blindKey;
                    if (!GetFundingBlindKey(pPayload, psmsg->nPayload, blindKey)) {
                        continue;
                    }
                    // Recompute commitment: RIPEMD160(blindKey || msgId || fee_le32)
                    uint32_t feeLE = (uint32_t)nExpectFee;
                    uint160 expectedCommitment;
                    CRIPEMD160()
                        .Write(blindKey.data(), blindKey.size())
                        .Write(msgId.begin(), 20)
                        .Write((const uint8_t*)&feeLE, 4)
                        .Finalize(expectedCommitment.begin());

                    size_t n = (vData.size()-1) / 20;
                    for (size_t k = 0; k < n; ++k) {
                        uint160 commitTx;
                        memcpy(commitTx.begin(), &vData[1+k*20], 20);
                        if (commitTx == expectedCommitment) {
                            fFound = true;
                            break;
                        }
                    }
                } else if (vData[0] == DO_FUND_MSG && vData.size() >= 25) {
                    // Legacy plaintext format: 'F' <20-byte msgId> <4-byte fee>
                    size_t n = (vData.size()-1) / 24;
                    for (size_t k = 0; k < n; ++k) {
                        uint160 msgIdTx;
                        memcpy(msgIdTx.begin(), &vData[1+k*24], 20);
                        uint32_t nAmount;
                        memcpy(&nAmount, &vData[1+k*24+20], 4);

                        if (msgIdTx == msgId) {
                            if (nAmount < nExpectFee) {
                                LogPrintf("%s: Transaction %s underfunded message %s, expected %d paid %d.\n", __func__, txid.ToString(), msgId.ToString(), nExpectFee, nAmount);
                                return SMSG_FUND_FAILED;
                            }
                            fFound = true;
                        }
                    }
                }
                if (fFound) break;
            }

            if (!fFound) {
                return errorN(SMSG_FUND_FAILED, "%s: Transaction %s does not fund message %s.\n", __func__, txid.ToString(), msgId.ToString());
            }

        }

        return SMSG_NO_ERROR; // smsg is valid and funded
    }

    if (psmsg->version[0] != 2 && psmsg->version[0] != SMSG_VERSION_TOPIC)
        return SMSG_UNKNOWN_VERSION;

    // Topic messages need at least PL_HDR + topic_len(1) + min topic(7) + parent_msgid(20) + retention(2)
    if (psmsg->version[0] == SMSG_VERSION_TOPIC
        && nPayload < SMSG_PL_HDR_LEN + 1 + 7 + 20 + 2) {
        return SMSG_GENERAL_ERROR;
    }

    uint8_t civ[32];
    uint8_t sha256Hash[32];
    int rv = SMSG_INVALID_HASH; // invalid

    uint32_t nonce;
    memcpy(&nonce, &psmsg->nonce[0], 4);

    LogPrint(BCLog::SMSG, "%s: nonce %u.\n", __func__, nonce);

    for (int i = 0; i < 32; i+=4) {
        memcpy(civ+i, &nonce, 4);
    }

    CHMAC_SHA256 ctx(&civ[0], 32);
    ctx.Write((uint8_t*) pHeader+4, SMSG_HDR_LEN-4);
    ctx.Write((uint8_t*) pPayload, nPayload);
    ctx.Finalize(sha256Hash);

    if (sha256Hash[31] == 0
        && sha256Hash[30] == 0
        && (~(sha256Hash[29]) & ((1<<0) | (1<<1) | (1<<2)))) {
        LogPrint(BCLog::SMSG, "Hash Valid.\n");
        rv = SMSG_NO_ERROR; // smsg is valid
    }

    if (omega::memcmp_nta(psmsg->hash, sha256Hash, 4) != 0) {
        LogPrint(BCLog::SMSG, "Checksum mismatch.\n");
        rv = SMSG_CHECKSUM_MISMATCH; // checksum mismatch
    }

    return rv;
};

int CSMSG::SetHash(uint8_t *pHeader, uint8_t *pPayload, uint32_t nPayload)
{
    /*  proof of work and checksum

        May run in a thread, if shutdown detected, return.

        returns SecureMessageCodes
    */

    SecureMessage *psmsg = (SecureMessage*) pHeader;

    int64_t nStart = GetTimeMillis();
    uint8_t civ[32];
    uint8_t sha256Hash[32];

    bool found = false;

    uint32_t nonce = 0;
    memcpy(&nonce, &psmsg->nonce[0], 4);

    //CBigNum bnTarget(2);
    //bnTarget = bnTarget.pow(256 - 40);

    // Break for HMAC_CTX_cleanup
    for (;;) {
        if (!fSecMsgEnabled) {
           break;
        }

        //psmsg->timestamp = GetTime();
        //memcpy(&psmsg->timestamp, &now, 8);
        memcpy(&psmsg->nonce[0], &nonce, 4);

        for (int i = 0; i < 32; i+=4) {
            memcpy(civ+i, &nonce, 4);
        }

        CHMAC_SHA256 ctx(&civ[0], 32);
        ctx.Write((uint8_t*) pHeader+4, SMSG_HDR_LEN-4);
        ctx.Write((uint8_t*) pPayload, nPayload);
        ctx.Finalize(sha256Hash);

        if (sha256Hash[31] == 0
            && sha256Hash[30] == 0
            && (~(sha256Hash[29]) & ((1<<0) | (1<<1) | (1<<2)))) {
        //    && sha256Hash[29] == 0)
            found = true;
            //if (fDebugSmsg)
            //    LogPrintf("Match %u\n", nonce);
            break;
        }

        if (nonce >= 4294967295U) { // UINT32_MAX
            LogPrint(BCLog::SMSG, "No match %u\n", nonce);
            break;
        }
        nonce++;
    }

    if (!fSecMsgEnabled) {
        LogPrint(BCLog::SMSG, "%s: Stopped, shutdown detected.\n", __func__);
        return SMSG_SHUTDOWN_DETECTED;
    }

    if (!found) {
        LogPrint(BCLog::SMSG, "%s: Failed, took %d ms, nonce %u\n", __func__, GetTimeMillis() - nStart, nonce);
        return SMSG_GENERAL_ERROR;
    }

    memcpy(psmsg->hash, sha256Hash, 4);

    LogPrint(BCLog::SMSG, "%s: Took %d ms, nonce %u\n", __func__, GetTimeMillis() - nStart, nonce);

    return SMSG_NO_ERROR;
};

int CSMSG::Encrypt(SecureMessage &smsg, const CKeyID &addressFrom, const CKeyID &addressTo, const std::string &message,
    const std::string &topic, const uint160 &parentMsgId, uint16_t nTopicRetentionDays)
{
#ifdef ENABLE_WALLET
    /* Create a secure message

    Using similar method to bitmessage.
    If bitmessage is secure this should be too.
    https://bitmessage.org/wiki/Encryption

    Some differences:
    bitmessage seems to use curve sect283r1
    *coin addresses use secp256k1

    returns SecureMessageCodes

    */

    bool fSendAnonymous = addressFrom.IsNull();

    if (LogAcceptCategory(BCLog::SMSG)) {
        LogPrint(BCLog::SMSG, "SecureMsgEncrypt(%s, %s, ...)\n",
            fSendAnonymous ? "anon" : EncodeDestination(PKHash(addressFrom)),
            EncodeDestination(PKHash(addressTo)));
    }

    if (smsg.timestamp == 0) {
        smsg.timestamp = GetTime();
    }

    CKeyID ckidFrom;
    CKey keyFrom;

    if (!fSendAnonymous) {
        ckidFrom = addressFrom;
        if (ckidFrom.IsNull()) {
            return errorN(SMSG_INVALID_ADDRESS_FROM, "%s: addressFrom is not valid.", __func__);
        }
    }

    CKeyID ckidDest = addressTo;

    // Public key K is the destination address
    CPubKey cpkDestK;
    if (GetStoredKey(ckidDest, cpkDestK) != 0
        && GetLocalKey(ckidDest, cpkDestK) != 0) { // maybe it's a local key (outbox?)
        return errorN(SMSG_PUBKEY_NOT_EXISTS, "%s: Could not get public key for destination address.", __func__);
    }

    // Generate 16 random bytes as IV.
    GetStrongRandBytes(&smsg.iv[0], 16);

    // Generate a new random EC key pair with private key called r and public key called R.
    CKey keyR;
    keyR.MakeNewKey(true); // make compressed key

    //uint256 P = keyR.ECDH(cpkDestK);
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_parse(secp256k1_context_smsg, &pubkey, cpkDestK.begin(), cpkDestK.size())) {
        return errorN(SMSG_INVALID_ADDRESS_TO, "%s: secp256k1_ec_pubkey_parse failed: %s.", __func__, HexStr(cpkDestK));
    }

    uint256 P;
    if (!secp256k1_ecdh(secp256k1_context_smsg, P.begin(), &pubkey, keyR.begin(), nullptr, nullptr)) {
        return errorN(SMSG_GENERAL_ERROR, "%s: secp256k1_ecdh failed.", __func__);
    }

    CPubKey cpkR = keyR.GetPubKey();
    if (!cpkR.IsValid()) {
        return errorN(SMSG_GENERAL_ERROR, "%s: Could not get public key for key R.", __func__);
    }

    memcpy(smsg.cpkR, cpkR.begin(), 33);

    // Use public key P and calculate the SHA512 hash H.
    //   The first 32 bytes of H are called key_e and the last 32 bytes are called key_m.
    std::vector<uint8_t> vchHashed;
    vchHashed.resize(64); // 512
    memset(vchHashed.data(), 0, 64);
    CSHA512().Write(P.begin(), 32).Finalize(&vchHashed[0]);
    std::vector<uint8_t> key_e(&vchHashed[0], &vchHashed[0]+32);
    std::vector<uint8_t> key_m(&vchHashed[32], &vchHashed[32]+32);

    std::vector<uint8_t> vchPayload;
    std::vector<uint8_t> vchCompressed;
    uint8_t *pMsgData;
    uint32_t lenMsgData;

    uint32_t lenMsg = message.size();
    if (lenMsg > 128) {
        // Only compress if over 128 bytes
        int worstCase = LZ4_compressBound(message.size());
        try { vchCompressed.resize(worstCase); } catch (std::exception &e) {
            return errorN(SMSG_ALLOCATE_FAILED, "%s: vchCompressed.resize %u threw: %s.", __func__, worstCase, e.what());
        }

        int lenComp = LZ4_compress_default((char*)message.c_str(), (char*)vchCompressed.data(), lenMsg, worstCase);
        if (lenComp < 1) {
            return errorN(SMSG_COMPRESS_FAILED, "%s: Could not compress message data.", __func__);
        }

        pMsgData = vchCompressed.data();
        lenMsgData = lenComp;
    } else {
        // No compression
        pMsgData = (uint8_t*)message.c_str();
        lenMsgData = lenMsg;
    }

    // Validate and apply topic for version-4 messages
    if (!topic.empty()) {
        if (fSendAnonymous) {
            return errorN(SMSG_GENERAL_ERROR, "%s: Topic messages cannot be anonymous.", __func__);
        }
        if (!IsValidTopic(topic)) {
            return errorN(SMSG_GENERAL_ERROR, "%s: Invalid topic string.", __func__);
        }
        smsg.version[0] = SMSG_VERSION_TOPIC;
        smsg.version[1] = 0;
        // Store FNV-1a hash of topic in nonce[0..3] for cleartext routing
        uint32_t topicHash = SMSGTopicHash(topic);
        memcpy(smsg.nonce, &topicHash, 4);
    }

    if (fSendAnonymous) {
        try { vchPayload.resize(9 + lenMsgData); } catch (std::exception &e) {
            return errorN(SMSG_ALLOCATE_FAILED, "%s: vchPayload.resize %u threw: %s.", __func__, 9 + lenMsgData, e.what());
        }

        memcpy(&vchPayload[9], pMsgData, lenMsgData);

        vchPayload[0] = 250; // id as anonymous message
        // Next 4 bytes are unused - there to ensure encrypted payload always > 8 bytes
        memcpy(&vchPayload[5], &lenMsg, 4); // length of uncompressed plain text
    } else {
        // For topic messages: topic string + parent_msgid + retention_days after the fixed PL_HDR
        uint32_t topicExtra = 0;
        if (!topic.empty()) {
            topicExtra = (uint32_t)(1 + topic.size() + 20 + 2); // topic_len + topic + parent_msgid + retention_days
        }
        uint32_t totalSize  = SMSG_PL_HDR_LEN + topicExtra + lenMsgData;
        try { vchPayload.resize(totalSize); } catch (std::exception &e) {
            return errorN(SMSG_ALLOCATE_FAILED, "%s: vchPayload.resize %u threw: %s.", __func__, totalSize, e.what());
        }

        if (topicExtra > 0) {
            uint32_t off = SMSG_PL_HDR_LEN;
            vchPayload[off] = (uint8_t)topic.size();
            off += 1;
            memcpy(&vchPayload[off], topic.c_str(), topic.size());
            off += (uint32_t)topic.size();
            memcpy(&vchPayload[off], parentMsgId.begin(), 20);
            off += 20;
            memcpy(&vchPayload[off], &nTopicRetentionDays, 2);
        }
        memcpy(&vchPayload[SMSG_PL_HDR_LEN + topicExtra], pMsgData, lenMsgData);
        // Compact signature proves ownership of from address and allows the public key to be recovered, recipient can always reply.
        auto* spk_man = pwallet ? pwallet->GetLegacyScriptPubKeyMan() : nullptr;
        bool fGotKey = spk_man && spk_man->GetKey(ckidFrom, keyFrom);
        if (!fGotKey) {
            // Fallback: key may be a standalone SMSG keystore key (e.g. generated without wallet)
            fGotKey = (ReadSmsgKey(ckidFrom, keyFrom) == SMSG_NO_ERROR);
        }
        if (!fGotKey) {
            return errorN(SMSG_UNKNOWN_KEY_FROM, "%s: Could not get private key for addressFrom.", __func__);
        }

        // Sign the plaintext
        std::vector<uint8_t> vchSignature;
        vchSignature.resize(65);
        keyFrom.SignCompact(Hash(message.begin(), message.end()), vchSignature);

        // Save some bytes by sending address raw
        vchPayload[0] = Params().Base58Prefix(CChainParams::PUBKEY_ADDRESS)[0];
        memcpy(&vchPayload[1], ckidFrom.begin(), 20); // memcpy(&vchPayload[1], ckidDest.pn, 20);

        memcpy(&vchPayload[1+20], &vchSignature[0], vchSignature.size());
        memcpy(&vchPayload[1+20+65], &lenMsg, 4); // length of uncompressed plain text
    }

    SecMsgCrypter crypter;
    crypter.SetKey(key_e, smsg.iv);
    std::vector<uint8_t> vchCiphertext;

    if (!crypter.Encrypt(vchPayload.data(), vchPayload.size(), vchCiphertext)) {
        return errorN(SMSG_ENCRYPT_FAILED, "%s: Encrypt failed.", __func__);
    }

    uint32_t nTailSize = smsg.GetPaidTailSize();
    try { smsg.pPayload = new uint8_t[vchCiphertext.size() + nTailSize]; } catch (std::exception &e) {
        return errorN(SMSG_ALLOCATE_FAILED, "%s: Could not allocate pPayload, exception: %s.", __func__, e.what());
    }

    memcpy(smsg.pPayload, vchCiphertext.data(), vchCiphertext.size());
    smsg.nPayload = vchCiphertext.size() + nTailSize;

    // Calculate a 32 byte MAC with HMACSHA256, using key_m as salt
    //  Message authentication code, (hash of timestamp + iv + destination + payload)
    CHMAC_SHA256 ctx(&key_m[0], 32);
    ctx.Write((uint8_t*) &smsg.timestamp, sizeof(smsg.timestamp));
    ctx.Write((uint8_t*) smsg.iv, sizeof(smsg.iv));
    ctx.Write((uint8_t*) vchCiphertext.data(), vchCiphertext.size());
    ctx.Finalize(smsg.mac);

#endif
    return SMSG_NO_ERROR;
};

int CSMSG::Send(CKeyID &addressFrom, CKeyID &addressTo, std::string &message,
    SecureMessage &smsg, std::string &sError, bool fPaid,
    size_t nDaysRetention, bool fTestFee, CAmount *nFee, bool fFromFile,
    const std::string &topic, const uint160 &parentMsgId, uint16_t nTopicRetentionDays)
{
#ifdef ENABLE_WALLET
    /* Encrypt secure message, and place it on the network
        Make a copy of the message to sender's first address and place in send queue db
        proof of work thread will pick up messages from  send queue db
    */

    bool fSendAnonymous = (addressFrom.IsNull());

    if (LogAcceptCategory(BCLog::SMSG)) {
        LogPrintf("SecureMsgSend(%s, %s, ...)\n",
            fSendAnonymous ? "anon" : EncodeDestination(PKHash(addressFrom)), EncodeDestination(PKHash(addressTo)));
    }

    if (!pwallet && fPaid) {
        return errorN(SMSG_WALLET_LOCKED, sError, __func__, "Wallet is not enabled (required for paid messages)");
    }
    if (pwallet && pwallet->IsLocked()) {
        return errorN(SMSG_WALLET_LOCKED, sError, __func__, "Wallet is locked, wallet must be unlocked to send messages");
    }

    std::string sFromFile;
    if (fFromFile) {
        FILE *fp;
        errno = 0;
        if (!(fp = fopen(message.c_str(), "rb"))) {
            return errorN(SMSG_GENERAL_ERROR, sError, __func__, "fopen failed: %s", strerror(errno));
        }

        if (fseek(fp, 0, SEEK_END) != 0) {
            fclose(fp);
            return errorN(SMSG_GENERAL_ERROR, sError, __func__, "fseek failed: %s", strerror(errno));
        }

        int64_t ofs = ftell(fp);
        if (ofs > SMSG_MAX_MSG_BYTES_PAID) {
            fclose(fp);
            return errorN(SMSG_MESSAGE_TOO_LONG, sError, __func__, "Message is too long, %d > %d", ofs, SMSG_MAX_MSG_BYTES_PAID);
        }
        rewind(fp);

        sFromFile.resize(ofs);

        int64_t nRead = fread(&sFromFile[0], 1, ofs, fp);
        fclose(fp);
        if (ofs != nRead) {
            return errorN(SMSG_GENERAL_ERROR, sError, __func__, "fread failed: %s", strerror(errno));
        }
    }

    std::string &sData = fFromFile ? sFromFile : message;

    if (fPaid) {
        if (sData.size() > SMSG_MAX_MSG_BYTES_PAID) {
            sError = strprintf("Message is too long, %d > %d", sData.size(), SMSG_MAX_MSG_BYTES_PAID);
            return errorN(SMSG_MESSAGE_TOO_LONG, "%s: %s.", __func__, sError);
        }
    } else
    if (sData.size() > (fSendAnonymous ? SMSG_MAX_AMSG_BYTES : SMSG_MAX_MSG_BYTES)) {
        sError = strprintf("Message is too long, %d > %d", sData.size(), fSendAnonymous ? SMSG_MAX_AMSG_BYTES : SMSG_MAX_MSG_BYTES);
        return errorN(SMSG_MESSAGE_TOO_LONG, "%s: %s.", __func__, sError);
    }

    int rv;
    bool fBlinded = false;
    if (fPaid) {
        LOCK(cs_main);
        fBlinded = ::ChainActive().Height() >= Params().GetConsensus().nConfidentialSmsgHeight;
    }
    if (!topic.empty()) {
        if (fPaid) {
            return errorN(SMSG_GENERAL_ERROR, sError, __func__, "Topic messages cannot be paid messages.");
        }
        if (!IsValidTopic(topic)) {
            return errorN(SMSG_GENERAL_ERROR, sError, __func__, "Invalid topic string.");
        }
        // Broadcast mode: if no explicit recipient, encrypt to the topic's shared key
        if (addressTo.IsNull()) {
            CKey topicKey = GetTopicSharedKey(topic);
            addressTo = topicKey.GetPubKey().GetID();
        }
    }

    smsg.~SecureMessage();
    new (&smsg) SecureMessage(fPaid, nDaysRetention, fBlinded);
    if ((rv = Encrypt(smsg, addressFrom, addressTo, sData, topic, parentMsgId, nTopicRetentionDays)) != 0) {
        sError = GetString(rv);
        return errorN(rv, "%s: %s.", __func__, sError);
    }

    // Place message in send queue, proof of work will happen in a thread.
    uint160 msgId;

    if (fPaid) {
        // Compute msgId once here — FundMsg receives it to avoid a second HashMsg.
        if (0 != HashMsg(smsg, smsg.pPayload, smsg.nPayload - smsg.GetPaidTailSize(), msgId)) {
            return errorN(SMSG_GENERAL_ERROR, sError, __func__, "HashMsg failed.");
        }
        if (fTestFee) {
            // Fee estimation only: do not commit tx or persist queue record.
            if (0 != FundMsg(smsg, sError, true, nFee, &msgId)) {
                return errorN(SMSG_FUND_FAILED, "%s: SecureMsgFund failed %s.", __func__, sError);
            }
            return SMSG_NO_ERROR;
        }
    } else {
        // HACK: Premine so hash unpaid outbox hashes match, remove with unpaid messages
        if (SMSG_NO_ERROR != SetHash((uint8_t*)&smsg, smsg.pPayload, smsg.nPayload)) {
            return errorN(SMSG_FUND_FAILED, "%s: SetHash failed %s.", __func__, sError);
        }
        // SetHash modifies nonce (PoW), so HashMsg must run after.
        HashMsg(smsg, smsg.pPayload, smsg.nPayload - smsg.GetPaidTailSize(), msgId);
    }

    std::string sPrefix("qm");
    uint8_t chKey[30];
    int64_t timestamp_be = bswap_64(smsg.timestamp);
    memcpy(&chKey[0], sPrefix.data(), 2);
    memcpy(&chKey[2], &timestamp_be, 8);
    memcpy(&chKey[10], msgId.begin(), 20);

    SecMsgStored smsgSQ;
    smsgSQ.timeReceived  = GetTime();
    smsgSQ.addrTo        = addressTo;

    try { smsgSQ.vchMessage.resize(SMSG_HDR_LEN + smsg.nPayload); } catch (std::exception &e) {
        LogPrintf("smsgSQ.vchMessage.resize %u threw: %s.\n", SMSG_HDR_LEN + smsg.nPayload, e.what());
        sError = "Could not allocate memory.";
        return SMSG_ALLOCATE_FAILED;
    }

    memcpy(&smsgSQ.vchMessage[0], smsg.data(), SMSG_HDR_LEN);
    memcpy(&smsgSQ.vchMessage[SMSG_HDR_LEN], smsg.pPayload, smsg.nPayload);
    // For paid messages payload tail is zeroed here; txid is unknown until FundMsg completes.

    if (fPaid) {
        // Persist qm* record before committing the funding tx. Zero txid in tail acts as a sentinel:
        // the PoW thread detects it on restart and re-funds, closing the crash-between-commit-and-persist window.
        {
            LOCK(cs_smsgDB);
            SecMsgDB dbSendQueue;
            if (!dbSendQueue.Open("cw") || !dbSendQueue.WriteSmesg(chKey, smsgSQ)) {
                return errorN(SMSG_GENERAL_ERROR, sError, __func__, "Failed to write pre-fund queue record.");
            }
        }

        if (0 != FundMsg(smsg, sError, false, nFee, &msgId)) {
            // Fund failed: remove the unfunded record so the PoW thread does not attempt recovery.
            {
                LOCK(cs_smsgDB);
                SecMsgDB dbRollback;
                if (dbRollback.Open("cw")) {
                    dbRollback.EraseSmesg(chKey);
                }
            }
            return errorN(SMSG_FUND_FAILED, "%s: SecureMsgFund failed %s.", __func__, sError);
        }

        // Overwrite record with funded payload (tail now contains txid, and blinding key if blinded).
        memcpy(&smsgSQ.vchMessage[SMSG_HDR_LEN], smsg.pPayload, smsg.nPayload);
        {
            LOCK(cs_smsgDB);
            SecMsgDB dbSendQueue;
            if (dbSendQueue.Open("cw")) {
                dbSendQueue.WriteSmesg(chKey, smsgSQ);
            }
        }
    } else {
        LOCK(cs_smsgDB);
        SecMsgDB dbSendQueue;
        if (dbSendQueue.Open("cw")) {
            dbSendQueue.WriteSmesg(chKey, smsgSQ);
            //NotifySecMsgSendQueueChanged(smsgOutbox);
        }
    }

    //  For outbox create a copy encrypted for owned address
    //   if the wallet is encrypted private key needed to decrypt will be unavailable

    LogPrint(BCLog::SMSG, "Encrypting message for outbox.\n");

    CKeyID addressOutbox;

    if (pwallet) {
        addressOutbox = m_cachedOutboxAddr;
        if (addressOutbox.IsNull()) {
            LOCK(pwallet->cs_wallet);
            for (const auto &entry : pwallet->mapAddressBook) {
                if (!pwallet->IsMine(entry.first))
                    continue;
                const PKHash *pkHash = std::get_if<PKHash>(&entry.first);
                if (!pkHash)
                    continue;
                addressOutbox = CKeyID(*pkHash);
                m_cachedOutboxAddr = addressOutbox;
                break;
            }
        }
    }

    // Fallback: use the sender's own address for outbox encryption
    if (addressOutbox.IsNull()) {
        addressOutbox = addressFrom;
    }

    if (addressOutbox.IsNull()) {
        LogPrintf("%s: Warning, could not find an address to encrypt outbox message with.\n", __func__);
    } else {
        if (LogAcceptCategory(BCLog::SMSG)) {
            LogPrintf("Encrypting a copy for outbox, using address %s\n", EncodeDestination(PKHash(addressOutbox)));
        }

        SecureMessage smsgForOutbox(fPaid, nDaysRetention, fBlinded);
        smsgForOutbox.timestamp = smsg.timestamp;
        if ((rv = Encrypt(smsgForOutbox, addressFrom, addressOutbox, sData)) != 0) {
            LogPrintf("%s: Encrypt for outbox failed, %d.\n", __func__, rv);
        } else {
            if (fPaid) {
                uint256 txfundId;
                if (!smsg.GetFundingTxid(txfundId)) {
                    return errorN(SMSG_GENERAL_ERROR, "%s: GetFundingTxid failed.\n");
                }
                // Copy funding txid to outbox message tail
                memcpy(smsgForOutbox.pPayload + smsgForOutbox.nPayload - 32, txfundId.begin(), 32);
                if (fBlinded) {
                    // Copy blinding key to outbox message tail
                    memcpy(smsgForOutbox.pPayload + smsgForOutbox.nPayload - 64,
                           smsg.pPayload + smsg.nPayload - 64, SMSG_BLIND_KEY_LEN);
                }
            }

            // Save sent message to db
            std::string sPrefix("sm");
            uint8_t chKey[30];
            int64_t timestamp_be = bswap_64(smsgForOutbox.timestamp);
            memcpy(&chKey[0], sPrefix.data(), 2);
            memcpy(&chKey[2], &timestamp_be, 8);
            memcpy(&chKey[10], msgId.begin(), 20);

            SecMsgStored smsgOutbox;

            smsgOutbox.timeReceived  = GetTime();
            smsgOutbox.addrTo        = addressTo;
            smsgOutbox.addrOutbox    = addressOutbox;

            try {
                smsgOutbox.vchMessage.resize(SMSG_HDR_LEN + smsgForOutbox.nPayload);
            } catch (std::exception &e) {
                LogPrintf("smsgOutbox.vchMessage.resize %u threw: %s.\n", SMSG_HDR_LEN + smsgForOutbox.nPayload, e.what());
                sError = "Could not allocate memory.";
                return SMSG_ALLOCATE_FAILED;
            }
            memcpy(&smsgOutbox.vchMessage[0], &smsgForOutbox.hash[0], SMSG_HDR_LEN);
            memcpy(&smsgOutbox.vchMessage[SMSG_HDR_LEN], smsgForOutbox.pPayload, smsgForOutbox.nPayload);

            bool fNotifyOutbox = false;
            {
                LOCK(cs_smsgDB);
                SecMsgDB dbSent;

                if (dbSent.Open("cw")) {
                    dbSent.WriteSmesg(chKey, smsgOutbox);
                    fNotifyOutbox = true;
                }
            } // cs_smsgDB
            if (fNotifyOutbox) {
                NotifySecMsgOutboxChanged(smsgOutbox);
            }
        }
    }

    if (LogAcceptCategory(BCLog::SMSG)) {
        LogPrintf("Secure message queued for sending to %s.\n", EncodeDestination(PKHash(addressTo)));
    }

#endif
    return SMSG_NO_ERROR;
};

int CSMSG::HashMsg(const SecureMessage &smsg, const uint8_t *pPayload, uint32_t nPayload, uint160 &hash)
{
    if (smsg.nPayload < nPayload) {
        return errorN(SMSG_GENERAL_ERROR, "%s: Data length mismatch.\n", __func__);
    }

    CRIPEMD160()
        .Write(smsg.data(), SMSG_HDR_LEN)
        .Write(pPayload, nPayload)
        .Finalize(hash.begin());

    return SMSG_NO_ERROR;
};

int CSMSG::FundMsg(SecureMessage &smsg, std::string &sError, bool fTestFee, CAmount *nFee, const uint160 *pPrecomputedMsgId)
{
#ifdef ENABLE_WALLET
    auto w = pwallet;
    if (!w) {
        return SMSG_WALLET_UNSET;
    }

    if (!w->GetBroadcastTransactions()) {
        return errorN(SMSG_GENERAL_ERROR, sError, __func__, "Broadcast transactions disabled.");
    }

    if (smsg.version[0] != 3) {
        return errorN(SMSG_UNKNOWN_VERSION, sError, __func__, "Bad message version.");
    }

    size_t nDaysRetention = smsg.nonce[0];
    if (nDaysRetention < 1 || nDaysRetention > 31) {
        return errorN(SMSG_GENERAL_ERROR, sError, __func__, "Bad message ttl.");
    }

    bool fBlinded = smsg.IsBlindedPaid();
    uint32_t nTailSize = smsg.GetPaidTailSize();

    uint256 txfundId;
    uint160 msgId;
    CMutableTransaction txFund;

    if (pPrecomputedMsgId) {
        msgId = *pPrecomputedMsgId;
    } else if (0 != HashMsg(smsg, smsg.pPayload, smsg.nPayload - nTailSize, msgId)) {
        return errorN(SMSG_GENERAL_ERROR, sError, __func__, "Message hash failed.");
    }

    txFund.nVersion = 2;

    size_t nMsgBytes = SMSG_HDR_LEN + smsg.nPayload;
    CAmount nMsgFee = ((nMsgFeePerKPerDay * nMsgBytes) / 1000) * nDaysRetention;

    CCoinControl coinControl;
    coinControl.m_feerate = CFeeRate(nFundingTxnFeePerK);
    coinControl.fOverrideFeeRate = true;

    if (nMsgFee > std::numeric_limits<uint32_t>::max()) {
        return errorN(SMSG_GENERAL_ERROR, sError, __func__, "Computed message fee overflow.");
    }
    uint32_t msgFee = nMsgFee;

    CScript scriptData;
    if (fBlinded) {
        // Generate random blinding key
        std::vector<uint8_t> blindKey(SMSG_BLIND_KEY_LEN);
        GetStrongRandBytes(blindKey.data(), SMSG_BLIND_KEY_LEN);

        // Compute commitment: RIPEMD160(blindKey || msgId || fee_le32)
        uint160 commitment;
        CRIPEMD160()
            .Write(blindKey.data(), SMSG_BLIND_KEY_LEN)
            .Write(msgId.begin(), 20)
            .Write((const uint8_t*)&msgFee, 4)
            .Finalize(commitment.begin());

        // Store blinding key in payload tail
        memcpy(smsg.pPayload + (smsg.nPayload - 64), blindKey.data(), SMSG_BLIND_KEY_LEN);

        // Build OP_RETURN: 'B' <20-byte commitment>
        std::vector<uint8_t> vData(21);
        vData[0] = 'B';
        memcpy(&vData[1], commitment.begin(), 20);
        scriptData << OP_RETURN << vData;
    } else {
        // Legacy plaintext format: 'F' <20-byte msgId> <4-byte fee>
        std::vector<uint8_t> vData(25);
        vData[0] = 'F';
        memcpy(&vData[1], msgId.begin(), 20);
        memcpy(&vData[21], &msgFee, 4);
        scriptData << OP_RETURN << vData;
    }

    CTxOut out0(nMsgFee, scriptData);
    txFund.vout.push_back(out0);

    int nChangePosInOut = -1;
    CAmount nFeeRet;
    const std::set<int> setSubtractFeeFromOutputs;

    {
        LOCK(w->cs_wallet);
        bilingual_str biError;
        if (!w->FundTransaction(txFund, nFeeRet, nChangePosInOut, biError, false, setSubtractFeeFromOutputs, coinControl)) {
            sError = biError.original;
            return errorN(SMSG_GENERAL_ERROR, "%s: FundTransaction failed.\n", __func__);
        }

        if (nFee) {
            *nFee = nFeeRet + nMsgFee;
        }

        if (fTestFee) {
            return SMSG_NO_ERROR;
        }

        if (!w->SignTransaction(txFund)) {
            return errorN(SMSG_GENERAL_ERROR, sError, __func__, "SignTransaction failed.");
        }

        txfundId = txFund.GetHash();

        mapValue_t mapValueSMSG;
        mapValueSMSG["smsg_msgid"] = HexStr(msgId);
        CTransactionRef txRef = MakeTransactionRef(txFund);
        if (!w->CommitTransaction(txRef, std::move(mapValueSMSG), {})) {
            return errorN(SMSG_GENERAL_ERROR, sError, __func__, "CommitTransaction failed; funding transaction not persisted.");
        }
    }
    // Store funding txid at the end of the payload tail
    memcpy(smsg.pPayload + (smsg.nPayload - 32), txfundId.begin(), 32);
#else
    return SMSG_WALLET_UNSET;
#endif
    return SMSG_NO_ERROR;
};

uint256 CSMSG::FindFundingTx(const uint160 &msgId) const
{
#ifdef ENABLE_WALLET
    auto w = pwallet;
    if (!w) return uint256();
    LOCK(w->cs_wallet);
    const std::string sFind = HexStr(msgId);
    for (const auto &entry : w->mapWallet) {
        const auto it = entry.second.mapValue.find("smsg_msgid");
        if (it != entry.second.mapValue.end() && it->second == sFind) {
            return entry.first;
        }
    }
#endif
    return uint256();
};

std::vector<uint8_t> CSMSG::GetMsgID(const SecureMessage *psmsg, const uint8_t *pPayload)
{
    std::vector<uint8_t> rv(28);
    int64_t timestamp_be = bswap_64(psmsg->timestamp);
    memcpy(rv.data(), &timestamp_be, 8);

    HashMsg(*psmsg, pPayload, psmsg->nPayload - psmsg->GetPaidTailSize(), *((uint160*)&rv[8]));

    return rv;
};

std::vector<uint8_t> CSMSG::GetMsgID(const SecureMessage &smsg)
{
    std::vector<uint8_t> rv(28);
    int64_t timestamp_be = bswap_64(smsg.timestamp);
    memcpy(rv.data(), &timestamp_be, 8);

    HashMsg(smsg, smsg.pPayload, smsg.nPayload - smsg.GetPaidTailSize(), *((uint160*)&rv[8]));

    return rv;
};

int CSMSG::Decrypt(bool fTestOnly, const CKey &keyDest, const CKeyID &address, const uint8_t *pHeader, const uint8_t *pPayload, uint32_t nPayload, MessageData &msg)
{
    /* Decrypt secure message

        address is the owned address to decrypt with.

        validate first in SecureMsgValidate

        returns SecureMessageErrors
    */

    if (LogAcceptCategory(BCLog::SMSG)) {
        LogPrintf("%s: using %s, testonly %d.\n", __func__, EncodeDestination(PKHash(address)), fTestOnly);
    }

    if (!pHeader
        || !pPayload) {
        return errorN(SMSG_GENERAL_ERROR, "%s: null pointer to header or payload.", __func__);
    }

    SecureMessage *psmsg = (SecureMessage*) pHeader;
    if (psmsg->version[0] == 3) {
        nPayload -= psmsg->GetPaidTailSize(); // Exclude funding tail (blind key + txid)
    } else
    if (psmsg->version[0] != 2 && psmsg->version[0] != SMSG_VERSION_TOPIC) {
        return errorN(SMSG_UNKNOWN_VERSION, "%s: Unknown version number.", __func__);
    }

    // Do an EC point multiply with private key k and public key R. This gives you public key P.
    //CPubKey R(psmsg->cpkR, psmsg->cpkR+33);
    //uint256 P = keyDest.ECDH(R);
    secp256k1_pubkey R;
    if (!secp256k1_ec_pubkey_parse(secp256k1_context_smsg, &R, psmsg->cpkR, 33)) {
        return errorN(SMSG_GENERAL_ERROR, "%s: secp256k1_ec_pubkey_parse failed: %s.", __func__, HexStr(Span<const uint8_t>(psmsg->cpkR, 33)));
    }

    uint256 P;
    if (!secp256k1_ecdh(secp256k1_context_smsg, P.begin(), &R, keyDest.begin(), nullptr, nullptr)) {
        return errorN(SMSG_GENERAL_ERROR, "%s: secp256k1_ecdh failed.", __func__);
    }

    // Use public key P to calculate the SHA512 hash H.
    //  The first 32 bytes of H are called key_e and the last 32 bytes are called key_m.
    std::vector<uint8_t> vchHashedDec;
    vchHashedDec.resize(64);    // 512 bits
    memset(vchHashedDec.data(), 0, 64);
    CSHA512().Write(P.begin(), 32).Finalize(&vchHashedDec[0]);
    std::vector<uint8_t> key_e(&vchHashedDec[0], &vchHashedDec[0]+32);
    std::vector<uint8_t> key_m(&vchHashedDec[32], &vchHashedDec[32]+32);

    // Message authentication code, (hash of timestamp + iv + destination + payload)
    uint8_t MAC[32];

    CHMAC_SHA256 ctx(key_m.data(), 32);
    ctx.Write((uint8_t*) &psmsg->timestamp, sizeof(psmsg->timestamp));
    ctx.Write((uint8_t*) psmsg->iv, sizeof(psmsg->iv));
    ctx.Write((uint8_t*) pPayload, nPayload);
    ctx.Finalize(MAC);

    if (omega::memcmp_nta(MAC, psmsg->mac, 32) != 0) {
        LogPrint(BCLog::SMSG, "MAC does not match.\n"); // expected if message is not to address on node
        return SMSG_MAC_MISMATCH;
    }

    if (fTestOnly) {
        return SMSG_NO_ERROR;
    }

    SecMsgCrypter crypter;
    crypter.SetKey(key_e, psmsg->iv);
    std::vector<uint8_t> vchPayload;
    if (!crypter.Decrypt(pPayload, nPayload, vchPayload)) {
        return errorN(SMSG_GENERAL_ERROR, "%s: Decrypt failed.", __func__);
    }

    if (vchPayload.empty()) {
        return errorN(SMSG_GENERAL_ERROR, "%s: Decrypted payload is empty.", __func__);
    }

    msg.timestamp = psmsg->timestamp;
    uint32_t lenData, lenPlain;

    uint8_t *pMsgData;
    bool fFromAnonymous;
    if (psmsg->version[0] != SMSG_VERSION_TOPIC && (uint32_t)vchPayload[0] == 250) {
        if (vchPayload.size() < 9) {
            return errorN(SMSG_GENERAL_ERROR, "%s: Anonymous payload too short.", __func__);
        }
        fFromAnonymous = true;
        lenData = vchPayload.size() - (9);
        memcpy(&lenPlain, &vchPayload[5], 4);
        pMsgData = &vchPayload[9];
    } else {
        fFromAnonymous = false;
        // For topic messages: topic string + parent_msgid + retention_days follow the fixed PL_HDR
        uint32_t topicOffset = 0;
        if (psmsg->version[0] == SMSG_VERSION_TOPIC) {
            if (vchPayload.size() <= SMSG_PL_HDR_LEN) {
                return errorN(SMSG_GENERAL_ERROR, "%s: Payload too small for topic message.", __func__);
            }
            uint8_t topic_len = vchPayload[SMSG_PL_HDR_LEN];
            if (topic_len == 0 || topic_len > SMSG_MAX_TOPIC_LEN
                || vchPayload.size() < SMSG_PL_HDR_LEN + 1 + (uint32_t)topic_len + 20 + 2) {
                return errorN(SMSG_GENERAL_ERROR, "%s: Invalid topic length in payload.", __func__);
            }
            uint32_t off = SMSG_PL_HDR_LEN + 1;
            msg.sTopic = std::string((char*)&vchPayload[off], topic_len);
            off += (uint32_t)topic_len;
            if (!IsValidTopic(msg.sTopic)) {
                return errorN(SMSG_GENERAL_ERROR, "%s: Invalid topic string in payload.", __func__);
            }
            memcpy(msg.parentMsgId.begin(), &vchPayload[off], 20);
            off += 20;
            memcpy(&msg.nRetentionDays, &vchPayload[off], 2);
            topicOffset = 1 + (uint32_t)topic_len + 20 + 2;
        }
        if (vchPayload.size() < (size_t)(SMSG_PL_HDR_LEN + topicOffset)) {
            return errorN(SMSG_GENERAL_ERROR, "%s: Payload too short for header.", __func__);
        }
        lenData = vchPayload.size() - (SMSG_PL_HDR_LEN + topicOffset);
        memcpy(&lenPlain, &vchPayload[1+20+65], 4);
        pMsgData = &vchPayload[SMSG_PL_HDR_LEN + topicOffset];
    }

    {
        uint32_t maxPlain = (psmsg->version[0] == 3) ? SMSG_MAX_MSG_BYTES_PAID : SMSG_MAX_MSG_BYTES;
        if (lenPlain > maxPlain) {
            return errorN(SMSG_GENERAL_ERROR, "%s: lenPlain %u exceeds limit.", __func__, lenPlain);
        }
    }
    try {
        msg.vchMessage.resize(lenPlain + 1);
    } catch (std::exception &e) {
        return errorN(SMSG_ALLOCATE_FAILED, "%s: msg.vchMessage.resize %u threw: %s.", __func__, lenPlain + 1, e.what());
    }

    if (lenPlain > 128) {
        // Decompress
        if (LZ4_decompress_safe((char*) pMsgData, (char*) &msg.vchMessage[0], lenData, lenPlain) != (int) lenPlain) {
            return errorN(SMSG_GENERAL_ERROR, "%s: Could not decompress message data.", __func__);
        }
    } else {
        // Plaintext
        memcpy(&msg.vchMessage[0], pMsgData, lenPlain);
    }

    msg.vchMessage[lenPlain] = '\0';

    if (fFromAnonymous) {
        // Anonymous sender
        msg.sFromAddress = "anon";
    } else {
        std::vector<uint8_t> vchUint160;
        vchUint160.resize(20);

        memcpy(&vchUint160[0], &vchPayload[1], 20);

        uint160 ui160(vchUint160);
        CKeyID ckidFrom(ui160);

        if (ckidFrom.IsNull()) {
            return errorN(SMSG_INVALID_ADDRESS, "%s: From Address is invalid.", __func__);
        }

        std::vector<uint8_t> vchSig;
        vchSig.resize(65);

        memcpy(&vchSig[0], &vchPayload[1+20], 65);

        CPubKey cpkFromSig;
        cpkFromSig.RecoverCompact(Hash(msg.vchMessage.begin(), msg.vchMessage.end()-1), vchSig);
        if (!cpkFromSig.IsValid()) {
            return errorN(SMSG_GENERAL_ERROR, "%s: Signature validation failed.", __func__);
        }

        // Get address for the compressed public key
        CKeyID coinAddrFromSig = cpkFromSig.GetID();

        if (ckidFrom != coinAddrFromSig) {
            return errorN(SMSG_GENERAL_ERROR, "%s: Signature validation failed.", __func__);
        }

        int rv = SMSG_GENERAL_ERROR;
        try {
            rv = InsertAddress(ckidFrom, cpkFromSig);
        } catch (std::exception &e) {
            LogPrintf("%s, exception: %s.\n", __func__, e.what());
            //return 1;
        }

        if (rv != SMSG_NO_ERROR) {
            if (rv == SMSG_PUBKEY_EXISTS) {
                LogPrint(BCLog::SMSG, "%s: Sender public key not added to db, %s.\n", __func__, GetString(rv));
            } else {
                LogPrintf("%s: Sender public key not added to db, %s.\n", __func__, GetString(rv));
            }
        }

        msg.sFromAddress = EncodeDestination(PKHash(ckidFrom));
    }

    if (LogAcceptCategory(BCLog::SMSG)) {
        LogPrintf("Decrypted message for %s.\n", EncodeDestination(PKHash(address)));
    }

    return SMSG_NO_ERROR;
};

int CSMSG::Decrypt(bool fTestOnly, const CKey &keyDest, const CKeyID &address, const SecureMessage &smsg, MessageData &msg)
{
    return CSMSG::Decrypt(fTestOnly, keyDest, address, smsg.data(), smsg.pPayload, smsg.nPayload, msg);
};

int CSMSG::Decrypt(bool fTestOnly, const CKeyID &address, const uint8_t *pHeader, const uint8_t *pPayload, uint32_t nPayload, MessageData &msg)
{
    // Fetch private key k, used to decrypt
    CKey keyDest;
    ReadSmsgKey(address, keyDest);

#ifdef ENABLE_WALLET
    if (!keyDest.IsValid()) {
        if (!pwallet) return SMSG_WALLET_UNSET;
        if (pwallet->IsLocked()) {
            return SMSG_WALLET_LOCKED;
        }
        auto *spk_dec = pwallet->GetLegacyScriptPubKeyMan();
        if (spk_dec) spk_dec->GetKey(address, keyDest);
    }
#endif
    if (!keyDest.IsValid()) {
        return errorN(SMSG_UNKNOWN_KEY, "%s: Could not get private key for addressDest.", __func__);
    }

    return CSMSG::Decrypt(fTestOnly, keyDest, address, pHeader, pPayload, nPayload, msg);
};

int CSMSG::Decrypt(bool fTestOnly, const CKeyID &address, const SecureMessage &smsg, MessageData &msg)
{
    return CSMSG::Decrypt(fTestOnly, address, smsg.data(), smsg.pPayload, smsg.nPayload, msg);
};

bool CSMSG::IsSubscribedTopicHash(uint32_t hash) const
{
    return m_subscribed_topic_hashes.count(hash) > 0;
};

CKey CSMSG::GetTopicSharedKey(const std::string &topic)
{
    // Deterministic key: SHA256("omega_topic_key:" + topic)
    uint256 h;
    CSHA256().Write((const uint8_t*)"omega_topic_key:", 16)
             .Write((const uint8_t*)topic.data(), topic.size())
             .Finalize(h.begin());
    CKey key;
    key.Set(h.begin(), h.end(), true);
    return key;
};

CKeyID CSMSG::ImportTopicKey(const std::string &topic)
{
    CKey topicKey = GetTopicSharedKey(topic);
    if (!topicKey.IsValid()) {
        LogPrintf("%s: derived key invalid for topic %s\n", __func__, topic);
        return CKeyID();
    }
    CKeyID addr = topicKey.GetPubKey().GetID();
    if (!keyStore.HaveKey(addr)) {
        SecMsgKey smsgKey;
        smsgKey.key = topicKey;
        smsgKey.sLabel = "topic:" + topic;
        smsgKey.nFlags = SMK_RECEIVE_ON | SMK_RECEIVE_ANON;
        keyStore.AddKey(addr, smsgKey);
        LOCK(cs_smsgDB);
        SecMsgDB db;
        if (db.Open("cr+")) {
            db.WriteKey(addr, smsgKey);
        }
        LogPrintf("Imported shared key for topic %s → %s\n", topic, EncodeDestination(PKHash(addr)));
    }
    return addr;
};

bool CSMSG::SubscribeTopic(const std::string &topic, std::string &sError)
{
    if (!IsValidTopic(topic)) {
        sError = "Invalid topic string.";
        return false;
    }
    // Import the topic's shared key so this node can decrypt broadcast messages
    ImportTopicKey(topic);
    {
        LOCK(cs_smsgSubs);
        m_subscribed_topics.insert(topic);
        m_subscribed_topic_hashes.insert(SMSGTopicHash(topic));
    }
    return SaveTopicSubs();
};

bool CSMSG::UnsubscribeTopic(const std::string &topic, std::string &sError)
{
    if (!IsValidTopic(topic)) {
        sError = "Invalid topic string.";
        return false;
    }
    {
        LOCK(cs_smsgSubs);
        m_subscribed_topics.erase(topic);
        // Rebuild the hash set from remaining subscriptions
        m_subscribed_topic_hashes.clear();
        for (const auto &t : m_subscribed_topics) {
            m_subscribed_topic_hashes.insert(SMSGTopicHash(t));
        }
    }
    return SaveTopicSubs();
};

bool CSMSG::LoadTopicSubs()
{
    LOCK(cs_smsgSubs);
    m_subscribed_topics.clear();
    m_subscribed_topic_hashes.clear();

    fs::path fpath = GetDataDir() / "smsgstore" / "topic_subs.dat";
    if (!fs::exists(fpath)) {
        return true; // no subscriptions yet, not an error
    }

    FILE *fp = fopen(fpath.string().c_str(), "r");
    if (!fp) {
        LogPrintf("%s: Could not open %s: %s\n", __func__, fpath.string(), strerror(errno));
        return false;
    }

    char buf[128];
    while (fgets(buf, sizeof(buf), fp)) {
        std::string line(buf);
        // Strip trailing newline/carriage-return
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') continue;
        if (IsValidTopic(line)) {
            m_subscribed_topics.insert(line);
            m_subscribed_topic_hashes.insert(SMSGTopicHash(line));
            ImportTopicKey(line);
        }
    }

    fclose(fp);
    LogPrintf("Loaded %zu topic subscriptions.\n", m_subscribed_topics.size());
    return true;
};

bool CSMSG::SaveTopicSubs()
{
    fs::path dir   = GetDataDir() / "smsgstore";
    fs::path fpath = dir / "topic_subs.dat";
    fs::path tmp   = dir / "topic_subs.dat~";

    // Ensure smsgstore directory exists
    if (!fs::exists(dir)) {
        try { fs::create_directories(dir); } catch (const fs::filesystem_error &ex) {
            LogPrintf("%s: Could not create dir %s: %s\n", __func__, dir.string(), ex.what());
            return false;
        }
    }

    FILE *fp = fopen(tmp.string().c_str(), "w");
    if (!fp) {
        LogPrintf("%s: Could not open %s: %s\n", __func__, tmp.string(), strerror(errno));
        return false;
    }

    for (const auto &t : m_subscribed_topics) {
        fprintf(fp, "%s\n", t.c_str());
    }
    fclose(fp);

    try {
        fs::rename(tmp, fpath);
    } catch (const fs::filesystem_error &ex) {
        LogPrintf("%s: Could not rename %s: %s\n", __func__, tmp.string(), ex.what());
        return false;
    }

    return true;
};

} // namespace smsg
