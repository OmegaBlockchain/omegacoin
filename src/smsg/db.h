// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef OMEGA_SMSG_DB_H
#define OMEGA_SMSG_DB_H

#include <leveldb/write_batch.h>
#include <leveldb/db.h>

#include <sync.h>
#include <pubkey.h>

class DataStream;
class uint256;

namespace smsg {

class SecMsgKey;
class SecMsgStored;
class SecMsgPurged;

extern RecursiveMutex cs_smsgDB;
extern leveldb::DB *smsgDB;

extern const std::string DBK_PUBLICKEY;
extern const std::string DBK_SECRETKEY;
extern const std::string DBK_INBOX;
extern const std::string DBK_OUTBOX;
extern const std::string DBK_QUEUED;
extern const std::string DBK_STASHED;
extern const std::string DBK_PURGED_TOKEN;
extern const std::string DBK_FUNDING_TX_DATA;
extern const std::string DBK_FUNDING_TX_LINK;

class SecMsgDB
{
public:
    ~SecMsgDB()
    {
        Finalise();
    }

    void Finalise() {
        // Deletes only data scoped to this SecMsgDB object.
        if (activeBatch) {
            delete activeBatch;
        }
        pdb = nullptr;
    }

    bool Open(const char *pszMode="r+");
    bool IsOpen() const { return pdb; };

    bool ScanBatch(const DataStream &key, std::string *value, bool *deleted) const;

    bool TxnBegin();
    bool TxnCommit();
    bool TxnAbort();
    bool CommitBatch(leveldb::WriteBatch *batch);

    bool ReadPK(const CKeyID &addr, CPubKey &pubkey);
    bool WritePK(const CKeyID &addr, const CPubKey &pubkey);
    bool ExistsPK(const CKeyID &addr);
    bool ErasePK(const CKeyID &addr);
    bool NextPKKey(leveldb::Iterator *it, CKeyID &key_id);

    bool ReadKey(const CKeyID &idk, SecMsgKey &key);
    bool WriteKey(const CKeyID &idk, const SecMsgKey &key);
    bool EraseKey(const CKeyID &idk);

    bool NextSmesg(leveldb::Iterator *it, const std::string &prefix, uint8_t *chKey, SecMsgStored &smsgStored);
    bool NextSmesgKey(leveldb::Iterator *it, const std::string &prefix, uint8_t *chKey);
    bool ReadSmesg(const uint8_t *chKey, SecMsgStored &smsgStored);
    bool WriteSmesg(const uint8_t *chKey, const SecMsgStored &smsgStored);
    bool ExistsSmesg(const uint8_t *chKey);
    bool EraseSmesg(const uint8_t *chKey);

    bool NextPrivKey(leveldb::Iterator *it, const std::string &prefix, CKeyID &idk, SecMsgKey &key);

    bool ReadPurged(const uint8_t *chKey, SecMsgPurged &smsgPurged);
    bool WritePurged(const uint8_t *chKey, const SecMsgPurged &smsgPurged);
    bool ErasePurged(const uint8_t *chKey);
    bool NextPurged(leveldb::Iterator *it, const std::string &prefix, uint8_t *chKey, SecMsgPurged &smsgPurged);

    bool ReadFundingData(const uint256 &key, std::vector<uint8_t> &data);
    bool WriteFundingData(const uint256 &key, int height, const std::vector<uint8_t> &data);
    bool EraseFundingData(int height, const uint256 &key);
    bool NextFundingDataLink(leveldb::Iterator *it, int &height, uint256 &key);

    bool WriteBestBlock(const uint256 &hash, int height);
    bool ReadBestBlock(uint256 &hash, int &height);
    bool EraseBestBlock();

    /**
     * Compact a certain range of keys in the database.
     */
    void Compact() const;

    leveldb::DB *pdb{nullptr};  // Points to the global instance
    leveldb::WriteBatch *activeBatch{nullptr};
};

bool PutBestBlock(leveldb::WriteBatch *batch, const uint256 &block_hash, int height);
bool PutFundingData(leveldb::WriteBatch *batch, const uint256 &key, int height, const std::vector<uint8_t> &data);

} // namespace smsg

#endif // OMEGA_SMSG_DB_H
