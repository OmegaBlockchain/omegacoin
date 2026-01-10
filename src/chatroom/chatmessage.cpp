// Copyright (c) 2018-2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chatroom/chatmessage.h>

#include <hash.h>
#include <random.h>
#include <util/time.h>

CChatMessage::CChatMessage()
    : nVersion(1),
      roomId(),
      senderPubKey(),
      nTimestamp(0),
      nNonce(0),
      nStemHopsRemaining(0),
      phase(PHASE_STEM),
      hashCached(false)
{
}

void CChatMessage::SetSenderPubKey(const CPubKey& key)
{
    if (!key.IsValid()) {
        throw std::invalid_argument("Invalid sender public key");
    }
    senderPubKey = key;
    hashCached = false;
}

void CChatMessage::SetPayload(const std::vector<uint8_t>& payload)
{
    if (payload.size() > MAX_MESSAGE_SIZE) {
        throw std::invalid_argument("Message payload too large");
    }
    vchPayload = payload;
    hashCached = false;
}

void CChatMessage::SetPayload(const std::string& text)
{
    std::vector<uint8_t> payload(text.begin(), text.end());
    SetPayload(payload);
}

std::string CChatMessage::GetPayloadString() const
{
    return std::string(vchPayload.begin(), vchPayload.end());
}

uint256 CChatMessage::GetSignatureHash() const
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << nVersion;
    ss << roomId;
    ss << senderPubKey;
    ss << vchPayload;
    ss << nTimestamp;
    ss << nNonce;
    return ss.GetHash();
}

bool CChatMessage::Sign(const CKey& key)
{
    if (!key.IsValid() || key.GetPubKey() != senderPubKey) {
        return false;
    }

    uint256 hash = GetSignatureHash();
    if (!key.SignCompact(hash, vchSig)) {
        return false;
    }

    hashCached = false;
    return true;
}

bool CChatMessage::VerifySignature() const
{
    if (vchSig.empty()) return false;

    uint256 hash = GetSignatureHash();
    CPubKey recoveredKey;
    return recoveredKey.RecoverCompact(hash, vchSig) && recoveredKey == senderPubKey;
}

uint256 CChatMessage::GetHash() const
{
    if (hashCached) return cachedHash;

    CHashWriter ss(SER_GETHASH, 0);
    ss << nVersion;
    ss << roomId;
    ss << senderPubKey;
    ss << vchPayload;
    ss << nTimestamp;
    ss << nNonce;
    ss << nStemHopsRemaining;
    ss << phase;
    ss << vchSig;

    cachedHash = ss.GetHash();
    hashCached = true;
    return cachedHash;
}

bool CChatMessage::IsValid() const
{
    return !roomId.IsNull() && senderPubKey.IsValid() &&
           CheckSize() && CheckTimestamp() && VerifySignature();
}

bool CChatMessage::CheckTimestamp() const
{
    int64_t now = GetTime();
    return nTimestamp <= now + MESSAGE_MAX_AGE && nTimestamp >= now - MESSAGE_MAX_AGE;
}

bool CChatMessage::CheckSize() const
{
    return vchPayload.size() <= MAX_MESSAGE_SIZE;
}

void CChatMessage::InitiateStem(uint8_t hops)
{
    if (hops < MIN_STEM_HOPS) hops = MIN_STEM_HOPS;
    if (hops > MAX_STEM_HOPS) hops = MAX_STEM_HOPS;

    nStemHopsRemaining = hops;
    phase = PHASE_STEM;
    hashCached = false;
}

bool CChatMessage::DecrementStemHop()
{
    if (nStemHopsRemaining == 0) return false;

    nStemHopsRemaining--;
    hashCached = false;

    if (nStemHopsRemaining == 0) SwitchToFluff();
    return true;
}

void CChatMessage::SwitchToFluff()
{
    phase = PHASE_FLUFF;
    nStemHopsRemaining = 0;
    hashCached = false;
}

bool CChatMessage::ShouldFluff() const
{
    return phase == PHASE_FLUFF || nStemHopsRemaining == 0;
}
