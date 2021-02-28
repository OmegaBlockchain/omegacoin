// Copyright (c) 2020 barrystyle
// Copyright (c) 2020 Kolby Moroz Liebl
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ANONMSG_H
#define ANONMSG_H

#include <hash.h>
#include <net.h>
#include <protocol.h>
#include <timedata.h>
#include <util.h>
#include <utilstrencodings.h>

#include <queue>

class CAnonMsg;

//
// CAnonMsg Class: contains serialization and validation methods
//

extern CAnonMsg anonMsg;

class CAnonMsg
{
private:
    int64_t msgTime;
    std::string msgData;

public:

    CAnonMsg()
    {
        msgTime = 0;
        msgData.clear();
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(msgTime);
        READWRITE(LIMITED_STRING(msgData, 140));
    }

    uint256 GetHash() const
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << msgTime;
        ss << msgData;
        return ss.GetHash();
    }

    bool setMessage(std::string& msgContent) {
        msgData = msgContent;
        msgTime = GetAdjustedTime();
        return true;
    }

    bool setMessageAndTime(std::string& msgContent, int64_t time) {
        msgData = msgContent;
        msgTime = time;
        return true;
    }

    std::string getMessage() const {
        return msgData;
    }

    int64_t getTime() const {
        return msgTime;
    }

    void Relay(CConnman& connman);

    void ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv, CConnman& connman);

    void CheckAndRemove();
};

extern std::map<uint256, CAnonMsg> mapAnonMsg;

void sortmap(std::map<int64_t, std::string>& M);

#endif // ANONMSG_H
