# Omega Chat – Documentchain-Inspired Integration

## Scope

Implement only minimal, high-value features:

Phase 1:
- message hash anchoring
- optional revision tracking
- RPC extensions

Phase 2:
- multimedia storage (off-chain)
- integrity + optional anchoring

No document management. No file workflows. No heavy storage logic.

---

# PHASE 1 — MESSAGE ANCHORING

## Goal

Provide proof-of-existence for messages without storing content on-chain.

## Flow

msg → hash → (optional) anchor → blockchain

## Hashing

- Algorithm: SHA256
- Input: raw message bytes (before encryption if used consistently)

hash = SHA256(message)

## Anchoring Modes

### 1. Default (off-chain)
- message stored in chat system only
- no blockchain interaction

### 2. Anchored (batched)
- hash added to batch
- committed periodically

### 3. Immediate (rare)
- direct tx per message
- only for critical messages

---

## Batch Anchoring (REQUIRED)

### Merkle Tree

messages → hashes → merkle tree → root

- 100–1000 messages per batch
- store only Merkle root on-chain

### Data stored on-chain

OP_RETURN / special tx:
- merkle_root
- timestamp

### Client responsibility

- store proof path (merkle branch)
- verify inclusion if needed

---

## RPC Extensions

Add minimal commands:

omega-cli anchormsg <hash>
omega-cli verifymsg <hash>
omega-cli getmsgproof <hash>

### Behaviour

- anchormsg → queues hash
- verifymsg → checks existence in anchored sets
- getmsgproof → returns merkle branch

---

## Message Format

{
  "text": "hello",
  "hash": "sha256...",
  "anchor": false
}

Optional:

{
  "anchor": true
}

---

## Revision Tracking (OPTIONAL BUT RECOMMENDED)

## Goal

Allow edits without losing history.

## Model

msg_v1 → hash1  
msg_v2 → hash2 + prev_hash=hash1

## Format

{
  "text": "edited",
  "hash": "hash2",
  "prev": "hash1"
}

## Rules

- never overwrite original
- always link previous hash
- anchoring applies per version

---

## Constraints

- no message content on-chain
- no ordering changes
- no consensus impact
- RPC must remain deterministic

---

# PHASE 2 — MULTIMEDIA

## Goal

Support images/video without blockchain bloat.

## Core Rule

media → off-chain  
hash → optional on-chain

---

## Upload Flow

client:
  compress
  (optional) encrypt
  upload → relay
  receive URL

  hash(media)
  send message + URL + hash

---

## Media Storage

### Required (V1)

- central relay server
- HTTP upload
- file system storage

### Optional (later)

- P2P relay
- IPFS fallback

---

## Media Message Format

{
  "type": "image",
  "url": "https://relay/file",
  "hash": "sha256...",
  "size": 123456,
  "mime": "image/webp"
}

---

## Validation

Client must:

download → hash → compare

Reject if mismatch.

---

## Compression Rules

### Images

- format: WebP or AVIF
- target: <300 KB

### Video

- codec: H.264 / H.265
- target: <5 MB

---

## Deduplication (SERVER)

if hash exists:
  reuse file
else:
  store

---

## TTL / Retention

- default: 7–30 days
- pinned: permanent

---

## Privacy (OPTIONAL)

encrypted_media = encrypt(media, room_key)

Server stores encrypted blob only.

---

## Anchoring Media

Same as messages:

hash(media) → batch → merkle root → chain

Do NOT store:

- URL
- filename
- metadata

---

# SECURITY

Never:

- store private keys in app storage
- expose RPC credentials
- log sensitive payloads

Use:

- Android Keystore
- secure config for RPC

---

# PERFORMANCE RULES

- never block UI thread
- async uploads only
- retry with backoff
- timeout all network calls

---

# DO NOT IMPLEMENT

- full document system
- file version UI
- blockchain storage of content
- heavy indexing
- IPFS as primary storage

---

# SUMMARY (IMPLEMENTATION ORDER)

1. message hashing
2. batch anchoring (merkle)
3. RPC commands
4. revision linking
5. media upload (relay)
6. media hashing + validation
7. optional media anchoring

Stop after each step is stable.
