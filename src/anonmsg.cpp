// Copyright (c) 2020 barrystyle
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "anonmsg.h"

std::queue<std::string> anonMsgReceived;
std::map<uint256, CAnonMsg> mapAnonMsgSeen;

bool getNextAnonMsg(std::string& nextMsg)
{
    LogPrintf("%s called\n", __func__);
    if (anonMsgReceived.size() > 0) {
        nextMsg = anonMsgReceived.front();
        LogPrintf("DEBUG->tolivefeed %s\n", nextMsg);
        anonMsgReceived.pop();
        return true;
    }
    return false;
}

void CAnonMsg::Relay() const
{
    LogPrintf("%s called\n", __func__);
    CInv inv(MSG_ANONMSG, this->GetHash());
    g_connman->ForEachNode([&inv](CNode* pnode) {
        pnode->PushInventory(inv);
    });
}
