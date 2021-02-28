// Copyright (c) 2020 barrystyle
// Copyright (c) 2020 Kolby Moroz Liebl
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "anonmsg.h"
#include "masternode-sync.h"
#include <boost/exception/to_string.hpp>

class CAnonMsg;

CAnonMsg anonMsg;

std::map<uint256, CAnonMsg> mapAnonMsg;

void sortmap(std::map<int64_t, std::string>& M)
{
    std::multimap<std::string, int64_t> MM;

    for (auto& it : M) {
        MM.insert({ it.second, it.first });
    }
}

void CAnonMsg::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    if(fLiteMode) return; // disable all OmegaCoin specific functionality

    if (strCommand == NetMsgType::ANONMSG) {

        CAnonMsg incomingMsg;
        int64_t msgTime;
        std::string msgData;

        vRecv >> msgTime;

        if (!vRecv.empty()) {
            vRecv >> LIMITED_STRING(msgData, 140);
        }

        incomingMsg.setMessageAndTime(msgData, msgTime);

        pfrom->setAskFor.erase(incomingMsg.GetHash());

        if ((msgTime + 24*60*60) < GetAdjustedTime()) {
            return;
        }

        std::string msgpayload = incomingMsg.getMessage();
        if (msgpayload.size() > 141) return;

        mapAnonMsg.insert(std::make_pair(incomingMsg.GetHash(), incomingMsg));

        incomingMsg.Relay(connman);

    } else if (strCommand == NetMsgType::GETANONMSG) {

        std::map<uint256, CAnonMsg>::iterator it = mapAnonMsg.begin();

        while(it != mapAnonMsg.end()) {
            connman.PushMessage(pfrom, NetMsgType::ANONMSG, it->second.getTime(), it->second.getMessage());
            it++;
        }
    }

}


void CAnonMsg::Relay(CConnman& connman)
{
    CInv inv(MSG_ANONMSG, this->GetHash());
    connman.RelayInv(inv);
}

void CAnonMsg::CheckAndRemove()
{
    if(!masternodeSync.IsBlockchainSynced()) return;

    for (auto message=mapAnonMsg.begin(); message!=mapAnonMsg.end(); ++message) {
        int64_t msgtime = message->second.getTime();
        if ((msgtime + 24*60*60) < GetAdjustedTime()) {
            mapAnonMsg.erase(message);
        }
    }

}
