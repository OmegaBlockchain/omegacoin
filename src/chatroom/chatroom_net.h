// Copyright (c) 2018-2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_CHATROOM_NET_H
#define DASH_CHATROOM_NET_H

#include <chatroom/chatmessage.h>
#include <chatroom/chatroom.h>
#include <net.h>
#include <protocol.h>

bool ProcessChatRoomAnnouncement(CNode* pfrom, CDataStream& vRecv);
bool ProcessChatMessageStem(CNode* pfrom, CDataStream& vRecv, CConnman& connman);
bool ProcessChatMessageFluff(CNode* pfrom, CDataStream& vRecv, CConnman& connman);
bool ProcessChatMessageInv(CNode* pfrom, const CInv& inv, CConnman& connman);
bool ProcessChatMessageGetData(CNode* pfrom, const std::deque<CInv>& vInv, CConnman& connman);

void SendChatRoomAnnouncement(const CChatRoom& room, CConnman& connman);
void SendChatMessageStem(const CChatMessage& msg, CNode* target, CConnman& connman);
void SendChatMessageFluff(const uint256& msgHash, CConnman& connman);
void SendChatMessage(const CChatMessage& msg, CNode* target, CConnman& connman);
void RelayChatroom(const CChatMessage& msg, NodeId from);

#endif // DASH_CHATROOM_NET_H
