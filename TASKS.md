# Omega App — Task Backlog

This file tracks implementation tasks for the Omega companion app.
RPC method names are from the live daemon audit on 2026-03-20.
See `docs/smsg-protocol.md` and `docs/rpc-interface.md` for full references.

---

## Phase 1 — Node Connection & Wallet

### P1-01 · RPC client

Implement `OmegaRpcClient` — HTTP JSON-RPC client that speaks to the local
omega daemon. All other client classes use this as the transport layer.

**Connection params needed** (see BLOCKCHAIN_READINESS.md §8):
- Default RPC port: TBD — grep `GetDefaultPort` in chainparams.cpp
- Auth: `rpcuser` / `rpcpassword` from omega.conf

---

### P1-02 · Wallet unlock flow

UI flow to prompt for passphrase and call `walletpassphrase <pass> <seconds>`.
Required before any key generation or signing operation.

**Note:** `smsglocalkeys` returns empty `wallet_keys` when wallet is locked —
the app must prompt unlock before attempting SMSG setup.

---

### P1-03 · NodeManager startup sequence

On every app connect to a node, in order:

1. Call `getblockchaininfo` — verify `verificationprogress` ≥ 0.99
2. Call `smsgdisable` (ignore error if already disabled)
3. Call `smsgenable "<wallet_name>"` — bind SMSG to the wallet
4. Call `smsgoptions set scanIncoming true`
5. Call `smsglocalkeys` — if `wallet_keys` is empty, generate one:
   - Call `getnewaddress` → address
   - Call `smsgaddlocaladdress <address>` — wallet-bound key, can send
6. Cache `wallet_keys[0].address` as the node's own messaging address

**Why:** `smsg=1` in omega.conf enables the SMSG subsystem but does NOT bind
it to a wallet. Without step 2-3, all SMSG RPCs fail with "Wallet unset".
See `docs/smsg-protocol.md` § Known Quirks.

**Particl pattern** (`SmsgService.ts`): calls `smsgSetWallet` (= `smsgenable`)
then `smsgAddLocalAddress` before allowing any message operations.

---

### P1-04 · Node health check

Periodic check (every 30s) verifying the node is still reachable and SMSG
is still bound. If `smsglocalkeys` returns empty `wallet_keys`, re-run the
startup sequence (P1-03).

---

## Phase 2 — SMSG Messaging Layer

### P2-01 · SMSG RPC wrapper

Implement `SmsgClient` — a typed wrapper around the omega-cli SMSG RPC methods.

**Methods:**

- `send(fromAddress: String, toAddress: String, message: String, daysRetention: Int): String`
  → `smsgsend` — returns msg hash. plaintext only.
  → `fromAddress` MUST be from `wallet_keys` (ismine:true). Never from `smsg_keys`.
- `sendAnon(toAddress: String, message: String): String`
  → `smsgsendanon` — encrypted send, sender anonymous
- `getBuckets(): List<SmsgBucket>`
  → `smsgbuckets` (NOT smsggetbuckets — that command does not exist)
- `getMessage(msgId: String): SmsgMessage`
  → `smsg` (NOT smsgget — that command does not exist)
- `getMessagesByAddress(address: String): List<SmsgMessage>`
  → `smsgview <address>` — fetch all messages for a room address
- `addAddress(address: String, publicKey: String)`
  → `smsgaddaddress`
- `addLocalAddress(address: String)`
  → `smsgaddlocaladdress` — simpler when key is already in wallet
- `getRecipientPubKey(address: String): String`
  → `smsggetpubkey` — for encrypted room invite flow (P2-10)
- `importPrivKey(wif: String, label: String)`
  → `smsgimportprivkey`
- `getInbox(): List<SmsgMessage>`
  → `smsginbox`
- `getOutbox(): List<SmsgMessage>`
  → `smsgoutbox`
- `purgeMessage(msgId: String)`
  → `smsgpurge` — for room expiry implementation

---

### P2-02 · SMSG poll worker

Background worker that polls `smsgbuckets` every 60 seconds, diffs against
local cache, and fetches new messages via `smsg <msgid>`.

Replace with ZMQ push (P4-05) once ZMQ SMSG topic is confirmed.

---

### P2-04 · Per-room keypair manager

Generates room keypairs and registers them with the node via `smsgaddlocaladdress`
(wallet-bound) or `smsgaddaddress` (contact pubkey only).

**Key type rules:**
- `wallet_keys` (from `getnewaddress` + `smsgaddlocaladdress`): private key in
  wallet, can sign outgoing messages, survives wallet backup/restore.
- `smsg_keys` (from `smsgaddaddress` with pubkey): contact keys, can receive
  messages but CANNOT be used as `address_from` in `smsgsend`.

---

### P2-05 · Room message history fetch

Use `smsgview <room_address>` to fetch full message history for a room on first
join, rather than waiting for bucket poll. This gives immediate history without
waiting for the 60-second poll cycle.

---

### P2-07 · TTL strategy

Two TTL tiers (based on Particl production experience):
- Free messages: `days_retention = 2`
- Important/paid messages: `days_retention = 7` (test max TTL first — see BLOCKCHAIN_READINESS §1.4.2)

Pass as integer to `smsgsend`: `smsgsend FROM TO "msg" false <days>`

---

### P2-08 · Compose screen — sender address constraint

The From address picker must only show addresses from `wallet_keys`
(returned by `smsglocalkeys`). Never show `smsg_keys` entries as senders.

**Why:** Using a `smsg_keys` address as `address_from` causes:
`"Unknown private key for from address"`
The private key does not exist in the wallet for contact-only keys.

---

### P2-09 · Structured message payload

Message body is a plain string in the RPC. For structured data (room messages,
invites, reactions), encode as JSON string in the body field:

```json
{"type": "msg", "v": 1, "body": "hello", "ts": 1234567890}
```

Use a `MessagePayload` sealed class to serialize/deserialize. Fallback to raw
string for plain text messages from other clients.

**Pattern from particl-market:** `SmsgService.ts` sends JSON-serialized
`ActionMessage` objects as the SMSG body.

---

### P2-10 · Encrypted room invites

Uses `smsggetpubkey` to discover invitee's public key, then sends invite
via `smsgsendanon`.

**Pre-send check:** Before calling `smsggetpubkey`, verify the recipient has
previously sent a message (their key will be in the node's address db).
If not found, they must share their pubkey out-of-band.

---

### P2-11 · Key backup screen

WIF export/import for SMSG keys. Import path uses `smsgimportprivkey`.

**Note:** Keys generated via `getnewaddress` + `smsgaddlocaladdress` are
already in the wallet and covered by standard wallet backup (`dumpwallet`).
Only standalone SMSG keys (imported via `smsgimportprivkey`) need separate
backup here.

---

## Phase 4 — Push Notifications

### P4-05 · ZMQ SMSG subscription

Subscribe to ZMQ `smsg` topic for instant new-message notifications instead
of 60-second polling (P2-02).

**Pre-requisite:** Confirm ZMQ SMSG topic is published — run
`omega-cli getzmqnotifications` and check for `smsg` entry.
See BLOCKCHAIN_READINESS §6.3.

**Particl pattern** (`SmsgService.ts`): subscribes to ZMQ `smsg` topic,
calls `smsginbox` on each notification to get message details.

If the `smsg` ZMQ topic is not present in the daemon, this reduces to
keeping the 60-second poll from P2-02.

---
