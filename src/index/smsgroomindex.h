// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INDEX_SMSGROOMINDEX_H
#define BITCOIN_INDEX_SMSGROOMINDEX_H

#include <index/base.h>
#include <serialize.h>

#include <vector>

struct SmsgRoomIndexEntry
{
    uint8_t nVersion{0};
    uint32_t nFlags{0};
    std::vector<uint8_t> vchRoomPubKey;
    uint32_t nRetentionDays{0};
    uint32_t nMaxMembers{0};
    int nHeight{0};

    SERIALIZE_METHODS(SmsgRoomIndexEntry, obj)
    {
        READWRITE(obj.nVersion, obj.nFlags, obj.vchRoomPubKey,
                  obj.nRetentionDays, obj.nMaxMembers, obj.nHeight);
    }
};

/**
 * SmsgRoomIndex maintains a LevelDB index of TRANSACTION_SMSG_ROOM (type 8)
 * special transactions, keyed by txid.  This allows efficient room lookup and
 * enumeration without scanning the full chain.
 */
class SmsgRoomIndex final : public BaseIndex
{
protected:
    class DB;

private:
    const std::unique_ptr<DB> m_db;

protected:
    bool WriteBlock(const CBlock& block, const CBlockIndex* pindex) override;
    bool Rewind(const CBlockIndex* current_tip, const CBlockIndex* new_tip) override;

    BaseIndex::DB& GetDB() const override;

    const char* GetName() const override { return "smsgroomindex"; }

public:
    explicit SmsgRoomIndex(size_t n_cache_size, bool f_memory = false, bool f_wipe = false);
    virtual ~SmsgRoomIndex() override;

    /// Look up a single room by its creation txid.
    bool FindRoom(const uint256& txid, SmsgRoomIndexEntry& entry) const;

    /// Return all indexed rooms.
    bool ListRooms(std::vector<std::pair<uint256, SmsgRoomIndexEntry>>& rooms);
};

/// The global SMSG room index.  May be null.
extern std::unique_ptr<SmsgRoomIndex> g_smsgroomindex;

#endif // BITCOIN_INDEX_SMSGROOMINDEX_H
