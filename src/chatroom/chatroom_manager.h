// Copyright (c) 2018-2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_CHATROOM_MANAGER_H
#define DASH_CHATROOM_MANAGER_H

#include <chatroom/chatroom.h>
#include <chatroom/chatmessage.h>
#include <net.h>
#include <sync.h>

#include <deque>
#include <map>
#include <set>

// already in chatroom_limits.h
// static const size_t MAX_MESSAGE_CACHE = 10000;
// static const int64_t MESSAGE_CACHE_TIME = 3600;
// static const int64_t FLUFF_BROADCAST_DELAY_MS_MIN = 1000;
// static const int64_t FLUFF_BROADCAST_DELAY_MS_MAX = 5000;

class CChatRoomManager
{
private:
    mutable RecursiveMutex cs_manager;

    std::deque<std::pair<int64_t, uint256>> recentMessages GUARDED_BY(cs_manager);
    std::set<uint256> messageHashSet GUARDED_BY(cs_manager);

    std::set<uint256> setLocalRooms GUARDED_BY(cs_manager);

    std::multimap<int64_t, uint256> mapScheduledFluff GUARDED_BY(cs_manager);

    std::map<uint256, NodeId> mapStemRoutes GUARDED_BY(cs_manager);

    void CleanupMessageCache() EXCLUSIVE_LOCKS_REQUIRED(cs_manager);
    CNode* SelectRandomPeer(CConnman& connman, NodeId exclude = -1);

public:
    CChatRoomManager() = default;

    bool CreateRoom(const std::string& name, const CKey& creatorKey, uint256& roomIdOut);
    bool AddLocalRoom(const uint256& roomId);
    bool RemoveLocalRoom(const uint256& roomId);
    bool IsLocalRoom(const uint256& roomId) const;
    std::vector<uint256> GetLocalRooms() const;

    bool SendMessage(
        const uint256& roomId,
        const std::string& text,
        const CKey& senderKey,
        CConnman& connman
    );

    bool ProcessIncomingMessage(
        const CChatMessage& msg,
        NodeId from,
        CConnman& connman
    );

    bool HaveSeenMessage(const uint256& hash) const;
    void MarkMessageSeen(const uint256& hash);

    bool RelayMessage(const CChatMessage& msg, CConnman& connman);
    bool ProcessStemMessage(const CChatMessage& msg, NodeId from, CConnman& connman);
    bool ProcessFluffMessage(const CChatMessage& msg, CConnman& connman);

    void ProcessScheduledBroadcasts(CConnman& connman);
    void ScheduleFluffBroadcast(const uint256& msgHash, int64_t delayMs = 0);

    bool ShouldStoreMessage(const CChatMessage& msg) const;

    bool MuteUser(const uint256& roomId, const CPubKey& target, const CKey& creatorKey);
    bool UnmuteUser(const uint256& roomId, const CPubKey& target, const CKey& creatorKey);

    void PeriodicMaintenance();
    size_t GetCacheSize() const;
};

extern std::unique_ptr<CChatRoomManager> g_chatroom_manager;

#endif // DASH_CHATROOM_MANAGER_H
