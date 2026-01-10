// Copyright (c) 2018-2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_CHATROOM_MESSAGE_H
#define DASH_CHATROOM_MESSAGE_H

#include <key.h>
#include <pubkey.h>
#include <serialize.h>
#include <uint256.h>
#include <random.h>
#include <chatroom/chatroom_limits.h>  // Import all constants from here
#include <vector>
#include <limits>

// All size and timing constants moved to chatroom_limits.h
// to prevent redefinition errors

enum MessagePhase : uint8_t {
    PHASE_STEM = 0,
    PHASE_FLUFF = 1
};

template<> struct is_serializable_enum<MessagePhase> : std::true_type {};

class CChatMessage
{
private:
    int32_t nVersion{1};
    uint256 roomId;
    CPubKey senderPubKey;
    std::vector<uint8_t> vchPayload;
    int64_t nTimestamp{0};
    uint64_t nNonce{0};
    uint8_t nStemHopsRemaining{0};
    MessagePhase phase{PHASE_STEM};
    std::vector<uint8_t> vchSig;

    mutable uint256 cachedHash;
    mutable bool hashCached{false};

public:
    CChatMessage();

    SERIALIZE_METHODS(CChatMessage, obj)
    {
        READWRITE(obj.nVersion);
        READWRITE(obj.roomId);
        READWRITE(obj.senderPubKey);
        READWRITE(obj.vchPayload);
        READWRITE(obj.nTimestamp);
        READWRITE(obj.nNonce);
        READWRITE(obj.nStemHopsRemaining);
        READWRITE(obj.phase);
        READWRITE(obj.vchSig);
    }

    void SetRoomId(const uint256& id) { roomId = id; hashCached = false; }
    void SetSenderPubKey(const CPubKey& key);
    void SetPayload(const std::vector<uint8_t>& payload);
    void SetPayload(const std::string& text);
    void SetTimestamp(int64_t time) { nTimestamp = time; hashCached = false; }
    void SetNonce(uint64_t nonce) { nNonce = nonce; hashCached = false; }
    void GenerateNonce() { nNonce = GetRand(std::numeric_limits<uint64_t>::max()); hashCached = false; }
    bool Sign(const CKey& key);

    uint256 GetRoomId() const { return roomId; }
    CPubKey GetSenderPubKey() const { return senderPubKey; }
    const std::vector<uint8_t>& GetPayload() const { return vchPayload; }
    std::string GetPayloadString() const;
    int64_t GetTimestamp() const { return nTimestamp; }
    uint64_t GetNonce() const { return nNonce; }
    uint8_t GetStemHops() const { return nStemHopsRemaining; }
    MessagePhase GetPhase() const { return phase; }
    uint256 GetHash() const;

    bool IsValid() const;
    bool VerifySignature() const;
    bool CheckTimestamp() const;
    bool CheckSize() const;

    void InitiateStem(uint8_t hops);
    bool DecrementStemHop();
    void SwitchToFluff();
    bool ShouldFluff() const;

private:
    uint256 GetSignatureHash() const;
};

#endif // DASH_CHATROOM_MESSAGE_H