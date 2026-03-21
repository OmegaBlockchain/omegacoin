// Copyright (c) 2017-2024 The Particl Core developers
// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef OMEGA_SMSG_KEYSTORE_H
#define OMEGA_SMSG_KEYSTORE_H

#include <key.h>
#include <pubkey.h>
#include <sync.h>

namespace smsg {

enum SMSGKeyFlagTypes
{
    SMK_RECEIVE_ON       = (1 << 1),
    SMK_RECEIVE_ANON     = (1 << 2),
    SMK_CONTACT_ONLY     = (1 << 3), // pubkey-only contact, no private key
};

class SecMsgKey
{
public:
    int64_t nCreateTime = 0; // 0 means unknown
    uint32_t nFlags = 0; // SMSGKeyFlagTypes
    std::string sLabel;
    CKey key;
    CPubKey pubkey;
    std::string hdKeypath; //optional HD/bip32 keypath
    CKeyID hdMasterKeyID; //id of the HD masterkey used to derive this key

    SERIALIZE_METHODS(SecMsgKey, obj)
    {
        READWRITE(obj.nCreateTime);
        READWRITE(obj.nFlags);
        READWRITE(obj.sLabel);
        READWRITE(obj.key);
        READWRITE(obj.hdKeypath);
        READWRITE(obj.hdMasterKeyID);
    }
};

class SecMsgKeyStore
{
protected:
    mutable CCriticalSection cs_KeyStore;

public:
    std::map<CKeyID, SecMsgKey> mapKeys;

    bool AddKey(const CKeyID &idk, SecMsgKey &key);
    bool HaveKey(const CKeyID &idk) const;
    bool EraseKey(const CKeyID &idk);
    bool GetPubKey(const CKeyID &idk, CPubKey &pk);

    bool Clear();
};

} // namespace smsg

#endif //OMEGA_SMSG_KEYSTORE_H
