// Copyright (c) 2018-2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_CHATROOM_RPC_H
#define DASH_CHATROOM_RPC_H

#include <rpc/server.h>
#include <univalue.h>

UniValue createchatroom(const JSONRPCRequest& request);
UniValue listchatrooms(const JSONRPCRequest& request);
UniValue joinchatroom(const JSONRPCRequest& request);
UniValue leavechatroom(const JSONRPCRequest& request);
UniValue getchatroominfo(const JSONRPCRequest& request);

UniValue sendchatmessage(const JSONRPCRequest& request);
UniValue getchatmessages(const JSONRPCRequest& request);

UniValue mutechatuser(const JSONRPCRequest& request);
UniValue unmutechatuser(const JSONRPCRequest& request);
UniValue listmutedusers(const JSONRPCRequest& request);

UniValue getchatsyncstatus(const JSONRPCRequest& request);
UniValue cleanupexpiredrooms(const JSONRPCRequest& request);

void RegisterChatRoomRPCCommands(CRPCTable& t);

#endif // DASH_CHATROOM_RPC_H
