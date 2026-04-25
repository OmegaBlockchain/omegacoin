// Copyright (c) 2017-2024 The Particl Core developers
// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef OMEGA_SMSG_SMESSAGE_H
#define OMEGA_SMSG_SMESSAGE_H

#include <key_io.h>
#include <net.h>
#include <node/context.h>
#include <serialize.h>
#include <ui_interface.h>
#include <lz4/lz4.h>
#include <smsg/keystore.h>
#include <interfaces/handler.h>
#include <validationinterface.h>
#include <secp256k1.h>

#include <boost/signals2/signal.hpp>

class CWallet;
struct FlatFilePos;

namespace smsg {

enum SecureMessageCodes {
    SMSG_NO_ERROR = 0,
    SMSG_GENERAL_ERROR,
    SMSG_UNKNOWN_VERSION,
    SMSG_INVALID_ADDRESS,
    SMSG_INVALID_ADDRESS_FROM,
    SMSG_INVALID_ADDRESS_TO,
    SMSG_INVALID_PUBKEY,
    SMSG_PUBKEY_MISMATCH,
    SMSG_PUBKEY_EXISTS,
    SMSG_PUBKEY_NOT_EXISTS,
    SMSG_KEY_EXISTS,
    SMSG_KEY_NOT_EXISTS,
    SMSG_UNKNOWN_KEY,
    SMSG_UNKNOWN_KEY_FROM,
    SMSG_ALLOCATE_FAILED,
    SMSG_MAC_MISMATCH,
    SMSG_WALLET_UNSET,
    SMSG_WALLET_NO_PUBKEY,
    SMSG_WALLET_NO_KEY,
    SMSG_WALLET_LOCKED,
    SMSG_DISABLED,
    SMSG_UNKNOWN_MESSAGE,
    SMSG_PAYLOAD_OVER_SIZE,
    SMSG_TIME_IN_FUTURE,
    SMSG_TIME_EXPIRED,
    SMSG_INVALID_HASH,
    SMSG_CHECKSUM_MISMATCH,
    SMSG_SHUTDOWN_DETECTED,
    SMSG_MESSAGE_TOO_LONG,
    SMSG_COMPRESS_FAILED,
    SMSG_ENCRYPT_FAILED,
    SMSG_FUND_FAILED,
    SMSG_PURGED_MSG,
};

const unsigned int SMSG_HDR_LEN        = 104;               // length of unencrypted header, 4 + 2 + 1 + 8 + 16 + 33 + 32 + 4 + 4
const unsigned int SMSG_PL_HDR_LEN     = 1+20+65+4;         // length of encrypted header in payload

const unsigned int SMSG_BUCKET_LEN     = 60 * 60 * 1;       // seconds
const unsigned int SMSG_RETENTION_OLD  = 60 * 60 * 48;      // seconds

const unsigned int SMSG_SECONDS_IN_DAY = 86400;
const unsigned int SMSG_MAX_PAID_TTL   = SMSG_SECONDS_IN_DAY * 31;        // seconds
const unsigned int SMSG_RETENTION      = SMSG_MAX_PAID_TTL;
const unsigned int SMSG_SEND_DELAY     = 2;                 // seconds, SecureMsgSendData will delay this long between firing
const unsigned int SMSG_THREAD_DELAY   = 30;
const unsigned int SMSG_THREAD_LOG_GAP = 6;

const unsigned int SMSG_TIME_LEEWAY    = 24;
const unsigned int SMSG_TIME_IGNORE    = 90;                // seconds a peer is ignored for if they fail to deliver messages for a smsgWant


const unsigned int SMSG_MAX_MSG_BYTES  = 24000;             // the user input part
const unsigned int SMSG_MAX_AMSG_BYTES = 512;               // the user input part (ANON)
const unsigned int SMSG_MAX_MSG_BYTES_PAID = 512 * 1024;    // the user input part (Paid)

// Max size of payload worst case compression
const unsigned int SMSG_MAX_MSG_WORST = LZ4_COMPRESSBOUND(SMSG_MAX_MSG_BYTES+SMSG_PL_HDR_LEN);
const unsigned int SMSG_MAX_MSG_WORST_PAID = LZ4_COMPRESSBOUND(SMSG_MAX_MSG_BYTES_PAID+SMSG_PL_HDR_LEN);

static const int MIN_SMSG_PROTO_VERSION = 70200;


const CAmount nFundingTxnFeePerK = 200000;
const CAmount nMsgFeePerKPerDay =   50000;

const unsigned int SMSG_BLIND_KEY_LEN = 32;   // blinding key length for confidential funding

// Topic channel message version
static const uint8_t SMSG_VERSION_TOPIC = 4;

// Maximum topic string length (must fit in 1 byte length prefix)
static const uint8_t SMSG_MAX_TOPIC_LEN = 64;

// Worst-case encrypted payload for a topic message:
// PL_HDR + 1 topic_len + 64 topic + 20 parent_msgid + 2 retention_days + message
const unsigned int SMSG_MAX_MSG_WORST_TOPIC = LZ4_COMPRESSBOUND(SMSG_MAX_MSG_BYTES + SMSG_PL_HDR_LEN + 1 + SMSG_MAX_TOPIC_LEN + 20 + 2);

/** FNV-1a 32-bit hash of a topic string — stored in nonce[0..3] for cleartext routing. */
inline uint32_t SMSGTopicHash(const std::string &topic)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : topic) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

/** Validate a topic string: must start with "omega.", ASCII [a-z0-9.], max 64 chars. */
inline bool IsValidTopic(const std::string &topic)
{
    if (topic.size() < 7 || topic.size() > SMSG_MAX_TOPIC_LEN) return false;
    if (topic.substr(0, 6) != "omega.") return false;
    for (char c : topic) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.'))
            return false;
    }
    // Reject consecutive dots and trailing dot
    if (topic.find("..") != std::string::npos) return false;
    if (topic.back() == '.') return false;
    return true;
}

// Trollbox — public chat channel (all nodes share this keypair, no secrecy by design)
const char* const TROLLBOX_PRIVKEY_HEX = "1e76e258b28dee7edec4f1d7443f80fcc84dba50473fbbb03e2b99082302b6bd";
const unsigned int TROLLBOX_MAX_MSG_BYTES = 256;                  // max message length (chars)
const unsigned int TROLLBOX_RETENTION     = 24 * 60 * 60;        // 24 hours
const int TROLLBOX_RATE_LIMIT_SECS        = 30;                  // min seconds between sends
const int TROLLBOX_MAX_DISPLAY            = 200;                  // max messages in GUI buffer

#define SMSG_MASK_UNREAD (1 << 0)

class SecMsgStored;

// Inbox db changed, called with lock cs_smsgDB held.
extern boost::signals2::signal<void (SecMsgStored &inboxHdr)> NotifySecMsgInboxChanged;

// Outbox db changed, called with lock cs_smsgDB held.
extern boost::signals2::signal<void (SecMsgStored &outboxHdr)> NotifySecMsgOutboxChanged;

// Wallet unlocked, called after all messages received while locked have been processed.
extern boost::signals2::signal<void ()> NotifySecMsgWalletUnlocked;

// Trollbox message received, called with lock cs_smsgDB held.
extern boost::signals2::signal<void (SecMsgStored &trollboxHdr)> NotifySecMsgTrollboxChanged;


uint32_t SMSGGetSecondsInDay();

inline bool GetFundingTxid(const uint8_t *pPayload, size_t nPayload, uint256 &txid)
{
    if (!pPayload || nPayload < 32) {
        return false;
    }
    memcpy(txid.begin(), pPayload+(nPayload-32), 32);
    return true;
};

/** Retrieve the blinding key from a confidential paid message payload.
 *  The blinding key occupies 32 bytes before the funding txid. */
inline bool GetFundingBlindKey(const uint8_t *pPayload, size_t nPayload, std::vector<uint8_t> &blindKey)
{
    if (!pPayload || nPayload < 64) {
        return false;
    }
    blindKey.resize(SMSG_BLIND_KEY_LEN);
    memcpy(blindKey.data(), pPayload+(nPayload-64), SMSG_BLIND_KEY_LEN);
    return true;
};

#pragma pack(push, 1)
class SecureMessage
{
public:
    SecureMessage() {};
    SecureMessage(bool fPaid, size_t nDaysRetention, bool fBlinded = false)
    {
        if (fPaid) {
            version[0] = 3;
            version[1] = fBlinded ? 1 : 0;

            nonce[0] = nDaysRetention;
        }
    };
    ~SecureMessage()
    {
        if (pPayload) {
            delete[] pPayload;
        }
        pPayload = nullptr;
    };

    SecureMessage(const SecureMessage&) = delete;
    SecureMessage& operator=(const SecureMessage&) = delete;
    SecureMessage(SecureMessage&&) = delete;
    SecureMessage& operator=(SecureMessage&&) = delete;

    void SetNull()
    {
        memset(iv, 0, 16);
        memset(cpkR, 0, 33);
        memset(mac, 0, 32);
    };

    bool IsPaidVersion() const
    {
        return version[0] == 3;
    };

    bool IsTopicVersion() const
    {
        return version[0] == SMSG_VERSION_TOPIC;
    };

    bool IsBlindedPaid() const
    {
        return version[0] == 3 && version[1] == 1;
    };

    /** Return the number of tail bytes appended after the ciphertext.
     *  Blinded paid: 64 (32 blind key + 32 txid), legacy paid: 32 (txid), free: 0. */
    uint32_t GetPaidTailSize() const
    {
        if (!IsPaidVersion()) return 0;
        return IsBlindedPaid() ? 64 : 32;
    };

    bool GetFundingTxid(uint256 &txid) const
    {
        if (version[0] != 3) {
            return false;
        }
        return smsg::GetFundingTxid(pPayload, nPayload, txid);
    };

    uint8_t *data()
    {
        return &hash[0];
    };

    const uint8_t *data() const
    {
        return &hash[0];
    };

    uint8_t hash[4] = {0, 0, 0, 0};
    uint8_t version[2] = {2, 1};
    uint8_t flags = 0;
    int64_t timestamp = 0;
    uint8_t iv[16];
    uint8_t cpkR[33];
    uint8_t mac[32];
    uint8_t nonce[4] = {0, 0, 0, 0}; // nDaysRetention when paid message
    uint32_t nPayload = 0;
    uint8_t *pPayload = nullptr;
};
#pragma pack(pop)

class MessageData
{
// Decrypted SecureMessage data
public:
    int64_t               timestamp;
    std::string           sToAddress;
    std::string           sFromAddress;
    std::string           sTopic;             // topic channel string (version[0]==SMSG_VERSION_TOPIC only)
    uint160               parentMsgId;        // references a prior message (listing update/reply); zero if none
    uint16_t              nRetentionDays = 0; // suggested local retention (days); 0 = default TTL
    std::vector<uint8_t>  vchMessage;         // null terminated plaintext
};

class SecMsgToken
{
public:
    SecMsgToken() {};
    SecMsgToken(int64_t ts, const uint8_t *p, int np, long int o, uint8_t ttl_)
    {
        timestamp = ts;

        if (np < 8) {
            memset(sample, 0, 8);
        } else {
            memcpy(sample, p, 8);
        }
        offset = o;
        ttl = ttl_;
    };

    bool operator <(const SecMsgToken &y) const
    {
        if (timestamp == y.timestamp) {
            return memcmp(sample, y.sample, 8) < 0;
        }
        return timestamp < y.timestamp;
    };

    std::string ToString() const;

    int64_t timestamp;
    uint8_t sample[8];    // first 8 bytes of payload
    int64_t offset;       // offset
    mutable uint8_t ttl;  // days
};

class SecMsgPurged // Purged token marker
{
public:
    SecMsgPurged() {};
    SecMsgPurged(int64_t ts, int64_t tp)
    {
        timestamp = ts;
        memset(sample, 0, 8);
        timepurged = tp;
    };

    bool operator <(const SecMsgPurged &y) const
    {
        if (timestamp == y.timestamp) {
            return memcmp(sample, y.sample, 8) < 0;
        }
        return timestamp < y.timestamp;
    };

    template<typename Stream>
    void Serialize(Stream &s) const
    {
        s << timestamp;
        s.write((char*)&sample[0], 8);
        s << timepurged;
    };
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> timestamp;
        s.read((char*)&sample[0], 8);
        s >> timepurged;
    };

    int64_t timestamp;
    uint8_t sample[8];
    int64_t timepurged;
};

class SecMsgBucket
{
public:
    SecMsgBucket()
    {
        timeChanged     = 0;
        hash            = 0;
        nLockCount      = 0;
        nLockPeerId     = 0;
        nLeastTTL       = 0;
        nActive         = 0;
    };

    void hashBucket();
    size_t CountActive();

    int64_t               timeChanged;
    uint32_t              hash;           // token set should get ordered the same on each node
    uint32_t              nLockCount;     // set when smsgWant first sent, unset at end of smsgMsg, ticks down in ThreadSecureMsg()
    uint32_t              nLeastTTL;      // lowest ttl in days of messages in bkt
    uint32_t              nActive;        // Number of untimedout messages in bucket
    NodeId                nLockPeerId;    // id of peer that bucket is locked for
    std::set<SecMsgToken> setTokens;
};

class SecMsgAddress
{
public:
    SecMsgAddress() {};
    SecMsgAddress(CKeyID addr, bool receiveOn, bool receiveAnon)
    {
        address         = addr;
        fReceiveEnabled = receiveOn;
        fReceiveAnon    = receiveAnon;
    };

    CKeyID address;
    bool fReceiveEnabled;
    bool fReceiveAnon;

    size_t GetSerializeSize(int nType, int nVersion) const
    {
        return 22;
    };
    template<typename Stream>
    void Serialize(Stream &s) const
    {
        s << address;
        s << fReceiveEnabled;
        s << fReceiveAnon;
    };
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> address;
        s >> fReceiveEnabled;
        s >> fReceiveAnon;
    };
};

class SecMsgOptions
{
public:
    SecMsgOptions()
    {
        // Default options
        fNewAddressRecv = true;
        fNewAddressAnon = true;
        fScanIncoming   = true;
    };

    bool fNewAddressRecv;
    bool fNewAddressAnon;
    bool fScanIncoming;
};

class SecMsgStored
{
public:
    int64_t              timeReceived;
    uint8_t              status;         // read etc
    uint16_t             folderId;
    CKeyID               addrTo;         // when in owned addr, when sent remote addr
    CKeyID               addrOutbox;     // owned address this copy was encrypted with
    std::vector<uint8_t> vchMessage;     // message header + encryped payload

    size_t GetSerializeSize(int nType, int nVersion) const
    {
        return sizeof(timeReceived) + sizeof(status) + sizeof(folderId) + 20 + 20 +
            GetSizeOfCompactSize(vchMessage.size()) + vchMessage.size() * sizeof(uint8_t);
    };
    template<typename Stream>
    void Serialize(Stream &s) const
    {
        s << timeReceived;
        s << status;
        s << folderId;
        s << addrTo;
        s << addrOutbox;
        s << vchMessage;
    };
    template <typename Stream>
    void Unserialize(Stream &s)
    {
        s >> timeReceived;
        s >> status;
        s >> folderId;
        s >> addrTo;
        s >> addrOutbox;
        s >> vchMessage;
    };
};

void AddOptions();
const char *GetString(size_t errorCode);

extern std::atomic<bool> fSecMsgEnabled;
class CSMSG : public CValidationInterface
{
public:
    int BuildBucketSet();
    int BuildPurgedSets();
    int AddWalletAddresses();
    int LoadKeyStore();

    int ReadIni();
    int WriteIni();

    bool Start(std::shared_ptr<CWallet> pwalletIn, bool fDontStart, bool fScanChain);
    bool StartDelayed(std::shared_ptr<CWallet> pwalletIn, bool fDontStart, bool fScanChain, int nDelaySeconds = 30);
    bool StartOnUnlock(std::shared_ptr<CWallet> pwalletIn, bool fScanChain, int nDelaySeconds = 5);
    bool Shutdown();

    bool Enable(std::shared_ptr<CWallet> pwallet);
    bool Disable();

    bool UnloadAllWallets();
    bool LoadWallet(std::shared_ptr<CWallet> pwallet_in);
    bool WalletUnloaded(CWallet *pwallet_removed);
    bool SetActiveWallet(std::shared_ptr<CWallet> pwallet_in);
    std::string GetWalletName();
    std::string LookupLabel(PKHash &hash);

    void GetNodesStats(int node_id, UniValue &result);
    void ClearBanned();

    int ReceiveData(CNode *pfrom, const std::string &strCommand, CDataStream &vRecv);
    bool SendData(CNode *pto, bool fSendTrickle);

    bool ScanBlock(const CBlock &block);
    bool ScanChainForPublicKeys(const std::vector<FlatFilePos> &vBlockPos, uint32_t &nScannedOut);
    bool ScanBlockChain();
    bool ScanBuckets();

    int ManageLocalKey(CKeyID &keyId, ChangeType mode);
    int WalletUnlocked();
    int EncryptSmsgKeys();
    int WalletKeyChanged(CKeyID &keyId, const std::string &sLabel, ChangeType mode);

    int ScanMessage(const uint8_t *pHeader, const uint8_t *pPayload, uint32_t nPayload, bool reportToGui);

    int GetStoredKey(const CKeyID &ckid, CPubKey &cpkOut);
    int GetLocalKey(const CKeyID &ckid, CPubKey &cpkOut);
    int GetLocalPublicKey(const std::string &strAddress, std::string &strPublicKey);

    int AddAddress(std::string &address, std::string &publicKey);
    int AddContact(std::string &address, std::string &publicKey, const std::string &label);
    int AddLocalAddress(const std::string &sAddress);
    int ImportPrivkey(const CKey &vchSecret, const std::string &sLabel);

    bool SetWalletAddressOption(const CKeyID &idk, std::string sOption, bool fValue);
    bool SetSmsgAddressOption(const CKeyID &idk, std::string sOption, bool fValue);

    int ReadSmsgKey(const CKeyID &idk, CKey &key);

    int DumpPrivkey(const CKeyID &idk, CKey &key_out);
    int RemoveAddress(const std::string &addr);
    int RemovePrivkey(const std::string &addr);

    int RetrieveFile(FILE *fp, const SecMsgToken &token, std::vector<uint8_t> &vchData);
    int Retrieve(const SecMsgToken &token, std::vector<uint8_t> &vchData);
    int Remove(const SecMsgToken &token);

    int Receive(CNode *pfrom, std::vector<uint8_t> &vchData);

    int CheckPurged(const SecureMessage *psmsg, const uint8_t *pPayload);

    int StoreUnscanned(const uint8_t *pHeader, const uint8_t *pPayload, uint32_t nPayload);
    int Store(const uint8_t *pHeader, const uint8_t *pPayload, uint32_t nPayload, bool fHashBucket, FILE *fpBatch = nullptr);
    int Store(const SecureMessage &smsg, bool fHashBucket);

    int Purge(std::vector<uint8_t> &vMsgId, std::string &sError);

    int Send(CKeyID &addressFrom, CKeyID &addressTo, std::string &message,
        SecureMessage &smsg, std::string &sError, bool fPaid=false,
        size_t nDaysRetention=0, bool fTestFee=false, CAmount *nFee=NULL,
        bool fFromFile=false, const std::string &topic="",
        const uint160 &parentMsgId=uint160(), uint16_t nTopicRetentionDays=0);


    int HashMsg(const SecureMessage &smsg, const uint8_t *pPayload, uint32_t nPayload, uint160 &hash);
    int FundMsg(SecureMessage &smsg, std::string &sError, bool fTestFee, CAmount *nFee, const uint160 *pPrecomputedMsgId = nullptr);
    uint256 FindFundingTx(const uint160 &msgId) const;

    std::vector<uint8_t> GetMsgID(const SecureMessage *psmsg, const uint8_t *pPayload);
    std::vector<uint8_t> GetMsgID(const SecureMessage &smsg);

    int Validate(const uint8_t *pHeader, const uint8_t *pPayload, uint32_t nPayload);
    int SetHash (uint8_t *pHeader, uint8_t *pPayload, uint32_t nPayload);

    int Encrypt(SecureMessage &smsg, const CKeyID &addressFrom, const CKeyID &addressTo, const std::string &message,
        const std::string &topic="", const uint160 &parentMsgId=uint160(), uint16_t nTopicRetentionDays=0);

    int Decrypt(bool fTestOnly, const CKey &keyDest, const CKeyID &address, const uint8_t *pHeader, const uint8_t *pPayload, uint32_t nPayload, MessageData &msg);
    int Decrypt(bool fTestOnly, const CKey &keyDest, const CKeyID &address, const SecureMessage &smsg, MessageData &msg);

    int Decrypt(bool fTestOnly, const CKeyID &address, const uint8_t *pHeader, const uint8_t *pPayload, uint32_t nPayload, MessageData &msg);
    int Decrypt(bool fTestOnly, const CKeyID &address, const SecureMessage &smsg, MessageData &msg);

    // Internal: skip secp256k1_ec_pubkey_parse when R is already parsed.
    int DecryptWithR(bool fTestOnly, const CKey &keyDest, const CKeyID &address,
                     const secp256k1_pubkey &R,
                     const uint8_t *pHeader, const uint8_t *pPayload, uint32_t nPayload, MessageData &msg);

    // Topic channel subscriptions
    bool SubscribeTopic(const std::string &topic, std::string &sError);
    bool UnsubscribeTopic(const std::string &topic, std::string &sError);
    bool IsSubscribedTopicHash(uint32_t hash) const;
    bool LoadTopicSubs();
    bool SaveTopicSubs();

    // Broadcast channel: derive a deterministic shared key from a topic string.
    // All subscribers hold the same key, enabling public/broadcast messaging (like Trollbox per-topic).
    static CKey GetTopicSharedKey(const std::string &topic);
    CKeyID ImportTopicKey(const std::string &topic);

    mutable CCriticalSection cs_smsgSubs; // guards m_subscribed_topics, m_subscribed_topic_hashes, m_topicHashToKeyID
    std::set<std::string> m_subscribed_topics;              // full topic strings
    std::set<uint32_t>    m_subscribed_topic_hashes;        // FNV-1a hashes for fast cleartext routing
    std::map<uint32_t, CKeyID> m_topicHashToKeyID;          // topic hash → keystore CKeyID for O(1) ScanMessage routing

    CCriticalSection cs_smsg; // all except inbox and outbox

    SecMsgKeyStore keyStore;
    std::map<int64_t, SecMsgBucket> buckets;
    std::vector<SecMsgAddress> addresses;
    std::set<SecMsgPurged> setPurged;
    std::set<int64_t> setPurgedTimestamps;
    SecMsgOptions options;
    std::shared_ptr<CWallet> pwallet;
    std::shared_ptr<CWallet> pactive_wallet;
    std::vector<std::shared_ptr<CWallet>> m_vpwallets;
    std::unique_ptr<interfaces::Handler> m_handler_unload;
    std::unique_ptr<interfaces::Handler> m_handler_status;
    std::unique_ptr<interfaces::Handler> m_handler_unlock_start; // for StartOnUnlock
    std::map<CWallet*, std::unique_ptr<interfaces::Handler>> m_wallet_unload_handlers;

    std::atomic<bool> m_fScanAbort{false};

    int64_t nLastProcessedPurged = 0;
    int64_t nLastTrollboxSend = 0;
    CKeyID trollboxAddress;
    CKeyID m_cachedOutboxAddr;

    NodeContext *m_node = nullptr;

protected:
    void UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload) override;
};

} // namespace smsg

extern smsg::CSMSG smsgModule;

#endif // OMEGA_SMSG_SMESSAGE_H

