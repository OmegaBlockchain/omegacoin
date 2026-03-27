// Copyright (c) 2017-2024 The Particl Core developers
// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <smsg/db.h>
#include <smsg/smessage.h>
#include <serialize.h>
#include <clientversion.h>
#include <algorithm>

namespace smsg {

/*
prefixes
    name

    pk      - public key
    sk      - secret key

    im      - inbox message
    sm      - sent message
    qm      - queued message
    pm      - purged message token
*/

RecursiveMutex cs_smsgDB;
leveldb::DB *smsgDB = nullptr;

bool SecMsgDB::Open(const char *pszMode)
{
    if (smsgDB)
    {
        pdb = smsgDB;
        return true;
    };

    bool fCreate = strchr(pszMode, 'c');

    fs::path fullpath = GetDataDir() / "smsgdb";

    if (!fCreate
        && (!fs::exists(fullpath)
            || !fs::is_directory(fullpath)))
    {
        LogPrintf("%s: DB does not exist.\n", __func__);
        return false;
    };

    leveldb::Options options;
    options.create_if_missing = fCreate;
    leveldb::Status s = leveldb::DB::Open(options, fullpath.string(), &smsgDB);

    if (!s.ok())
    {
        LogPrintf("%s: Error opening db: %s.\n", __func__, s.ToString());
        return false;
    };

    pdb = smsgDB;

    return true;
};


class SecMsgBatchScanner : public leveldb::WriteBatch::Handler
{
public:
    std::string needle;
    bool *deleted;
    std::string* foundValue;
    bool foundEntry;

    SecMsgBatchScanner() : foundEntry(false) {}

    virtual void Put(const leveldb::Slice &key, const leveldb::Slice &value)
    {
        if (key.ToString() == needle)
        {
            foundEntry = true;
            *deleted = false;
            *foundValue = value.ToString();
        };
    };

    virtual void Delete(const leveldb::Slice &key)
    {
        if (key.ToString() == needle)
        {
            foundEntry = true;
            *deleted = true;
        };
    };
};

// When performing a read, if we have an active batch we need to check it first
// before reading from the database, as the rest of the code assumes that once
// a database transaction begins reads are consistent with it. It would be good
// to change that assumption in future and avoid the performance hit, though in
// practice it does not appear to be large.
bool SecMsgDB::ScanBatch(const CDataStream &key, std::string *value, bool *deleted) const
{
    if (!activeBatch)
        return false;

    *deleted = false;
    SecMsgBatchScanner scanner;
    scanner.needle = key.str();
    scanner.deleted = deleted;
    scanner.foundValue = value;
    leveldb::Status s = activeBatch->Iterate(&scanner);
    if (!s.ok())
        return error("SecMsgDB ScanBatch error: %s\n", s.ToString());

    return scanner.foundEntry;
}

bool SecMsgDB::TxnBegin()
{
    if (activeBatch)
        return true;
    activeBatch = new leveldb::WriteBatch();
    return true;
};

bool SecMsgDB::TxnCommit()
{
    if (!activeBatch)
        return false;

    leveldb::WriteOptions writeOptions;
    writeOptions.sync = true;
    leveldb::Status status = pdb->Write(writeOptions, activeBatch);
    delete activeBatch;
    activeBatch = nullptr;

    if (!status.ok())
        return error("SecMsgDB batch commit failure: %s\n", status.ToString());

    return true;
};

bool SecMsgDB::TxnAbort()
{
    delete activeBatch;
    activeBatch = nullptr;
    return true;
};

bool SecMsgDB::ReadScanHeight(int &nHeight)
{
    if (!pdb)
        return false;

    std::string sKey("sh");
    std::string strValue;

    leveldb::Status s = pdb->Get(leveldb::ReadOptions(), sKey, &strValue);
    if (!s.ok()) {
        if (s.IsNotFound())
            return false;
        return error("LevelDB read failure: %s\n", s.ToString());
    }

    try {
        CDataStream ssValue(strValue.data(), strValue.data() + strValue.size(), SER_DISK, CLIENT_VERSION);
        ssValue >> nHeight;
    } catch (std::exception &e) {
        LogPrintf("%s unserialize threw: %s.\n", __func__, e.what());
        return false;
    }

    return true;
}

bool SecMsgDB::WriteScanHeight(int nHeight)
{
    if (!pdb)
        return false;

    std::string sKey("sh");
    CDataStream ssValue(SER_DISK, CLIENT_VERSION);
    ssValue << nHeight;

    if (activeBatch) {
        activeBatch->Put(sKey, ssValue.str());
        return true;
    }

    leveldb::WriteOptions writeOptions;
    writeOptions.sync = true;
    leveldb::Status s = pdb->Put(writeOptions, sKey, ssValue.str());
    if (!s.ok())
        return error("SecMsgDB write scan height failed: %s\n", s.ToString());

    return true;
}

bool SecMsgDB::ReadPK(const CKeyID &addr, CPubKey &pubkey)
{
    if (!pdb)
        return false;

    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey.reserve(sizeof(addr) + 2);
    ssKey << 'p';
    ssKey << 'k';
    ssKey << addr;
    std::string strValue;

    bool readFromDb = true;
    if (activeBatch)
    {
        // Check activeBatch first
        bool deleted = false;
        readFromDb = ScanBatch(ssKey, &strValue, &deleted) == false;
        if (deleted)
            return false;
    };

    if (readFromDb)
    {
        leveldb::Status s = pdb->Get(leveldb::ReadOptions(), ssKey.str(), &strValue);
        if (!s.ok())
        {
            if (s.IsNotFound())
                return false;
            return error("LevelDB read failure: %s\n", s.ToString());
        };
    };

    try {
        CDataStream ssValue(strValue.data(), strValue.data() + strValue.size(), SER_DISK, CLIENT_VERSION);
        ssValue >> pubkey;
    } catch (std::exception &e) {
        LogPrintf("%s unserialize threw: %s.\n", __func__, e.what());
        return false;
    };

    return true;
};

bool SecMsgDB::WritePK(const CKeyID &addr, CPubKey &pubkey)
{
    if (!pdb)
        return false;

    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey.reserve(sizeof(addr) + 2);
    ssKey << 'p';
    ssKey << 'k';
    ssKey << addr;
    CDataStream ssValue(SER_DISK, CLIENT_VERSION);
    ssValue.reserve(sizeof(pubkey));
    ssValue << pubkey;

    if (activeBatch)
    {
        activeBatch->Put(ssKey.str(), ssValue.str());
        return true;
    };

    leveldb::WriteOptions writeOptions;
    writeOptions.sync = true;
    leveldb::Status s = pdb->Put(writeOptions, ssKey.str(), ssValue.str());
    if (!s.ok())
        return error("SecMsgDB write failure: %s\n", s.ToString());

    return true;
};

bool SecMsgDB::ExistsPK(const CKeyID &addr)
{
    if (!pdb)
        return false;

    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey.reserve(sizeof(addr)+2);
    ssKey << 'p';
    ssKey << 'k';
    ssKey << addr;
    std::string unused;

    if (activeBatch)
    {
        bool deleted;
        if (ScanBatch(ssKey, &unused, &deleted) && !deleted)
            return true;
    };

    leveldb::Status s = pdb->Get(leveldb::ReadOptions(), ssKey.str(), &unused);
    return s.IsNotFound() == false;
};

bool SecMsgDB::ReadKey(const CKeyID &idk, SecMsgKey &key)
{
    if (!pdb)
        return false;

    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey.reserve(sizeof(idk) + 2);
    ssKey << 's';
    ssKey << 'k';
    ssKey << idk;
    std::string strValue;

    bool readFromDb = true;
    if (activeBatch)
    {
        // Check activeBatch first
        bool deleted = false;
        readFromDb = ScanBatch(ssKey, &strValue, &deleted) == false;
        if (deleted)
            return false;
    };

    if (readFromDb)
    {
        leveldb::Status s = pdb->Get(leveldb::ReadOptions(), ssKey.str(), &strValue);
        if (!s.ok())
        {
            if (s.IsNotFound())
                return false;
            return error("LevelDB read failure: %s\n", s.ToString());
        };
    };

    try {
        CDataStream ssValue(strValue.data(), strValue.data() + strValue.size(), SER_DISK, CLIENT_VERSION);
        ssValue >> key;
    } catch (std::exception &e) {
        LogPrintf("%s unserialize threw: %s.\n", __func__, e.what());
        return false;
    };

    return true;
};

bool SecMsgDB::WriteKey(const CKeyID &idk, const SecMsgKey &key)
{
    if (!pdb)
        return false;
    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey.reserve(sizeof(idk) + 2);
    ssKey << 's';
    ssKey << 'k';
    ssKey << idk;

    CDataStream ssValue(SER_DISK, CLIENT_VERSION);
    ssValue.reserve(sizeof(key));
    ssValue << key;

    if (activeBatch)
    {
        activeBatch->Put(ssKey.str(), ssValue.str());
        return true;
    };

    leveldb::WriteOptions writeOptions;
    writeOptions.sync = true;
    leveldb::Status s = pdb->Put(writeOptions, ssKey.str(), ssValue.str());
    if (!s.ok())
        return error("%s failed: %s\n", __func__, s.ToString());

    return true;
};

bool SecMsgDB::EraseKey(const CKeyID &idk)
{
    if (!pdb)
        return false;

    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey.reserve(sizeof(idk) + 2);
    ssKey << 's';
    ssKey << 'k';
    ssKey << idk;

    if (activeBatch) {
        activeBatch->Delete(ssKey.str());
        return true;
    }

    leveldb::WriteOptions writeOptions;
    writeOptions.sync = true;
    leveldb::Status s = pdb->Delete(writeOptions, ssKey.str());

    if (s.ok() || s.IsNotFound())
        return true;
    LogPrintf("SecMsgDB erase key failed: %s\n", s.ToString());
    return false;
};

bool SecMsgDB::ErasePK(const CKeyID &addr)
{
    if (!pdb)
        return false;

    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey.reserve(sizeof(addr) + 2);
    ssKey << 'p';
    ssKey << 'k';
    ssKey << addr;

    if (activeBatch) {
        activeBatch->Delete(ssKey.str());
        return true;
    }

    leveldb::WriteOptions writeOptions;
    writeOptions.sync = true;
    leveldb::Status s = pdb->Delete(writeOptions, ssKey.str());

    if (s.ok() || s.IsNotFound())
        return true;
    LogPrintf("SecMsgDB erase pk failed: %s\n", s.ToString());
    return false;
};

bool SecMsgDB::NextSmesg(leveldb::Iterator *it, const std::string &prefix, uint8_t *chKey, SecMsgStored &smsgStored)
{
    if (!pdb)
        return false;

    if (!it->Valid()) // First run
        it->Seek(prefix);
    else
        it->Next();

    if (!(it->Valid()
        && it->key().size() == 30
        && memcmp(it->key().data(), prefix.data(), 2) == 0))
        return false;

    memcpy(chKey, it->key().data(), 30);

    try {
        CDataStream ssValue(it->value().data(), it->value().data() + it->value().size(), SER_DISK, CLIENT_VERSION);
        ssValue >> smsgStored;
    } catch (std::exception &e) {
        LogPrintf("%s unserialize threw: %s.\n", __func__, e.what());
        return false;
    };

    return true;
};

bool SecMsgDB::NextSmesgKey(leveldb::Iterator *it, const std::string &prefix, uint8_t *chKey)
{
    if (!pdb)
        return false;

    if (!it->Valid()) // First run
        it->Seek(prefix);
    else
        it->Next();

    if (!(it->Valid()
        && it->key().size() == 30
        && memcmp(it->key().data(), prefix.data(), 2) == 0))
        return false;

    memcpy(chKey, it->key().data(), 30);

    return true;
};

bool SecMsgDB::ReadSmesg(const uint8_t *chKey, SecMsgStored &smsgStored)
{
    if (!pdb)
        return false;

    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey.write((const char*)chKey, 30);
    std::string strValue;

    bool readFromDb = true;
    if (activeBatch)
    {
        // Check activeBatch first
        bool deleted = false;
        readFromDb = ScanBatch(ssKey, &strValue, &deleted) == false;
        if (deleted)
            return false;
    };

    if (readFromDb)
    {
        leveldb::Status s = pdb->Get(leveldb::ReadOptions(), ssKey.str(), &strValue);
        if (!s.ok())
        {
            if (s.IsNotFound())
                return false;
            return error("LevelDB read failure: %s\n", s.ToString());
        };
    };

    try {
        CDataStream ssValue(strValue.data(), strValue.data() + strValue.size(), SER_DISK, CLIENT_VERSION);
        ssValue >> smsgStored;
    } catch (std::exception &e) {
        LogPrintf("%s unserialize threw: %s.\n", __func__, e.what());
        return false;
    };

    return true;
};

bool SecMsgDB::WriteSmesg(const uint8_t *chKey, SecMsgStored &smsgStored)
{
    if (!pdb)
        return false;

    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey.write((const char*)chKey, 30);
    CDataStream ssValue(SER_DISK, CLIENT_VERSION);
    ssValue << smsgStored;

    if (activeBatch)
    {
        activeBatch->Put(ssKey.str(), ssValue.str());
        return true;
    };

    leveldb::WriteOptions writeOptions;
    writeOptions.sync = true;
    leveldb::Status s = pdb->Put(writeOptions, ssKey.str(), ssValue.str());
    if (!s.ok())
        return error("SecMsgDB write failed: %s\n", s.ToString());

    return true;
};

bool SecMsgDB::ExistsSmesg(const uint8_t *chKey)
{
    if (!pdb)
        return false;

    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey.write((const char*)chKey, 30);
    std::string unused;

    if (activeBatch)
    {
        bool deleted;
        if (ScanBatch(ssKey, &unused, &deleted) && !deleted)
            return true;
    };

    leveldb::Status s = pdb->Get(leveldb::ReadOptions(), ssKey.str(), &unused);
    return s.IsNotFound() == false;
    return true;
};

bool SecMsgDB::EraseSmesg(const uint8_t *chKey)
{
    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey.write((const char*)chKey, 30);

    if (activeBatch)
    {
        activeBatch->Delete(ssKey.str());
        return true;
    };

    leveldb::WriteOptions writeOptions;
    writeOptions.sync = true;
    leveldb::Status s = pdb->Delete(writeOptions, ssKey.str());

    if (s.ok() || s.IsNotFound())
        return true;
    return error("SecMsgDB erase failed: %s\n", s.ToString());
};

bool SecMsgDB::ReadPurged(const uint8_t *chKey, SecMsgPurged &smsgPurged)
{
    if (!pdb)
        return false;

    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey.write((const char*)chKey, 30);
    std::string strValue;

    bool readFromDb = true;
    if (activeBatch)
    {
        // Check activeBatch first
        bool deleted = false;
        readFromDb = ScanBatch(ssKey, &strValue, &deleted) == false;
        if (deleted)
            return false;
    };

    if (readFromDb)
    {
        leveldb::Status s = pdb->Get(leveldb::ReadOptions(), ssKey.str(), &strValue);
        if (!s.ok())
        {
            if (s.IsNotFound())
                return false;
            return error("LevelDB read failure: %s\n", s.ToString());
        };
    };

    try {
        CDataStream ssValue(strValue.data(), strValue.data() + strValue.size(), SER_DISK, CLIENT_VERSION);
        ssValue >> smsgPurged;
    } catch (std::exception &e) {
        LogPrintf("%s unserialize threw: %s.\n", __func__, e.what());
        return false;
    };

    return true;
};

bool SecMsgDB::WritePurged(const uint8_t *chKey, SecMsgPurged &smsgPurged)
{
    if (!pdb)
        return false;

    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey.write((const char*)chKey, 30);
    CDataStream ssValue(SER_DISK, CLIENT_VERSION);
    ssValue << smsgPurged;

    if (activeBatch)
    {
        activeBatch->Put(ssKey.str(), ssValue.str());
        return true;
    };

    leveldb::WriteOptions writeOptions;
    writeOptions.sync = true;
    leveldb::Status s = pdb->Put(writeOptions, ssKey.str(), ssValue.str());
    if (!s.ok())
        return error("SecMsgDB write failed: %s\n", s.ToString());

    return true;
};

bool SecMsgDB::ErasePurged(const uint8_t *chKey)
{
    return EraseSmesg(chKey);
};


bool SecMsgDB::NextPurged(leveldb::Iterator *it, const std::string &prefix, uint8_t *chKey, SecMsgPurged &smsgPurged)
{
    if (!pdb)
        return false;

    if (!it->Valid()) // First run
        it->Seek(prefix);
    else
        it->Next();

    if (!(it->Valid()
        && it->key().size() == 30
        && memcmp(it->key().data(), prefix.data(), 2) == 0))
        return false;

    memcpy(chKey, it->key().data(), 30);

    try {
        CDataStream ssValue(it->value().data(), it->value().data() + it->value().size(), SER_DISK, CLIENT_VERSION);
        ssValue >> smsgPurged;
    } catch (std::exception &e) {
        LogPrintf("%s unserialize threw: %s.\n", __func__, e.what());
        return false;
    };

    return true;
};

bool SecMsgDB::NextPrivKey(leveldb::Iterator *it, const std::string &prefix, CKeyID &idk, SecMsgKey &key)
{
    if (!pdb)
        return false;

    if (!it->Valid()) // First run
        it->Seek(prefix);
    else
        it->Next();

    if (!(it->Valid()
        && it->key().size() == 22
        && memcmp(it->key().data(), prefix.data(), 2) == 0))
        return false;

    memcpy(idk.begin(), it->key().data()+2, 20);

    try {
        CDataStream ssValue(it->value().data(), it->value().data() + it->value().size(), SER_DISK, CLIENT_VERSION);
        ssValue >> key;
    } catch (std::exception &e) {
        LogPrintf("%s unserialize threw: %s.\n", __func__, e.what());
        return false;
    };

    return true;
};

// Build the LevelDB key for a topic index entry.
// Format: "ti" + uint8(topic_len) + topic_bytes + int64_be(timestamp) + uint160(msgId)
// Storing timestamp big-endian ensures entries are sorted by time within a topic prefix.
static std::string MakeTopicKey(const std::string &topic, int64_t timestamp, const uint160 &msgId)
{
    std::string sKey;
    sKey.reserve(2 + 1 + topic.size() + 8 + 20);
    sKey += "ti";
    sKey += (char)(uint8_t)topic.size();
    sKey += topic;
    for (int i = 7; i >= 0; --i) {
        sKey += (char)((timestamp >> (i * 8)) & 0xFF);
    }
    sKey.append((const char*)msgId.begin(), 20);
    return sKey;
}

static std::string MakeTopicPrefix(const std::string &topic)
{
    std::string sPrefix;
    sPrefix.reserve(2 + 1 + topic.size());
    sPrefix += "ti";
    sPrefix += (char)(uint8_t)topic.size();
    sPrefix += topic;
    return sPrefix;
}

bool SecMsgDB::WriteTopicIndex(const std::string &topic, int64_t timestamp, const uint160 &msgId,
                               const uint160 &parentMsgId, uint16_t nRetentionDays)
{
    if (!pdb) return false;

    std::string sKey = MakeTopicKey(topic, timestamp, msgId);
    // Value: msgId(20) + parentMsgId(20) + retentionDays(2) = 42 bytes
    std::string sValue;
    sValue.reserve(42);
    sValue.append((const char*)msgId.begin(), 20);
    sValue.append((const char*)parentMsgId.begin(), 20);
    sValue.append((const char*)&nRetentionDays, 2);

    if (activeBatch) {
        activeBatch->Put(sKey, sValue);
        return true;
    }

    leveldb::WriteOptions wo;
    wo.sync = false; // topic index is derived; can be rebuilt if lost
    leveldb::Status s = pdb->Put(wo, sKey, sValue);
    if (!s.ok())
        return error("SecMsgDB WriteTopicIndex failed: %s\n", s.ToString());

    return true;
};

bool SecMsgDB::ReadTopicMessages(const std::string &topic,
                                  std::vector<TopicEntry> &out,
                                  size_t maxEntries)
{
    if (!pdb) return false;

    std::string sPrefix = MakeTopicPrefix(topic);
    leveldb::Iterator *it = pdb->NewIterator(leveldb::ReadOptions());

    // Helper: parse a key+value pair into a TopicEntry
    auto parseEntry = [&](leveldb::Slice k, leveldb::Slice v) -> bool {
        size_t tsOffset = 2 + 1 + topic.size();
        if (k.size() < tsOffset + 8 + 20) return false;
        TopicEntry e;
        e.timestamp = 0;
        for (int i = 0; i < 8; ++i) {
            e.timestamp = (e.timestamp << 8) | (uint8_t)k[tsOffset + i];
        }
        memcpy(e.msgId.begin(), k.data() + tsOffset + 8, 20);
        // Parse value: msgId(20) + parentMsgId(20) + retentionDays(2)
        if (v.size() >= 42) {
            memcpy(e.parentMsgId.begin(), v.data() + 20, 20);
            memcpy(&e.nRetentionDays, v.data() + 40, 2);
        } else if (v.size() >= 20) {
            // Legacy entries: value is just msgId
            e.parentMsgId.SetNull();
            e.nRetentionDays = 0;
        }
        out.push_back(e);
        return true;
    };

    if (maxEntries > 0) {
        // Reverse scan: seek past the prefix range, then iterate backwards.
        std::string sEnd = sPrefix;
        sEnd.back() = (char)((uint8_t)sEnd.back() + 1);
        it->Seek(sEnd);
        if (it->Valid()) {
            it->Prev();
        } else {
            it->SeekToLast();
        }
        while (it->Valid() && out.size() < maxEntries) {
            leveldb::Slice k = it->key();
            if (k.size() < sPrefix.size() || memcmp(k.data(), sPrefix.data(), sPrefix.size()) != 0)
                break;
            parseEntry(k, it->value());
            it->Prev();
        }
        std::reverse(out.begin(), out.end());
    } else {
        // Forward scan: return all entries in ascending order.
        for (it->Seek(sPrefix);
             it->Valid() && it->key().starts_with(sPrefix);
             it->Next()) {
            parseEntry(it->key(), it->value());
        }
    }

    bool ok = it->status().ok();
    delete it;
    return ok;
};

bool SecMsgDB::EraseTopicIndex(const std::string &topic, int64_t timestamp, const uint160 &msgId)
{
    if (!pdb) return false;

    std::string sKey = MakeTopicKey(topic, timestamp, msgId);

    if (activeBatch) {
        activeBatch->Delete(sKey);
        return true;
    }

    leveldb::WriteOptions wo;
    wo.sync = false;
    leveldb::Status s = pdb->Delete(wo, sKey);
    if (!s.ok() && !s.IsNotFound())
        return error("SecMsgDB EraseTopicIndex failed: %s\n", s.ToString());

    return true;
};

} // namespace smsg
