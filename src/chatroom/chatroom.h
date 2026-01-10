// Copyright (c) 2018-2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_CHATROOM_H
#define DASH_CHATROOM_H

#include <key.h>
#include <pubkey.h>
#include <serialize.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <chatroom/chatroom_limits.h>  // Import all constants from here

#include <set>
#include <string>

// Room state thresholds and intervals are now in chatroom_limits.h
// Removed duplicate definitions to prevent redefinition errors

enum ChatRoomState : uint8_t {
    STATE_ACTIVE = 0,
    STATE_INACTIVE = 1,
    STATE_EXPIRED = 2
};

class CChatRoom
{
private:
    int32_t nVersion{1};
    uint256 roomId;
    std::string roomName;
    CPubKey creatorPubKey;
    int64_t nTimeCreated{0};
    int64_t nLastActivity{0};

    std::set<CPubKey> setMutedKeys;
    std::vector<uint8_t> vchMuteListSig;

    mutable uint256 cachedHash;
    mutable bool hashCached{false};

public:
    CChatRoom();

    SERIALIZE_METHODS(CChatRoom, obj)
    {
        READWRITE(obj.nVersion);
        READWRITE(obj.roomId);
        READWRITE(obj.roomName);
        READWRITE(obj.creatorPubKey);
        READWRITE(obj.nTimeCreated);
        READWRITE(obj.nLastActivity);
        READWRITE(obj.setMutedKeys);
        READWRITE(obj.vchMuteListSig);
    }

    void SetRoomName(const std::string& name);
    void SetCreator(const CPubKey& pubkey);
    void SetTimeCreated(int64_t time) { nTimeCreated = time; hashCached = false; }
    void GenerateRoomId();

    uint256 GetRoomId() const { return roomId; }
    std::string GetRoomName() const { return roomName; }
    CPubKey GetCreatorPubKey() const { return creatorPubKey; }
    int64_t GetTimeCreated() const { return nTimeCreated; }
    int64_t GetLastActivity() const { return nLastActivity; }
    uint256 GetHash() const;

    bool IsValid() const;
    bool IsCreator(const CPubKey& key) const;
    bool IsMuted(const CPubKey& key) const;
    size_t GetMutedCount() const { return setMutedKeys.size(); }
    std::set<CPubKey> GetMutedKeys() const { return setMutedKeys; }

    void UpdateActivity(int64_t timestamp);
    int64_t GetTimeSinceActivity() const;
    ChatRoomState GetState() const;
    bool IsExpired() const;
    bool ShouldAnnounce() const;

    bool AddMute(const CPubKey& target, const CKey& creatorKey);
    bool RemoveMute(const CPubKey& target, const CKey& creatorKey);
    bool VerifyMuteListSignature() const;
    uint256 GetMuteListHashOnly() const { return GetMuteListHash(); }

private:
    uint256 GetMuteListHash() const;
    void SignMuteList(const CKey& key);
};

#endif // DASH_CHATROOM_H