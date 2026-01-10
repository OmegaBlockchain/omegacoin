// Copyright (c) 2025 Dash Core Group
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_CHATROOM_LIMITS_H
#define DASH_CHATROOM_LIMITS_H

#include <cstddef>
#include <cstdint>

// ============================================================================
// MESSAGE SIZE LIMITS
// ============================================================================

// Maximum message payload size (4KB)
// Used for validation at all layers: RPC, network, manager
static constexpr size_t MAX_CHATROOM_MSG_SIZE = 4096;

// Maximum number of messages per query
static constexpr size_t MAX_CHATROOM_MESSAGES = 1000;

// Maximum database entry size
static constexpr size_t MAX_CHATROOM_DB_ENTRY = 4096;

// Maximum message size (from chatmessage.h - same as MAX_CHATROOM_MSG_SIZE)
// Kept for backward compatibility, but prefer MAX_CHATROOM_MSG_SIZE
static constexpr size_t MAX_MESSAGE_SIZE = MAX_CHATROOM_MSG_SIZE;

// ============================================================================
// ROOM LIMITS
// ============================================================================

// Maximum room name length in characters
static constexpr size_t MAX_ROOM_NAME_LENGTH = 64;

// ============================================================================
// TIME LIMITS (from chatmessage.h)
// ============================================================================

// Timestamp tolerance - messages valid within this window (2 hours)
static constexpr int64_t MESSAGE_MAX_AGE = 7200;

// ============================================================================
// ROOM ACTIVITY THRESHOLDS (from chatroom.h)
// ============================================================================

// Room considered active if activity within 7 days
static constexpr int64_t ROOM_ACTIVE_THRESHOLD = 7 * 86400;

// Room considered inactive (eligible for cleanup) after 14 days
static constexpr int64_t ROOM_INACTIVE_THRESHOLD = 14 * 86400;

// How often to announce active rooms (1 hour)
static constexpr int64_t ROOM_ANNOUNCEMENT_INTERVAL = 3600;

// ============================================================================
// DANDELION++ PRIVACY PARAMETERS (from chatmessage.h)
// ============================================================================

// Minimum stem hops for privacy
static constexpr uint8_t MIN_STEM_HOPS = 3;

// Maximum stem hops
static constexpr uint8_t MAX_STEM_HOPS = 10;

// Default stem hops (good balance of privacy and latency)
static constexpr uint8_t DEFAULT_STEM_HOPS = 5;

// ============================================================================
// CACHE PARAMETERS (from chatroom_manager.h)
// ============================================================================

// Maximum number of message hashes in deduplication cache
static constexpr size_t MAX_MESSAGE_CACHE = 10000;

// How long to keep messages in cache (1 hour)
static constexpr int64_t MESSAGE_CACHE_TIME = 3600;

// ============================================================================
// BROADCAST DELAYS (from chatroom_manager.h)
// ============================================================================

// Minimum random delay before broadcasting FLUFF messages (1 second)
static constexpr int64_t FLUFF_BROADCAST_DELAY_MS_MIN = 1000;

// Maximum random delay before broadcasting FLUFF messages (5 seconds)
static constexpr int64_t FLUFF_BROADCAST_DELAY_MS_MAX = 5000;

// ============================================================================
// NETWORK LIMITS
// ============================================================================

// Maximum INV messages in a single batch
static constexpr size_t MAX_CHATROOM_INV_BATCH = 100;

#endif // DASH_CHATROOM_LIMITS_H