// Copyright (c) 2018-2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chatroom/chatroom_net.h>
#include <chatroom/chatroom_manager.h>
#include <chatroom/chatroom_db.h>
#include <chatroom/chatroom_limits.h>

#include <logging.h>
#include <netmessagemaker.h>
#include <protocol.h>
#include <validation.h>

bool ProcessChatRoomAnnouncement(CNode* pfrom, CDataStream& vRecv)
{
    if (!g_chatroom_db || !g_chatroom_manager) {
        return false;
    }

    CChatRoom room;
    try {
        vRecv >> room;
    } catch (const std::exception& e) {
        LogPrint(BCLog::NET, "ChatRoom: Failed to deserialize room announcement from peer=%d: %s\n", 
                 pfrom->GetId(), e.what());
        return false;
    }

    if (!room.IsValid()) {
        LogPrint(BCLog::NET, "ChatRoom: Invalid room announcement from peer=%d\n", pfrom->GetId());
        return false;
    }

    // Check if we already have this room
    if (g_chatroom_db->ExistsRoom(room.GetRoomId())) {
        return true;
    }

    // Store the room
    if (!g_chatroom_db->WriteRoom(room)) {
        LogPrint(BCLog::NET, "ChatRoom: Failed to store room from peer=%d\n", pfrom->GetId());
        return false;
    }

    LogPrint(BCLog::NET, "ChatRoom: Received room announcement '%s' from peer=%d\n", 
             room.GetRoomName(), pfrom->GetId());

    return true;
}

bool ProcessChatMessageStem(CNode* pfrom, CDataStream& vRecv, CConnman& connman)
{
    if (!g_chatroom_manager) {
        return false;
    }

    CChatMessage msg;
    try {
        vRecv >> msg;
    } catch (const std::exception& e) {
        LogPrint(BCLog::NET, "ChatRoom: Failed to deserialize STEM message from peer=%d: %s\n", 
                 pfrom->GetId(), e.what());
        return false;
    }

    // Validate message size before processing
    if (msg.GetPayload().size() > MAX_CHATROOM_MSG_SIZE) {
        LogPrint(BCLog::NET, "ChatRoom: STEM message too large (%d bytes) from peer=%d\n", 
                 msg.GetPayload().size(), pfrom->GetId());
        return false;
    }

    // Verify this is actually a stem message
    if (msg.GetPhase() != PHASE_STEM) {
        LogPrint(BCLog::NET, "ChatRoom: Received STEM message not in STEM phase from peer=%d\n", 
                 pfrom->GetId());
        return false;
    }

    return g_chatroom_manager->ProcessIncomingMessage(msg, pfrom->GetId(), connman);
}

bool ProcessChatMessageFluff(CNode* pfrom, CDataStream& vRecv, CConnman& connman)
{
    if (!g_chatroom_manager) {
        return false;
    }

    CChatMessage msg;
    try {
        vRecv >> msg;
    } catch (const std::exception& e) {
        LogPrint(BCLog::NET, "ChatRoom: Failed to deserialize FLUFF message from peer=%d: %s\n", 
                 pfrom->GetId(), e.what());
        return false;
    }

    // Validate message size before processing
    if (msg.GetPayload().size() > MAX_CHATROOM_MSG_SIZE) {
        LogPrint(BCLog::NET, "ChatRoom: FLUFF message too large (%d bytes) from peer=%d\n",
                 msg.GetPayload().size(), pfrom->GetId());
        return false;
    }

    // Fluff messages must be in fluff phase
    if (msg.GetPhase() != PHASE_FLUFF) {
        LogPrint(BCLog::NET, "ChatRoom: Received FLUFF message not in FLUFF phase from peer=%d\n", 
                 pfrom->GetId());
        return false;
    }

    return g_chatroom_manager->ProcessIncomingMessage(msg, pfrom->GetId(), connman);
}

bool ProcessChatMessageInv(CNode* pfrom, const CInv& inv, CConnman& connman)
{
    if (!g_chatroom_manager) {
        return false;
    }

    // Check if we already have this message
    if (g_chatroom_manager->HaveSeenMessage(inv.hash)) {
        return true;
    }

    // Request the message
    std::vector<CInv> vGetData;
    vGetData.push_back(inv);
    
    const CNetMsgMaker msgMaker(pfrom->GetSendVersion());
    connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::GETDATA, vGetData));

    LogPrint(BCLog::NET, "ChatRoom: Requesting message %s from peer=%d\n", 
             inv.hash.ToString(), pfrom->GetId());

    return true;
}

bool ProcessChatMessageGetData(CNode* pfrom, const std::deque<CInv>& vInv, CConnman& connman)
{
    if (!g_chatroom_db || !g_chatroom_manager) {
        return false;
    }

    const CNetMsgMaker msgMaker(pfrom->GetSendVersion());
    size_t found = 0;

    for (const CInv& inv : vInv) {
        if (inv.type != MSG_CHATMSG) {
            continue;
        }

        CChatMessage msg;
        if (!g_chatroom_db->ReadMessage(inv.hash, msg)) {
            // Don't have this message
            LogPrint(BCLog::NET, "ChatRoom: Don't have message %s requested by peer=%d\n",
                     inv.hash.ToString(), pfrom->GetId());
            continue;
        }

        // Send the message based on its phase
        if (msg.GetPhase() == PHASE_STEM) {
            connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::CHATMSGSTEM, msg));
            LogPrint(BCLog::NET, "ChatRoom: Sent STEM message %s to peer=%d\n",
                     inv.hash.ToString(), pfrom->GetId());
        } else {
            connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::CHATMSGFLUFF, msg));
            LogPrint(BCLog::NET, "ChatRoom: Sent FLUFF message %s to peer=%d\n",
                     inv.hash.ToString(), pfrom->GetId());
        }
        found++;
    }

    if (found > 0) {
        LogPrint(BCLog::NET, "ChatRoom: Sent %d messages to peer=%d\n", found, pfrom->GetId());
    }

    return true;
}

void SendChatRoomAnnouncement(const CChatRoom& room, CConnman& connman)
{
    if (!room.IsValid()) {
        LogPrint(BCLog::NET, "ChatRoom: Cannot announce invalid room\n");
        return;
    }

    if (!room.ShouldAnnounce()) {
        LogPrint(BCLog::NET, "ChatRoom: Room '%s' should not be announced (inactive)\n", 
                 room.GetRoomName());
        return;
    }

    size_t announced = 0;
    connman.ForEachNode([&room, &announced, &connman](CNode* pnode) {
        if (pnode->fDisconnect || !pnode->fSuccessfullyConnected) {
            return;
        }

        const CNetMsgMaker msgMaker(pnode->GetSendVersion());
        connman.PushMessage(pnode, msgMaker.Make(NetMsgType::CHATROOM, room));
        announced++;
    });

    LogPrint(BCLog::NET, "ChatRoom: Announced room '%s' to %d peers\n", 
             room.GetRoomName(), announced);
}

void SendChatMessageStem(const CChatMessage& msg, CNode* target, CConnman& connman)
{
    if (!target || target->fDisconnect) {
        LogPrint(BCLog::NET, "ChatRoom: Cannot send STEM to disconnected peer\n");
        return;
    }

    if (msg.GetPhase() != PHASE_STEM) {
        LogPrint(BCLog::NET, "ChatRoom: Attempted to send non-STEM message via STEM\n");
        return;
    }

    const CNetMsgMaker msgMaker(target->GetSendVersion());
    connman.PushMessage(target, msgMaker.Make(NetMsgType::CHATMSGSTEM, msg));

    LogPrint(BCLog::NET, "ChatRoom: Sent STEM message %s to peer=%d\n", 
             msg.GetHash().ToString(), target->GetId());
}

void SendChatMessageFluff(const uint256& msgHash, CConnman& connman)
{
    if (!g_chatroom_db) {
        return;
    }

    CChatMessage msg;
    if (!g_chatroom_db->ReadMessage(msgHash, msg)) {
        LogPrint(BCLog::NET, "ChatRoom: Cannot broadcast unknown message %s\n", 
                 msgHash.ToString());
        return;
    }

    // Ensure message is in fluff phase
    if (msg.GetPhase() != PHASE_FLUFF) {
        LogPrint(BCLog::NET, "ChatRoom: Attempted to broadcast non-FLUFF message %s\n",
                 msgHash.ToString());
        return;
    }

    size_t broadcast = 0;
    connman.ForEachNode([&msg, &broadcast, &connman](CNode* pnode) {
        if (pnode->fDisconnect || !pnode->fSuccessfullyConnected) {
            return;
        }

        const CNetMsgMaker msgMaker(pnode->GetSendVersion());
        connman.PushMessage(pnode, msgMaker.Make(NetMsgType::CHATMSGFLUFF, msg));
        broadcast++;
    });

    LogPrint(BCLog::NET, "ChatRoom: Broadcast FLUFF message %s to %d peers\n", 
             msgHash.ToString(), broadcast);
}

void SendChatMessage(const CChatMessage& msg, CNode* target, CConnman& connman)
{
    if (!target || target->fDisconnect) {
        LogPrint(BCLog::NET, "ChatRoom: Cannot send message to disconnected peer\n");
        return;
    }

    if (msg.GetPhase() == PHASE_STEM) {
        SendChatMessageStem(msg, target, connman);
    } else {
        const CNetMsgMaker msgMaker(target->GetSendVersion());
        connman.PushMessage(target, msgMaker.Make(NetMsgType::CHATMSGFLUFF, msg));
        
        LogPrint(BCLog::NET, "ChatRoom: Sent FLUFF message %s to peer=%d\n",
                 msg.GetHash().ToString(), target->GetId());
    }
}

void RelayChatroom(const CChatMessage& msg, NodeId from)
{
    // This function can be used for additional relay strategies
    // Currently, relay is handled by the manager's ProcessIncomingMessage
    // This is a placeholder for future enhancements
    
    LogPrint(BCLog::NET, "ChatRoom: Relay requested for message %s from peer=%d\n",
             msg.GetHash().ToString(), from);
}