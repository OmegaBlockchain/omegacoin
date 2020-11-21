// Copyright (c) 2020 barrystyle
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <anonmsg.h>

#include <net_processing.h>
#include <net.h>
#include <rpc/client.h>
#include <rpc/server.h>
#include <rpc/protocol.h>
#include <utilstrencodings.h>

UniValue sendanonmsg(const UniValue& params, bool fHelp)
{
    UniValue result(UniValue::VOBJ);
    std::string strMsg = params[0].get_str();
    if (strMsg.empty()) {
        return result;
    }

    //! create and relay a message
    CAnonMsg testCase;
    testCase.setMessage(strMsg);

    //! relay message and store
    testCase.Relay();
    mapAnonMsgSeen.insert(std::make_pair(testCase.GetHash(),testCase));
    result.push_back("ok");

    return result;
}
