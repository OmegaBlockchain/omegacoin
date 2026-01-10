// Copyright (c) 2025 Dash Core Group
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_CHATROOM_DB_H
#define DASH_CHATROOM_DB_H

#include <chatroom/chatroom.h>
#include <chatroom/chatmessage.h>
#include <dbwrapper.h>
#include <sync.h>

#include <memory>
#include <vector>

class CChatRoomDB
{
private:
    std::unique_ptr<CDBWrapper> db;
    mutable RecursiveMutex cs_db;

    static constexpr char DB_ROOM = 'r';
    static constexpr char DB_MESSAGE = 'm';
    static constexpr char DB_ROOM_INDEX = 'i';
    static constexpr char DB_CREATOR_INDEX = 'c';
    static constexpr char DB_MESSAGE_INDEX = 'x';

public:
    explicit CChatRoomDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);
    ~CChatRoomDB();

    CChatRoomDB(const CChatRoomDB&) = delete;
    CChatRoomDB& operator=(const CChatRoomDB&) = delete;

    bool WriteRoom(const CChatRoom& room);
    bool ReadRoom(const uint256& roomId, CChatRoom& room) const;
    bool ExistsRoom(const uint256& roomId) const;
    bool EraseRoom(const uint256& roomId);

    bool WriteMessage(const CChatMessage& msg);
    bool ReadMessage(const uint256& hash, CChatMessage& msg) const;
    bool ExistsMessage(const uint256& hash) const;

    bool GetRoomByName(const std::string& name, uint256& roomId) const;
    std::vector<uint256> GetRoomsByCreator(const CPubKey& creator) const;
    std::vector<std::string> ListAllRooms(size_t limit = 1000) const;

    std::vector<CChatMessage> GetRoomMessages(
        const uint256& roomId,
        int64_t timeFrom = 0,
        int64_t timeTo = 0,
        size_t limit = 100
    ) const;

    std::vector<uint256> GetExpiredRooms(int64_t olderThan) const;
    bool DeleteRoom(const uint256& roomId);
    size_t CleanupExpiredRooms();
    bool CleanupOldMessages(int64_t olderThan);

    size_t GetMessageCount() const;
    size_t GetRoomCount() const;

private:
    std::vector<uint8_t> MakeRoomKey(const uint256& roomId) const;
    std::vector<uint8_t> MakeMessageKey(const uint256& roomId, int64_t timestamp, const uint256& hash) const;
    std::vector<uint8_t> MakeRoomIndexKey(const std::string& name) const;
    std::vector<uint8_t> MakeCreatorIndexKey(const CPubKey& creator) const;
    std::vector<uint8_t> MakeMessageIndexKey(const uint256& roomId, int64_t timestamp) const;
};

extern std::unique_ptr<CChatRoomDB> g_chatroom_db;

#endif // DASH_CHATROOM_DB_H
