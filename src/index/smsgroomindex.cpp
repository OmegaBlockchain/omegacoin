// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/smsgroomindex.h>

#include <evo/smsgroomtx.h>
#include <evo/specialtx.h>
#include <primitives/transaction.h>
#include <util/system.h>
#include <validation.h>

constexpr char DB_SMSGROOM = 'R';

std::unique_ptr<SmsgRoomIndex> g_smsgroomindex;

class SmsgRoomIndex::DB : public BaseIndex::DB
{
public:
    explicit DB(size_t n_cache_size, bool f_memory = false, bool f_wipe = false);

    bool WriteRoom(const uint256& txid, const SmsgRoomIndexEntry& entry);
    bool ReadRoom(const uint256& txid, SmsgRoomIndexEntry& entry) const;
    bool ListAllRooms(std::vector<std::pair<uint256, SmsgRoomIndexEntry>>& rooms);
};

SmsgRoomIndex::DB::DB(size_t n_cache_size, bool f_memory, bool f_wipe) :
    BaseIndex::DB(GetDataDir() / "indexes" / "smsgroomindex", n_cache_size, f_memory, f_wipe)
{}

bool SmsgRoomIndex::DB::WriteRoom(const uint256& txid, const SmsgRoomIndexEntry& entry)
{
    return Write(std::make_pair(DB_SMSGROOM, txid), entry);
}

bool SmsgRoomIndex::DB::ReadRoom(const uint256& txid, SmsgRoomIndexEntry& entry) const
{
    return Read(std::make_pair(DB_SMSGROOM, txid), entry);
}

bool SmsgRoomIndex::DB::ListAllRooms(std::vector<std::pair<uint256, SmsgRoomIndexEntry>>& rooms)
{
    std::unique_ptr<CDBIterator> cursor(NewIterator());
    std::pair<char, uint256> key;
    cursor->Seek(std::make_pair(DB_SMSGROOM, uint256()));

    while (cursor->Valid()) {
        if (!cursor->GetKey(key) || key.first != DB_SMSGROOM) {
            break;
        }
        SmsgRoomIndexEntry entry;
        if (!cursor->GetValue(entry)) {
            return false;
        }
        rooms.emplace_back(key.second, entry);
        cursor->Next();
    }
    return true;
}

SmsgRoomIndex::SmsgRoomIndex(size_t n_cache_size, bool f_memory, bool f_wipe)
    : m_db(std::make_unique<SmsgRoomIndex::DB>(n_cache_size, f_memory, f_wipe))
{}

SmsgRoomIndex::~SmsgRoomIndex() {}

bool SmsgRoomIndex::WriteBlock(const CBlock& block, const CBlockIndex* pindex)
{
    for (const auto& tx : block.vtx) {
        if (tx->nVersion != 3 || tx->nType != TRANSACTION_SMSG_ROOM) {
            continue;
        }

        CSmsgRoomTx roomTx;
        if (!GetTxPayload(*tx, roomTx)) {
            continue;
        }

        SmsgRoomIndexEntry entry;
        entry.nVersion = roomTx.nVersion;
        entry.nFlags = roomTx.nFlags;
        entry.vchRoomPubKey = roomTx.vchRoomPubKey;
        entry.nRetentionDays = roomTx.nRetentionDays;
        entry.nMaxMembers = roomTx.nMaxMembers;
        entry.nHeight = pindex->nHeight;

        if (!m_db->WriteRoom(tx->GetHash(), entry)) {
            return false;
        }
    }
    return true;
}

BaseIndex::DB& SmsgRoomIndex::GetDB() const { return *m_db; }

bool SmsgRoomIndex::FindRoom(const uint256& txid, SmsgRoomIndexEntry& entry) const
{
    return m_db->ReadRoom(txid, entry);
}

bool SmsgRoomIndex::ListRooms(std::vector<std::pair<uint256, SmsgRoomIndexEntry>>& rooms)
{
    return m_db->ListAllRooms(rooms);
}
