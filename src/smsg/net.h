// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef OMEGA_SMSG_NET_H
#define OMEGA_SMSG_NET_H

#include <sync.h>

#include <map>

struct SecMsgBucketInfo
{
    uint32_t m_active = 0;
    uint32_t m_hash = 0;
};

class SecMsgNode
{
public:
    CCriticalSection cs_smsg_net;
    int64_t lastSeen = 0;
    int64_t lastMatched = 0;
    int64_t ignoreUntil = 0;
    uint32_t nWakeCounter = 0;
    bool fEnabled = false;
    uint32_t m_version = 0;
    uint32_t misbehaving = 0;
    uint32_t m_num_want_sent = 0;
    uint32_t m_receive_counter = 0;
    uint32_t m_ignored_counter = 0;
    std::map<int64_t, SecMsgBucketInfo> m_buckets;
    std::map<int64_t, int64_t> m_buckets_last_shown;
};

#endif // OMEGA_SMSG_NET_H
