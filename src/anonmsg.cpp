// Copyright (c) 2020 barrystyle
// Copyright (c) 2020 Kolby Moroz Liebl
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "anonmsg.h"
#include <boost/exception/to_string.hpp>

std::map<uint256, CAnonMsg> mapAnonMsg;

void getAnonMessages(std::list<std::string>& listMsg)
{
    for (auto message=mapAnonMsg.begin(); message!=mapAnonMsg.end(); ++message) {
        std::string msgpayload = message->second.getMessage();
        int64_t msgtime = message->second.getTime();
        //if (msgpayload.size() > 256) return false;
        std::string messageStr = msgpayload +" "+"("+boost::to_string(msgtime)+")";
        listMsg.push_back(messageStr);
    }
    return;
}

void CAnonMsg::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    if(fLiteMode) return; // disable all OmegaCoin specific functionality

    if (strCommand == NetMsgType::ANONMSG) {

        CAnonMsg incomingMsg;
        vRecv >> incomingMsg;

        pfrom->setAskFor.erase(incomingMsg.GetHash());

        int64_t msgtime = incomingMsg.getTime();
        if ((msgtime + 24*60*60) < GetAdjustedTime()) {
            return;
        }

        mapAnonMsg.insert(incomingMsg);

        incomingMsg.Relay(connman);

    } else if (strCommand == NetMsgType::GETANONMSG) {

        std::map<uint256, CAnonMsg>::iterator it = mapAnonMsg.begin();

        while(it != mapAnonMsg.end()) {
            connman.PushMessage(pfrom, NetMsgType::ANONMSG, it->second);
            it++;
        }
    }

}


void CAnonMsg::Relay() const
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
