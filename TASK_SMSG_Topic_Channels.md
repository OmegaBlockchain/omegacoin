Task: Implement SMSG Topic Channels in Omega Blockchain
========================================================

Target: Omega blockchain (Dash 19.x base with Particl SMSG module)

Goal: extend the Particl SMSG module to support Topic Channels with
cleartext topic routing and a per-topic LevelDB index.

This task is intended for Claude Code to execute in structured
development steps.


1. Objective
------------

Implement Topic Channels in the SMSG module so that Omega nodes can:

- classify messages by topic
- route and filter messages by topic without decryption
- subscribe to topics
- index messages by topic in LevelDB
- reduce unnecessary storage and decryption overhead
- enable structured application messaging (listings, chat, contracts)


2. Functional Requirements
--------------------------

### 2.1 Cleartext Topic Routing

Every topic-channel message must carry a 4-byte topic hash in the
cleartext header so nodes can route and filter WITHOUT decrypting.

Full topic string is stored in the encrypted payload header and is
indexed locally after decryption.

### 2.2 Topic Routing Decision

On message receive, node must:

1. Read 4-byte topic hash from cleartext header
2. Compare against subscribed topic hashes
3. Decide: store / forward / ignore

No decryption required for the routing decision.

### 2.3 Topic Subscription

RPC:

    smsgsubscribe <topic>
    smsgunsubscribe <topic>
    smsglisttopics

Node stores topic preferences in `smsgstore/topic_subs.dat`.

### 2.4 Topic Query

RPC:

    smsggetmessages <topic>

Returns messages by topic from the local index.

### 2.5 Topic Index

Node maintains a LevelDB index (in the existing smsg DB):

    key:   t:<topic>:<timestamp>:<msgid>   (ASCII, null-terminated fields)
    value: msgid (20 bytes)

Encrypted payload remains untouched in bucket storage.


3. Topic Design
---------------

### 3.1 Topic Naming

Format:

    omega.<domain>[.<subdomain>]

Examples:

    omega.listings
    omega.listings.uk
    omega.listings.eu
    omega.chat
    omega.contract
    omega.match
    omega.system

Rules:

- lowercase ASCII only
- max 64 characters
- characters: [a-z0-9.]
- no spaces
- dot-separated segments
- must begin with `omega.`


4. Wire Format
--------------

### 4.1 Existing SecureMessage Header (DO NOT CHANGE LAYOUT)

`SecureMessage` is a `#pragma pack(push, 1)` packed struct — fixed
104-byte binary layout (`SMSG_HDR_LEN = 104`). It is read and written
as raw bytes using `fread` / `fwrite`. No fields may be added,
removed, or reordered.

Current layout (offsets):

    [0]   hash[4]        — proof-of-work hash
    [4]   version[2]     — message type/version
    [6]   flags[1]       — currently unused (= 0)
    [7]   timestamp[8]   — unix timestamp (already present)
    [15]  iv[16]         — AES IV
    [31]  cpkR[33]       — ephemeral secp256k1 pubkey (ECDH)
    [64]  mac[32]        — HMAC-SHA256
    [96]  nonce[4]       — used as nDaysRetention in paid messages (version[0]==3)
    [100] nPayload[4]    — length of encrypted payload in bytes

    Total: 104 bytes

**timestamp and ttl already exist** — do not re-add them.

### 4.2 New Message Version: version[0] = 4

Introduce `version[0] = 4` for topic-channel free messages.

Use `nonce[0..3]` (unused for free messages) to store a 4-byte
FNV-1a hash of the full topic string:

    nonce[0..3] = FNV32(topic_string)

This is the only cleartext topic data. It is sufficient for routing
decisions without decryption.

`version[1]`:
- `0` = free topic message
- `1` = reserved (future: paid topic message)

### 4.3 Existing Encrypted Payload Header (SMSG_PL_HDR_LEN)

The encrypted payload begins with its own inner header:

    SMSG_PL_HDR_LEN = 1 + 20 + 65 + 4 = 90 bytes
    [0]   type flags (1 byte)
    [1]   from-address hash (20 bytes)
    [21]  from-pubkey (65 bytes)
    [86]  reserved (4 bytes)

For version[0]==4, extend the encrypted payload header to include the
full topic string. Append after the existing 90 bytes:

    [90]  topic_len (1 byte, max 64)
    [91]  topic (topic_len bytes, ASCII, no null terminator)

`SMSG_PL_HDR_LEN` becomes `1 + 20 + 65 + 4 + 1 + topic_len` for
version 4 messages. Since topic_len is variable, a new constant is
NOT appropriate — compute it at runtime from `version[0]` and the
first byte of the decrypted payload at offset 90.

### 4.4 Version Matrix

    version[0]=2, version[1]=1  — existing free message (unchanged)
    version[0]=3, version[1]=0  — existing paid message (unchanged)
    version[0]=3, version[1]=1  — existing blinded paid message (unchanged)
    version[0]=4, version[1]=0  — NEW: topic-channel free message
    version[0]=0, version[1]=0  — purged message tombstone (unchanged)

### 4.5 Backward Compatibility

`CSMSG::Store()` does NOT check `version[0]` before storing. Old
nodes will store and relay `version[0]==4` messages without
understanding them. `CSMSG::Decrypt()` returns `SMSG_UNKNOWN_VERSION`
for version[0] != 2 and != 3, so old nodes silently skip decryption.

Result: network propagation works; old nodes carry but cannot read
topic messages. No hard fork required.


5. Code Changes
---------------

### 5.1 Actual Source Files

All SMSG code lives in:

    src/smsg/smessage.h      — SecureMessage struct, constants, CSMSG class declaration
    src/smsg/smessage.cpp    — all SMSG logic: Send, Encrypt, Decrypt, Store,
                               Receive, Validate, ScanMessage, ReceiveData
    src/smsg/db.h            — SecMsgDB class declaration
    src/smsg/db.cpp          — LevelDB read/write helpers
    src/smsg/rpcsmessage.cpp — all smsg RPC commands
    src/smsg/net.h           — SecMsgNode / SecMsgBucketInfo (header-only)
    src/smsg/keystore.h/.cpp — key management

There are NO files named smsg.cpp, smsgnet.cpp, smsgstore.cpp,
smsgmessage.cpp, or smsgtypes.h in this codebase.

### 5.2 smessage.h — Add Constants and FNV Helper

Add after existing constants:

    // Topic channel message version
    static const uint8_t SMSG_VERSION_TOPIC = 4;

    // Max topic string length (fits in 1 byte length prefix)
    static const uint8_t SMSG_MAX_TOPIC_LEN = 64;

    // FNV-1a 32-bit hash of a topic string (for cleartext routing via nonce[0..3])
    inline uint32_t SMSGTopicHash(const std::string &topic)
    {
        uint32_t h = 2166136261u;
        for (unsigned char c : topic) {
            h ^= c;
            h *= 16777619u;
        }
        return h;
    }

### 5.3 smessage.h — Add Topic Validation Helper

    inline bool IsValidTopic(const std::string &topic)
    {
        if (topic.size() < 7 || topic.size() > SMSG_MAX_TOPIC_LEN) return false;
        if (topic.substr(0, 6) != "omega.") return false;
        for (char c : topic) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.'))
                return false;
        }
        return true;
    }

### 5.4 smessage.cpp — CSMSG::Encrypt() for version 4

Function: `int CSMSG::Encrypt(SecureMessage &smsg, ...)` (line ~3982)

When `smsg.version[0] == SMSG_VERSION_TOPIC`:

1. Validate topic string (passed as new parameter) via `IsValidTopic()`
2. Set `smsg.nonce[0..3]` = `SMSGTopicHash(topic)` as little-endian uint32
3. Build encrypted payload header as for version 2 EXCEPT append:
   - 1 byte: `(uint8_t)topic.size()`
   - N bytes: `topic` (ASCII, no null terminator)
4. Remainder of Encrypt logic is identical to version 2

Add `topic` parameter to `CSMSG::Send()` (line ~4158); pass through to
`Encrypt()`. Default = empty string (produces version 2 message).

### 5.5 smessage.cpp — CSMSG::Decrypt() for version 4

Function: `int CSMSG::Decrypt(...)` (line ~4533)

After decryption succeeds, check `psmsg->version[0]`:
- If `== 2` or `== 3`: existing path unchanged
- If `== 4`: read topic from decrypted payload at offset 90:
  - byte at [90] = topic_len
  - bytes [91..91+topic_len-1] = topic string
  - validate with `IsValidTopic()`; return `SMSG_UNKNOWN_VERSION` if invalid
  - store topic in `msg.sTopic` (add field to `MessageData`)

Add `std::string sTopic` to `MessageData` struct (smessage.h, ~line 219).

### 5.6 smessage.cpp — CSMSG::Validate() for version 4

Function: `int CSMSG::Validate(...)` (line ~3728)

Currently rejects version[0] != 2 with `SMSG_UNKNOWN_VERSION`.
Add a branch:

    if (psmsg->version[0] == SMSG_VERSION_TOPIC) {
        // validate nonce (topic hash present), then fall through to PoW check
        // topic string validated after decryption, not here
        goto validate_pow;
    }

### 5.7 smessage.cpp — CSMSG::Receive() — Topic Routing Filter

Function: `int CSMSG::Receive(CNode *pfrom, ...)` (line ~3363)

After reading the 104-byte header but before calling `Store()`:

    if (psmsg->version[0] == SMSG_VERSION_TOPIC) {
        uint32_t topicHash;
        memcpy(&topicHash, psmsg->nonce, 4);
        // topicHash is little-endian
        if (!smsgModule.IsSubscribedTopicHash(topicHash)
            && !IsSystemTopic(topicHash)) {
            // forward without storing
            return SMSG_NO_ERROR;
        }
    }

Add `bool CSMSG::IsSubscribedTopicHash(uint32_t hash)` — checks the
in-memory subscription set (see §5.9).

### 5.8 db.cpp / db.h — Topic Index

Add to `SecMsgDB`:

    bool WriteTopicIndex(const std::string &topic, int64_t timestamp,
                         const uint160 &msgId);
    bool ReadTopicMessages(const std::string &topic,
                           std::vector<std::pair<int64_t,uint160>> &out);
    bool EraseTopicIndex(const std::string &topic, int64_t timestamp,
                         const uint160 &msgId);

Key format (std::string concatenation):

    "t:" + topic + ":" + strprintf("%020d", timestamp) + ":" + HexStr(msgId)

Value: msgId (20 bytes raw).

Call `WriteTopicIndex()` from `CSMSG::ScanMessage()` (line ~2694) after
a version-4 message is successfully decrypted and `msg.sTopic` is set.

### 5.9 smessage.cpp — Subscription Storage

Add to `CSMSG` class (smessage.h):

    std::set<std::string>  m_subscribed_topics;       // full topic strings
    std::set<uint32_t>     m_subscribed_topic_hashes; // FNV hashes for fast routing

Persist to `GetDataDir() / "smsgstore" / "topic_subs.dat"` as one
topic per line (plain text). Load in `CSMSG::Start()`, save on every
subscribe/unsubscribe.

    bool CSMSG::SubscribeTopic(const std::string &topic);
    bool CSMSG::UnsubscribeTopic(const std::string &topic);
    bool CSMSG::LoadTopicSubs();
    bool CSMSG::SaveTopicSubs();


6. RPC Implementation
---------------------

File: `src/smsg/rpcsmessage.cpp`

### 6.1 smsgsubscribe

    smsgsubscribe "omega.listings"

Calls `smsgModule.SubscribeTopic(topic)`. Returns `{"result":"ok"}`.

### 6.2 smsgunsubscribe

    smsgunsubscribe "omega.listings"

Calls `smsgModule.UnsubscribeTopic(topic)`. Returns `{"result":"ok"}`.

### 6.3 smsglisttopics

Returns array of currently subscribed topics:

    ["omega.listings", "omega.chat"]

### 6.4 smsggetmessages

    smsggetmessages "omega.listings"

Queries `SecMsgDB::ReadTopicMessages(topic, ...)`. Returns array:

    [
      { "msgid": "...", "timestamp": 1234567890, "from": "oXXX",
        "payload_type": "" }
    ]

### 6.5 smsgsend extension

Extend existing `smsgsend` to accept optional topic parameter:

    smsgsend "oFrom" "oTo" "message" false false 2 "" "omega.listings"

When topic non-empty, sets `version[0] = SMSG_VERSION_TOPIC` and
routes through new Encrypt path.


7. System Topic
---------------

`omega.system` is always stored regardless of subscription, analogous
to the Trollbox. Define:

    static const char* SMSG_SYSTEM_TOPIC = "omega.system";

`IsSystemTopic(uint32_t hash)` returns `hash == SMSGTopicHash("omega.system")`.


8. Forwarding Logic
-------------------

On receive:

1. If `version[0] != SMSG_VERSION_TOPIC`: existing logic unchanged.
2. If `version[0] == SMSG_VERSION_TOPIC`:
   - If `IsSystemTopic(hash)`: always store + forward.
   - If `IsSubscribedTopicHash(hash)`: store + forward.
   - Otherwise: forward only (do not store, do not attempt decryption).


9. Backward Compatibility
-------------------------

Messages with `version[0] == 2` or `3`: existing behaviour, unchanged.

Messages with `version[0] == 4` on old nodes:
- `CSMSG::Store()` stores them (no version check).
- `CSMSG::Decrypt()` returns `SMSG_UNKNOWN_VERSION` — silently skipped.
- Old nodes relay topic messages transparently.

No consensus change. No hard fork. No special activation height.

If a version-4 message arrives at a new node with no topic subscription,
it is forwarded but not stored. No fallback to `omega.legacy` — messages
without a valid topic are dropped.


10. Acceptance Criteria
-----------------------

- `version[0]=4` messages carry 4-byte FNV topic hash in `nonce[0..3]`
- Full topic string survives Encrypt/Decrypt round-trip
- `IsValidTopic()` rejects malformed topics
- `IsSubscribedTopicHash()` drives store/drop decision without decryption
- LevelDB topic index populated by `ScanMessage()` after decryption
- `smsgsubscribe` / `smsgunsubscribe` / `smsglisttopics` / `smsggetmessages` work
- Subscription persists across restart via `topic_subs.dat`
- `version[0]==2` and `version[0]==3` paths completely unchanged
- Existing smsg unit tests continue to pass


11. Testing
-----------

### Unit Tests

- `IsValidTopic()` — valid and invalid inputs
- `SMSGTopicHash()` — determinism, collision resistance spot-check
- Encrypt/Decrypt round-trip for version-4 message, topic recovered intact
- `WriteTopicIndex` / `ReadTopicMessages` on a temp LevelDB instance
- Subscription load/save round-trip

### Integration Tests

Scenario:

1. node1: `smsgsubscribe omega.listings`
2. node2: `smsgsend oFrom oTo "payload" ... "omega.listings"`
3. node1 receives, routes by hash, stores
4. node1: `smsggetmessages omega.listings` — returns message

Expected: success, topic field correct, version[0]==4 confirmed.

### Regression

- Send a standard v2 message between nodes — unchanged behaviour
- Send a v3 paid message — unchanged behaviour
- Old-node simulation: receive a v4 message, confirm Store() accepts it,
  confirm Decrypt() returns SMSG_UNKNOWN_VERSION without crash


12. Security
------------

Validate on receive:

- `version[0] == SMSG_VERSION_TOPIC`: reject if `nonce[0..3]` are all zero
- After decryption: `IsValidTopic()` on recovered topic string; reject if invalid
- `topic_len` byte in encrypted payload header must be 1–64; reject otherwise
- Existing timestamp sanity check (`SMSG_TIME_LEEWAY`, `SMSG_TIME_IGNORE`) applies
- `nPayload` must be >= `SMSG_PL_HDR_LEN + 1 + topic_len + 1` (minimum); reject if not

Reject: oversized topics, non-ASCII, missing `omega.` prefix, zero-length.


13. Performance
---------------

Routing decision (hash lookup in `std::set<uint32_t>`) is O(log n) on
the number of subscriptions — negligible.

LevelDB topic index write: one extra DB write per received+decrypted
topic message — same order as existing inbox writes.

No scan of message body required for routing.


14. TTL Handling
----------------

`timestamp + ttl < current_time` → Drop message.

For version-4 free messages, TTL defaults to 48 hours
(`SMSG_RETENTION_OLD`), same as version-2 free messages.
`nonce[0]` carries the topic hash low byte, NOT TTL — TTL is fixed
at the retention default for free topic messages.

Paid topic messages (version[0]=4, version[1]=1) may use `nonce[0]`
for `nDaysRetention` in a future extension — reserved for now.


15. Documentation
-----------------

Update:

- `README.md` — add Topic Channels to the SMSG section
- `src/smsg/rpcsmessage.cpp` help strings — all four new RPC commands
- Release notes


16. Deliverables
----------------

- Modified `src/smsg/smessage.h` (constants, helpers, MessageData.sTopic)
- Modified `src/smsg/smessage.cpp` (Encrypt, Decrypt, Validate, Receive,
  Send, ScanMessage, subscription methods)
- Modified `src/smsg/db.h` / `db.cpp` (topic index methods)
- Modified `src/smsg/rpcsmessage.cpp` (4 new RPC commands + smsgsend extension)
- New `src/test/smsg_topic_tests.cpp` (unit tests)
- Build verified — no regressions in existing smsg tests


17. Future Extensions
---------------------

- Paid topic messages (version[0]=4, version[1]=1)
- Geo topics (omega.listings.uk, omega.listings.eu)
- Reputation topic (stake-weighted message ranking)
- Contract signalling (omega.contract)
- Mobile light clients subscribing to specific topic hashes only
- Proof-of-work tags for spam resistance on high-volume topics
