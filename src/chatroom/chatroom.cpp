// Copyright (c) 2018-2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chatroom/chatroom.h>

#include <hash.h>
#include <util/time.h>

CChatRoom::CChatRoom()
    : nVersion(1),
      roomId(),
      roomName(),
      creatorPubKey(),
      nTimeCreated(0),
      nLastActivity(0),
      hashCached(false)
{
}

void CChatRoom::SetRoomName(const std::string& name)
{
    if (name.empty() || name.length() > MAX_ROOM_NAME_LENGTH) {
        throw std::invalid_argument("Invalid room name length");
    }
    roomName = name;
    hashCached = false;
}

void CChatRoom::SetCreator(const CPubKey& pubkey)
{
    if (!pubkey.IsValid()) {
        throw std::invalid_argument("Invalid creator public key");
    }
    creatorPubKey = pubkey;
    hashCached = false;
}

void CChatRoom::GenerateRoomId()
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << roomName;
    ss << creatorPubKey;
    ss << nTimeCreated;
    roomId = ss.GetHash();
    hashCached = false;
}

uint256 CChatRoom::GetHash() const
{
    if (hashCached) {
        return cachedHash;
    }

    CHashWriter ss(SER_GETHASH, 0);
    ss << nVersion;
    ss << roomId;
    ss << roomName;
    ss << creatorPubKey;
    ss << nTimeCreated;
    ss << nLastActivity;

    cachedHash = ss.GetHash();
    hashCached = true;
    return cachedHash;
}

bool CChatRoom::IsValid() const
{
    if (roomName.empty() || roomName.length() > MAX_ROOM_NAME_LENGTH) {
        return false;
    }

    if (!creatorPubKey.IsValid()) {
        return false;
    }

    if (nTimeCreated <= 0) {
        return false;
    }

    if (roomId.IsNull()) {
        return false;
    }

    if (!VerifyMuteListSignature()) {
        return false;
    }

    return true;
}

bool CChatRoom::IsCreator(const CPubKey& key) const
{
    return creatorPubKey == key;
}

bool CChatRoom::IsMuted(const CPubKey& key) const
{
    return setMutedKeys.count(key) > 0;
}

void CChatRoom::UpdateActivity(int64_t timestamp)
{
    int64_t now = GetTime();
    if (timestamp > now) {
        timestamp = now;
    }
    nLastActivity = std::max(nLastActivity, timestamp);
    hashCached = false;
}

int64_t CChatRoom::GetTimeSinceActivity() const
{
    int64_t now = GetTime();

    if (nLastActivity == 0) {
        return now - nTimeCreated;
    }

    return now - nLastActivity;
}

ChatRoomState CChatRoom::GetState() const
{
    int64_t timeSince = GetTimeSinceActivity();

    if (timeSince > ROOM_INACTIVE_THRESHOLD) {
        return STATE_EXPIRED;
    }
    if (timeSince > ROOM_ACTIVE_THRESHOLD) {
        return STATE_INACTIVE;
    }
    return STATE_ACTIVE;
}

bool CChatRoom::IsExpired() const
{
    return GetState() == STATE_EXPIRED;
}

bool CChatRoom::ShouldAnnounce() const
{
    return GetState() == STATE_ACTIVE;
}

uint256 CChatRoom::GetMuteListHash() const
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << roomId;
    ss << setMutedKeys;
    return ss.GetHash();
}

void CChatRoom::SignMuteList(const CKey& key)
{
    if (!key.IsValid()) {
        throw std::invalid_argument("Invalid signing key");
    }

    if (key.GetPubKey() != creatorPubKey) {
        throw std::invalid_argument("Only creator can sign mute list");
    }

    uint256 hash = GetMuteListHash();

    if (!key.SignCompact(hash, vchMuteListSig)) {
        throw std::runtime_error("Failed to sign mute list");
    }
}

bool CChatRoom::AddMute(const CPubKey& target, const CKey& creatorKey)
{
    if (!IsCreator(creatorKey.GetPubKey())) {
        return false;
    }

    if (!target.IsValid()) {
        return false;
    }

    auto result = setMutedKeys.insert(target);
    if (!result.second) {
        return false;
    }

    hashCached = false;

    try {
        SignMuteList(creatorKey);
    } catch (const std::exception& e) {
        setMutedKeys.erase(target);
        return false;
    }

    return true;
}

bool CChatRoom::RemoveMute(const CPubKey& target, const CKey& creatorKey)
{
    if (!IsCreator(creatorKey.GetPubKey())) {
        return false;
    }

    auto it = setMutedKeys.find(target);
    if (it == setMutedKeys.end()) {
        return false;
    }

    CPubKey backup = *it;
    setMutedKeys.erase(it);
    hashCached = false;

    try {
        SignMuteList(creatorKey);
    } catch (const std::exception& e) {
        setMutedKeys.insert(backup);
        return false;
    }

    return true;
}

bool CChatRoom::VerifyMuteListSignature() const
{
    if (vchMuteListSig.empty()) {
        return setMutedKeys.empty();
    }

    uint256 hash = GetMuteListHash();
    CPubKey recoveredKey;

    if (!recoveredKey.RecoverCompact(hash, vchMuteListSig)) {
        return false;
    }

    return recoveredKey == creatorPubKey;
}
