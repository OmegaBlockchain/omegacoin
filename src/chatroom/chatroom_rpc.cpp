// Copyright (c) 2018-2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chatroom/chatroom_rpc.h>
#include <chatroom/chatroom.h>
#include <chatroom/chatroom_db.h>
#include <chatroom/chatroom_manager.h>

#include <node/context.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <util/strencodings.h>
#include <wallet/wallet.h>
#include <wallet/rpcwallet.h>
#include <init.h>
#include <net.h> // potrzebne dla CConnman
#include <key.h>
#include <wallet/scriptpubkeyman.h>

extern CConnman* chat_connman;

static CKey GetWalletKey(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet not available");
    }

    LOCK(wallet->cs_wallet);

    // Pobierz menedżera kluczy
    LegacyScriptPubKeyMan* spk_man = wallet->GetLegacyScriptPubKeyMan();
    if (!spk_man) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Legacy key manager not available");
    }

    // Pobierz wszystkie klucze
    std::set<CKeyID> keyIDs = spk_man->GetKeys();
    if (keyIDs.empty()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "No keys in wallet. Please run 'getnewaddress' first.");
    }

    // Weź pierwszy klucz
    CKeyID firstKeyID = *keyIDs.begin();

    // Pobierz klucz prywatny
    CKey key;
    if (!spk_man->GetKey(firstKeyID, key)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available (wallet locked?)");
    }

    return key;
}

UniValue createchatroom(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            "createchatroom \"roomname\"\n"
            "\nCreate a new chatroom.\n"
            "\nArguments:\n"
            "1. roomname    (string, required) Name of the chatroom\n"
            "\nResult:\n"
            "\"roomid\"     (string) The room ID\n"
            "\nExamples:\n"
            + HelpExampleCli("createchatroom", "\"MyRoom\"")
            + HelpExampleRpc("createchatroom", "\"MyRoom\"")
        );
    }

    if (!g_chatroom_manager) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Chatroom system not initialized");
    }

    std::string roomName = request.params[0].get_str();

    if (roomName.empty() || roomName.length() > MAX_ROOM_NAME_LENGTH) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid room name length");
    }

    CKey creatorKey = GetWalletKey(request);

    uint256 roomId;
    if (!g_chatroom_manager->CreateRoom(roomName, creatorKey, roomId)) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to create room");
    }

    return roomId.GetHex();
}

UniValue listchatrooms(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 1) {
        throw std::runtime_error(
            "listchatrooms ( \"state\" )\n"
            "\nList all chatrooms.\n"
            "\nArguments:\n"
            "1. state    (string, optional) Filter: \"active\", \"inactive\", \"expired\", \"all\". Default: \"active\"\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"name\": \"roomname\",\n"
            "    \"roomid\": \"hex\",\n"
            "    \"state\": \"active|inactive|expired\",\n"
            "    \"lastactivity\": timestamp,\n"
            "    \"messagecount\": n\n"
            "  }, ...\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("listchatrooms", "")
            + HelpExampleRpc("listchatrooms", "\"all\"")
        );
    }

    std::string filterState = "active";
    if (request.params.size() > 0) {
        filterState = request.params[0].get_str();
    }

    std::vector<std::string> allRooms = g_chatroom_db->ListAllRooms();
    UniValue result(UniValue::VARR);

    for (const auto& roomName : allRooms) {
        uint256 roomId;
        if (!g_chatroom_db->GetRoomByName(roomName, roomId)) {
            continue;
        }

        CChatRoom room;
        if (!g_chatroom_db->ReadRoom(roomId, room)) {
            continue;
        }

        ChatRoomState state = room.GetState();
        std::string stateStr;
        switch (state) {
            case STATE_ACTIVE: stateStr = "active"; break;
            case STATE_INACTIVE: stateStr = "inactive"; break;
            case STATE_EXPIRED: stateStr = "expired"; break;
        }

        if (filterState != "all" && filterState != stateStr) {
            continue;
        }

        UniValue entry(UniValue::VOBJ);
        entry.pushKV("name", roomName);
        entry.pushKV("roomid", roomId.GetHex());
        entry.pushKV("state", stateStr);
        entry.pushKV("lastactivity", room.GetLastActivity());

        auto messages = g_chatroom_db->GetRoomMessages(roomId, 0, 0, 10000);
        entry.pushKV("messagecount", (int)messages.size());

        result.push_back(entry);
    }

    return result;
}

UniValue joinchatroom(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            "joinchatroom \"roomname\"\n"
            "\nJoin a chatroom (store messages locally).\n"
            "\nArguments:\n"
            "1. roomname    (string, required) Name of the chatroom\n"
            "\nResult:\n"
            "\"success\"    (string) Confirmation message\n"
            "\nExamples:\n"
            + HelpExampleCli("joinchatroom", "\"MyRoom\"")
            + HelpExampleRpc("joinchatroom", "\"MyRoom\"")
        );
    }

    std::string roomName = request.params[0].get_str();

    uint256 roomId;
    if (!g_chatroom_db->GetRoomByName(roomName, roomId)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Room not found");
    }

    g_chatroom_manager->AddLocalRoom(roomId);

    return "Joined room successfully";
}

UniValue leavechatroom(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            "leavechatroom \"roomname\"\n"
            "\nLeave a chatroom (stop storing messages).\n"
            "\nArguments:\n"
            "1. roomname    (string, required) Name of the chatroom\n"
            "\nResult:\n"
            "\"success\"    (string) Confirmation message\n"
            "\nExamples:\n"
            + HelpExampleCli("leavechatroom", "\"MyRoom\"")
            + HelpExampleRpc("leavechatroom", "\"MyRoom\"")
        );
    }

    std::string roomName = request.params[0].get_str();

    uint256 roomId;
    if (!g_chatroom_db->GetRoomByName(roomName, roomId)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Room not found");
    }

    g_chatroom_manager->RemoveLocalRoom(roomId);

    return "Left room successfully";
}

UniValue getchatroominfo(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            "getchatroominfo \"roomname\"\n"
            "\nGet detailed information about a chatroom.\n"
            "\nArguments:\n"
            "1. roomname    (string, required) Name of the chatroom\n"
            "\nResult:\n"
            "{\n"
            "  \"roomid\": \"hex\",\n"
            "  \"name\": \"string\",\n"
            "  \"created\": timestamp,\n"
            "  \"lastactivity\": timestamp,\n"
            "  \"state\": \"active|inactive|expired\",\n"
            "  \"dayssinceactivity\": n,\n"
            "  \"messagecount\": n,\n"
            "  \"mutedusers\": n\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getchatroominfo", "\"MyRoom\"")
            + HelpExampleRpc("getchatroominfo", "\"MyRoom\"")
        );
    }

    std::string roomName = request.params[0].get_str();

    uint256 roomId;
    if (!g_chatroom_db->GetRoomByName(roomName, roomId)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Room not found");
    }

    CChatRoom room;
    if (!g_chatroom_db->ReadRoom(roomId, room)) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to read room");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("roomid", room.GetRoomId().GetHex());
    result.pushKV("name", room.GetRoomName());
    result.pushKV("created", room.GetTimeCreated());
    result.pushKV("lastactivity", room.GetLastActivity());

    ChatRoomState state = room.GetState();
    std::string stateStr;
    switch (state) {
        case STATE_ACTIVE: stateStr = "active"; break;
        case STATE_INACTIVE: stateStr = "inactive"; break;
        case STATE_EXPIRED: stateStr = "expired"; break;
    }
    result.pushKV("state", stateStr);

    int64_t daysSince = room.GetTimeSinceActivity() / 86400;
    result.pushKV("dayssinceactivity", daysSince);

    auto messages = g_chatroom_db->GetRoomMessages(roomId, 0, 0, 10000);
    result.pushKV("messagecount", (int)messages.size());
    result.pushKV("mutedusers", (int)room.GetMutedCount());

    return result;
}

UniValue sendchatmessage(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 2) {
        throw std::runtime_error(
            "sendchatmessage \"roomname\" \"message\"\n"
            "\nSend a message to a chatroom.\n"
            "\nArguments:\n"
            "1. roomname    (string, required) Name of the chatroom\n"
            "2. message     (string, required) Message text\n"
            "\nResult:\n"
            "\"success\"    (string) Confirmation message\n"
            "\nExamples:\n"
            + HelpExampleCli("sendchatmessage", "\"MyRoom\" \"Hello World\"")
            + HelpExampleRpc("sendchatmessage", "\"MyRoom\", \"Hello World\"")
        );
    }

    std::string roomName = request.params[0].get_str();
    std::string message = request.params[1].get_str();

    if (message.size() > MAX_MESSAGE_SIZE) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Message too large");
    }

    uint256 roomId;
    if (!g_chatroom_db->GetRoomByName(roomName, roomId)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Room not found");
    }

    CKey senderKey = GetWalletKey(request);

    if (!g_chatroom_manager->SendMessage(roomId, message, senderKey, *chat_connman)) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to send message");
    }

    return "Message sent successfully";
}

UniValue getchatmessages(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2) {
        throw std::runtime_error(
            "getchatmessages \"roomname\" ( count )\n"
            "\nGet recent messages from a chatroom.\n"
            "\nArguments:\n"
            "1. roomname    (string, required) Name of the chatroom\n"
            "2. count       (numeric, optional) Number of messages. Default: 50\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"timestamp\": n,\n"
            "    \"sender\": \"pubkey\",\n"
            "    \"message\": \"text\"\n"
            "  }, ...\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("getchatmessages", "\"MyRoom\" 100")
            + HelpExampleRpc("getchatmessages", "\"MyRoom\", 100")
        );
    }

    std::string roomName = request.params[0].get_str();
    size_t count = 50;
    if (request.params.size() > 1) {
        count = request.params[1].get_int();
    }

    uint256 roomId;
    if (!g_chatroom_db->GetRoomByName(roomName, roomId)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Room not found");
    }

    auto messages = g_chatroom_db->GetRoomMessages(roomId, 0, 0, count);

    UniValue result(UniValue::VARR);
    for (const auto& msg : messages) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("timestamp", msg.GetTimestamp());
        entry.pushKV("sender", HexStr(msg.GetSenderPubKey()));
        entry.pushKV("message", msg.GetPayloadString());
        result.push_back(entry);
    }

    return result;
}

UniValue mutechatuser(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 2) {
        throw std::runtime_error(
            "mutechatuser \"roomname\" \"pubkey\"\n"
            "\nMute a user in a chatroom (creator only).\n"
            "\nArguments:\n"
            "1. roomname    (string, required) Name of the chatroom\n"
            "2. pubkey      (string, required) User's public key\n"
            "\nResult:\n"
            "\"success\"    (string) Confirmation message\n"
            "\nExamples:\n"
            + HelpExampleCli("mutechatuser", "\"MyRoom\" \"03abc...\"")
            + HelpExampleRpc("mutechatuser", "\"MyRoom\", \"03abc...\"")
        );
    }

    std::string roomName = request.params[0].get_str();
    std::string pubkeyHex = request.params[1].get_str();

    uint256 roomId;
    if (!g_chatroom_db->GetRoomByName(roomName, roomId)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Room not found");
    }

    CPubKey targetPubKey(ParseHex(pubkeyHex));
    if (!targetPubKey.IsValid()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid public key");
    }

    CKey creatorKey = GetWalletKey(request);

    if (!g_chatroom_manager->MuteUser(roomId, targetPubKey, creatorKey)) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to mute user (not creator?)");
    }

    return "User muted successfully";
}

UniValue unmutechatuser(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 2) {
        throw std::runtime_error(
            "unmutechatuser \"roomname\" \"pubkey\"\n"
            "\nUnmute a user in a chatroom (creator only).\n"
            "\nArguments:\n"
            "1. roomname    (string, required) Name of the chatroom\n"
            "2. pubkey      (string, required) User's public key\n"
            "\nResult:\n"
            "\"success\"    (string) Confirmation message\n"
            "\nExamples:\n"
            + HelpExampleCli("unmutechatuser", "\"MyRoom\" \"03abc...\"")
            + HelpExampleRpc("unmutechatuser", "\"MyRoom\", \"03abc...\"")
        );
    }

    std::string roomName = request.params[0].get_str();
    std::string pubkeyHex = request.params[1].get_str();

    uint256 roomId;
    if (!g_chatroom_db->GetRoomByName(roomName, roomId)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Room not found");
    }

    CPubKey targetPubKey(ParseHex(pubkeyHex));
    if (!targetPubKey.IsValid()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid public key");
    }

    CKey creatorKey = GetWalletKey(request);  // <-- NAPRAWIONE: było bez `request`

    if (!g_chatroom_manager->UnmuteUser(roomId, targetPubKey, creatorKey)) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to unmute user");
    }

    return "User unmuted successfully";
}

UniValue listmutedusers(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            "listmutedusers \"roomname\"\n"
            "\nList all muted users in a chatroom.\n"
            "\nArguments:\n"
            "1. roomname    (string, required) Name of the chatroom\n"
            "\nResult:\n"
            "[\n"
            "  \"pubkey\",\n"
            "  ...\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("listmutedusers", "\"MyRoom\"")
            + HelpExampleRpc("listmutedusers", "\"MyRoom\"")
        );
    }

    std::string roomName = request.params[0].get_str();

    uint256 roomId;
    if (!g_chatroom_db->GetRoomByName(roomName, roomId)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Room not found");
    }

    CChatRoom room;
    if (!g_chatroom_db->ReadRoom(roomId, room)) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to read room");
    }

    UniValue result(UniValue::VARR);
    for (const auto& pubkey : room.GetMutedKeys()) {
        result.push_back(HexStr(pubkey));
    }

    return result;
}

UniValue getchatsyncstatus(const JSONRPCRequest& request)
{
    if (request.fHelp) {
        throw std::runtime_error(
            "getchatsyncstatus\n"
            "\nGet chatroom sync status.\n"
            "\nResult:\n"
            "{\n"
            "  \"roomcount\": n,\n"
            "  \"messagecount\": n,\n"
            "  \"cachesize\": n\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getchatsyncstatus", "")
            + HelpExampleRpc("getchatsyncstatus", "")
        );
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("roomcount", (int)g_chatroom_db->GetRoomCount());
    result.pushKV("messagecount", (int)g_chatroom_db->GetMessageCount());
    result.pushKV("cachesize", (int)g_chatroom_manager->GetCacheSize());

    return result;
}

UniValue cleanupexpiredrooms(const JSONRPCRequest& request)
{
    if (request.fHelp) {
        throw std::runtime_error(
            "cleanupexpiredrooms\n"
            "\nManually trigger cleanup of expired rooms.\n"
            "\nResult:\n"
            "{\n"
            "  \"removed\": n\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("cleanupexpiredrooms", "")
            + HelpExampleRpc("cleanupexpiredrooms", "")
        );
    }

    size_t removed = g_chatroom_db->CleanupExpiredRooms();

    UniValue result(UniValue::VOBJ);
    result.pushKV("removed", (int)removed);

    return result;
}

static const CRPCCommand commands[] = {
    {"chatroom", "createchatroom",       &createchatroom,       {"roomname"}},
    {"chatroom", "listchatrooms",        &listchatrooms,        {"state"}},
    {"chatroom", "joinchatroom",         &joinchatroom,         {"roomname"}},
    {"chatroom", "leavechatroom",        &leavechatroom,        {"roomname"}},
    {"chatroom", "getchatroominfo",      &getchatroominfo,      {"roomname"}},
    {"chatroom", "sendchatmessage",      &sendchatmessage,      {"roomname", "message"}},
    {"chatroom", "getchatmessages",      &getchatmessages,      {"roomname", "count"}},
    {"chatroom", "mutechatuser",         &mutechatuser,         {"roomname", "pubkey"}},
    {"chatroom", "unmutechatuser",       &unmutechatuser,       {"roomname", "pubkey"}},
    {"chatroom", "listmutedusers",       &listmutedusers,       {"roomname"}},
    {"chatroom", "getchatsyncstatus",    &getchatsyncstatus,    {}},
    {"chatroom", "cleanupexpiredrooms",  &cleanupexpiredrooms,  {}},
};

void RegisterChatRoomRPCCommands(CRPCTable& t)
{
    // Zastąpione ARRAYLEN
    for (size_t i = 0; i < sizeof(commands)/sizeof(commands[0]); ++i) {
        t.appendCommand(commands[i].name, &commands[i]);
    }
}
