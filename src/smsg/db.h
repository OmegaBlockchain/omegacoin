// Copyright (c) 2017-2024 The Particl Core developers
// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef OMEGA_SMSG_DB_H
#define OMEGA_SMSG_DB_H

#include <leveldb/db.h>
#include <leveldb/write_batch.h>

#include <sync.h>
#include <smsg/keystore.h>

class CDataStream;

namespace smsg {

static constexpr uint32_t SMSG_DB_VERSION = 1;

class SecMsgStored;

class SecMsgPurged;

extern CCriticalSection cs_smsgDB;
extern leveldb::DB *smsgDB;

class SecMsgDB
{
public:
    SecMsgDB()
    {
        activeBatch = nullptr;
    };

    ~SecMsgDB()
    {
        // Deletes only data scoped to this SecMsgDB object.
        if (activeBatch)
            delete activeBatch;
    };

    bool Open(const char *pszMode="r+");

    bool ScanBatch(const CDataStream &key, std::string *value, bool *deleted) const;

    bool TxnBegin();
    bool TxnCommit();
    bool TxnAbort();

    bool ReadScanHeight(int &nHeight);
    bool WriteScanHeight(int nHeight);

    bool ReadPK(const CKeyID &addr, CPubKey &pubkey);
    bool WritePK(const CKeyID &addr, CPubKey &pubkey);
    bool ExistsPK(const CKeyID &addr);

    bool ReadKey(const CKeyID &idk, SecMsgKey &key);
    bool WriteKey(const CKeyID &idk, const SecMsgKey &key);
    bool EraseKey(const CKeyID &idk);

    bool ErasePK(const CKeyID &addr);

    bool NextSmesg(leveldb::Iterator *it, const std::string &prefix, uint8_t *chKey, SecMsgStored &smsgStored);
    bool NextSmesgKey(leveldb::Iterator *it, const std::string &prefix, uint8_t *chKey);
    bool ReadSmesg(const uint8_t *chKey, SecMsgStored &smsgStored);
    bool WriteSmesg(const uint8_t *chKey, SecMsgStored &smsgStored);
    bool ExistsSmesg(const uint8_t *chKey);
    bool EraseSmesg(const uint8_t *chKey);


    bool ReadPurged(const uint8_t *chKey, SecMsgPurged &smsgPurged);
    bool WritePurged(const uint8_t *chKey, SecMsgPurged &smsgPurged);
    bool ErasePurged(const uint8_t *chKey);
    bool NextPurged(leveldb::Iterator *it, const std::string &prefix, uint8_t *chKey, SecMsgPurged &smsgPurged);

    bool NextPrivKey(leveldb::Iterator *it, const std::string &prefix, CKeyID &idk, SecMsgKey &key);

    // Topic index entry — stored as value in LevelDB
    struct TopicEntry {
        int64_t timestamp;
        uint160 msgId;
        uint160 parentMsgId;  // zero if no parent
        uint16_t nRetentionDays = 0;
    };

    // Topic channel index: key = "ti" + uint8(topic_len) + topic + int64_be(timestamp) + uint160(msgId)
    // Value = msgId(20) + parentMsgId(20) + retentionDays(2)
    bool WriteTopicIndex(const std::string &topic, int64_t timestamp, const uint160 &msgId,
                         const uint160 &parentMsgId = uint160(), uint16_t nRetentionDays = 0);
    bool ReadTopicMessages(const std::string &topic,
                           std::vector<TopicEntry> &out,
                           size_t maxEntries = 0);
    bool EraseTopicIndex(const std::string &topic, int64_t timestamp, const uint160 &msgId);

    leveldb::DB *pdb; // points to the global instance
    leveldb::WriteBatch *activeBatch;
};

} // namespace smsg

#endif // OMEGA_SMSG_DB_H
