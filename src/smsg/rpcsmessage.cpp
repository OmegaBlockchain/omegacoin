// Copyright (c) 2017-2024 The Particl Core developers
// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/server.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <string>

#include <smsg/smessage.h>
#include <smsg/db.h>
#include <smsg/msganchor.h>
#include <util/system.h>
#include <util/strencodings.h>
#include <core_io.h>
#include <base58.h>
#include <rpc/blockchain.h>
#include <rpc/util.h>
#include <script/script.h>
#include <wallet/ismine.h>
#include <wallet/rpcwallet.h>

#include <chainparams.h>
#include <consensus/validation.h>
#include <evo/smsgroomtx.h>
#include <evo/specialtx.h>
#include <evo/specialtxman.h>
#include <index/smsgroomindex.h>
#include <util/translation.h>
#include <util/validation.h>
#include <key.h>
#include <key_io.h>
#include <validation.h>

#ifdef ENABLE_WALLET
#include <wallet/coincontrol.h>
#include <wallet/wallet.h>
extern UniValue sendrawtransaction(const JSONRPCRequest& request);
#endif

#include <univalue.h>

extern CConnman* g_connman;

static void EnsureSMSGIsEnabled()
{
    if (!smsg::fSecMsgEnabled)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Secure messaging is disabled.");
};

static UniValue smsgenable(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "smsgenable ( \"walletname\" )\n"
            "\nArguments:\n"
            "1. \"walletname\"      (string, optional, default=\"wallet.dat\") enable smsg on a specific wallet.\n"
            "Enable secure messaging.\n"
            "SMSG only be active on one wallet.\n");

    if (smsg::fSecMsgEnabled)
        throw JSONRPCError(RPC_MISC_ERROR, "Secure messaging is already enabled.");

    UniValue result(UniValue::VOBJ);

    std::shared_ptr<CWallet> pwallet;
    std::string walletName = "none";
#ifdef ENABLE_WALLET
    auto vpwallets = GetWallets();

    if (!request.params[0].isNull())
    {
        std::string sFindWallet = request.params[0].get_str();

        for (auto pw : vpwallets)
        {
            if (pw->GetName() != sFindWallet)
                continue;
            pwallet = pw;
            break;
        };
        if (!pwallet)
            throw JSONRPCError(RPC_MISC_ERROR, "Wallet not found: \"" + sFindWallet + "\"");
    } else
    {
        if (vpwallets.size() > 0)
            pwallet = vpwallets[0];
    };
    if (pwallet)
        walletName = pwallet->GetName();
#endif

    result.pushKV("result", (smsgModule.Enable(pwallet) ? "Enabled secure messaging." : "Failed to enable secure messaging."));
    result.pushKV("wallet", walletName);

    return result;
}

static UniValue smsgdisable(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "smsgdisable\n"
            "Disable secure messaging.");

    if (!smsg::fSecMsgEnabled)
        throw JSONRPCError(RPC_MISC_ERROR, "Secure messaging is already disabled.");

    UniValue result(UniValue::VOBJ);

    result.pushKV("result", (smsgModule.Disable() ? "Disabled secure messaging." : "Failed to disable secure messaging."));

    return result;
}

static UniValue smsgoptions(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 3)
        throw std::runtime_error(
            "smsgoptions ( list with_description|set \"optname\" \"value\" )\n"
            "List and manage options.\n"
            "\nExamples\n"
            "smsgoptions list 1\n"
            " list possible options with descriptions.\n"
            );

    std::string mode = "list";
    if (request.params.size() > 0)
        mode = request.params[0].get_str();

    UniValue result(UniValue::VOBJ);

    if (mode == "list")
    {
        UniValue options(UniValue::VARR);

        bool fDescriptions = false;
        if (!request.params[1].isNull())
            fDescriptions = GetBool(request.params[1]);

        UniValue option(UniValue::VOBJ);
        option.pushKV("name", "newAddressRecv");
        option.pushKV("value", smsgModule.options.fNewAddressRecv);
        if (fDescriptions)
            option.pushKV("description", "Enable receiving messages for newly created addresses.");
        options.push_back(option);

        option = UniValue(UniValue::VOBJ);
        option.pushKV("name", "newAddressAnon");
        option.pushKV("value", smsgModule.options.fNewAddressAnon);
        if (fDescriptions)
            option.pushKV("description", "Enable receiving anonymous messages for newly created addresses.");
        options.push_back(option);

        option = UniValue(UniValue::VOBJ);
        option.pushKV("name", "scanIncoming");
        option.pushKV("value", smsgModule.options.fScanIncoming);
        if (fDescriptions)
            option.pushKV("description", "Scan incoming blocks for public keys, -smsgscanincoming must also be set");
        options.push_back(option);

        result.pushKV("options", options);
        result.pushKV("result", "Success.");
    } else
    if (mode == "set")
    {
        if (request.params.size() < 3)
        {
            result.pushKV("result", "Too few parameters.");
            result.pushKV("expected", "set <optname> <value>");
            return result;
        };

        std::string optname = request.params[1].get_str();
        std::string value   = request.params[2].get_str();

        std::transform(optname.begin(), optname.end(), optname.begin(), ::tolower);

        bool fValue;
        if (optname == "newaddressrecv")
        {
            if (omega::GetStringBool(value, fValue))
            {
                smsgModule.options.fNewAddressRecv = fValue;
            } else
            {
                result.pushKV("result", "Unknown value.");
                return result;
            };
            result.pushKV("set option", std::string("newAddressRecv = ") + (smsgModule.options.fNewAddressRecv ? "true" : "false"));
        } else
        if (optname == "newaddressanon")
        {
            if (omega::GetStringBool(value, fValue))
            {
                smsgModule.options.fNewAddressAnon = fValue;
            } else
            {
                result.pushKV("result", "Unknown value.");
                return result;
            };
            result.pushKV("set option", std::string("newAddressAnon = ") + (smsgModule.options.fNewAddressAnon ? "true" : "false"));
        } else
        if (optname == "scanincoming")
        {
            if (omega::GetStringBool(value, fValue))
            {
                smsgModule.options.fScanIncoming = fValue;
            } else
            {
                result.pushKV("result", "Unknown value.");
                return result;
            };
            result.pushKV("set option", std::string("scanIncoming = ") + (smsgModule.options.fScanIncoming ? "true" : "false"));
        } else
        {
            result.pushKV("result", "Option not found.");
            return result;
        };
    } else
    {
        result.pushKV("result", "Unknown Mode.");
        result.pushKV("expected", "smsgoptions [list|set <optname> <value>]");
    };

    return result;
}

static UniValue smsglocalkeys(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 3)
        throw std::runtime_error(
            "smsglocalkeys ( whitelist|all|wallet|recv +/- \"address\"|anon +/- \"address\" )\n"
            "List and manage keys.");

    EnsureSMSGIsEnabled();

    UniValue result(UniValue::VOBJ);

    std::string mode = "whitelist";
    if (request.params.size() > 0)
    {
        mode = request.params[0].get_str();
    };

    if (mode == "whitelist"
        || mode == "all")
    {
        LOCK(smsgModule.cs_smsg);
        uint32_t nKeys = 0;
        int all = mode == "all" ? 1 : 0;

        UniValue keys(UniValue::VARR);
#ifdef ENABLE_WALLET
        if (smsgModule.pwallet)
        {
            for (auto it = smsgModule.addresses.begin(); it != smsgModule.addresses.end(); ++it)
            {
                if (!all
                    && !it->fReceiveEnabled)
                    continue;

                CKeyID &keyID = it->address;
                std::string sPublicKey;
                CPubKey pubKey;

                if (smsgModule.pwallet)
                {
                    LegacyScriptPubKeyMan& spk_man = EnsureLegacyScriptPubKeyMan(*smsgModule.pwallet);
                    if (!spk_man.GetPubKey(keyID, pubKey)) {
                        LogPrintf("%s: GetPubKey failed for %s — key in addresses but not in wallet\n",
                            __func__, EncodeDestination(PKHash(keyID)));
                        continue;
                    }
                    if (!pubKey.IsValid()
                        || !pubKey.IsCompressed())
                    {
                        continue;
                    };
                    sPublicKey = EncodeBase58(pubKey.begin(), pubKey.end());
                };

                UniValue objM(UniValue::VOBJ);
                std::string sInfo, sLabel;
                PKHash pkh = PKHash(keyID);
                sLabel = smsgModule.LookupLabel(pkh);
                if (all) {
                    sInfo = std::string("Receive ") + (it->fReceiveEnabled ? "on,  " : "off, ");
                }
                sInfo += std::string("Anon ") + (it->fReceiveAnon ? "on" : "off");
                //result.pushKV("key", it->sAddress + " - " + sPublicKey + " " + sInfo + " - " + sLabel);
                objM.pushKV("address", EncodeDestination(PKHash(keyID)));
                objM.pushKV("public_key", sPublicKey);
                objM.pushKV("receive", (it->fReceiveEnabled ? "1" : "0"));
                objM.pushKV("anon", (it->fReceiveAnon ? "1" : "0"));
                objM.pushKV("label", sLabel);
                keys.push_back(objM);

                nKeys++;
            };
            result.pushKV("wallet_keys", keys);
        };
#endif

        keys = UniValue(UniValue::VARR);
        for (auto &p : smsgModule.keyStore.mapKeys)
        {
            auto &key = p.second;
            UniValue objM(UniValue::VOBJ);
            CPubKey pk = key.key.GetPubKey();
            objM.pushKV("address", EncodeDestination(PKHash(p.first)));
            objM.pushKV("public_key", EncodeBase58(pk.begin(), pk.end()));
            objM.pushKV("receive", (key.nFlags & smsg::SMK_RECEIVE_ON ? "1" : "0"));
            objM.pushKV("anon", (key.nFlags & smsg::SMK_RECEIVE_ANON ? "1" : "0"));
            objM.pushKV("label", key.sLabel);
            keys.push_back(objM);

            nKeys++;
        };
        result.pushKV("smsg_keys", keys);

        result.pushKV("result", strprintf("%u", nKeys));
    } else
    if (mode == "recv")
    {
        if (request.params.size() < 3)
        {
            result.pushKV("result", "Too few parameters.");
            result.pushKV("expected", "recv <+/-> <address>");
            return result;
        };

        std::string op      = request.params[1].get_str();
        std::string addr    = request.params[2].get_str();

        bool fValue;
        if (!omega::GetStringBool(op, fValue))
        {
            result.pushKV("result", "Unknown value.");
            return result;
        };

        CTxDestination coinAddress = DecodeDestination(addr);
        if (!IsValidDestinationString(addr))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address.");
        CKeyID keyID = ToKeyID(std::get<PKHash>(coinAddress));

        if (!smsgModule.SetWalletAddressOption(keyID, "receive", fValue)
            && !smsgModule.SetSmsgAddressOption(keyID, "receive", fValue))
        {
            result.pushKV("result", "Address not found.");
            return result;
        };

        std::string sInfo;
        sInfo = std::string("Receive ") + (fValue ? "on" : "off");
        result.pushKV("result", "Success.");
        result.pushKV("key", EncodeDestination(coinAddress) + " " + sInfo);
        return result;
    } else
    if (mode == "anon")
    {
        if (request.params.size() < 3)
        {
            result.pushKV("result", "Too few parameters.");
            result.pushKV("expected", "anon <+/-> <address>");
            return result;
        };

        std::string op      = request.params[1].get_str();
        std::string addr    = request.params[2].get_str();

        bool fValue;
        if (!omega::GetStringBool(op, fValue))
        {
            result.pushKV("result", "Unknown value.");
            return result;
        };

        CTxDestination coinAddress = DecodeDestination(addr);
        if (!IsValidDestinationString(addr))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address.");
        CKeyID keyID = ToKeyID(std::get<PKHash>(coinAddress));

        if (!smsgModule.SetWalletAddressOption(keyID, "anon", fValue)
            && !smsgModule.SetSmsgAddressOption(keyID, "anon", fValue))
        {
            result.pushKV("result", "Address not found.");
            return result;
        };

        std::string sInfo;
        sInfo += std::string("Anon ") + (fValue ? "on" : "off");
        result.pushKV("result", "Success.");
        result.pushKV("key", EncodeDestination(coinAddress) + " " + sInfo);
        return result;

    } else
    if (mode == "wallet")
    {
#ifdef ENABLE_WALLET
        if (!smsgModule.pwallet)
            throw JSONRPCError(RPC_MISC_ERROR, "No wallet.");
        uint32_t nKeys = 0;
        UniValue keys(UniValue::VOBJ);

        for (const auto &entry : smsgModule.pwallet->mapAddressBook)
        {
            if (!smsgModule.pwallet->IsMine(entry.first))
                continue;

            CTxDestination coinAddress(entry.first);
            if (!IsValidDestination(entry.first))
                continue;

            std::string address = EncodeDestination(coinAddress);
            std::string sPublicKey;

            CKeyID keyID = ToKeyID(std::get<PKHash>(coinAddress));

            LegacyScriptPubKeyMan& spk_man = EnsureLegacyScriptPubKeyMan(*smsgModule.pwallet);
            CPubKey pubKey;
            if (!spk_man.GetPubKey(keyID, pubKey))
                continue;
            if (!pubKey.IsValid()
                || !pubKey.IsCompressed())
            {
                continue;
            };

            sPublicKey = EncodeBase58(pubKey.begin(), pubKey.end());
            UniValue objM(UniValue::VOBJ);

            objM.pushKV("key", address);
            objM.pushKV("publickey", sPublicKey);
            objM.pushKV("label", entry.second.name);

            keys.push_back(objM);
            nKeys++;
        };
        result.pushKV("keys", keys);
        result.pushKV("result", strprintf("%u", nKeys));
#else
        throw JSONRPCError(RPC_MISC_ERROR, "No wallet.");
#endif
    } else
    {
        result.pushKV("result", "Unknown Mode.");
        result.pushKV("expected", "smsglocalkeys [whitelist|all|wallet|recv <+/-> <address>|anon <+/-> <address>]");
    };

    return result;
};

static UniValue smsgscanchain(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "smsgscanchain\n"
            "Look for public keys in the block chain.");

    EnsureSMSGIsEnabled();

    UniValue result(UniValue::VOBJ);
    if (!smsgModule.ScanBlockChain())
    {
        result.pushKV("result", "Scan Chain Failed.");
    } else
    {
        result.pushKV("result", "Scan Chain Completed.");
    };
    return result;
}

static UniValue smsgscanbuckets(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "smsgscanbuckets\n"
            "Force rescan of all messages in the bucket store.\n"
            "Wallet must be unlocked if any receiving keys are stored in the wallet.\n");

    EnsureSMSGIsEnabled();

#ifdef ENABLE_WALLET
    if (smsgModule.pwallet && smsgModule.pwallet->IsLocked()
        && smsgModule.addresses.size() > 0)
        throw JSONRPCError(RPC_MISC_ERROR, "Wallet is locked.");
#endif

    UniValue result(UniValue::VOBJ);
    if (!smsgModule.ScanBuckets())
    {
        result.pushKV("result", "Scan Buckets Failed.");
    } else
    {
        result.pushKV("result", "Scan Buckets Completed.");
    };

    return result;
}

static UniValue smsgaddaddress(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
            "smsgaddaddress \"address\" \"pubkey\"\n"
            "Add address and matching public key to database.");

    EnsureSMSGIsEnabled();

    std::string addr = request.params[0].get_str();
    std::string pubk = request.params[1].get_str();

    UniValue result(UniValue::VOBJ);
    int rv = smsgModule.AddAddress(addr, pubk);
    if (rv != 0)
    {
        result.pushKV("result", "Public key not added to db.");
        result.pushKV("reason", smsg::GetString(rv));
    } else
    {
        result.pushKV("result", "Public key added to db.");
    };

    return result;
}

static UniValue smsgaddlocaladdress(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "smsgaddlocaladdress \"address\"\n"
            "Enable receiving messages on <address>.\n"
            "Key for \"address\" must exist in the wallet.");

    EnsureSMSGIsEnabled();

    std::string addr = request.params[0].get_str();

    UniValue result(UniValue::VOBJ);
    int rv = smsgModule.AddLocalAddress(addr);
    if (rv != 0)
    {
        result.pushKV("result", "Address not added.");
        result.pushKV("reason", smsg::GetString(rv));
    } else
    {
        result.pushKV("result", "Receiving messages enabled for address.");
    };

    return result;
}

static UniValue smsgimportprivkey(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "smsgimportprivkey \"privkey\" ( \"label\" )\n"
            "\nAdds a private key (as returned by dumpprivkey) to the smsg database.\n"
            "\nThe imported key can receive messages even if the wallet is locked.\n"
            "\nArguments:\n"
            "1. \"privkey\"          (string, required) The private key (see dumpprivkey)\n"
            "2. \"label\"            (string, optional, default=\"\") An optional label\n"
            "\nExamples:\n"
            "\nDump a private key\n"
            + HelpExampleCli("dumpprivkey", "\"myaddress\"") +
            "\nImport the private key\n"
            + HelpExampleCli("smsgimportprivkey", "\"mykey\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("smsgimportprivkey", "\"mykey\", \"testing\"")
        );

    EnsureSMSGIsEnabled();

    CKey key;
    if (!request.params[0].isStr())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key encoding");

    key = DecodeSecret(request.params[0].get_str());
    if (!key.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key encoding");

    std::string strLabel = "";
    if (!request.params[1].isNull())
        strLabel = request.params[1].get_str();

    int rv = smsgModule.ImportPrivkey(key, strLabel);
    if (0 != rv)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Import failed.");

    return NullUniValue;
}

static UniValue smsggetpubkey(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "smsggetpubkey \"address\"\n"
            "Return the base58 encoded compressed public key for an address.\n"
            "\nArguments:\n"
            "1. \"address\"          (string, required) The address to find the pubkey for.\n"
            "\nResult:\n"
            "{\n"
            "  \"address\": \"...\"             (string) address of public key\n"
            "  \"publickey\": \"...\"           (string) public key of address\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("smsggetpubkey", "\"myaddress\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("smsggetpubkey", "\"myaddress\""));

    EnsureSMSGIsEnabled();

    std::string address = request.params[0].get_str();
    std::string publicKey;

    UniValue result(UniValue::VOBJ);
    int rv = smsgModule.GetLocalPublicKey(address, publicKey);
    switch (rv)
    {
        case smsg::SMSG_NO_ERROR:
            result.pushKV("address", address);
            result.pushKV("publickey", publicKey);
            return result; // success, don't check db
        case smsg::SMSG_WALLET_NO_PUBKEY:
            break; // check db
        //case 1:
        default:
            throw JSONRPCError(RPC_INTERNAL_ERROR, smsg::GetString(rv));
    };

    CTxDestination coinAddress = DecodeDestination(address);
    if (!IsValidDestinationString(address))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address.");
    CKeyID keyID = ToKeyID(std::get<PKHash>(coinAddress));

    CPubKey cpkFromDB;
    rv = smsgModule.GetStoredKey(keyID, cpkFromDB);

    switch (rv)
    {
        case smsg::SMSG_NO_ERROR:
            if (!cpkFromDB.IsValid()
                || !cpkFromDB.IsCompressed())
            {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Invalid public key.");
            } else
            {
                publicKey = EncodeBase58(cpkFromDB.begin(), cpkFromDB.end());

                result.pushKV("address", address);
                result.pushKV("publickey", publicKey);
            };
            break;
        case smsg::SMSG_PUBKEY_NOT_EXISTS:
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Address not found in wallet or db.");
        default:
            throw JSONRPCError(RPC_INTERNAL_ERROR, smsg::GetString(rv));
    };

    return result;
}

static UniValue smsgsend(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 3 || request.params.size() > 11)
        throw std::runtime_error(
            "smsgsend \"address_from\" \"address_to\" \"message\" ( paid_msg days_retention testfee fromfile decodehex topic parent_msgid retention_days )\n"
            "Send an encrypted message from \"address_from\" to \"address_to\".\n"
            "For broadcast topic messages, set \"address_to\" to \"\" and specify a topic.\n"
            "The message is encrypted to the topic's shared key so all subscribers can read it.\n"
            "\nArguments:\n"
            "1. \"address_from\"       (string, required) The address of the sender.\n"
            "2. \"address_to\"         (string, required) Recipient address, or \"\" for broadcast topic messages.\n"
            "3. \"message\"            (string, required) The message.\n"
            "4. paid_msg             (bool, optional, default=false) Send as paid message.\n"
            "5. days_retention       (int, optional, default=1) Days paid message will be retained by network.\n"
            "6. testfee              (bool, optional, default=false) Don't send the message, only estimate the fee.\n"
            "7. fromfile             (bool, optional, default=false) Send file as message, path in \"message\".\n"
            "8. decodehex            (bool, optional, default=false) Decode \"message\" from hex before sending.\n"
            "9. \"topic\"              (string, optional) Topic channel, e.g. \"omega.listings.uk\".\n"
            "10. \"parent_msgid\"      (string, optional) Hex msgid of prior message (listing update/reply).\n"
            "11. retention_days      (int, optional, default=0) Suggested local retention for subscribers (days).\n"
            "\nResult:\n"
            "{\n"
            "  \"result\": \"Sent\"/\"Not Sent\"       (string)\n"
            "  \"msgid\": \"...\"                    (string) message identifier\n"
            "  \"topic\": \"...\"                    (string) topic channel, if specified\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("smsgsend", "\"myaddress\" \"\" \"listing payload\" false 0 false false false \"omega.listings.uk\" \"\" 30") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("smsgsend", "\"myaddress\", \"\", \"message\", false, 0, false, false, false, \"omega.listings\""));

    EnsureSMSGIsEnabled();

    RPCTypeCheck(request.params,
        {UniValue::VSTR, UniValue::VSTR, UniValue::VSTR,
         UniValue::VBOOL, UniValue::VNUM, UniValue::VBOOL}, true);

    std::string addrFrom  = request.params[0].get_str();
    std::string addrTo    = request.params[1].get_str();
    std::string msg       = request.params[2].get_str();

    if (msg.empty())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Message must not be empty.");
    if (msg.find('\0') != std::string::npos)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Message must not contain null bytes.");

    auto parseBool = [](const UniValue &v, bool def) -> bool {
        if (v.isNull()) return def;
        if (v.isBool()) return v.get_bool();
        const std::string &s = v.get_str();
        if (s == "true" || s == "1") return true;
        if (s == "false" || s == "0") return false;
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Expected bool (true/false)");
    };
    bool fPaid      = parseBool(request.params[3], false);
    int nRetention  = request.params[4].isNull() ? 1 : request.params[4].get_int();
    if (nRetention < 1 || nRetention > 31)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "days_retention must be 1-31.");
    bool fTestFee   = parseBool(request.params[5], false);
    bool fFromFile  = parseBool(request.params[6], false);
    bool fDecodeHex = parseBool(request.params[7], false);
    std::string topic = request.params.size() > 8 && !request.params[8].isNull()
                        ? request.params[8].get_str() : "";

    // Parent message ID for update/reply referencing
    uint160 parentMsgId;
    if (request.params.size() > 9 && !request.params[9].isNull()) {
        std::string sParent = request.params[9].get_str();
        if (!sParent.empty()) {
            if (!IsHex(sParent) || sParent.size() != 40)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "parent_msgid must be 40 hex characters.");
            std::vector<uint8_t> vParent = ParseHex(sParent);
            memcpy(parentMsgId.begin(), vParent.data(), 20);
        }
    }

    // Suggested local retention for topic subscribers
    uint16_t nTopicRetentionDays = 0;
    if (request.params.size() > 10 && !request.params[10].isNull()) {
        int val = request.params[10].get_int();
        if (val < 0 || val > 365)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "retention_days must be 0-365.");
        nTopicRetentionDays = (uint16_t)val;
    }

    if (!topic.empty() && !smsg::IsValidTopic(topic))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid topic string.");

    if (fFromFile && fDecodeHex)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Can't use decodehex with fromfile.");

    if (fDecodeHex)
    {
        if (!IsHex(msg))
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Expect hex encoded message with decodehex.");
        std::vector<uint8_t> vData = ParseHex(msg);
        msg = std::string(vData.begin(), vData.end());
    };

    CAmount nFee;
    CKeyID kiFrom, kiTo;
    CTxDestination coinAddress = DecodeDestination(addrFrom);
    if (!IsValidDestination(coinAddress))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid from address.");
    kiFrom = ToKeyID(std::get<PKHash>(coinAddress));

    // Broadcast mode: empty addressTo with a topic — recipient is derived from topic shared key
    if (addrTo.empty()) {
        if (topic.empty())
            throw JSONRPCError(RPC_INVALID_PARAMETER, "address_to is required unless a topic is specified for broadcast.");
        // kiTo stays null — Send() will derive it from the topic
    } else {
        coinAddress = DecodeDestination(addrTo);
        if (!IsValidDestination(coinAddress))
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid to address.");
        kiTo = ToKeyID(std::get<PKHash>(coinAddress));
    }

    UniValue result(UniValue::VOBJ);
    std::string sError;
    smsg::SecureMessage smsgOut;
    if (smsgModule.Send(kiFrom, kiTo, msg, smsgOut, sError, fPaid, nRetention, fTestFee, &nFee, fFromFile, topic, parentMsgId, nTopicRetentionDays) != 0)
    {
        result.pushKV("result", "Send failed.");
        result.pushKV("error", sError);
    } else
    {
        result.pushKV("result", fTestFee ? "Not Sent." : "Sent.");

        if (!fTestFee)
            result.pushKV("msgid", HexStr(smsgModule.GetMsgID(smsgOut)));

        if (fPaid)
        {
            if (!fTestFee)
            {
                uint256 txid;
                smsgOut.GetFundingTxid(txid);
                result.pushKV("txid", txid.ToString());
            };
            result.pushKV("fee", ValueFromAmount(nFee));
        }
        if (!topic.empty())
            result.pushKV("topic", topic);
    };

    return result;
}

static UniValue smsgsendanon(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
            "smsgsendanon \"address_to\" \"message\"\n"
            "DEPRECATED. Send an anonymous encrypted message to addrTo.");

    EnsureSMSGIsEnabled();

    std::string addrTo    = request.params[0].get_str();
    std::string msg       = request.params[1].get_str();

    CKeyID kiTo, kiFrom;
    CTxDestination coinAddress = DecodeDestination(addrTo);
    if (!IsValidDestination(coinAddress))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid to address.");
    kiTo = ToKeyID(std::get<PKHash>(coinAddress));

    UniValue result(UniValue::VOBJ);
    std::string sError;
    smsg::SecureMessage smsgOut;
    if (smsgModule.Send(kiFrom, kiTo, msg, smsgOut, sError) != 0)
    {
        result.pushKV("result", "Send failed.");
        result.pushKV("error", sError);
    } else
    {
        result.pushKV("msgid", HexStr(smsgModule.GetMsgID(smsgOut)));
        result.pushKV("result", "Sent.");
    };

    return result;
}

static UniValue smsginbox(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            "smsginbox ( \"mode\" \"filter\" )\n"
            "Decrypt and display received messages.\n"
            "Warning: clear will delete all messages.\n"
            "\nArguments:\n"
            "1. \"mode\"    (string, optional, default=\"unread\") \"all|unread|clear\" List all messages, unread messages or clear all messages.\n"
            "2. \"filter\"  (string, optional) Filter messages when in list mode. Applied to from, to and text fields.\n"
            "\nResult:\n"
            "{\n"
            "  \"msgid\": \"str\"                    (string) The message identifier\n"
            "  \"version\": \"str\"                  (string) The message version\n"
            "  \"received\": \"time\"                (string) Time the message was received\n"
            "  \"sent\": \"time\"                    (string) Time the message was sent\n"
            "  \"daysretention\": int              (int) Number of days message will stay in the network for\n"
            "  \"from\": \"str\"                     (string) Address the message was sent from\n"
            "  \"to\": \"str\"                       (string) Address the message was sent to\n"
            "  \"text\": \"str\"                     (string) Message text\n"
            "}\n");

    EnsureSMSGIsEnabled();

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VSTR}, true);

    std::string mode = request.params[0].isStr() ? request.params[0].get_str() : "unread";
    std::string filter = request.params[1].isStr() ? request.params[1].get_str() : "";

    UniValue result(UniValue::VOBJ);

    {
        LOCK(smsg::cs_smsgDB);

        smsg::SecMsgDB dbInbox;
        if (!dbInbox.Open("cr+"))
            throw std::runtime_error("Could not open DB.");

        uint32_t nMessages = 0;
        std::string sPrefix("im");
        uint8_t chKey[30];

        if (mode == "clear")
        {
            dbInbox.TxnBegin();

            leveldb::Iterator *it = dbInbox.pdb->NewIterator(leveldb::ReadOptions());
            while (dbInbox.NextSmesgKey(it, sPrefix, chKey))
            {
                dbInbox.EraseSmesg(chKey);
                nMessages++;
            };
            delete it;
            dbInbox.TxnCommit();

            result.pushKV("result", strprintf("Deleted %u messages.", nMessages));
        } else
        if (mode == "all"
            || mode == "unread")
        {
            int fCheckReadStatus = mode == "unread" ? 1 : 0;

            smsg::SecMsgStored smsgStored;
            smsg::MessageData msg;

            dbInbox.TxnBegin();

            leveldb::Iterator *it = dbInbox.pdb->NewIterator(leveldb::ReadOptions());
            UniValue messageList(UniValue::VARR);

            while (dbInbox.NextSmesg(it, sPrefix, chKey, smsgStored))
            {
                if (fCheckReadStatus
                    && !(smsgStored.status & SMSG_MASK_UNREAD))
                    continue;
                if (smsgStored.vchMessage.size() < smsg::SMSG_HDR_LEN)
                    continue;
                uint8_t *pHeader = &smsgStored.vchMessage[0];
                const smsg::SecureMessage *psmsg = (smsg::SecureMessage*) pHeader;

                UniValue objM(UniValue::VOBJ);
                objM.pushKV("msgid", HexStr(Span<uint8_t>(&chKey[2], &chKey[2] + 28))); // timestamp+hash
                objM.pushKV("version", strprintf("%02x%02x", psmsg->version[0], psmsg->version[1]));

                uint32_t nPayload = smsgStored.vchMessage.size() - smsg::SMSG_HDR_LEN;
                int rv = smsgModule.Decrypt(false, smsgStored.addrTo, pHeader, pHeader + smsg::SMSG_HDR_LEN, nPayload, msg);
                if (rv == 0) {
                    std::string sAddrTo = EncodeDestination(PKHash(smsgStored.addrTo));
                    std::string sText = std::string((char*)msg.vchMessage.data());
                    if (filter.size() > 0
                        && !(omega::stringsMatchI(msg.sFromAddress, filter, 3) ||
                            omega::stringsMatchI(sAddrTo, filter, 3) ||
                            omega::stringsMatchI(sText, filter, 3)))
                        continue;

                    PushTime(objM, "received", smsgStored.timeReceived);
                    PushTime(objM, "sent", msg.timestamp);
                    objM.pushKV("paid", UniValue(psmsg->IsPaidVersion()));

                    uint32_t nDaysRetention = psmsg->IsPaidVersion() ? psmsg->nonce[0] : 2;
                    int64_t ttl = smsg::SMSGGetSecondsInDay() * nDaysRetention;
                    objM.pushKV("daysretention", (int)nDaysRetention);
                    PushTime(objM, "expiration", psmsg->timestamp + ttl);

                    uint32_t nPayload = smsgStored.vchMessage.size() - smsg::SMSG_HDR_LEN;
                    objM.pushKV("payloadsize", (int)nPayload);

                    objM.pushKV("from", msg.sFromAddress);
                    objM.pushKV("to", sAddrTo);
                    objM.pushKV("text", sText);
                } else
                {
                    if (filter.size() > 0)
                        continue;

                    objM.pushKV("status", "Decrypt failed");
                    objM.pushKV("error", smsg::GetString(rv));
                };

                messageList.push_back(objM);

                // only set 'read' status if the message decrypted successfully
                if (fCheckReadStatus && rv == 0)
                {
                    smsgStored.status &= ~SMSG_MASK_UNREAD;
                    dbInbox.WriteSmesg(chKey, smsgStored);
                };
                nMessages++;
            };
            delete it;
            dbInbox.TxnCommit();

            result.pushKV("messages", messageList);
            result.pushKV("result", strprintf("%u", nMessages));
        } else
        {
            result.pushKV("result", "Unknown Mode.");
            result.pushKV("expected", "all|unread|clear.");
        };
    } // cs_smsgDB

    return result;
};

static UniValue smsgoutbox(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            "smsgoutbox ( \"mode\" \"filter\" )\n"
            "Decrypt and display all sent messages.\n"
            "Warning: \"mode\"=\"clear\" will delete all sent messages.\n"
            "\nArguments:\n"
            "1. \"mode\"    (string, optional, default=\"all\") \"all|clear\" List or clear messages.\n"
            "2. \"filter\"  (string, optional) Filter messages when in list mode. Applied to from, to and text fields.\n"
            "\nResult:\n"
            "{\n"
            "  \"msgid\": \"str\"                    (string) The message identifier\n"
            "  \"version\": \"str\"                  (string) The message version\n"
            "  \"sent\": \"time\"                    (string) Time the message was sent\n"
            "  \"from\": \"str\"                     (string) Address the message was sent from\n"
            "  \"to\": \"str\"                       (string) Address the message was sent to\n"
            "  \"text\": \"str\"                     (string) Message text\n"
            "}\n");

    EnsureSMSGIsEnabled();

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VSTR}, true);

    std::string mode = request.params[0].isStr() ? request.params[0].get_str() : "all";
    std::string filter = request.params[1].isStr() ? request.params[1].get_str() : "";


    UniValue result(UniValue::VOBJ);

    std::string sPrefix("sm");
    uint8_t chKey[30];
    memset(&chKey[0], 0, sizeof(chKey));

    {
        LOCK(smsg::cs_smsgDB);

        smsg::SecMsgDB dbOutbox;
        if (!dbOutbox.Open("cr+"))
            throw std::runtime_error("Could not open DB.");

        uint32_t nMessages = 0;

        if (mode == "clear")
        {
            dbOutbox.TxnBegin();

            leveldb::Iterator *it = dbOutbox.pdb->NewIterator(leveldb::ReadOptions());
            while (dbOutbox.NextSmesgKey(it, sPrefix, chKey))
            {
                dbOutbox.EraseSmesg(chKey);
                nMessages++;
            };
            delete it;
            dbOutbox.TxnCommit();

            result.pushKV("result", strprintf("Deleted %u messages.", nMessages));
        } else
        if (mode == "all")
        {
            smsg::SecMsgStored smsgStored;
            smsg::MessageData msg;
            leveldb::Iterator *it = dbOutbox.pdb->NewIterator(leveldb::ReadOptions());

            UniValue messageList(UniValue::VARR);

            while (dbOutbox.NextSmesg(it, sPrefix, chKey, smsgStored))
            {
                if (smsgStored.vchMessage.size() < smsg::SMSG_HDR_LEN)
                    continue;
                uint8_t *pHeader = &smsgStored.vchMessage[0];
                const smsg::SecureMessage *psmsg = (smsg::SecureMessage*) pHeader;

                UniValue objM(UniValue::VOBJ);
                objM.pushKV("msgid", HexStr(Span<uint8_t>(&chKey[2], &chKey[2] + 28))); // timestamp+hash
                objM.pushKV("version", strprintf("%02x%02x", psmsg->version[0], psmsg->version[1]));

                int rv = smsgModule.DecryptOutboxStored(smsgStored, msg);
                if (rv == 0) {
                    std::string sAddrTo = EncodeDestination(PKHash(smsgStored.addrTo));
                    std::string sText = std::string((char*)msg.vchMessage.data());
                    if (filter.size() > 0
                        && !(omega::stringsMatchI(msg.sFromAddress, filter, 3) ||
                            omega::stringsMatchI(sAddrTo, filter, 3) ||
                            omega::stringsMatchI(sText, filter, 3)))
                        continue;

                    PushTime(objM, "sent", msg.timestamp);
                    bool fIsPaid = smsg::OutboxHdrIsPaid(*psmsg);
                    objM.pushKV("paid", UniValue(fIsPaid));

                    uint32_t nDaysRetention = fIsPaid ? psmsg->nonce[0] : 2;
                    int64_t ttl = smsg::SMSGGetSecondsInDay() * nDaysRetention;
                    objM.pushKV("daysretention", (int)nDaysRetention);
                    PushTime(objM, "expiration", psmsg->timestamp + ttl);

                    uint32_t nPayload = smsgStored.vchMessage.size() - smsg::SMSG_HDR_LEN;
                    objM.pushKV("payloadsize", (int)nPayload);

                    objM.pushKV("from", msg.sFromAddress);
                    objM.pushKV("to", sAddrTo);
                    objM.pushKV("text", sText);
                } else
                {
                    if (filter.size() > 0)
                        continue;

                    objM.pushKV("status", "Decrypt failed");
                    objM.pushKV("error", smsg::GetString(rv));
                };
                messageList.push_back(objM);
                nMessages++;
            };
            delete it;

            result.pushKV("messages" ,messageList);
            result.pushKV("result", strprintf("%u", nMessages));
        } else
        {
            result.pushKV("result", "Unknown Mode.");
            result.pushKV("expected", "all|clear.");
        };
    }

    return result;
};


static UniValue smsgbuckets(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "smsgbuckets ( stats|dump )\n"
            "Display bucket statistics, or write them to smsgstore/buckets_dump.json.\n"
            "\nArguments:\n"
            "1. mode      (string, optional, default=\"stats\") \"stats\" to display, \"dump\" to write to file.\n"
            "\nTo clear all bucket data use smsgclearbuckets.\n");

    EnsureSMSGIsEnabled();

    std::string mode = "stats";
    if (request.params.size() > 0)
    {
        mode = request.params[0].get_str();
    };

    UniValue result(UniValue::VOBJ);
    UniValue arrBuckets(UniValue::VARR);

    char cbuf[256];
    if (mode == "stats")
    {
        uint32_t nBuckets = 0;
        uint32_t nMessages = 0;
        uint64_t nBytes = 0;
        {
            LOCK(smsgModule.cs_smsg);
            std::map<int64_t, smsg::SecMsgBucket>::iterator it;
            it = smsgModule.buckets.begin();

            for (it = smsgModule.buckets.begin(); it != smsgModule.buckets.end(); ++it)
            {
                std::set<smsg::SecMsgToken> &tokenSet = it->second.setTokens;

                std::string sBucket = std::to_string(it->first);
                std::string sFile = sBucket + "_01.dat";
                std::string sHash = std::to_string(it->second.hash);

                size_t nActiveMessages = it->second.CountActive();

                nBuckets++;
                nMessages += nActiveMessages;

                UniValue objM(UniValue::VOBJ);
                objM.pushKV("bucket", sBucket);
                PushTime(objM, "time", it->first);
                objM.pushKV("no. messages", strprintf("%u", tokenSet.size()));
                objM.pushKV("active messages", strprintf("%u", nActiveMessages));
                objM.pushKV("hash", sHash);
                objM.pushKV("last changed", GetTimeString(it->second.timeChanged, cbuf, sizeof(cbuf)));

                fs::path fullPath = GetDataDir() / "smsgstore" / sFile;
                if (!fs::exists(fullPath))
                {
                    // If there is a file for an empty bucket something is wrong.
                    if (tokenSet.size() == 0)
                        objM.pushKV("file size", "Empty bucket.");
                    else
                        objM.pushKV("file size, error", "File not found.");
                } else
                {
                    try {
                        uint64_t nFBytes = 0;
                        nFBytes = fs::file_size(fullPath);
                        nBytes += nFBytes;
                        objM.pushKV("file size", omega::BytesReadable(nFBytes));
                    } catch (const fs::filesystem_error& ex)
                    {
                        objM.pushKV("file size, error", ex.what());
                    };
                };

                arrBuckets.push_back(objM);
            };
        }; // cs_smsg

        UniValue objM(UniValue::VOBJ);
        objM.pushKV("numbuckets", (int)nBuckets);
        objM.pushKV("numpurged", (int)smsgModule.setPurged.size());
        objM.pushKV("messages", (int)nMessages);
        objM.pushKV("size", omega::BytesReadable(nBytes));
        result.pushKV("buckets", arrBuckets);
        result.pushKV("total", objM);

    } else
    if (mode == "dump")
    {
        // Write bucket statistics to smsgstore/buckets_dump.json — read-only, non-destructive.
        UniValue dump(UniValue::VARR);
        {
            LOCK(smsgModule.cs_smsg);
            for (auto it = smsgModule.buckets.begin(); it != smsgModule.buckets.end(); ++it)
            {
                std::set<smsg::SecMsgToken> &tokenSet = it->second.setTokens;
                UniValue objM(UniValue::VOBJ);
                objM.pushKV("bucket", std::to_string(it->first));
                objM.pushKV("messages", (int)tokenSet.size());
                objM.pushKV("active", (int)it->second.CountActive());
                objM.pushKV("hash", std::to_string(it->second.hash));
                dump.push_back(objM);
            }
        } // cs_smsg

        fs::path dumpPath = GetDataDir() / "smsgstore" / "buckets_dump.json";
        try {
            std::ofstream ofs(dumpPath.string());
            ofs << dump.write(2);
            ofs.close();
        } catch (const std::exception &ex) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("Failed to write dump file: %s", ex.what()));
        }

        result.pushKV("result", "Bucket data written to file.");
        result.pushKV("file", dumpPath.string());
    } else
    {
        result.pushKV("result", "Unknown Mode.");
        result.pushKV("expected", "stats|dump.");
    };

    return result;
};

static UniValue smsgclearbuckets(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "smsgclearbuckets confirm\n"
            "Permanently delete all SMSG message data (.dat files in smsgstore/).\n"
            "\nArguments:\n"
            "1. confirm   (bool, required) Must be true to execute. Any other value aborts.\n"
            "\nWARNING: This is irreversible. All stored SMSG messages are lost.\n"
            "\nResult:\n"
            "{ \"result\": \"Removed all buckets.\", \"removed\": n }\n");

    if (!request.params[0].get_bool())
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            "Pass true to confirm permanent deletion of all SMSG message data.");

    EnsureSMSGIsEnabled();

    int nRemoved = 0;
    {
        LOCK(smsgModule.cs_smsg);
        for (auto it = smsgModule.buckets.begin(); it != smsgModule.buckets.end(); ++it)
        {
            fs::path fullPath = GetDataDir() / "smsgstore" / (std::to_string(it->first) + "_01.dat");
            try {
                fs::remove(fullPath);
                nRemoved++;
            } catch (const fs::filesystem_error &ex) {
                LogPrintf("smsgclearbuckets: error removing %s: %s\n", fullPath.string(), ex.what());
            }
        }
        smsgModule.buckets.clear();
    } // cs_smsg

    UniValue result(UniValue::VOBJ);
    result.pushKV("result", "Removed all buckets.");
    result.pushKV("removed", nRemoved);
    return result;
}

static bool sortMsgAsc(const std::pair<int64_t, UniValue> &a, const std::pair<int64_t, UniValue> &b)
{
    return a.first < b.first;
};

static bool sortMsgDesc(const std::pair<int64_t, UniValue> &a, const std::pair<int64_t, UniValue> &b)
{
    return a.first > b.first;
};

static UniValue smsgview(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 6)
        throw std::runtime_error(
            "smsgview  ( \"address/label\"|(asc/desc|-from yyyy-mm-dd|-to yyyy-mm-dd) )\n"
            "View messages by address."
            "Setting address to '*' will match all addresses"
            "'abc*' will match addresses with labels beginning 'abc'"
            "'*abc' will match addresses with labels ending 'abc'"
            "Full date/time format for from and to is yyyy-mm-ddThh:mm:ss"
            "From and to will accept incomplete inputs like: -from 2016");

    EnsureSMSGIsEnabled();

#ifdef ENABLE_WALLET
    if (!smsgModule.pwallet)
        throw JSONRPCError(RPC_MISC_ERROR, "No wallet connected to SMSG.");
    if (smsgModule.pwallet->IsLocked())
        throw JSONRPCError(RPC_MISC_ERROR, "Wallet is locked.");

    char cbuf[256];
    bool fMatchAll = false;
    bool fDesc = false;
    int64_t tFrom = 0, tTo = 0;
    std::vector<CKeyID> vMatchAddress;
    std::string sTemp;

    if (request.params.size() > 0)
    {
        sTemp = request.params[0].get_str();

        // Blank address or "*" will match all
        if (sTemp.length() < 1) // Error instead?
            fMatchAll = true;
        else
        if (sTemp.length() == 1 && sTemp[0] == '*')
            fMatchAll = true;

        if (!fMatchAll)
        {
            CTxDestination checkValid = DecodeDestination(sTemp);
            if (IsValidDestination(checkValid))
            {
                CKeyID ki = ToKeyID(std::get<PKHash>(checkValid));
                vMatchAddress.push_back(ki);
            } else
            {
                // Lookup address by label, can match multiple addresses

                // TODO: Use Boost.Regex?
                int matchType = 0; // 0 full match, 1 startswith, 2 endswith
                if (sTemp[0] == '*')
                {
                    matchType = 1;
                    sTemp.erase(0, 1);
                } else
                if (sTemp[sTemp.length()-1] == '*')
                {
                    matchType = 2;
                    sTemp.erase(sTemp.length()-1, 1);
                };

                std::map<CTxDestination, CAddressBookData>::iterator itl;

                for (itl = smsgModule.pwallet->mapAddressBook.begin(); itl != smsgModule.pwallet->mapAddressBook.end(); ++itl)
                {
                    if (omega::stringsMatchI(itl->second.name, sTemp, matchType))
                    {
                        CTxDestination checkValid = itl->first;
                        if (IsValidDestination(checkValid))
                        {
                            CKeyID ki = ToKeyID(std::get<PKHash>(checkValid));
                            vMatchAddress.push_back(ki);
                        } else
                        {
                            LogPrintf("Warning: matched invalid address: %s\n", EncodeDestination(checkValid).c_str());
                        };
                    };
                };
            };
        };
    } else
    {
        fMatchAll = true;
    };

    size_t i = 1;
    while (i < request.params.size())
    {
        sTemp = request.params[i].get_str();
        if (sTemp == "-from")
        {
            if (i >= request.params.size()-1)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Argument required for: " + sTemp);
            i++;
            sTemp = request.params[i].get_str();
            tFrom = strToEpoch(sTemp.c_str());
            if (tFrom < 0)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "from format error: " + std::string(strerror(errno)));
        } else
        if (sTemp == "-to")
        {
            if (i >= request.params.size()-1)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Argument required for: " + sTemp);
            i++;
            sTemp = request.params[i].get_str();
            tTo = strToEpoch(sTemp.c_str());
            if (tTo < 0)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "to format error: " + std::string(strerror(errno)));
        } else
        if (sTemp == "asc")
        {
            fDesc = false;
        } else
        if (sTemp == "desc")
        {
            fDesc = true;
        } else
        {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown parameter: " + sTemp);
        };

        i++;
    };

    if (!fMatchAll && vMatchAddress.size() < 1)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "No address found.");

    UniValue result(UniValue::VOBJ);

    std::map<CKeyID, std::string> mLabelCache;
    std::vector<std::pair<int64_t, UniValue> > vMessages;

    std::vector<std::string> vPrefixes;
    vPrefixes.push_back("im");
    vPrefixes.push_back("sm");

    uint8_t chKey[30];
    size_t nMessages = 0;
    UniValue messageList(UniValue::VARR);

    size_t debugEmptySent = 0;

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB dbMsg;
        if (!dbMsg.Open("cr"))
            throw std::runtime_error("Could not open DB.");

        std::vector<std::string>::iterator itp;
        std::vector<CKeyID>::iterator its;
        for (itp = vPrefixes.begin(); itp < vPrefixes.end(); ++itp)
        {
            bool fInbox = *itp == std::string("im");

            dbMsg.TxnBegin();

            leveldb::Iterator *it = dbMsg.pdb->NewIterator(leveldb::ReadOptions());
            smsg::SecMsgStored smsgStored;
            smsg::MessageData msg;

            while (dbMsg.NextSmesg(it, *itp, chKey, smsgStored))
            {
                if (!fInbox && smsgStored.addrOutbox.IsNull())
                {
                    debugEmptySent++;
                    continue;
                };

                if (smsgStored.vchMessage.size() < smsg::SMSG_HDR_LEN)
                    continue;
                int rv;
                if (fInbox) {
                    uint32_t nPayload = smsgStored.vchMessage.size() - smsg::SMSG_HDR_LEN;
                    rv = smsgModule.Decrypt(false, smsgStored.addrTo,
                        &smsgStored.vchMessage[0], &smsgStored.vchMessage[smsg::SMSG_HDR_LEN], nPayload, msg);
                } else {
                    rv = smsgModule.DecryptOutboxStored(smsgStored, msg);
                }
                if (rv == 0)
                {
                    if ((tFrom > 0 && msg.timestamp < tFrom)
                        || (tTo > 0 && msg.timestamp > tTo))
                        continue;

                    CKeyID kiFrom;
                    CTxDestination addrFrom = DecodeDestination(msg.sFromAddress);
                    if (IsValidDestination(addrFrom))
                        kiFrom = ToKeyID(std::get<PKHash>(addrFrom));

                    if (!fMatchAll)
                    {
                        bool fSkip = true;

                        for (its = vMatchAddress.begin(); its < vMatchAddress.end(); ++its)
                        {
                            if (*its == kiFrom
                                || *its == smsgStored.addrTo)
                            {
                                fSkip = false;
                                break;
                            };
                        };

                        if (fSkip)
                            continue;
                    };

                    // Get labels for addresses, cache found labels.
                    std::string lblFrom, lblTo;
                    std::map<CKeyID, std::string>::iterator itl;

                    if ((itl = mLabelCache.find(kiFrom)) != mLabelCache.end())
                    {
                        lblFrom = itl->second;
                    } else
                    {
                        CKeyID address_parsed(kiFrom);
                        std::map<CTxDestination, CAddressBookData>::iterator
                            mi(smsgModule.pwallet->mapAddressBook.find(PKHash(address_parsed)));
                        if (mi != smsgModule.pwallet->mapAddressBook.end())
                            lblFrom = mi->second.name;
                        mLabelCache[kiFrom] = lblFrom;
                    };

                    if ((itl = mLabelCache.find(smsgStored.addrTo)) != mLabelCache.end())
                    {
                        lblTo = itl->second;
                    } else
                    {
                        CKeyID address_parsed(smsgStored.addrTo);
                        std::map<CTxDestination, CAddressBookData>::iterator
                            mi(smsgModule.pwallet->mapAddressBook.find(PKHash(address_parsed)));
                        if (mi != smsgModule.pwallet->mapAddressBook.end())
                            lblTo = mi->second.name;
                        mLabelCache[smsgStored.addrTo] = lblTo;
                    };

                    std::string sFrom = kiFrom.IsNull() ? "anon" : EncodeDestination(PKHash(kiFrom));
                    std::string sTo = EncodeDestination(PKHash(smsgStored.addrTo));
                    if (lblFrom.length() != 0)
                        sFrom += " (" + lblFrom + ")";
                    if (lblTo.length() != 0)
                        sTo += " (" + lblTo + ")";

                    UniValue objM(UniValue::VOBJ);
                    PushTime(objM, "sent", msg.timestamp);
                    objM.pushKV("from", sFrom);
                    objM.pushKV("to", sTo);
                    objM.pushKV("text", std::string((char*)&msg.vchMessage[0]));

                    vMessages.push_back(std::make_pair(msg.timestamp, objM));
                } else
                {
                    LogPrintf("%s: SecureMsgDecrypt failed, %s.\n", __func__, HexStr(Span<uint8_t>(chKey, chKey+18)).c_str());
                };
            };
            delete it;

            dbMsg.TxnCommit();
        };
    } // cs_smsgDB


    std::sort(vMessages.begin(), vMessages.end(), fDesc ? sortMsgDesc : sortMsgAsc);

    std::vector<std::pair<int64_t, UniValue> >::iterator itm;
    for (itm = vMessages.begin(); itm < vMessages.end(); ++itm)
    {
        messageList.push_back(itm->second);
        nMessages++;
    };

    result.pushKV("messages", messageList);

    if (LogAcceptCategory(BCLog::SMSG))
        result.pushKV("debug empty sent", (int)debugEmptySent);

    result.pushKV("result", strprintf("Displayed %u messages.", nMessages));
    if (tFrom > 0)
        result.pushKV("from", GetTimeString(tFrom, cbuf, sizeof(cbuf)));
    if (tTo > 0)
        result.pushKV("to", GetTimeString(tTo, cbuf, sizeof(cbuf)));
#else
    UniValue result(UniValue::VOBJ);
    throw JSONRPCError(RPC_MISC_ERROR, "No wallet.");
#endif
    return result;
}

static UniValue smsgone(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw std::runtime_error(
            "smsg \"msgid\" ( options )\n"
            "View smsg by msgid.\n"
            "\nArguments:\n"
            "1. \"msgid\"              (string, required) The id of the message to view.\n"
            "2. options              (json, optional) Options object.\n"
            "{\n"
            "       \"delete\": bool                 (bool, optional) Delete msg if true.\n"
            "       \"setread\": bool                (bool, optional) Set read status to value.\n"
            "       \"encoding\": str                (string, optional, default=\"ascii\") Display message data in encoding, values: \"hex\".\n"
            "}\n"
            "\nResult:\n"
            "{\n"
            "  \"msgid\": \"...\"                    (string) The message identifier\n"
            "  \"version\": \"str\"                  (string) The message version\n"
            "  \"location\": \"str\"                 (string) inbox|outbox|sending\n"
            "  \"received\": int                     (int) Time the message was received\n"
            "  \"to\": \"str\"                       (string) Address the message was sent to\n"
            "  \"read\": bool                        (bool) Read status\n"
            "  \"sent\": int                         (int) Time the message was created\n"
            "  \"paid\": bool                        (bool) Paid or free message\n"
            "  \"daysretention\": int                (int) Number of days message will stay in the network for\n"
            "  \"expiration\": int                   (int) Time the message will be dropped from the network\n"
            "  \"payloadsize\": int                  (int) Size of user message\n"
            "  \"from\": \"str\"                     (string) Address the message was sent from\n"
            "}\n");

    EnsureSMSGIsEnabled();

    RPCTypeCheckObj(request.params,
        {
            {"msgid",             UniValueType(UniValue::VSTR)},
            {"option",            UniValueType(UniValue::VOBJ)},
        }, true, false);

    std::string sMsgId = request.params[0].get_str();

    if (!IsHex(sMsgId) || sMsgId.size() != 56)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "msgid must be 28 bytes in hex string.");
    std::vector<uint8_t> vMsgId = ParseHex(sMsgId.c_str());
    std::string sType;

    uint8_t chKey[30];
    chKey[1] = 'm';
    memcpy(chKey+2, vMsgId.data(), 28);
    smsg::SecMsgStored smsgStored;
    UniValue result(UniValue::VOBJ);

    UniValue options = request.params[1];
    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB dbMsg;
        if (!dbMsg.Open("cr+"))
            throw std::runtime_error("Could not open DB.");

        if ((chKey[0] = 'i') && dbMsg.ReadSmesg(chKey, smsgStored)) {
            sType = "inbox";
        } else
        if ((chKey[0] = 's') && dbMsg.ReadSmesg(chKey, smsgStored)) {
            sType = "outbox";
        } else
        if ((chKey[0] = 'q') && dbMsg.ReadSmesg(chKey, smsgStored)) {
            sType = "sending";
        } else {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Unknown message id.");
        };

        if (options.isObject())
        {
            options = request.params[1].get_obj();
            if (options["delete"].isBool() && options["delete"].get_bool() == true)
            {
                if (!dbMsg.EraseSmesg(chKey))
                    throw JSONRPCError(RPC_INTERNAL_ERROR, "EraseSmesg failed.");
                result.pushKV("operation", "Deleted");
            } else
            {
                // Can't mix delete and other operations
                if (options["setread"].isBool())
                {
                    bool nv = options["setread"].get_bool();
                    if (nv)
                        smsgStored.status &= ~SMSG_MASK_UNREAD;
                    else
                        smsgStored.status |= SMSG_MASK_UNREAD;

                    if (!dbMsg.WriteSmesg(chKey, smsgStored))
                        throw JSONRPCError(RPC_INTERNAL_ERROR, "WriteSmesg failed.");
                    result.pushKV("operation", strprintf("Set read status to: %s", nv ? "true" : "false"));
                };
            };
        };
    }

    if (smsgStored.vchMessage.size() < smsg::SMSG_HDR_LEN)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Message record too short.");
    const smsg::SecureMessage *psmsg = (smsg::SecureMessage*) &smsgStored.vchMessage[0];

    result.pushKV("msgid", sMsgId);
    result.pushKV("version", strprintf("%02x%02x", psmsg->version[0], psmsg->version[1]));
    result.pushKV("location", sType);
    PushTime(result, "received", smsgStored.timeReceived);
    result.pushKV("to", EncodeDestination(PKHash(smsgStored.addrTo)));
    //result.pushKV("addressoutbox", CKeyID(smsgStored.addrOutbox).ToString());
    result.pushKV("read", UniValue(bool(!(smsgStored.status & SMSG_MASK_UNREAD))));

    PushTime(result, "sent", psmsg->timestamp);
    bool fIsPaid = smsg::OutboxHdrIsPaid(*psmsg);
    result.pushKV("paid", UniValue(fIsPaid));

    uint32_t nDaysRetention = fIsPaid ? psmsg->nonce[0] : 2;
    int64_t ttl = smsg::SMSGGetSecondsInDay() * nDaysRetention;
    result.pushKV("daysretention", (int)nDaysRetention);
    PushTime(result, "expiration", psmsg->timestamp + ttl);


    smsg::MessageData msg;
    bool fInbox = sType == "inbox" ? true : false;
    uint32_t nPayload = smsgStored.vchMessage.size() - smsg::SMSG_HDR_LEN;
    result.pushKV("payloadsize", (int)nPayload);

    std::string sEnc;
    if (options.isObject() && options["encoding"].isStr())
        sEnc = options["encoding"].get_str();

    int rv;
    if (!fInbox && psmsg->version[0] == smsg::SMSG_OUTBOX_LOCAL_FMT) {
        rv = smsgModule.DecryptOutboxStored(smsgStored, msg);
    } else {
        rv = smsgModule.Decrypt(false, fInbox ? smsgStored.addrTo : smsgStored.addrOutbox,
            &smsgStored.vchMessage[0], &smsgStored.vchMessage[smsg::SMSG_HDR_LEN], nPayload, msg);
    }
    if (rv == 0)
    {
        result.pushKV("from", msg.sFromAddress);

        if (sEnc == "")
        {
            // TODO: detect non ascii chars
            if (msg.vchMessage.size() < smsg::SMSG_MAX_MSG_BYTES)
                result.pushKV("text", std::string((char*)msg.vchMessage.data()));
            else
                result.pushKV("hex", HexStr(msg.vchMessage));
        } else
        if (sEnc == "ascii")
        {
            result.pushKV("text", std::string((char*)msg.vchMessage.data()));
        } else
        if (sEnc == "hex")
        {
            result.pushKV("hex", HexStr(msg.vchMessage));
        } else
        {
            result.pushKV("unknown_encoding", sEnc);
        };
    } else
    {
        result.pushKV("error", "decrypt failed");
    };

    return result;
}

static UniValue smsgpurge(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "smsgpurge \"msgid\"\n"
            "Purge smsg by msgid.\n"
            "\nArguments:\n"
            "1. \"msgid\"              (string, required) The id of the message to purge.\n"
            "\nResult:\n"
        );

    EnsureSMSGIsEnabled();

    if (!request.params[0].isStr())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "msgid must be a string.");

    std::string sMsgId = request.params[0].get_str();

    if (!IsHex(sMsgId) || sMsgId.size() != 56)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "msgid must be 28 bytes in hex string.");
    std::vector<uint8_t> vMsgId = ParseHex(sMsgId.c_str());

    std::string sError;
    if (smsg::SMSG_NO_ERROR != smsgModule.Purge(vMsgId, sError))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Error: " + sError);

    return NullUniValue;
}

// ---- Phase 1: Mobile Dashboard & Connection Health ----

static UniValue smsggetinfo(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "smsggetinfo\n"
            "Returns SMSG status information.\n"
            "\nResult:\n"
            "{\n"
            "  \"enabled\": true|false,\n"
            "  \"active_wallet\": \"str\",\n"
            "  \"enabled_wallets\": [\"str\",...]\n"
            "}\n");

    UniValue result(UniValue::VOBJ);
    result.pushKV("enabled", smsg::fSecMsgEnabled);
    result.pushKV("active_wallet", smsgModule.GetWalletName());

    UniValue wallets(UniValue::VARR);
    for (const auto &pw : smsgModule.m_vpwallets) {
        if (pw) {
            wallets.push_back(pw->GetName());
        }
    }
    result.pushKV("enabled_wallets", wallets);

    return result;
}

static UniValue smsgpeers(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "smsgpeers ( node_id )\n"
            "Returns SMSG peer information.\n"
            "\nArguments:\n"
            "1. node_id    (numeric, optional) Return info for a specific peer.\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"id\": n,\n"
            "    \"version\": n,\n"
            "    \"ignoreuntil\": n,\n"
            "    \"misbehaving\": n,\n"
            "    \"numwantsent\": n,\n"
            "    \"receivecounter\": n,\n"
            "    \"ignoredcounter\": n\n"
            "  }\n"
            "]\n");

    EnsureSMSGIsEnabled();

    int node_id = -1;
    if (request.params.size() > 0) {
        node_id = request.params[0].get_int();
    }

    UniValue result(UniValue::VARR);
    smsgModule.GetNodesStats(node_id, result);
    return result;
}

static UniValue smsgaddresses(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "smsgaddresses\n"
            "Returns SMSG receiving addresses.\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"address\": \"str\",\n"
            "    \"receive_enabled\": true|false,\n"
            "    \"receive_anon\": true|false\n"
            "  }\n"
            "]\n");

    EnsureSMSGIsEnabled();

    UniValue result(UniValue::VARR);
    LOCK(smsgModule.cs_smsg);
    for (const auto &addr : smsgModule.addresses) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("address", EncodeDestination(PKHash(addr.address)));
        entry.pushKV("receive_enabled", addr.fReceiveEnabled);
        entry.pushKV("receive_anon", addr.fReceiveAnon);
        result.push_back(entry);
    }
    return result;
}

static UniValue smsgzmqpush(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            "smsgzmqpush ( \"timefrom\" \"timeto\" )\n"
            "Returns unread message IDs from inbox.\n"
            "\nArguments:\n"
            "1. timefrom    (numeric, optional, default=0) Skip messages received before timestamp.\n"
            "2. timeto      (numeric, optional, default=max) Skip messages received after timestamp.\n"
            "\nResult:\n"
            "{\n"
            "  \"numsent\": n,\n"
            "  \"msgids\": [\"str\",...]\n"
            "}\n");

    EnsureSMSGIsEnabled();

    int64_t timeFrom = 0, timeTo = std::numeric_limits<int64_t>::max();
    if (request.params.size() > 0)
        timeFrom = request.params[0].get_int64();
    if (request.params.size() > 1)
        timeTo = request.params[1].get_int64();

    UniValue result(UniValue::VOBJ);
    UniValue msgids(UniValue::VARR);
    int nSent = 0;

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB db;
        if (!db.Open("cr+"))
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Could not open DB.");

        std::string sPrefix("im");
        uint8_t chKey[30];
        smsg::SecMsgStored smsgStored;

        leveldb::Iterator *it = db.pdb->NewIterator(leveldb::ReadOptions());
        while (db.NextSmesg(it, sPrefix, chKey, smsgStored)) {
            if (!(smsgStored.status & SMSG_MASK_UNREAD))
                continue;
            if (smsgStored.timeReceived < timeFrom || smsgStored.timeReceived > timeTo)
                continue;
            msgids.push_back(HexStr(Span<uint8_t>(&chKey[2], &chKey[2] + 28)));
            nSent++;
        }
        delete it;
    }

    result.pushKV("numsent", nSent);
    result.pushKV("msgids", msgids);
    return result;
}

static UniValue smsggetfeerate(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "smsggetfeerate\n"
            "Returns current SMSG fee rate.\n"
            "\nResult:\n"
            "{\n"
            "  \"feeperkperday\": n,\n"
            "  \"fundingtxnfeeperk\": n\n"
            "}\n");

    UniValue result(UniValue::VOBJ);
    result.pushKV("feeperkperday", smsg::nMsgFeePerKPerDay);
    result.pushKV("fundingtxnfeeperk", smsg::nFundingTxnFeePerK);
    return result;
}

static UniValue smsggetdifficulty(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "smsggetdifficulty\n"
            "Returns current SMSG PoW difficulty.\n"
            "\nResult:\n"
            "{\n"
            "  \"currentdifficulty\": n\n"
            "}\n");

    UniValue result(UniValue::VOBJ);
    uint32_t nSecondsInDay = smsg::SMSGGetSecondsInDay();
    result.pushKV("secondsinday", (uint64_t)nSecondsInDay);
    return result;
}

// ---- Phase 2: Full CRUD & Backup/Restore ----

static UniValue smsgremoveaddress(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "smsgremoveaddress \"address\"\n"
            "Remove address from SMSG address book and pubkey database.\n"
            "\nArguments:\n"
            "1. \"address\"    (string, required) The address to remove.\n"
            "\nResult:\n"
            "{\n"
            "  \"result\": \"str\"\n"
            "}\n");

    EnsureSMSGIsEnabled();

    std::string addr = request.params[0].get_str();
    int rv = smsgModule.RemoveAddress(addr);
    if (rv != smsg::SMSG_NO_ERROR)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Remove failed: %s", smsg::GetString(rv)));

    UniValue result(UniValue::VOBJ);
    result.pushKV("result", "Success");
    return result;
}

static UniValue smsgremoveprivkey(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "smsgremoveprivkey \"address\"\n"
            "Remove private key for address from SMSG keystore.\n"
            "\nArguments:\n"
            "1. \"address\"    (string, required) The address whose private key to remove.\n"
            "\nResult:\n"
            "{\n"
            "  \"result\": \"str\"\n"
            "}\n");

    EnsureSMSGIsEnabled();

    std::string addr = request.params[0].get_str();
    int rv = smsgModule.RemovePrivkey(addr);
    if (rv != smsg::SMSG_NO_ERROR)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Remove failed: %s", smsg::GetString(rv)));

    UniValue result(UniValue::VOBJ);
    result.pushKV("result", "Success");
    return result;
}

static UniValue smsgdumpprivkey(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "smsgdumpprivkey \"address\"\n"
            "Dump SMSG private key for address as WIF.\n"
            "\nArguments:\n"
            "1. \"address\"    (string, required) The address.\n"
            "\nResult:\n"
            "\"privkey\"    (string) The WIF-encoded private key.\n");

    EnsureSMSGIsEnabled();

    std::string addr = request.params[0].get_str();
    CTxDestination dest = DecodeDestination(addr);
    if (!IsValidDestination(dest))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address.");

    const PKHash *pkhash = std::get_if<PKHash>(&dest);
    if (!pkhash)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Address is not a P2PKH address.");

    CKeyID idk = ToKeyID(*pkhash);
    CKey key;
    int rv = smsgModule.DumpPrivkey(idk, key);
    if (rv != smsg::SMSG_NO_ERROR)
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Dump failed: %s", smsg::GetString(rv)));

    return EncodeSecret(key);
}

static UniValue smsgfund(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "smsgfund \"msgid\" ( testfee )\n"
            "Fund a paid secure message from the outbox.\n"
            "\nArguments:\n"
            "1. \"msgid\"     (string, required) The message ID (28 bytes, hex encoded).\n"
            "2. testfee      (bool, optional, default=false) Only estimate the fee, don't submit.\n"
            "\nResult:\n"
            "{\n"
            "  \"txid\": \"str\",\n"
            "  \"fee\": n\n"
            "}\n");

    EnsureSMSGIsEnabled();

    std::string sMsgId = request.params[0].get_str();
    if (!IsHex(sMsgId) || sMsgId.size() != 56)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "msgid must be 28 bytes in hex string.");

    bool fTestFee = false;
    if (request.params.size() > 1)
        fTestFee = request.params[1].get_bool();

    std::vector<uint8_t> vMsgId = ParseHex(sMsgId.c_str());

    // Read message from outbox
    smsg::SecMsgStored smsgStored;
    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB db;
        if (!db.Open("cr+"))
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Could not open DB.");

        uint8_t chKey[30];
        chKey[0] = 's';
        chKey[1] = 'm';
        memcpy(&chKey[2], vMsgId.data(), 28);
        if (!db.ReadSmesg(chKey, smsgStored))
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Message not found in outbox.");
    }

    if (smsgStored.vchMessage.size() < smsg::SMSG_HDR_LEN)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Message too short.");

    smsg::SecureMessage secMsg;
    memcpy(&secMsg, smsgStored.vchMessage.data(), smsg::SMSG_HDR_LEN);
    uint32_t nPayload = smsgStored.vchMessage.size() - smsg::SMSG_HDR_LEN;
    // Point pPayload into smsgStored's buffer — must null before secMsg destructor runs
    secMsg.pPayload = &smsgStored.vchMessage[smsg::SMSG_HDR_LEN];
    secMsg.nPayload = nPayload;
    struct PayloadGuard { smsg::SecureMessage *p; ~PayloadGuard() { if (p) p->pPayload = nullptr; } } guard{&secMsg};

    std::string sError;
    CAmount nFee = 0;
    int rv = smsgModule.FundMsg(secMsg, sError, fTestFee, &nFee);

    UniValue result(UniValue::VOBJ);
    if (rv != smsg::SMSG_NO_ERROR) {
        secMsg.pPayload = nullptr; // prevent double-free
        throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("FundMsg failed: %s", sError));
    }

    result.pushKV("fee", ValueFromAmount(nFee));
    if (!fTestFee) {
        // Copy funded header back into stored message
        memcpy(smsgStored.vchMessage.data(), &secMsg, smsg::SMSG_HDR_LEN);

        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB db;
        if (db.Open("cr+")) {
            uint8_t chKey[30];
            chKey[0] = 's';
            chKey[1] = 'm';
            memcpy(&chKey[2], vMsgId.data(), 28);
            db.WriteSmesg(chKey, smsgStored);
        }
        result.pushKV("result", "Funded successfully.");
    } else {
        result.pushKV("result", "Fee estimated.");
    }
    secMsg.pPayload = nullptr; // prevent double-free — destructor will delete[] pPayload
    return result;
}

static UniValue smsgimport(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "smsgimport \"hex\"\n"
            "Import a secure message from hex.\n"
            "\nArguments:\n"
            "1. \"hex\"    (string, required) The message in hex (header + payload).\n"
            "\nResult:\n"
            "{\n"
            "  \"msgid\": \"str\"\n"
            "}\n");

    EnsureSMSGIsEnabled();

    std::string sHex = request.params[0].get_str();
    if (!IsHex(sHex))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Argument must be hex encoded.");

    std::vector<uint8_t> vData = ParseHex(sHex);
    if (vData.size() < smsg::SMSG_HDR_LEN)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Data too short.");

    const uint8_t *pHeader = vData.data();
    const uint8_t *pPayload = vData.data() + smsg::SMSG_HDR_LEN;
    uint32_t nPayload = vData.size() - smsg::SMSG_HDR_LEN;

    int rv = smsgModule.Validate(pHeader, pPayload, nPayload);
    if (rv != smsg::SMSG_NO_ERROR)
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Validation failed: %s", smsg::GetString(rv)));

    {
        LOCK(smsgModule.cs_smsg);
        rv = smsgModule.Store(pHeader, pPayload, nPayload, true);
    }
    if (rv != smsg::SMSG_NO_ERROR)
        throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("Store failed: %s", smsg::GetString(rv)));

    rv = smsgModule.ScanMessage(pHeader, pPayload, nPayload, true);

    const smsg::SecureMessage *psmsg = (const smsg::SecureMessage*)pHeader;
    std::vector<uint8_t> vMsgId = smsgModule.GetMsgID(psmsg, pPayload);

    UniValue result(UniValue::VOBJ);
    result.pushKV("msgid", HexStr(vMsgId));
    return result;
}

static UniValue smsgsetwallet(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "smsgsetwallet \"walletname\"\n"
            "Set the active wallet for SMSG.\n"
            "\nArguments:\n"
            "1. \"walletname\"    (string, required) The wallet name.\n"
            "\nResult:\n"
            "{\n"
            "  \"result\": \"str\",\n"
            "  \"wallet\": \"str\"\n"
            "}\n");

    EnsureSMSGIsEnabled();

    std::string walletName = request.params[0].get_str();

    std::shared_ptr<CWallet> pFound = nullptr;
    for (const auto &pw : smsgModule.m_vpwallets) {
        if (pw && pw->GetName() == walletName) {
            pFound = pw;
            break;
        }
    }

    if (!pFound)
        throw JSONRPCError(RPC_WALLET_NOT_FOUND, strprintf("Wallet \"%s\" not found in SMSG wallet list.", walletName));

    if (!smsgModule.SetActiveWallet(pFound))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "SetActiveWallet failed.");

    UniValue result(UniValue::VOBJ);
    result.pushKV("result", "Active wallet set.");
    result.pushKV("wallet", walletName);
    return result;
}

// ---- Phase 3: Debugging & Final Parity ----

static UniValue smsgdebug(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            "smsgdebug ( \"command\" \"arg1\" )\n"
            "Commands useful for debugging.\n"
            "\nArguments:\n"
            "1. \"command\"    (string, optional) \"clearbanned\"|\"dumpids\".\n"
            "2. \"arg1\"      (string, optional) For dumpids: \"true\" = active only (default), \"false\" = all.\n"
            "\nResult:\n"
            "Varies by command.\n"
            "\nWith no arguments returns bucket summary.\n");

    EnsureSMSGIsEnabled();

    std::string mode = "none";
    if (request.params.size() > 0)
        mode = request.params[0].get_str();

    UniValue result(UniValue::VOBJ);

    if (mode == "clearbanned") {
        result.pushKV("command", mode);
        smsgModule.ClearBanned();
        result.pushKV("result", "Cleared banned peers.");
    } else
    if (mode == "dumpids") {
        fs::path filepath = GetDataDir() / "smsg_ids.txt";
        if (fs::exists(filepath))
            throw JSONRPCError(RPC_INVALID_PARAMETER, "smsg_ids.txt already exists in the datadir. Please move it out of the way first.");

        bool active_only = true;
        if (request.params.size() > 1) {
            std::string sArg = request.params[1].get_str();
            if (sArg == "false" || sArg == "0")
                active_only = false;
        }

        int64_t now = GetAdjustedTime();

        FILE *fp = fopen(filepath.string().c_str(), "w");
        if (!fp)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Cannot open dump file.");

        int num_messages = 0;
        {
            LOCK(smsgModule.cs_smsg);
            std::vector<uint8_t> vch_msg;
            for (auto it = smsgModule.buckets.begin(); it != smsgModule.buckets.end(); ++it) {
                const std::set<smsg::SecMsgToken> &tokenSet = it->second.setTokens;
                for (const auto &token : tokenSet) {
                    if (active_only && token.timestamp + token.ttl * smsg::SMSGGetSecondsInDay() < now)
                        continue;
                    if (smsgModule.Retrieve(token, vch_msg) != smsg::SMSG_NO_ERROR) {
                        LogPrintf("SecureMsgRetrieve failed %d.\n", token.timestamp);
                        continue;
                    }
                    if (vch_msg.size() < smsg::SMSG_HDR_LEN)
                        continue;
                    const smsg::SecureMessage *psmsg = (const smsg::SecureMessage*)vch_msg.data();
                    if (psmsg->version[0] == 0 && psmsg->version[1] == 0)
                        continue; // skip purged
                    std::vector<uint8_t> msgId = smsgModule.GetMsgID(psmsg, vch_msg.data() + smsg::SMSG_HDR_LEN);
                    fprintf(fp, "%d,%s\n", (int)it->first, HexStr(msgId).c_str());
                    num_messages++;
                }
            }
        } // cs_smsg
        fclose(fp);

        result.pushKV("active_only", active_only);
        result.pushKV("messages", num_messages);
        result.pushKV("file", filepath.string());
    } else
    if (mode == "none") {
        // Return summary info
        LOCK(smsgModule.cs_smsg);
        uint32_t nBuckets = smsgModule.buckets.size();
        uint32_t nMessages = 0;
        for (auto it = smsgModule.buckets.begin(); it != smsgModule.buckets.end(); ++it) {
            nMessages += it->second.nActive;
        }
        result.pushKV("enabled", smsg::fSecMsgEnabled);
        result.pushKV("buckets", (int)nBuckets);
        result.pushKV("active_messages", (int)nMessages);
        result.pushKV("purged", (int)smsgModule.setPurged.size());
        result.pushKV("addresses", (int)smsgModule.addresses.size());
        result.pushKV("keystore_keys", (int)smsgModule.keyStore.mapKeys.size());
    } else {
        result.pushKV("error", "Unknown command.");
        result.pushKV("expected", "clearbanned|dumpids");
    }

    return result;
}

static UniValue trollboxsend(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2) {
        throw std::runtime_error(
            "trollboxsend \"message\" ( paid )\n"
            "\nSend a message to the public Trollbox channel.\n"
            "\nArguments:\n"
            "1. \"message\"    (string, required) Message text (max 256 chars)\n"
            "2. paid           (boolean, optional, default=false) Send as paid message (displayed in red)\n"
            "\nResult:\n"
            "{\n"
            "  \"result\": \"Sent.\"\n"
            "}\n");
    }

    EnsureSMSGIsEnabled();

    std::string sMessage = request.params[0].get_str();
    if (sMessage.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Message must not be empty.");
    }
    if (sMessage.size() > smsg::TROLLBOX_MAX_MSG_BYTES) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            strprintf("Message too long (%d > %d)", sMessage.size(), smsg::TROLLBOX_MAX_MSG_BYTES));
    }

    bool fPaid = false;
    if (request.params.size() > 1)
        fPaid = request.params[1].get_bool();

    // Rate limit
    int64_t now = GetTime();
    if (now - smsgModule.nLastTrollboxSend < smsg::TROLLBOX_RATE_LIMIT_SECS) {
        int remaining = smsg::TROLLBOX_RATE_LIMIT_SECS - (int)(now - smsgModule.nLastTrollboxSend);
        throw JSONRPCError(RPC_MISC_ERROR,
            strprintf("Rate limited. Wait %d seconds.", remaining));
    }

    // Find first available sender address
    CKeyID kiFrom;
    {
        LOCK(smsgModule.cs_smsg);
#ifdef ENABLE_WALLET
        if (smsgModule.pwallet) {
            for (auto& a : smsgModule.addresses) {
                if (a.fReceiveEnabled && a.address != smsgModule.trollboxAddress) {
                    kiFrom = a.address;
                    break;
                }
            }
        }
#endif
        if (kiFrom.IsNull()) {
            for (auto& p : smsgModule.keyStore.mapKeys) {
                if (p.first == smsgModule.trollboxAddress) continue;
                auto& key = p.second;
                if (!(key.nFlags & smsg::SMK_RECEIVE_ON)) continue;
                if (key.nFlags & smsg::SMK_CONTACT_ONLY) continue;
                kiFrom = p.first;
                break;
            }
        }
    }

    if (kiFrom.IsNull())
        throw JSONRPCError(RPC_WALLET_ERROR, "No sending address available. Generate an SMSG key first.");

    CKeyID kiTo = smsgModule.trollboxAddress;
    std::string sError;
    smsg::SecureMessage smsgOut;

    int rv = smsgModule.Send(kiFrom, kiTo, sMessage, smsgOut, sError, fPaid, fPaid ? 1 : 0);
    if (rv != 0) {
        throw JSONRPCError(RPC_MISC_ERROR, strprintf("Send failed: %s", sError));
    }

    smsgModule.nLastTrollboxSend = GetTime();

    UniValue result(UniValue::VOBJ);
    result.pushKV("result", "Sent.");
    result.pushKV("from", EncodeDestination(PKHash(kiFrom)));
    result.pushKV("paid", fPaid);
    return result;
}

static UniValue trollboxlist(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 1) {
        throw std::runtime_error(
            "trollboxlist ( count )\n"
            "\nList recent Trollbox messages.\n"
            "\nArguments:\n"
            "1. count       (numeric, optional, default=50) Number of messages to return\n"
            "\nResult:\n"
            "[\n"
            "  { \"time\": n, \"from\": \"addr\", \"text\": \"...\", \"paid\": true|false }\n"
            "]\n");
    }

    EnsureSMSGIsEnabled();

    int nCount = 50;
    if (request.params.size() > 0)
        nCount = request.params[0].get_int();
    if (nCount <= 0 || nCount > 1000)
        nCount = 50;

    struct TbMsg { int64_t time; std::string from; std::string text; bool paid; };
    std::vector<TbMsg> msgs;

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB db;
        if (!db.Open("cr+"))
            throw JSONRPCError(RPC_DATABASE_ERROR, "Failed to open smsg DB.");

        std::string sPrefix("tb");
        uint8_t chKey[30];
        smsg::SecMsgStored smsgStored;
        smsg::MessageData msg;

        leveldb::Iterator* it = db.pdb->NewIterator(leveldb::ReadOptions());
        while (db.NextSmesg(it, sPrefix, chKey, smsgStored))
        {
            if (smsgStored.vchMessage.size() < smsg::SMSG_HDR_LEN)
                continue;
            uint8_t* pHeader = &smsgStored.vchMessage[0];
            const smsg::SecureMessage* psmsg = (smsg::SecureMessage*)pHeader;
            uint32_t nPayload = smsgStored.vchMessage.size() - smsg::SMSG_HDR_LEN;
            int rv = smsgModule.Decrypt(false, smsgStored.addrTo, pHeader, pHeader + smsg::SMSG_HDR_LEN, nPayload, msg);
            if (rv != 0) continue;

            TbMsg m;
            m.time = msg.timestamp;
            m.from = msg.sFromAddress;
            m.text = std::string((char*)msg.vchMessage.data());
            m.paid = psmsg->IsPaidVersion();
            msgs.push_back(m);
        }
        delete it;
    }

    // Keep only most recent N
    if ((int)msgs.size() > nCount) {
        msgs.erase(msgs.begin(), msgs.begin() + (msgs.size() - nCount));
    }

    UniValue result(UniValue::VARR);
    for (const auto& m : msgs) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("time", m.time);
        obj.pushKV("from", m.from);
        obj.pushKV("text", m.text);
        obj.pushKV("paid", m.paid);
        result.push_back(obj);
    }

    return result;
}

// ---- Topic Channel RPCs ----

static UniValue smsgsubscribe(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "smsgsubscribe \"topic\"\n"
            "\nSubscribe to an SMSG topic channel. Incoming messages for this topic will be stored.\n"
            "\nArguments:\n"
            "1. \"topic\"   (string, required) Topic string, e.g. \"omega.listings\"\n"
            "\nResult:\n"
            "{ \"result\": \"ok\" }\n"
            "\nExamples:\n"
            + HelpExampleCli("smsgsubscribe", "\"omega.listings\""));

    EnsureSMSGIsEnabled();

    std::string topic = request.params[0].get_str();
    std::string sError;
    if (!smsgModule.SubscribeTopic(topic, sError))
        throw JSONRPCError(RPC_INVALID_PARAMETER, sError);

    UniValue result(UniValue::VOBJ);
    result.pushKV("result", "ok");
    result.pushKV("topic", topic);
    return result;
}

static UniValue smsgunsubscribe(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "smsgunsubscribe \"topic\"\n"
            "\nUnsubscribe from an SMSG topic channel.\n"
            "\nArguments:\n"
            "1. \"topic\"   (string, required) Topic string, e.g. \"omega.listings\"\n"
            "\nResult:\n"
            "{ \"result\": \"ok\" }\n"
            "\nExamples:\n"
            + HelpExampleCli("smsgunsubscribe", "\"omega.listings\""));

    EnsureSMSGIsEnabled();

    std::string topic = request.params[0].get_str();
    std::string sError;
    if (!smsgModule.UnsubscribeTopic(topic, sError))
        throw JSONRPCError(RPC_INVALID_PARAMETER, sError);

    UniValue result(UniValue::VOBJ);
    result.pushKV("result", "ok");
    result.pushKV("topic", topic);
    return result;
}

static UniValue smsglisttopics(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "smsglisttopics\n"
            "\nList currently subscribed SMSG topic channels.\n"
            "\nResult:\n"
            "[\"omega.listings\", ...]\n"
            "\nExamples:\n"
            + HelpExampleCli("smsglisttopics", ""));

    EnsureSMSGIsEnabled();

    UniValue result(UniValue::VARR);
    {
        LOCK(smsgModule.cs_smsgSubs);
        for (const auto &t : smsgModule.m_subscribed_topics) {
            result.push_back(t);
        }
    }
    return result;
}

static UniValue smsggetmessages(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "smsggetmessages \"topic\" ( count )\n"
            "\nRetrieve indexed messages for an SMSG topic channel.\n"
            "\nArguments:\n"
            "1. \"topic\"   (string, required) Topic string, e.g. \"omega.listings\"\n"
            "2. count      (int, optional, default=50) Maximum number of messages to return (newest first).\n"
            "\nResult:\n"
            "[{ \"msgid\": \"...\", \"timestamp\": n, \"from\": \"...\", \"topic\": \"...\" }, ...]\n"
            "\nExamples:\n"
            + HelpExampleCli("smsggetmessages", "\"omega.listings\"")
            + HelpExampleCli("smsggetmessages", "\"omega.listings\" 20"));

    EnsureSMSGIsEnabled();

    std::string topic = request.params[0].get_str();
    if (!smsg::IsValidTopic(topic))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid topic string.");

    int nCount = request.params.size() > 1 ? request.params[1].get_int() : 50;
    if (nCount < 1 || nCount > 1000)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "count must be between 1 and 1000.");

    std::vector<smsg::SecMsgDB::TopicEntry> entries;
    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB db;
        if (!db.Open("r"))
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Cannot open SMSG database.");
        db.ReadTopicMessages(topic, entries, (size_t)nCount);
    }

    UniValue result(UniValue::VARR);
    // entries are ascending; iterate newest-first
    for (int i = (int)entries.size() - 1; i >= 0; --i) {
        const auto &e = entries[i];
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("msgid",     HexStr(Span<const uint8_t>(e.msgId.begin(), e.msgId.end())));
        obj.pushKV("timestamp", e.timestamp);
        obj.pushKV("topic",     topic);
        if (!e.parentMsgId.IsNull())
            obj.pushKV("parent_msgid", HexStr(Span<const uint8_t>(e.parentMsgId.begin(), e.parentMsgId.end())));
        if (e.nRetentionDays > 0)
            obj.pushKV("retention_days", (int)e.nRetentionDays);
        result.push_back(obj);
    }
    return result;
}

// ---- Room RPCs (0.20.3) ----

static UniValue smsgcreateroom(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw std::runtime_error(
            "smsgcreateroom \"name\" ( flags retention_days )\n"
            "\nCreate an SMSG room via a TRANSACTION_SMSG_ROOM special transaction.\n"
            "Generates a room keypair, constructs and broadcasts the room transaction,\n"
            "and auto-subscribes to the room topic.\n"
            "\nArguments:\n"
            "1. \"name\"           (string, required) Room name (stored in OP_RETURN, max 64 chars).\n"
            "2. flags             (numeric, optional, default=1) Room flags: 1=open, 2=moderated, 3=both.\n"
            "3. retention_days    (numeric, optional, default=31) Message retention 1-31 days.\n"
            "\nResult:\n"
            "{\n"
            "  \"txid\": \"...\",\n"
            "  \"room_address\": \"...\",\n"
            "  \"room_pubkey\": \"...\",\n"
            "  \"room_privkey_wif\": \"...\",\n"
            "  \"topic\": \"omega.room.<short_txid>\"\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("smsgcreateroom", "\"General Chat\"")
            + HelpExampleCli("smsgcreateroom", "\"Moderated Room\" 3 14"));

#ifdef ENABLE_WALLET
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) throw JSONRPCError(RPC_WALLET_ERROR, "Wallet not available.");
    CWallet* const pwallet = wallet.get();

    EnsureWalletIsUnlocked(pwallet);

    std::string sName = request.params[0].get_str();
    if (sName.empty() || sName.size() > 64)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Room name must be 1-64 characters.");
    for (unsigned char c : sName) {
        if (c < 0x20 || c == 0x7f || c == '<' || c == '>' || c == '&' || c == '"' || c == '\'' || c == '%')
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Room name contains invalid characters.");
    }

    uint32_t nFlags = SMSG_ROOM_OPEN;
    if (request.params.size() > 1 && !request.params[1].isNull())
        nFlags = (uint32_t)request.params[1].get_int();
    if (nFlags & ~(SMSG_ROOM_OPEN | SMSG_ROOM_MODERATED))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid flags. Use 1=open, 2=moderated, 3=both.");

    uint32_t nRetentionDays = 31;
    if (request.params.size() > 2 && !request.params[2].isNull()) {
        nRetentionDays = (uint32_t)request.params[2].get_int();
        if (nRetentionDays < 1 || nRetentionDays > 31)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "retention_days must be 1-31.");
    }

    // Generate fresh room keypair
    CKey roomKey;
    roomKey.MakeNewKey(true);
    CPubKey roomPubKey = roomKey.GetPubKey();

    // Construct room payload
    CSmsgRoomTx roomTx;
    roomTx.nVersion = CSmsgRoomTx::CURRENT_VERSION;
    roomTx.nFlags = nFlags;
    roomTx.vchRoomPubKey.assign(roomPubKey.begin(), roomPubKey.end());
    roomTx.nRetentionDays = nRetentionDays;
    roomTx.nMaxMembers = 0;

    // Build special transaction
    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_SMSG_ROOM;

    // OP_RETURN output carrying the room name
    CScript opReturnScript = CScript() << OP_RETURN << std::vector<uint8_t>(sName.begin(), sName.end());
    tx.vout.emplace_back(0, opReturnScript);

    // Serialise payload into tx
    SetTxPayload(tx, roomTx);

    // Fund the transaction using wallet
    {
        pwallet->BlockUntilSyncedToCurrentChain();
        LOCK(pwallet->cs_wallet);

        // Build recipient list from outputs
        std::vector<CRecipient> vecSend;
        for (const auto& txOut : tx.vout) {
            CRecipient recipient = {txOut.scriptPubKey, txOut.nValue, false};
            vecSend.push_back(recipient);
        }

        CCoinControl coinControl;
        coinControl.fRequireAllInputs = false;

        CTransactionRef newTx;
        CAmount nFee;
        int nChangePos = -1;
        bilingual_str strFailReason;

        if (!pwallet->CreateTransaction(vecSend, newTx, nFee, nChangePos, strFailReason, coinControl, false, tx.vExtraPayload.size())) {
            throw JSONRPCError(RPC_WALLET_ERROR, strFailReason.original);
        }

        tx.vin = newTx->vin;
        tx.vout = newTx->vout;
    }

    // Validate the funded tx
    {
        LOCK(cs_main);
        CValidationState state;
        if (!CheckSpecialTx(CTransaction(tx), ::ChainActive().Tip(), state, ::ChainstateActive().CoinsTip(), false)) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, FormatStateMessage(state));
        }
    }

    // Sign and broadcast
    CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
    ds << tx;

    JSONRPCRequest signReq(request);
    signReq.params.setArray();
    signReq.params.push_back(HexStr(ds));
    UniValue signResult = signrawtransactionwithwallet(signReq);

    JSONRPCRequest sendReq(request);
    sendReq.params.setArray();
    sendReq.params.push_back(signResult["hex"].get_str());
    std::string txid = sendrawtransaction(sendReq).get_str();

    // Derive room topic from txid
    std::string shortTxid = txid.substr(0, 12);
    std::string topic = "omega.room." + shortTxid;

    // Auto-subscribe to the room topic
    if (smsg::fSecMsgEnabled) {
        std::string sError;
        smsgModule.SubscribeTopic(topic, sError);
    }

    // Import room private key into SMSG keystore so we can decrypt room messages
    if (smsg::fSecMsgEnabled) {
        std::string sLabel = "room:" + sName;
        smsgModule.ImportPrivkey(roomKey, sLabel);
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", txid);
    result.pushKV("room_address", EncodeDestination(PKHash(roomPubKey.GetID())));
    result.pushKV("room_pubkey", HexStr(roomPubKey));
    result.pushKV("room_privkey_wif", EncodeSecret(roomKey));
    result.pushKV("topic", topic);
    result.pushKV("name", sName);
    result.pushKV("flags", (int)nFlags);
    result.pushKV("retention_days", (int)nRetentionDays);
    return result;
#else
    throw JSONRPCError(RPC_WALLET_ERROR, "Wallet support not compiled in.");
#endif
}

static UniValue smsglistrooms(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "smsglistrooms ( flags_filter )\n"
            "\nList all SMSG rooms registered on the blockchain.\n"
            "\nArguments:\n"
            "1. flags_filter  (numeric, optional) Filter by flags bitmask. Omit for all rooms.\n"
            "\nResult:\n"
            "[\n"
            "  { \"txid\": \"...\", \"flags\": n, \"pubkey\": \"...\", \"retention_days\": n, \"height\": n, \"topic\": \"...\" }\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("smsglistrooms", "")
            + HelpExampleCli("smsglistrooms", "1"));

    if (!g_smsgroomindex)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "SMSG room index not available.");

    g_smsgroomindex->BlockUntilSyncedToCurrentChain();

    uint32_t nFlagsFilter = 0;
    bool fFilter = false;
    if (request.params.size() > 0 && !request.params[0].isNull()) {
        nFlagsFilter = (uint32_t)request.params[0].get_int();
        fFilter = true;
    }

    std::vector<std::pair<uint256, SmsgRoomIndexEntry>> rooms;
    if (!g_smsgroomindex->ListRooms(rooms))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to read room index.");

    UniValue result(UniValue::VARR);
    for (const auto& [txid, entry] : rooms) {
        if (fFilter && !(entry.nFlags & nFlagsFilter))
            continue;

        std::string shortTxid = txid.ToString().substr(0, 12);
        std::string topic = "omega.room." + shortTxid;

        UniValue obj(UniValue::VOBJ);
        obj.pushKV("txid", txid.ToString());
        obj.pushKV("version", (int)entry.nVersion);
        obj.pushKV("flags", (int)entry.nFlags);
        obj.pushKV("flags_readable",
            std::string(entry.nFlags & SMSG_ROOM_OPEN ? "open" : "private") +
            std::string(entry.nFlags & SMSG_ROOM_MODERATED ? "+moderated" : ""));
        obj.pushKV("pubkey", HexStr(entry.vchRoomPubKey));
        obj.pushKV("retention_days", (int)entry.nRetentionDays);
        obj.pushKV("max_members", (int)entry.nMaxMembers);
        obj.pushKV("height", entry.nHeight);
        obj.pushKV("topic", topic);
        result.push_back(obj);
    }
    return result;
}

static UniValue smsggetroominfo(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "smsggetroominfo \"room_txid\"\n"
            "\nReturn detailed information about an SMSG room.\n"
            "\nArguments:\n"
            "1. \"room_txid\"  (string, required) The txid of the room creation transaction.\n"
            "\nResult:\n"
            "{\n"
            "  \"txid\": \"...\", \"flags\": n, \"pubkey\": \"...\", \"retention_days\": n,\n"
            "  \"height\": n, \"confirmations\": n, \"topic\": \"...\", \"room_address\": \"...\"\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("smsggetroominfo", "\"abc123...\""));

    if (!g_smsgroomindex)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "SMSG room index not available.");

    g_smsgroomindex->BlockUntilSyncedToCurrentChain();

    uint256 txid = ParseHashV(request.params[0], "room_txid");

    SmsgRoomIndexEntry entry;
    if (!g_smsgroomindex->FindRoom(txid, entry))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Room not found in index.");

    int nConfirmations = 0;
    {
        LOCK(cs_main);
        const int nTip = ::ChainActive().Height();
        if (entry.nHeight > 0 && nTip >= entry.nHeight)
            nConfirmations = nTip - entry.nHeight + 1;
    }

    std::string shortTxid = txid.ToString().substr(0, 12);
    std::string topic = "omega.room." + shortTxid;

    // Derive room address from pubkey
    std::string roomAddress;
    if (entry.vchRoomPubKey.size() == CPubKey::COMPRESSED_SIZE) {
        CPubKey pk(entry.vchRoomPubKey);
        if (pk.IsFullyValid())
            roomAddress = EncodeDestination(PKHash(pk.GetID()));
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", txid.ToString());
    result.pushKV("version", (int)entry.nVersion);
    result.pushKV("flags", (int)entry.nFlags);
    result.pushKV("flags_readable",
        std::string(entry.nFlags & SMSG_ROOM_OPEN ? "open" : "private") +
        std::string(entry.nFlags & SMSG_ROOM_MODERATED ? "+moderated" : ""));
    result.pushKV("pubkey", HexStr(entry.vchRoomPubKey));
    result.pushKV("room_address", roomAddress);
    result.pushKV("retention_days", (int)entry.nRetentionDays);
    result.pushKV("max_members", (int)entry.nMaxMembers);
    result.pushKV("height", entry.nHeight);
    result.pushKV("confirmations", nConfirmations);
    result.pushKV("topic", topic);
    return result;
}

static UniValue smsgjoinroom(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "smsgjoinroom \"room_txid\" ( \"privkey_wif\" )\n"
            "\nJoin an SMSG room: subscribe to the room topic and register the room key.\n"
            "For open rooms, only the txid is needed.\n"
            "For private rooms, provide the room private key (WIF) received via invite.\n"
            "\nArguments:\n"
            "1. \"room_txid\"     (string, required) The txid of the room creation transaction.\n"
            "2. \"privkey_wif\"   (string, optional) Room private key WIF (for private rooms).\n"
            "\nResult:\n"
            "{ \"result\": \"joined\", \"topic\": \"...\" }\n"
            "\nExamples:\n"
            + HelpExampleCli("smsgjoinroom", "\"abc123...\"")
            + HelpExampleCli("smsgjoinroom", "\"abc123...\" \"5Jxyz...\""));

    EnsureSMSGIsEnabled();

    if (!g_smsgroomindex)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "SMSG room index not available.");

    g_smsgroomindex->BlockUntilSyncedToCurrentChain();

    uint256 txid = ParseHashV(request.params[0], "room_txid");

    SmsgRoomIndexEntry entry;
    if (!g_smsgroomindex->FindRoom(txid, entry))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Room not found in index.");

    std::string shortTxid = txid.ToString().substr(0, 12);
    std::string topic = "omega.room." + shortTxid;

    // Subscribe to room topic
    std::string sError;
    if (!smsgModule.SubscribeTopic(topic, sError))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to subscribe to topic: " + sError);

    // For private rooms with a WIF key, import it
    if (request.params.size() > 1 && !request.params[1].isNull()) {
        std::string sWif = request.params[1].get_str();
        CKey key = DecodeSecret(sWif);
        if (!key.IsValid())
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid private key WIF.");

        std::string sLabel = "room:" + shortTxid;
        if (smsgModule.ImportPrivkey(key, sLabel) != 0)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to import room key.");
    } else {
        // Open room — register the room public key so we can route messages
        if (entry.vchRoomPubKey.size() == CPubKey::COMPRESSED_SIZE) {
            CPubKey pk(entry.vchRoomPubKey);
            if (pk.IsFullyValid()) {
                std::string sAddr = EncodeDestination(PKHash(pk.GetID()));
                std::string sPubHex = HexStr(pk);
                smsgModule.AddAddress(sAddr, sPubHex);
            }
        }
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("result", "joined");
    result.pushKV("txid", txid.ToString());
    result.pushKV("topic", topic);
    return result;
}

// --- Message Anchoring -------------------------------------------

static void EnsureAnchorReady()
{
    if (!smsg::g_msgAnchor.IsLoaded()) {
        throw JSONRPCError(RPC_DATABASE_ERROR,
            "Anchor state is not initialized. Enable secure messaging first.");
    }
}

static const char *QueueHashStatusString(const smsg::QueueHashResult result)
{
    switch (result) {
    case smsg::QueueHashResult::QUEUED:
        return "queued";
    case smsg::QueueHashResult::PENDING:
        return "pending";
    case smsg::QueueHashResult::CONFIRMED:
        return "confirmed";
    case smsg::QueueHashResult::FULL:
        return "full";
    case smsg::QueueHashResult::QUEUE_ERROR:
        return "error";
    }
    return "error";
}

static smsg::AnchorHashStatus RefreshAnchorStatus(const uint256 &hash,
                                                  smsg::AnchorBatchInfo *batch_info_out = nullptr)
{
    smsg::AnchorBatchInfo batch_info;
    const smsg::AnchorHashStatus status = smsg::g_msgAnchor.GetHashStatus(hash, &batch_info);
    if (status == smsg::AnchorHashStatus::NOT_FOUND || status == smsg::AnchorHashStatus::PENDING) {
        return status;
    }
    if (status == smsg::AnchorHashStatus::CONFIRMED) {
        if (batch_info_out) {
            *batch_info_out = batch_info;
        }
        return status;
    }

    const smsg::AnchorTxLookupResult lookup = smsg::LookupAnchorTransaction(
        batch_info.txid, batch_info.merkle_root, batch_info.nAnchorTime,
        /* mempool */ nullptr);
    if (!lookup.confirmed) {
        if (batch_info_out) {
            *batch_info_out = batch_info;
        }
        return status;
    }

    std::string error;
    const int64_t confirmed_time = lookup.confirmed_time != 0
        ? lookup.confirmed_time
        : static_cast<int64_t>(time(nullptr));
    if (!smsg::g_msgAnchor.MarkBatchConfirmed(batch_info.txid, confirmed_time, &error)) {
        throw JSONRPCError(RPC_DATABASE_ERROR,
            error.empty() ? "Failed to persist confirmed anchor batch." : error);
    }
    batch_info.nCommitted = confirmed_time;
    if (batch_info_out) {
        *batch_info_out = batch_info;
    }
    return smsg::AnchorHashStatus::CONFIRMED;
}

static UniValue anchormsg(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "anchormsg \"msghash\" ( \"prevhash\" )\n"
            "\nQueue a message hash for batch anchoring.\n"
            "\nArguments:\n"
            "1. \"msghash\"   (string, required) SHA-256 hex of the message (64 chars).\n"
            "2. \"prevhash\"  (string, optional) SHA-256 hex of the previous revision.\n"
            "\nResult:\n"
            "{\n"
            "  \"queued\": true|false,\n"
            "  \"status\": \"queued|pending|confirmed|full\",\n"
            "  \"pending\": n\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("anchormsg", "\"<sha256hex>\"")
            + HelpExampleCli("anchormsg", "\"<sha256hex>\" \"<prevhex>\""));

    std::string sHash = request.params[0].get_str();
    if (sHash.size() != 64 || !IsHex(sHash))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "msghash must be 64 hex chars.");

    uint256 hash = uint256S(sHash);
    uint256 prev;
    if (request.params.size() > 1 && !request.params[1].isNull()) {
        std::string sPrev = request.params[1].get_str();
        if (sPrev.size() != 64 || !IsHex(sPrev))
            throw JSONRPCError(RPC_INVALID_PARAMETER, "prevhash must be 64 hex chars.");
        prev = uint256S(sPrev);
    }

    EnsureAnchorReady();
    std::string error;
    const smsg::QueueHashResult queue_result = smsg::g_msgAnchor.QueueHash(hash, prev, &error);
    if (queue_result == smsg::QueueHashResult::QUEUE_ERROR) {
        throw JSONRPCError(RPC_INVALID_REQUEST,
            error.empty() ? "Failed to queue anchor hash." : error);
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("queued",  queue_result == smsg::QueueHashResult::QUEUED);
    result.pushKV("status",  QueueHashStatusString(queue_result));
    result.pushKV("pending", (uint64_t)smsg::g_msgAnchor.PendingCount());
    return result;
}

static UniValue verifymsg(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "verifymsg \"msghash\"\n"
            "\nCheck whether a message hash has been anchored on-chain.\n"
            "\nArguments:\n"
            "1. \"msghash\"  (string, required) SHA-256 hex of the message.\n"
            "\nResult:\n"
            "{\n"
            "  \"anchored\": true|false,\n"
            "  \"pending\": true|false,\n"
            "  \"txid\": \"...\",\n"
            "  \"root\": \"...\"\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("verifymsg", "\"<sha256hex>\""));

    std::string sHash = request.params[0].get_str();
    if (sHash.size() != 64 || !IsHex(sHash))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "msghash must be 64 hex chars.");

    uint256 hash = uint256S(sHash);
    EnsureAnchorReady();

    smsg::AnchorBatchInfo batch_info;
    const smsg::AnchorHashStatus status = RefreshAnchorStatus(hash, &batch_info);
    const bool anchored = status == smsg::AnchorHashStatus::CONFIRMED;
    const bool pending = status == smsg::AnchorHashStatus::PENDING
        || status == smsg::AnchorHashStatus::SUBMITTED;
    const bool has_batch = status == smsg::AnchorHashStatus::SUBMITTED
        || status == smsg::AnchorHashStatus::CONFIRMED;

    UniValue result(UniValue::VOBJ);
    result.pushKV("anchored", anchored);
    result.pushKV("pending",  pending);
    result.pushKV("txid",     has_batch ? batch_info.txid : "");
    result.pushKV("root",     has_batch ? batch_info.merkle_root.GetHex() : "");
    return result;
}

static UniValue getmsgproof(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "getmsgproof \"msghash\"\n"
            "\nReturn the Merkle branch proving inclusion of a message hash in an anchored batch.\n"
            "\nArguments:\n"
            "1. \"msghash\"  (string, required) SHA-256 hex of the message.\n"
            "\nResult:\n"
            "{\n"
            "  \"hash\": \"...\",\n"
            "  \"pending\": true|false,\n"
            "  \"index\": n,\n"
            "  \"branch\": [\"hex\", ...]\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getmsgproof", "\"<sha256hex>\""));

    std::string sHash = request.params[0].get_str();
    if (sHash.size() != 64 || !IsHex(sHash))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "msghash must be 64 hex chars.");

    uint256 hash = uint256S(sHash);
    EnsureAnchorReady();

    smsg::AnchorBatchInfo batch_info;
    const smsg::AnchorHashStatus status = RefreshAnchorStatus(hash, &batch_info);
    if (status == smsg::AnchorHashStatus::NOT_FOUND) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Hash not found in anchor state.");
    }
    if (status != smsg::AnchorHashStatus::CONFIRMED) {
        UniValue pending_result(UniValue::VOBJ);
        pending_result.pushKV("hash", hash.GetHex());
        pending_result.pushKV("anchored", false);
        pending_result.pushKV("pending", true);
        pending_result.pushKV("txid",
                              status == smsg::AnchorHashStatus::SUBMITTED ? batch_info.txid : "");
        pending_result.pushKV("root",
                              status == smsg::AnchorHashStatus::SUBMITTED
                                  ? batch_info.merkle_root.GetHex()
                                  : "");
        return pending_result;
    }

    std::vector<uint256> branch;
    uint32_t index = 0;
    if (!smsg::g_msgAnchor.GetProof(hash, branch, index))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Confirmed anchor batch found but proof could not be constructed.");

    UniValue jBranch(UniValue::VARR);
    for (const auto &h : branch) jBranch.push_back(h.GetHex());

    UniValue result(UniValue::VOBJ);
    result.pushKV("hash",   hash.GetHex());
    result.pushKV("anchored", true);
    result.pushKV("pending", false);
    result.pushKV("txid",   batch_info.txid);
    result.pushKV("root",   batch_info.merkle_root.GetHex());
    result.pushKV("index",  (uint64_t)index);
    result.pushKV("branch", jBranch);
    return result;
}

static UniValue anchorcommit(const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "anchorcommit\n"
            "\nCommit all queued message hashes to the blockchain via OP_RETURN.\n"
            "Requires a funded wallet.\n"
            "\nResult:\n"
            "{\n"
            "  \"txid\": \"...\",\n"
            "  \"root\": \"...\",\n"
            "  \"count\": n\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("anchorcommit", ""));

#ifdef ENABLE_WALLET
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) throw JSONRPCError(RPC_WALLET_ERROR, "Wallet not available.");
    CWallet* const pwallet = wallet.get();
    EnsureWalletIsUnlocked(pwallet);
    NodeContext& node = EnsureNodeContext(request.context);

    EnsureAnchorReady();

    smsg::AnchorCommitResult commit_result;
    std::string error;
    if (!smsg::CommitPendingAnchorBatch(pwallet, node, /* wait_callback */ true,
                                        commit_result, &error)) {
        switch (commit_result.error) {
        case smsg::AnchorCommitError::PREPARE:
            throw JSONRPCError(RPC_INVALID_REQUEST,
                error.empty() ? "Failed to prepare anchor batch." : error);
        case smsg::AnchorCommitError::WALLET_UNAVAILABLE:
        case smsg::AnchorCommitError::CREATE_TRANSACTION:
            throw JSONRPCError(RPC_WALLET_ERROR,
                error.empty() ? "Failed to create anchor transaction." : error);
        case smsg::AnchorCommitError::BROADCAST:
            if (commit_result.tx_error != TransactionError::OK) {
                throw JSONRPCTransactionError(commit_result.tx_error, error);
            }
            throw JSONRPCError(RPC_TRANSACTION_ERROR,
                error.empty() ? "Failed to broadcast anchor transaction." : error);
        case smsg::AnchorCommitError::NOT_INITIALIZED:
        case smsg::AnchorCommitError::DATABASE:
            throw JSONRPCError(RPC_DATABASE_ERROR,
                error.empty() ? "Failed to persist anchor state." : error);
        case smsg::AnchorCommitError::NONE:
            break;
        }
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid",  commit_result.txid);
    result.pushKV("root",  commit_result.merkle_root.GetHex());
    result.pushKV("count", (uint64_t)commit_result.count);
    return result;
#else
    throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Wallet support required.");
#endif
}

// ---------------------------------------------------------------------------

static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         argNames
  //  --------------------- ------------------------  -----------------------  ----------
    { "smsg",               "smsgenable",             &smsgenable,             {} },
    { "smsg",               "smsgdisable",            &smsgdisable,            {} },
    { "smsg",               "smsgoptions",            &smsgoptions,            {} },
    { "smsg",               "smsglocalkeys",          &smsglocalkeys,          {} },
    { "smsg",               "smsgscanchain",          &smsgscanchain,          {} },
    { "smsg",               "smsgscanbuckets",        &smsgscanbuckets,        {} },
    { "smsg",               "smsgaddaddress",         &smsgaddaddress,         {"address","pubkey"} },
    { "smsg",               "smsgaddlocaladdress",    &smsgaddlocaladdress,    {"address"} },
    { "smsg",               "smsgimportprivkey",      &smsgimportprivkey,      {"privkey","label"} },
    { "smsg",               "smsggetpubkey",          &smsggetpubkey,          {"address"} },
    { "smsg",               "smsgsend",               &smsgsend,               {"address_from","address_to","message","paid_msg","days_retention","testfee","fromfile","decodehex","topic","parent_msgid","retention_days"} },
    { "smsg",               "smsgsendanon",           &smsgsendanon,           {"address_to","message"} },
    { "smsg",               "smsginbox",              &smsginbox,              {"mode","filter"} },
    { "smsg",               "smsgoutbox",             &smsgoutbox,             {"mode","filter"} },
    { "smsg",               "smsgbuckets",            &smsgbuckets,            {"mode"} },
    { "smsg",               "smsgclearbuckets",       &smsgclearbuckets,       {"confirm"} },
    { "smsg",               "smsgview",               &smsgview,               {}},
    { "smsg",               "smsg",                   &smsgone,                {"msgid","options"} },
    { "smsg",               "smsgpurge",              &smsgpurge,              {"msgid"} },
    // Phase 1 — Mobile Dashboard & Connection Health
    { "smsg",               "smsggetinfo",            &smsggetinfo,            {} },
    { "smsg",               "smsgpeers",              &smsgpeers,              {"node_id"} },
    { "smsg",               "smsgaddresses",          &smsgaddresses,          {} },
    { "smsg",               "smsgzmqpush",            &smsgzmqpush,            {"timefrom","timeto"} },
    { "smsg",               "smsggetfeerate",         &smsggetfeerate,         {} },
    { "smsg",               "smsggetdifficulty",      &smsggetdifficulty,      {} },
    // Phase 2 — Full CRUD & Backup/Restore
    { "smsg",               "smsgremoveaddress",      &smsgremoveaddress,      {"address"} },
    { "smsg",               "smsgremoveprivkey",      &smsgremoveprivkey,      {"address"} },
    { "smsg",               "smsgdumpprivkey",        &smsgdumpprivkey,        {"address"} },
    { "smsg",               "smsgfund",               &smsgfund,              {"msgid","testfee"} },
    { "smsg",               "smsgimport",             &smsgimport,             {"hex"} },
    { "smsg",               "smsgsetwallet",          &smsgsetwallet,          {"walletname"} },
    // Phase 3 — Debugging & Final Parity
    { "smsg",               "smsgdebug",              &smsgdebug,              {"command","arg1"} },
    // Trollbox — public chat
    { "smsg",               "trollboxsend",           &trollboxsend,           {"message","paid"} },
    { "smsg",               "trollboxlist",           &trollboxlist,            {"count"} },
    // Topic channels
    { "smsg",               "smsgsubscribe",          &smsgsubscribe,          {"topic"} },
    { "smsg",               "smsgunsubscribe",        &smsgunsubscribe,        {"topic"} },
    { "smsg",               "smsglisttopics",         &smsglisttopics,         {} },
    { "smsg",               "smsggetmessages",        &smsggetmessages,        {"topic","count"} },
    // Room management (0.20.3)
    { "smsg",               "smsgcreateroom",         &smsgcreateroom,         {"name","flags","retention_days"} },
    { "smsg",               "smsglistrooms",          &smsglistrooms,          {"flags_filter"} },
    { "smsg",               "smsggetroominfo",        &smsggetroominfo,        {"room_txid"} },
    { "smsg",               "smsgjoinroom",           &smsgjoinroom,           {"room_txid","privkey_wif"} },
    // Message anchoring (Phase 1 — Documentchain integration)
    { "smsg",               "anchormsg",              &anchormsg,              {"msghash","prevhash"} },
    { "smsg",               "verifymsg",              &verifymsg,              {"msghash"} },
    { "smsg",               "getmsgproof",            &getmsgproof,            {"msghash"} },
    { "smsg",               "anchorcommit",           &anchorcommit,           {} },
};

void RegisterSmsgRPCCommands(CRPCTable &t)
{
    for (const auto& command : commands) {
        t.appendCommand(command.name, &command);
    }
}
