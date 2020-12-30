// Copyright (c) 2020 barrystyle
// Copyright (c) 2020 Kolby Moroz Liebl
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <anonmsg.h>

#include <net_processing.h>
#include <net.h>
#include <rpc/client.h>
#include <rpc/server.h>
#include <rpc/protocol.h>
#include <utilstrencodings.h>
#include <boost/exception/to_string.hpp>

#include <univalue.h>

UniValue sendanonmsg(const UniValue& params, bool fHelp)
{
    std::string strMsg = params[0].get_str();
    if (strMsg.empty()) {
        return "Failed: Message is empty";
    }

    //! create and relay a message
    CAnonMsg testCase;
    testCase.setMessage(strMsg);

    //! relay message and store
    testCase.Relay(*g_connman);
    mapAnonMsg.insert(std::make_pair(testCase.GetHash(),testCase));

    return "sent";
}

UniValue listanonmsg(const UniValue& params, bool fHelp)
{

    UniValue ret(UniValue::VARR);
    std::map<int64_t, std::string> messagesToSort;
    for (auto message=mapAnonMsg.begin(); message!=mapAnonMsg.end(); ++message) {
        messagesToSort.insert(std::make_pair(message->second.getTime(), message->second.getMessage()));
    }
    sortmap(messagesToSort);
    for (auto message=messagesToSort.begin(); message!=messagesToSort.end(); ++message) {
        UniValue obj(UniValue::VOBJ);
        obj.push_back(Pair("timestamp", message->first));
        obj.push_back(Pair("message", message->second));
        ret.push_back(obj);
    }

    return ret;
}