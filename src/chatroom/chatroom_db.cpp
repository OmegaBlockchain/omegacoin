// Copyright (c) 2018-2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chatroom/chatroom_db.h>

#include <logging.h>
#include <util/system.h>

std::unique_ptr<CChatRoomDB> g_chatroom_db;

CChatRoomDB::CChatRoomDB(size_t nCacheSize, bool fMemory, bool fWipe)
{
    fs::path datadir = GetDataDir() / "chatroom";
    fs::create_directories(datadir);

    db = std::make_unique<CDBWrapper>(datadir, nCacheSize, fMemory, fWipe);
}

CChatRoomDB::~CChatRoomDB()
{
}

std::vector<uint8_t> CChatRoomDB::MakeRoomKey(const uint256& roomId) const
{
    std::vector<uint8_t> key;
    key.push_back(DB_ROOM);
    key.insert(key.end(), roomId.begin(), roomId.end());
    return key;
}

std::vector<uint8_t> CChatRoomDB::MakeMessageKey(const uint256& roomId, int64_t timestamp, const uint256& hash) const
{
    std::vector<uint8_t> key;
    key.push_back(DB_MESSAGE);
    key.insert(key.end(), roomId.begin(), roomId.end());

    uint8_t timeBytes[8];
    for (int i = 7; i >= 0; i--) {
        timeBytes[i] = timestamp & 0xFF;
        timestamp >>= 8;
    }
    key.insert(key.end(), timeBytes, timeBytes + 8);

    key.insert(key.end(), hash.begin(), hash.end());
    return key;
}

std::vector<uint8_t> CChatRoomDB::MakeRoomIndexKey(const std::string& name) const
{
    std::vector<uint8_t> key;
    key.push_back(DB_ROOM_INDEX);
    key.insert(key.end(), name.begin(), name.end());
    return key;
}

std::vector<uint8_t> CChatRoomDB::MakeCreatorIndexKey(const CPubKey& creator) const
{
    std::vector<uint8_t> key;
    key.push_back(DB_CREATOR_INDEX);
    std::vector<uint8_t> pubkeyBytes(creator.begin(), creator.end());
    key.insert(key.end(), pubkeyBytes.begin(), pubkeyBytes.end());
    return key;
}

std::vector<uint8_t> CChatRoomDB::MakeMessageIndexKey(const uint256& roomId, int64_t timestamp) const
{
    std::vector<uint8_t> key;
    key.push_back(DB_MESSAGE_INDEX);
    key.insert(key.end(), roomId.begin(), roomId.end());

    uint8_t timeBytes[8];
    for (int i = 7; i >= 0; i--) {
        timeBytes[i] = timestamp & 0xFF;
        timestamp >>= 8;
    }
    key.insert(key.end(), timeBytes, timeBytes + 8);
    return key;
}

bool CChatRoomDB::WriteRoom(const CChatRoom& room)
{
    LOCK(cs_db);

    CDBBatch batch(*db);

    batch.Write(MakeRoomKey(room.GetRoomId()), room);
    batch.Write(MakeRoomIndexKey(room.GetRoomName()), room.GetRoomId());

    std::vector<uint256> creatorRooms = GetRoomsByCreator(room.GetCreatorPubKey());
    if (std::find(creatorRooms.begin(), creatorRooms.end(), room.GetRoomId()) == creatorRooms.end()) {
        creatorRooms.push_back(room.GetRoomId());
    }
    batch.Write(MakeCreatorIndexKey(room.GetCreatorPubKey()), creatorRooms);

    return db->WriteBatch(batch);
}

bool CChatRoomDB::ReadRoom(const uint256& roomId, CChatRoom& room) const
{
    LOCK(cs_db);
    return db->Read(MakeRoomKey(roomId), room);
}

bool CChatRoomDB::ExistsRoom(const uint256& roomId) const
{
    LOCK(cs_db);
    return db->Exists(MakeRoomKey(roomId));
}

bool CChatRoomDB::EraseRoom(const uint256& roomId)
{
    LOCK(cs_db);

    CChatRoom room;
    if (!ReadRoom(roomId, room)) {
        return false;
    }

    CDBBatch batch(*db);
    batch.Erase(MakeRoomKey(roomId));
    batch.Erase(MakeRoomIndexKey(room.GetRoomName()));

    return db->WriteBatch(batch);
}

bool CChatRoomDB::WriteMessage(const CChatMessage& msg)
{
    LOCK(cs_db);

    CDBBatch batch(*db);

    std::vector<uint8_t> key = MakeMessageKey(msg.GetRoomId(), msg.GetTimestamp(), msg.GetHash());
    batch.Write(key, msg);

    batch.Write(MakeMessageIndexKey(msg.GetRoomId(), msg.GetTimestamp()), msg.GetHash());

    return db->WriteBatch(batch);
}

bool CChatRoomDB::ReadMessage(const uint256& hash, CChatMessage& msg) const
{
    LOCK(cs_db);

    std::unique_ptr<CDBIterator> pcursor(db->NewIterator());
    std::vector<uint8_t> prefix(1, DB_MESSAGE);
    pcursor->Seek(prefix);

    while (pcursor->Valid()) {
        std::vector<uint8_t> key;
        if (!pcursor->GetKey(key) || key.empty() || key[0] != DB_MESSAGE) {
            break;
        }

        CChatMessage tempMsg;
        if (pcursor->GetValue(tempMsg)) {
            if (tempMsg.GetHash() == hash) {
                msg = tempMsg;
                return true;
            }
        }

        pcursor->Next();
    }

    return false;
}

bool CChatRoomDB::ExistsMessage(const uint256& hash) const
{
    CChatMessage msg;
    return ReadMessage(hash, msg);
}

bool CChatRoomDB::GetRoomByName(const std::string& name, uint256& roomId) const
{
    LOCK(cs_db);
    return db->Read(MakeRoomIndexKey(name), roomId);
}

std::vector<uint256> CChatRoomDB::GetRoomsByCreator(const CPubKey& creator) const
{
    LOCK(cs_db);

    std::vector<uint256> rooms;
    db->Read(MakeCreatorIndexKey(creator), rooms);
    return rooms;
}

std::vector<std::string> CChatRoomDB::ListAllRooms(size_t limit) const
{
    LOCK(cs_db);

    std::vector<std::string> roomNames;
    std::unique_ptr<CDBIterator> pcursor(db->NewIterator());

    std::vector<uint8_t> prefix(1, DB_ROOM_INDEX);
    pcursor->Seek(prefix);

    while (pcursor->Valid() && roomNames.size() < limit) {
        std::vector<uint8_t> key;
        if (!pcursor->GetKey(key) || key.empty() || key[0] != DB_ROOM_INDEX) {
            break;
        }

        std::string roomName(key.begin() + 1, key.end());
        roomNames.push_back(roomName);

        pcursor->Next();
    }

    return roomNames;
}

std::vector<CChatMessage> CChatRoomDB::GetRoomMessages(
    const uint256& roomId,
    int64_t timeFrom,
    int64_t timeTo,
    size_t limit
) const
{
    LOCK(cs_db);

    std::vector<CChatMessage> messages;
    std::unique_ptr<CDBIterator> pcursor(db->NewIterator());

    std::vector<uint8_t> prefix;
    prefix.push_back(DB_MESSAGE);
    prefix.insert(prefix.end(), roomId.begin(), roomId.end());

    pcursor->Seek(prefix);

    while (pcursor->Valid() && messages.size() < limit) {
        std::vector<uint8_t> key;
        if (!pcursor->GetKey(key)) break;

        if (key.size() < prefix.size() ||
            !std::equal(prefix.begin(), prefix.end(), key.begin())) {
            break;
        }

        CChatMessage msg;
        if (pcursor->GetValue(msg)) {
            int64_t msgTime = msg.GetTimestamp();

            if ((timeFrom == 0 || msgTime >= timeFrom) &&
                (timeTo == 0 || msgTime <= timeTo)) {
                messages.push_back(msg);
            }
        }

        pcursor->Next();
    }

    return messages;
}

std::vector<uint256> CChatRoomDB::GetExpiredRooms(int64_t olderThan) const
{
    LOCK(cs_db);

    std::vector<uint256> expired;
    std::unique_ptr<CDBIterator> pcursor(db->NewIterator());

    std::vector<uint8_t> prefix(1, DB_ROOM);
    pcursor->Seek(prefix);

    while (pcursor->Valid()) {
        std::vector<uint8_t> key;
        if (!pcursor->GetKey(key) || key.empty() || key[0] != DB_ROOM) {
            break;
        }

        CChatRoom room;
        if (pcursor->GetValue(room)) {
            if (room.GetTimeSinceActivity() > olderThan) {
                expired.push_back(room.GetRoomId());
            }
        }

        pcursor->Next();
    }

    return expired;
}

bool CChatRoomDB::DeleteRoom(const uint256& roomId)
{
    LOCK(cs_db);

    CChatRoom room;
    if (!ReadRoom(roomId, room)) {
        return false;
    }

    CDBBatch batch(*db);

    batch.Erase(MakeRoomKey(roomId));
    batch.Erase(MakeRoomIndexKey(room.GetRoomName()));

    std::vector<uint256> creatorRooms = GetRoomsByCreator(room.GetCreatorPubKey());
    creatorRooms.erase(
        std::remove(creatorRooms.begin(), creatorRooms.end(), roomId),
        creatorRooms.end()
    );
    batch.Write(MakeCreatorIndexKey(room.GetCreatorPubKey()), creatorRooms);

    std::unique_ptr<CDBIterator> pcursor(db->NewIterator());
    std::vector<uint8_t> msgPrefix = {DB_MESSAGE};
    msgPrefix.insert(msgPrefix.end(), roomId.begin(), roomId.end());

    pcursor->Seek(msgPrefix);
    while (pcursor->Valid()) {
        std::vector<uint8_t> key;
        if (!pcursor->GetKey(key)) break;

        if (key.size() < msgPrefix.size() ||
            !std::equal(msgPrefix.begin(), msgPrefix.end(), key.begin())) {
            break;
        }

        batch.Erase(key);
        pcursor->Next();
    }

    return db->WriteBatch(batch);
}

size_t CChatRoomDB::CleanupExpiredRooms()
{
    std::vector<uint256> expired = GetExpiredRooms(ROOM_INACTIVE_THRESHOLD);

    size_t deleted = 0;
    for (const auto& roomId : expired) {
        if (DeleteRoom(roomId)) {
            deleted++;
        }
    }

    if (deleted > 0) {
        LogPrint(BCLog::NET, "ChatRoom: Cleaned up %d expired rooms\n", deleted);
    }

    return deleted;
}

bool CChatRoomDB::CleanupOldMessages(int64_t olderThan)
{
    LOCK(cs_db);

    CDBBatch batch(*db);
    size_t deleted = 0;

    std::unique_ptr<CDBIterator> pcursor(db->NewIterator());
    std::vector<uint8_t> prefix(1, DB_MESSAGE);
    pcursor->Seek(prefix);

    while (pcursor->Valid()) {
        std::vector<uint8_t> key;
        if (!pcursor->GetKey(key) || key.empty() || key[0] != DB_MESSAGE) {
            break;
        }

        CChatMessage msg;
        if (pcursor->GetValue(msg)) {
            if (msg.GetTimestamp() < olderThan) {
                batch.Erase(key);
                deleted++;
            }
        }

        pcursor->Next();
    }

    if (deleted > 0) {
        LogPrint(BCLog::NET, "ChatRoom: Cleaned up %d old messages\n", deleted);
        return db->WriteBatch(batch);
    }

    return true;
}

size_t CChatRoomDB::GetMessageCount() const
{
    LOCK(cs_db);

    size_t count = 0;
    std::unique_ptr<CDBIterator> pcursor(db->NewIterator());

    std::vector<uint8_t> prefix(1, DB_MESSAGE);
    pcursor->Seek(prefix);

    while (pcursor->Valid()) {
        std::vector<uint8_t> key;
        if (!pcursor->GetKey(key) || key.empty() || key[0] != DB_MESSAGE) {
            break;
        }
        count++;
        pcursor->Next();
    }

    return count;
}

size_t CChatRoomDB::GetRoomCount() const
{
    LOCK(cs_db);

    size_t count = 0;
    std::unique_ptr<CDBIterator> pcursor(db->NewIterator());

    std::vector<uint8_t> prefix(1, DB_ROOM);
    pcursor->Seek(prefix);

    while (pcursor->Valid()) {
        std::vector<uint8_t> key;
        if (!pcursor->GetKey(key) || key.empty() || key[0] != DB_ROOM) {
            break;
        }
        count++;
        pcursor->Next();
    }

    return count;
}
