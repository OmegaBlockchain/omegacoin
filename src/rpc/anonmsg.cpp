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
    LogPrintf("test 1 %s\n", params[0].get_str());
    UniValue result(UniValue::VOBJ);
    std::string strMsg = params[0].get_str();
    if (strMsg.empty()) {
        return result;
        LogPrintf("test 2 %s\n", params[0].get_str());
    }

    LogPrintf("test 3 %s\n", params[0].get_str());
    //! create and relay a message
    CAnonMsg testCase;
    testCase.setMessage(strMsg);

    LogPrintf("test 4 %s\n", params[0].get_str());

    //! relay message and store
    testCase.Relay();
    mapAnonMsgSeen.insert(std::make_pair(testCase.GetHash(),testCase));
    result.push_back("ok");

    LogPrintf("test 5 %s\n", params[0].get_str());

    return result;
}
