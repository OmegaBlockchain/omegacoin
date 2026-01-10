// Copyright (c) 2018-2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chatroom/chatroom_manager.h>
#include <chatroom/chatroom_db.h>
#include <chatroom/chatroom_net.h>
#include <chatroom/chatroom_limits.h>

#include <logging.h>
#include <random.h>
#include <util/time.h>

std::unique_ptr<CChatRoomManager> g_chatroom_manager;

void CChatRoomManager::CleanupMessageCache()
{
    AssertLockHeld(cs_manager);

    int64_t now = GetTime();

    while (!recentMessages.empty() &&
           recentMessages.front().first < now - MESSAGE_CACHE_TIME) {
        messageHashSet.erase(recentMessages.front().second);
        recentMessages.pop_front();
    }

    while (messageHashSet.size() > MAX_MESSAGE_CACHE) {
        messageHashSet.erase(recentMessages.front().second);
        recentMessages.pop_front();
    }
}

CNode* CChatRoomManager::SelectRandomPeer(CConnman& connman, NodeId exclude)
{
    std::vector<CNode*> nodes;
    connman.ForEachNode([&nodes, exclude](CNode* pnode) {
        if (pnode->GetId() != exclude && !pnode->fDisconnect) {
            nodes.push_back(pnode);
        }
    });

    if (nodes.empty()) {
        return nullptr;
    }

    size_t idx = GetRand(nodes.size());
    return nodes[idx];
}

bool CChatRoomManager::CreateRoom(const std::string& name, const CKey& creatorKey, uint256& roomIdOut)
{
    if (!creatorKey.IsValid()) {
        return false;
    }

    // Check if room already exists
    uint256 existingId;
    if (g_chatroom_db->GetRoomByName(name, existingId)) {
        return false;
    }

    CChatRoom room;
    room.SetRoomName(name);
    room.SetCreator(creatorKey.GetPubKey());
    room.SetTimeCreated(GetTime());
    room.GenerateRoomId();

    if (!g_chatroom_db->WriteRoom(room)) {
        return false;
    }

    roomIdOut = room.GetRoomId();
    AddLocalRoom(roomIdOut);

    LogPrint(BCLog::NET, "ChatRoom: Created room '%s' (id=%s)\n",
             name, roomIdOut.GetHex());

    return true;
}

bool CChatRoomManager::AddLocalRoom(const uint256& roomId)
{
    LOCK(cs_manager);
    return setLocalRooms.insert(roomId).second;
}

bool CChatRoomManager::RemoveLocalRoom(const uint256& roomId)
{
    LOCK(cs_manager);
    return setLocalRooms.erase(roomId) > 0;
}

bool CChatRoomManager::IsLocalRoom(const uint256& roomId) const
{
    LOCK(cs_manager);
    return setLocalRooms.count(roomId) > 0;
}

std::vector<uint256> CChatRoomManager::GetLocalRooms() const
{
    LOCK(cs_manager);
    return std::vector<uint256>(setLocalRooms.begin(), setLocalRooms.end());
}

bool CChatRoomManager::SendMessage(
    const uint256& roomId,
    const std::string& text,
    const CKey& senderKey,
    CConnman& connman)
{
    if (!senderKey.IsValid()) {
        return false;
    }

    // Validate message size BEFORE creating message
    if (text.size() > MAX_CHATROOM_MSG_SIZE) {
        LogPrint(BCLog::NET, "ChatRoom: Message too large (%d bytes)\n", text.size());
        return false;
    }

    CChatRoom room;
    if (!g_chatroom_db->ReadRoom(roomId, room)) {
        return false;
    }

    if (room.IsExpired()) {
        return false;
    }

    CPubKey senderPubKey = senderKey.GetPubKey();
    if (room.IsMuted(senderPubKey)) {
        LogPrint(BCLog::NET, "ChatRoom: Sender is muted in room\n");
        return false;
    }

    CChatMessage msg;
    msg.SetRoomId(roomId);
    msg.SetSenderPubKey(senderPubKey);
    msg.SetPayload(text);
    msg.SetTimestamp(GetTime());
    msg.GenerateNonce();

    if (!msg.Sign(senderKey)) {
        return false;
    }

    msg.InitiateStem(DEFAULT_STEM_HOPS);

    // Store message in DB
    g_chatroom_db->WriteMessage(msg);

    // Update room activity
    room.UpdateActivity(msg.GetTimestamp());
    g_chatroom_db->WriteRoom(room);

    // Mark as seen
    MarkMessageSeen(msg.GetHash());

    return ProcessStemMessage(msg, -1, connman);
}

bool CChatRoomManager::ProcessIncomingMessage(
    const CChatMessage& msg,
    NodeId from,
    CConnman& connman)
{
    // Validate message size first
    if (msg.GetPayload().size() > MAX_CHATROOM_MSG_SIZE) {
        LogPrint(BCLog::NET, "ChatRoom: Oversized message from peer=%d\n", from);
        return false;
    }

    if (!msg.IsValid()) {
        LogPrint(BCLog::NET, "ChatRoom: Invalid message from peer=%d\n", from);
        return false;
    }

    if (HaveSeenMessage(msg.GetHash())) {
        return true;
    }

    CChatRoom room;
    if (!g_chatroom_db->ReadRoom(msg.GetRoomId(), room)) {
        LogPrint(BCLog::NET, "ChatRoom: Message for unknown room from peer=%d\n", from);
        return false;
    }

    if (room.IsMuted(msg.GetSenderPubKey())) {
        LogPrint(BCLog::NET, "ChatRoom: Message from muted user, dropping\n");
        return false;
    }

    MarkMessageSeen(msg.GetHash());

    room.UpdateActivity(msg.GetTimestamp());
    g_chatroom_db->WriteRoom(room);

    if (ShouldStoreMessage(msg)) {
        g_chatroom_db->WriteMessage(msg);
    }

    if (msg.GetPhase() == PHASE_STEM) {
        return ProcessStemMessage(msg, from, connman);
    } else {
        return ProcessFluffMessage(msg, connman);
    }
}

bool CChatRoomManager::HaveSeenMessage(const uint256& hash) const
{
    LOCK(cs_manager);
    return messageHashSet.count(hash) > 0;
}

void CChatRoomManager::MarkMessageSeen(const uint256& hash)
{
    LOCK(cs_manager);

    int64_t now = GetTime();
    recentMessages.push_back({now, hash});
    messageHashSet.insert(hash);

    CleanupMessageCache();
}

bool CChatRoomManager::ProcessStemMessage(const CChatMessage& msg, NodeId from, CConnman& connman)
{
    CChatMessage forwardMsg = msg;

    if (forwardMsg.GetStemHops() == 0 || !forwardMsg.DecrementStemHop()) {
        forwardMsg.SwitchToFluff();
        auto range = std::chrono::milliseconds(FLUFF_BROADCAST_DELAY_MS_MAX - FLUFF_BROADCAST_DELAY_MS_MIN);
        int64_t delay = GetRandMillis(range).count() + FLUFF_BROADCAST_DELAY_MS_MIN;
        ScheduleFluffBroadcast(forwardMsg.GetHash(), delay);
        return true;
    }

    CNode* target = SelectRandomPeer(connman, from);
    if (!target) {
        forwardMsg.SwitchToFluff();
        ScheduleFluffBroadcast(forwardMsg.GetHash(), 0);
        return true;
    }

    SendChatMessageStem(forwardMsg, target, connman);

    LOCK(cs_manager);
    mapStemRoutes[forwardMsg.GetHash()] = target->GetId();

    return true;
}

bool CChatRoomManager::ProcessFluffMessage(const CChatMessage& msg, CConnman& connman)
{
    return RelayMessage(msg, connman);
}

bool CChatRoomManager::RelayMessage(const CChatMessage& msg, CConnman& connman)
{
    CInv inv(MSG_CHATMSG, msg.GetHash());

    connman.ForEachNode([&inv](CNode* pnode) {
        if (!pnode->fDisconnect && pnode->fSuccessfullyConnected) {
            pnode->PushInventory(inv);
        }
    });

    return true;
}

void CChatRoomManager::ScheduleFluffBroadcast(const uint256& msgHash, int64_t delayMs)
{
    LOCK(cs_manager);

    int64_t scheduleTime = GetTimeMillis() + delayMs;
    mapScheduledFluff.insert({scheduleTime, msgHash});
}

void CChatRoomManager::ProcessScheduledBroadcasts(CConnman& connman)
{
    // Collect hashes to process (avoid holding lock during I/O)
    std::vector<uint256> toProcess;

    {
        LOCK(cs_manager);
        int64_t now = GetTimeMillis();

        auto it = mapScheduledFluff.begin();
        while (it != mapScheduledFluff.end() && it->first <= now) {
            toProcess.push_back(it->second);
            it = mapScheduledFluff.erase(it);
        }
    }

    // Process outside lock (I/O can block)
    for (const uint256& msgHash : toProcess) {
        CChatMessage msg;
        if (g_chatroom_db->ReadMessage(msgHash, msg)) {
            msg.SwitchToFluff();
            g_chatroom_db->WriteMessage(msg); // Update phase in DB
            RelayMessage(msg, connman);
        }
    }
}

bool CChatRoomManager::ShouldStoreMessage(const CChatMessage& msg) const
{
    LOCK(cs_manager);
    return setLocalRooms.count(msg.GetRoomId()) > 0;
}

bool CChatRoomManager::MuteUser(const uint256& roomId, const CPubKey& target, const CKey& creatorKey)
{
    CChatRoom room;
    if (!g_chatroom_db->ReadRoom(roomId, room)) {
        return false;
    }

    if (!room.AddMute(target, creatorKey)) {
        return false;
    }

    return g_chatroom_db->WriteRoom(room);
}

bool CChatRoomManager::UnmuteUser(const uint256& roomId, const CPubKey& target, const CKey& creatorKey)
{
    CChatRoom room;
    if (!g_chatroom_db->ReadRoom(roomId, room)) {
        return false;
    }

    if (!room.RemoveMute(target, creatorKey)) {
        return false;
    }

    return g_chatroom_db->WriteRoom(room);
}

void CChatRoomManager::PeriodicMaintenance()
{
    {
        LOCK(cs_manager);
        CleanupMessageCache();
    }

    if (g_chatroom_db) {
        size_t removed = g_chatroom_db->CleanupExpiredRooms();
        if (removed > 0) {
            LogPrint(BCLog::NET, "ChatRoom: Cleaned up %d expired rooms\n", removed);
        }

        int64_t cutoff = GetTime() - (30 * 86400); // 30 days
        g_chatroom_db->CleanupOldMessages(cutoff);
    }
}

size_t CChatRoomManager::GetCacheSize() const
{
    LOCK(cs_manager);
    return messageHashSet.size();
}
