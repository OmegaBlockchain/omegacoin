# TASK: Omega Secure Messenger — Android App

**Created:** 2026-03-30
**Last revised:** 2026-04-01 (topic channels confirmed; smsgclearbuckets added; ZMQ SMSG verified)
**Target platform:** Android 9.0+ (API 28+)
**Blockchain:** Omega 0.20.x (Dash 0.19 lineage)
**Core transport:** SMSG (SecureMessage protocol)
**Priority:** Maximum user privacy

---

## §0 Architecture Decision: Companion Daemon Model

### Why not embedded daemon?

Running a full Omega node inside an Android process is technically possible
(cross-compile C++ to ARM/AArch64) but impractical:

- ~2 GB blockchain sync, growing
- Continuous P2P connections drain battery
- SMSG bucket relay requires always-on networking
- Android aggressively kills background processes after ~10 minutes

### Why not a centralised relay?

A relay server that participates in SMSG on behalf of mobile clients would
destroy the privacy guarantee. The relay operator sees all message metadata,
and potentially plaintext for public rooms. This defeats the purpose.

### Chosen model: Companion Daemon

```
┌──────────────────────┐          ┌─────────────────────────┐
│   Android App        │  RPC     │   Omega Daemon          │
│   (Kotlin/Compose)   │◄────────►│   (user's VPS / home    │
│                      │  ZMQ     │    server / Tor hidden   │
│   - UI               │◄─────── │    service)              │
│   - Local SQLite     │          │                         │
│   - Key management   │          │   - Full node           │
│   - PSBT signing     │          │   - SMSG enabled        │
│                      │          │   - Wallet              │
└──────────────────────┘          └─────────────────────────┘
```

**The user runs their own Omega daemon** (VPS, home server, Raspberry Pi, or
Tor hidden service). The Android app connects to it over:

- **JSON-RPC** (port 7778) — wallet ops, SMSG send/receive, room management
- **ZMQ** (configurable port) — real-time push notifications for new blocks,
  transactions, and SMSG messages

**Privacy properties:**
- No third-party server ever sees messages or keys
- User controls their own node — full sovereignty
- Tor support for RPC/ZMQ connections (optional but recommended)
- Node can run on the same LAN (zero external exposure)

### Adoption barrier mitigation

Running a daemon is a high barrier. Mitigations:

1. **One-click deployment script** — ship `omega-deploy.sh` that provisions
   a VPS (e.g. cheapest Hetzner/OVH) with Omega daemon, Tor hidden service,
   firewall, and auto-updates. User pastes a single command.
2. **Raspberry Pi image** — pre-built SD card image with Omega + Tor,
   discoverable via LAN mDNS.
3. **In-app setup wizard** — walks the user through deployment options with
   copy-paste commands and a "Test Connection" button.
4. **Trusted public node fallback** — for users who cannot run a daemon, the
   app can connect to a community-run public node. **Privacy warning shown
   prominently:** "Your messages and transactions are visible to this node's
   operator. Run your own node for full privacy." This mode disables
   marketplace escrow and private rooms (only trollbox and open public rooms).

### Connection security

| Method | Transport |
|--------|-----------|
| LAN | RPC + ZMQ over TLS (self-signed cert, TOFU-pinned) |
| Internet | RPC + ZMQ over Tor hidden service (.onion) |
| Fallback | RPC + ZMQ over WireGuard/VPN to home network |

**CRITICAL: ZMQ must be tunnelled.** ZMQ uses plain TCP by default. If the
daemon is exposed to the internet, ZMQ traffic can be intercepted, leaking
transaction and message metadata. All ZMQ connections MUST go through one of:
- Tor (`.onion` address — inherently encrypted)
- WireGuard/VPN tunnel
- stunnel or socat TLS wrapper (LAN with untrusted segments)
- Same-host loopback only (bind ZMQ to `127.0.0.1`, access via SSH tunnel)

**RPC credential storage:**
- Stored in Android Keystore via `EncryptedSharedPreferences`
- Encrypted with a device-bound key
- Optionally protected by biometric authentication (BiometricPrompt, API 28+)
- Never written to logcat, crash reports, or unencrypted storage

**TLS certificate pinning (TOFU model):**
1. First connection: app downloads daemon's self-signed certificate
2. User verifies the certificate fingerprint (displayed on both daemon
   console and app) — manual out-of-band confirmation
3. App stores `SHA-256(cert)` in Android Keystore
4. All subsequent connections verify against pinned hash — reject on mismatch
5. "Reset Certificate Pin" option in settings (requires re-verification)
6. Tor connections (.onion) bypass TLS — the onion address itself provides
   authentication and encryption

The app stores the daemon connection profile (encrypted):
`{host, rpcPort, zmqPort, rpcUser, rpcPassword, tlsCertHash, torEnabled}`

---

## §1 Wallet Implementation

### Decision: BIP157/158 Light Wallet + Full Daemon Fallback

Omega 0.20.x has **BIP157/158 compact block filter** support compiled in
(`NODE_COMPACT_FILTERS` service bit, `blockfilter.h/cpp`,
`blockfilterindex.h/cpp`). This is the most privacy-preserving light client
protocol available — it does NOT leak address information to the server
(unlike bloom filters / BIP37).

However, because the app already requires a companion daemon for SMSG, the
wallet can simply use the daemon's full wallet via RPC. BIP157/158 becomes
relevant only in a future "daemon-less" mode.

### §1.1 Wallet architecture (Phase 1 — RPC wallet)

The Android app is a **thin wallet UI** that delegates all wallet operations
to the companion daemon's built-in wallet via RPC.

```
App (Kotlin)                         Daemon
─────────────                        ──────
getbalances ──────────────────────► wallet RPC
getnewaddress ────────────────────► wallet RPC
listtransactions ─────────────────► wallet RPC
sendtoaddress ────────────────────► wallet RPC
estimatesmartfee ─────────────────► wallet RPC
ZMQ subscribe (rawtx, hashblock) ◄─ ZMQ push
```

**What the app stores locally:**
- Transaction cache (SQLite) — for offline viewing
- Address book — labels, room associations
- Connection profile — daemon credentials
- BIP39 mnemonic backup (encrypted, optional)

**What the app does NOT store:**
- Private keys (held by daemon wallet)
- UTXOs (queried live from daemon)
- Blockchain data

### §1.2 HD wallet & seed management

Omega has native BIP39 mnemonic support (`upgradetohd` RPC, `dumphdinfo`).

**SECURITY NOTE — mnemonic transmission:**
`upgradetohd "mnemonic"` sends the recovery phrase over the RPC connection.
Even over TLS/Tor, this is sensitive. Mitigations:

- **Preferred:** User generates the seed directly on the daemon via
  `upgradetohd` (no arguments) — daemon generates the mnemonic locally.
  App then retrieves it once via `dumphdinfo` for backup display.
- **Restore scenario:** If restoring from backup, the mnemonic must travel
  over RPC. Ensure the connection is TLS/Tor. Alternatively, instruct the
  user to paste the mnemonic directly into the daemon console or config
  file, bypassing the network entirely.
- **Never** transmit the mnemonic over an unencrypted RPC connection.

**Onboarding flow (new wallet):**
1. App connects to daemon (over TLS or Tor)
2. Calls `getwalletinfo` — checks `"hdseedid"` field
3. If no HD seed: calls `upgradetohd` with no arguments (daemon generates)
4. Calls `dumphdinfo` to retrieve the mnemonic
5. App displays mnemonic for backup with warnings:
   - "Write these words on paper. Do not screenshot."
   - "Anyone with these words can steal your funds."
   - Screenshot detection: if Android screenshot event fires, show warning
6. User confirms backup (re-enter words or tap "I have written it down")
7. App erases mnemonic from memory — never persists it

**Onboarding flow (restore from backup):**
1. Fresh daemon install
2. **Preferred:** User pastes mnemonic directly into daemon console:
   `omega-cli upgradetohd "mnemonic words here"`
3. **Fallback (remote daemon):** App sends `upgradetohd "mnemonic" "" ""`
   via RPC over TLS/Tor only. App immediately erases mnemonic from memory.
4. Triggers `rescanblockchain` to recover transaction history
5. Re-registers SMSG keys via `smsgaddlocaladdress`

**Encrypted wallet backup:**
- Option to export an encrypted backup file (wallet + SMSG keys)
- Encrypted with a user-provided passphrase (Argon2id + AES-256-GCM)
  Argon2id is memory-hard and resistant to GPU/ASIC brute-force attacks.
  Parameters: 64 MB memory, 3 iterations, 4 parallelism (OWASP minimum).
  Note: the daemon itself uses PBKDF2-HMAC-SHA512 for BIP39 seed
  derivation — that is a daemon concern, not changeable by the app.
- Exportable to external storage, cloud, or USB
- Importable on a fresh daemon install

### §1.3 Future: BIP157/158 autonomous light wallet (Phase 5+)

For users who cannot run a daemon, a future mode could:

1. Connect to any Omega full node advertising `NODE_COMPACT_FILTERS`
2. Download compact block filters (GCS) via `getcfilters` P2P message
3. Client-side filter matching against own addresses
4. Download only matching blocks
5. Construct and sign transactions locally (app holds keys)
6. Broadcast via `sendrawtransaction` to any connected node

This requires implementing:
- BIP157/158 client protocol in Kotlin/Rust
- Local key derivation (BIP32/39/44 with Omega's coin type 5)
- Transaction construction and signing
- SMSG light client (see §6)

**Estimated effort:** 3-6 months additional. Defer to Phase 5.

### §1.4 Lightning Network

Omega does not have a Lightning Network implementation. The HTLC and
channel infrastructure from Bitcoin's LN is not ported. **Lightning is not
an option for Omega 0.20.x.** If instant micro-payments are needed, use
InstantSend (already supported via Dash's LLMQ infrastructure).

---

## §2 SMSG Integration — Core Messaging

### §2.0 SMSG cryptographic properties (code-verified)

**Encryption scheme:** ECIES (Elliptic Curve Integrated Encryption Scheme),
modelled on BitMessage (`src/smsg/smessage.cpp:4026`).

Per-message flow:
1. Sender generates fresh ephemeral keypair `(r, R)` — `keyR.MakeNewKey(true)`
2. ECDH: `P = r × K_recipient` (shared secret from ephemeral × long-term)
3. `SHA-512(P)` → `key_e` (AES-256-CBC) + `key_m` (HMAC-SHA256)
4. Encrypt payload with `key_e`, MAC with `key_m`
5. Ephemeral public key `R` stored in message header (33 bytes)
6. Ephemeral private key `r` discarded after encryption

**NO PERFECT FORWARD SECRECY.** Decryption (`smessage.cpp:4661`) computes
`P = k_recipient × R` using the recipient's long-term private key. If
`k_recipient` is ever compromised, **all past messages** to that address
can be decrypted (the attacker has `k` and `R` is in every message header).

This is inherent to ECIES / BitMessage-style encryption. True PFS requires
both parties to contribute ephemeral keys (e.g. Signal's Double Ratchet).
SMSG is store-and-forward, not interactive, so a ratchet is non-trivial.

**Mitigations for lack of PFS:**
- Use per-room addresses: if one key is compromised, only that room's
  history is exposed, not all conversations
- Encourage `smsgsendanon` for sensitive DMs (no sender identity to link)
- Advise users to rotate SMSG addresses periodically
- Long-term: investigate a session-based ratchet for DMs (Phase 5+)

**Proof-of-work (spam prevention):**
Free messages require PoW: 19 bits of difficulty (`smessage.cpp:3992` —
2 zero bytes + 3 zero bits). This is ~500K hash iterations, taking
milliseconds on modern hardware. **Insufficient against determined
spammers.** A GPU can generate thousands of valid messages per second.

**Anti-spam strategy (app + daemon):**
- Trollbox: 30-second rate limit enforced by daemon (sufficient)
- Public rooms: app-side rate limiting per address (e.g. 1 msg / 5 seconds).
  Client-side filter: if > N messages from one address in T seconds, mute.
- Marketplace: require paid messages for listings (economic cost deters
  spam). Minimum listing fee: configurable, e.g. 0.01 OMEGA.
- Future (daemon-side): increase PoW difficulty or require small OMEGA
  fee per free message. Requires consensus change.

**SMSG code audit requirement:**
The SMSG implementation is ported from Particl. Before app launch, a
focused security audit of the modified SMSG code is required:
- Review all changes from upstream Particl
- Check for known Particl SMSG bugs / CVEs
- Verify topic channel key derivation (FNV-1a hash routing)
- `IsValidTopic()` hardened in 0.20.3: rejects consecutive dots (`..`) and trailing dot — validates `omega.*` prefix, lowercase alphanumeric+dots, length 7–64 (`smsg/smessage.h`)
- Test boundary conditions (max payload, TTL overflow, nonce exhaustion)
- Fuzz test `Encrypt()` / `Decrypt()` / `SetHash()` paths

### §2.1 Confirmed RPC commands (tested on live nodes)

| Command | Purpose | Status |
|---------|---------|--------|
| `smsgenable` | Enable SMSG on daemon | 🟢 |
| `smsgdisable` | Disable SMSG | 🟢 |
| `smsgsend` | Send message (address_from, address_to, message, paid_msg, days_retention, testfee, fromfile, decodehex, **topic**, parent_msgid) | 🟢 |
| `smsgsendanon` | Send anonymous encrypted message | 🟢 |
| `smsginbox` | Read inbox | 🟢 |
| `smsgoutbox` | Read outbox | 🟢 |
| `smsg` | Fetch single message by ID | 🟢 |
| `smsgbuckets` | List message buckets; `dump` mode writes `smsgstore/buckets_dump.json` (non-destructive) | 🟢 |
| `smsgscanbuckets` | Force rescan | 🟢 |
| `smsgaddaddress` | Add external address + pubkey | 🟢 |
| `smsgaddlocaladdress` | Add wallet address to SMSG | 🟢 |
| `smsggetpubkey` | Get pubkey for address | 🟢 |
| `smsgimportprivkey` | Import SMSG private key | 🟢 |
| `smsglocalkeys` | List registered keys | 🟢 |
| `smsgview` | View messages by address/date | 🟢 |
| `smsgpurge` | Delete message locally | 🟢 |
| `trollboxsend` | Send trollbox message | 🟢 |
| `trollboxlist` | List trollbox messages | 🟢 |
| `smsgsubscribe` | Subscribe to topic channel | 🟢 |
| `smsgunsubscribe` | Unsubscribe from topic | 🟢 |
| `smsglisttopics` | List subscribed topics | 🟢 |
| `smsggetmessages` | Get messages for topic | 🟢 |
| `smsgcreateroom` | Create room (type 8 special tx) | 🟢 New in 0.20.3 |
| `smsglistrooms` | List all rooms from index | 🟢 New in 0.20.3 |
| `smsggetroominfo` | Get room details by txid | 🟢 New in 0.20.3 |
| `smsgjoinroom` | Join room (subscribe + register key) | 🟢 New in 0.20.3 |
| `smsgclearbuckets` | Permanently delete all smsgstore .dat files (`confirm=true` required) | 🟢 New in 0.20.3 — admin/maintenance only |

### §2.2 Message types used by the app

| Use case | RPC command | Encryption | TTL |
|----------|------------|------------|-----|
| Public room message | `smsgsend` | None (plaintext) | 7 days |
| Private DM | `smsgsendanon` | ECDH+AES-256 | 7 days |
| Room invite | `smsgsendanon` | Encrypted | 7 days |
| Marketplace listing | `smsgsend` to topic | Plaintext | 7-31 days (paid) |
| Escrow PSBT exchange | `smsgsendanon` | Encrypted | 7 days |
| Trollbox | `trollboxsend` | None | 24 hours |

### §2.3 ZMQ real-time notifications

The daemon must be configured with ZMQ endpoints:

```ini
# omega.conf
zmqpubhashblock=tcp://0.0.0.0:7780
zmqpubrawtx=tcp://0.0.0.0:7780
zmqpubhashsmsg=tcp://0.0.0.0:7780  # SHA256(full_msg) on inbox receipt
zmqpubrawsmsg=tcp://0.0.0.0:7780   # full SMSG bytes on inbox receipt
```

The app subscribes to ZMQ topics for push-style notifications:
- `hashblock` — new block (update balance, confirmations)
- `rawtx` — new transaction (instant balance update)
- `hashsmsg` — new SMSG message hash (lightweight ping; app then polls `smsginbox` / `smsggetmessages`)
- `rawsmsg` — full SMSG bytes on receipt (topic hash visible in cleartext `nonce[0..3]`; use for topic routing)

**Fallback polling** (if ZMQ SMSG topic unavailable):

Battery-aware polling strategy using Android WorkManager:

| State | Interval | What is polled |
|-------|----------|----------------|
| App foreground, room open | 15 seconds | `smsggetmessages` for active room only |
| App foreground, trollbox open | 15 seconds | `trollboxlist` |
| App foreground, other tab | 30 seconds | `smsginbox` (unread count only) |
| App background | 5 minutes | `smsginbox` (via WorkManager periodic work) |
| App background + low battery | 15 minutes | `smsginbox` (via WorkManager, battery-not-low constraint) |
| Screen off / Doze | Deferred | WorkManager respects Doze; no wakeups |

- Only poll rooms that are currently visible in the UI
- Background polling uses `WorkManager` with `NetworkType.CONNECTED` constraint
- Foreground polling uses coroutine-based timer in the ViewModel
- If ZMQ is available, polling is disabled entirely (ZMQ push only)

---

## §3 Trollbox

### §3.1 What it is

A global, ephemeral public chat channel. All messages visible to all nodes.
No rooms, no moderation, no persistence beyond 24 hours. The omega-qt
desktop wallet already has trollbox support.

### §3.2 Constraints (from daemon implementation)

| Property | Value |
|----------|-------|
| Max message length | 256 characters |
| Message retention | 24 hours |
| Rate limit | 30 seconds between sends |
| Max displayed | 200 messages |
| Encryption | None (public by design) |
| Sender identity | Visible (address included) |

### §3.3 App implementation

- Dedicated tab in bottom navigation: "Trollbox"
- Single scrolling message list, newest at bottom
- Text input with character counter (256 max)
- Rate limit enforced client-side (grey out send for 30s)
- Sender shown as truncated address: `oXyz...abc`
- No reply threading (flat list)
- Messages auto-expire — no local persistence needed
- Poll via `trollboxlist 200` or ZMQ push

### §3.4 Privacy note

Trollbox messages include the sender's address. Users should be warned that
trollbox participation reveals their address to the network. For anonymous
discussion, use public rooms with anonymous posting enabled.

---

## §4 Public Discussion Rooms

### §4.1 Room creation (on-chain)

Rooms are created via `TRANSACTION_SMSG_ROOM` (special tx type 8), which
registers the room on the blockchain.

**CSmsgRoomTx payload:**
```
nVersion:       1
nFlags:         SMSG_ROOM_OPEN (0x01) | SMSG_ROOM_MODERATED (0x02)
vchRoomPubKey:  33-byte compressed pubkey (room encryption key)
nRetentionDays: 1-31 (default 31)
nMaxMembers:    0 = unlimited
```

**Room identity:** The txid of the creation transaction.

**Room types:**
- **Open room** (`SMSG_ROOM_OPEN`): Anyone may join and read
- **Moderated room** (`SMSG_ROOM_MODERATED`): Owner can censor/kick
- **Open + Moderated**: Anyone can join, but owner moderates

### §4.2 Room ownership

The creator of the room transaction holds the room private key. This key
grants:
- Ability to send moderation commands (kick, ban, invite)
- Ability to decrypt moderation-encrypted messages (if room is moderated)
- Ownership proof (signed by room key)

### §4.3 Room message flow — topic-based model

**CRITICAL DESIGN FIX:** SMSG encrypts all point-to-point messages to the
recipient's public key. If public room messages were sent to the room
address via `smsgsend`, only the room private key holder could decrypt them
— making "public" rooms effectively private. This is wrong.

**Solution:** Public rooms use **SMSG topic channels**, not point-to-point
addressing. Topic messages (version 4) use a shared deterministic key
derived from the topic string. All subscribers can read them.

**Room-to-topic mapping:**
- Each room registers a topic: `omega.room.<short_txid>` (first 12 hex
  chars of room creation txid)
- Topic is stored in the room's on-chain metadata (OP_RETURN)
- All room members subscribe to the topic via `smsgsubscribe`

**Sending a public room message (open room, plaintext):**
```
App → smsgsend <user_address> <room_topic_address> "message" false 7
      with topic parameter: omega.room.<short_txid>
```
All nodes subscribed to the topic can read the message. No private key
needed. Messages are visible to anyone who subscribes.

**Sending an anonymous public room message:**
```
App → smsgsendanon <room_topic_address> "message"
```
Sender identity is hidden, but message content is readable by all
subscribers (encrypted to the shared topic key, not a private key).

**Private (invite-only) room messages:**
For rooms WITHOUT `SMSG_ROOM_OPEN` flag, the room private key is
distributed only to invited members (via encrypted invite). Messages are
sent to the room address via `smsgsend` — only holders of the room
private key can decrypt.

| Room type | Message transport | Who can read |
|-----------|------------------|--------------|
| Open | Topic channel (`omega.room.<id>`) | Anyone subscribed |
| Open + Moderated | Topic channel | Anyone subscribed (kicked users filtered client-side) |
| Private (invite-only) | Point-to-point to room address | Only members with room private key |

**EAVESDROPPING WARNING:** Public room topics are readable by any node that
subscribes. A passive adversary can silently subscribe to all room topics
and record every message. Users must understand that **public room content
is not confidential**. The app must display a clear notice when joining an
open room: "Messages in this room are visible to anyone on the network."

For sensitive discussions in open rooms, consider an optional **end-to-end
encrypted mode**: distribute a symmetric key to participants out-of-band
(e.g. QR code), and encrypt message payloads before posting to the topic.
This is an app-layer overlay, not a protocol change. Deferred to Phase 5.

### §4.4 Room discovery

All room creation transactions are on-chain and publicly queryable. The
app maintains a "Room Directory" tab:

1. Scan blockchain for `TRANSACTION_SMSG_ROOM` (type 8) transactions
2. Parse `CSmsgRoomTx` payload from each
3. Extract topic name from OP_RETURN metadata
4. Display: room name, topic, last activity, retention period, flags
5. User taps "Join":
   - Open room → `smsgsubscribe "omega.room.<short_txid>"`
   - Private room → request invite from owner (out-of-band or via DM)
6. App stores joined room in local database

**Performance note:** Scanning the entire blockchain for type 8 transactions
is expensive on mobile. Mitigations:
- Cache room list in local SQLite; only scan new blocks since last check
- Daemon-side `smsglistrooms` RPC (D-11) should return indexed results,
  not require full chain scan
- Paginate directory results (50 rooms per page)
- Background refresh via WorkManager (not on every tab open)
- Clock skew between devices and daemon: use daemon's `getblockchaininfo`
  timestamp as authoritative time source, not device clock, for all
  expiry and retention calculations

### §4.5 Room expiry

Rooms have an adjustable expiry: **one week from the last message** by
default. Extended retention costs OMEGA (paid messages).

**Implementation (client + daemon hybrid):**
- App tracks `last_message_timestamp` per room in local SQLite
- Periodically queries daemon via `smsggetmessages <topic> 1` to get the
  actual latest message timestamp (guards against stale local state)
- If `now - last_message_timestamp > 7 days`, room is marked "expired"
- Expired rooms remain in directory but greyed out
- Room owner can revive by sending a new message (resets timer)
- Owner can pay for extended retention: `smsgsend ... true <days>`
- Old messages purged locally via `smsgpurge`
- If a new message arrives on an expired room topic, the room is
  automatically un-expired and the user is notified
- Auto-resubscribe to expired rooms if daemon receives a new message
  (daemon keeps topic subscription active even when app marks it expired)

### §4.6 Moderation (room owner powers)

| Action | Implementation |
|--------|---------------|
| **Invite** | Owner sends encrypted invite via `smsgsendanon` or QR code (see below) |
| **Kick** | Owner broadcasts signed kick command to room topic; app-layer enforcement |
| **Ban** | Owner broadcasts signed ban command; app maintains local ban list |
| **Mute** | App-layer only — client ignores messages from muted address |

**Kick/ban protocol:**
```json
{
  "type": "mod",
  "action": "kick|ban|unban",
  "target": "<address>",
  "reason": "optional reason",
  "timestamp": 1712000000,
  "sig": "<owner signature over SHA256(action+target+timestamp)>"
}
```
Clients verify the signature against the room owner's key. Kicked/banned
users' messages are filtered client-side. The network still relays them
(no protocol-level censorship), but compliant clients hide them.

**Moderation bypass:** A banned user can trivially create a new address and
rejoin an open room. Moderation is advisory, not enforceable. Document this
clearly in the app's room settings. For stronger enforcement, future work
could explore cryptographic group membership (e.g. group signatures or
anonymous credential-based blocklisting), but this is a research topic.

**Invitation privacy:**
Sending a room private key over SMSG means the invite persists in the
SMSG network (in bucket storage) for the message's TTL. If an attacker
later compromises the recipient's SMSG key, they could decrypt the invite
and obtain the room key.

Mitigations:
- **Preferred (in-person):** QR code scanned from the owner's device.
  The QR encodes `{room_txid, room_privkey_wif, topic}`. No network
  transit — zero exposure.
- **Remote invite:** Use `smsgsendanon` with shortest TTL (1 day).
  The invite auto-expires from the network quickly.
- **Ephemeral invite key (future):** Owner generates a one-time-use
  keypair for the invite, so compromising the invitee's main key does
  not expose the invite. Requires app-layer key negotiation.

**Moderation key resilience:**

The room owner's single private key is a single point of failure. If lost
or compromised, the room becomes unmoderable and ownership is irrecoverable.

Mitigations:
1. **Key backup** — room creation flow requires the owner to back up the
   room private key (WIF export via `smsgdumpprivkey`). Displayed once with
   the same warnings as mnemonic backup.
2. **Co-moderation** — owner can delegate moderation by sharing a secondary
   signing key with trusted moderators. Mod commands signed by either the
   owner key or a delegated mod key are accepted by clients.
   Delegation message (encrypted to moderator):
   ```json
   {"type": "mod_delegate", "mod_pubkey": "<pubkey>", "permissions": ["kick","ban"],
    "sig": "<owner signature>"}
   ```
3. **Key rotation (future, requires daemon change)** — a new special
   transaction type (`TRANSACTION_SMSG_ROOM_UPDATE`) that allows the owner
   to rotate the room key on-chain. The new key replaces the old one; all
   members are notified to update. This requires consensus-level changes
   and is deferred to a future daemon release.
4. **Multisig room ownership (future)** — room creation with a 2-of-3
   multisig owner key, so no single key loss kills the room. Requires
   `CSmsgRoomTx` payload extension. Deferred.

### §4.7 User privacy in rooms

- Users cannot follow other users (no "profile" concept)
- Users follow rooms only
- Each user generates a fresh address per room (unlinkable across rooms)
- Anonymous posting via `smsgsendanon` hides sender entirely
- Room member list is not broadcast (only the owner knows invitees for
  private rooms; open rooms have no member list at all)
- No read receipts, no typing indicators, no presence status

---

## §5 Marketplace with Escrow

### §5.1 Listing flow

Marketplace listings are SMSG messages sent to a well-known topic channel
(e.g., `omega.marketplace`).

**Listing message format:**
```json
{
  "type": "listing",
  "title": "Item description",
  "price_omega": 100.0,
  "category": "goods|services|digital",
  "images": ["<base64 thumbnail>"],
  "contact_room": "<room_txid>",
  "expiry": 1712000000,
  "seller_address": "<omega_address>"
}
```

Listings are public (unencrypted) and browseable by all nodes subscribed
to the marketplace topic.

**Address privacy:** If a seller reuses the same address across listings,
an observer can link all their listings (and transaction history) to one
identity. **The app MUST generate a fresh address for each listing.**
The `seller_address` field uses a one-time address; the `contact_room`
field uses an ephemeral room for buyer-seller communication.

**Listing fee (anti-spam):** Marketplace listings MUST use paid SMSG
messages. The economic cost of paid messages (fee per KB per day) deters
spam listings. Minimum recommended fee: equivalent of ~0.01 OMEGA per
listing. This is enforced by the app (set `paid_msg=true` in `smsgsend`).

### §5.2 Purchase flow (2-of-3 multisig escrow via PSBT)

Omega has confirmed PSBT support. The escrow uses a 2-of-3 multisig:
**buyer + seller + arbiter**.

```
Step 1: Buyer signals intent
        → smsgsendanon <seller_address> { "type": "buy", "listing_id": "..." }

Step 2: Agree on arbiter
        → Both parties exchange arbiter's public key via encrypted SMSG

Step 3: Create multisig escrow address
        → App calls: createmultisig 2 ["buyer_pubkey","seller_pubkey","arbiter_pubkey"]
        → Returns: { "address": "oEscrow...", "redeemScript": "..." }

Step 4: Buyer funds escrow
        → sendtoaddress "oEscrow..." <amount>

Step 5: Seller delivers goods/service

Step 6: Release — buyer signs PSBT releasing funds to seller
        → createpsbt [{"txid":"...","vout":0}] [{"seller_addr": amount}]
        → walletprocesspsbt <psbt>  (buyer signs)
        → Send partial PSBT to seller via smsgsendanon
        → Seller calls walletprocesspsbt (seller signs — 2 of 3 reached)
        → finalizepsbt → sendrawtransaction

Dispute: If buyer and seller disagree, arbiter + aggrieved party
         sign to release funds appropriately.
```

### §5.3 PSBT size and reliability

PSBTs can be large (especially with multiple inputs or complex scripts).
SMSG message limits:
- Free messages: 24 KB max
- Paid messages: 512 KB max

A typical 2-of-3 multisig PSBT is ~500-2000 bytes (well within limits).
However, for safety:

- **Size check:** App validates PSBT size before sending. If > 20 KB,
  use paid SMSG (`smsgsend ... true 7`).
- **Chunked messages (future):** If PSBTs ever exceed 24 KB (unlikely for
  escrow), implement chunked SMSG delivery with reassembly. Deferred.
- **Dedicated topic:** Escrow communication uses a per-trade topic
  (`omega.escrow.<trade_id>`) to keep messages organised and separable
  from room chat.
- **PSBT validation (anti-tampering):** Before signing any PSBT received
  from the counterparty, the app MUST validate:
  - Output addresses match the agreed escrow parameters
  - Output amounts match the agreed price
  - No unexpected inputs or outputs added
  - The PSBT was not modified in transit (compare against locally-stored
    trade parameters)
  - Use `decodepsbt` to inspect before `walletprocesspsbt` to sign
  - Never blindly sign a PSBT constructed by an untrusted party

### §5.4 Escrow timeouts

Without timeouts, funds can be locked indefinitely if one party disappears.

| Timeout | Duration | Action |
|---------|----------|--------|
| Funding timeout | 24 hours | If buyer does not fund escrow within 24h of agreement, trade is cancelled |
| Delivery timeout | 14 days (configurable) | If seller does not deliver within timeout, buyer + arbiter can release refund |
| Release timeout | 7 days | If buyer does not confirm receipt within 7 days after delivery, funds auto-release to seller (requires nLockTime PSBT) |
| Dispute window | 14 days from funding | Either party can raise dispute; after window closes, auto-release to seller |

**nLockTime auto-release:**
The escrow PSBT includes a pre-signed time-locked transaction that releases
funds to the seller after the dispute window. This is created at funding
time and signed by buyer + arbiter. If no dispute is raised, the
time-locked transaction becomes valid and the seller can broadcast it.

**Edge cases:**
- **Seller vanishes:** Buyer must be able to recover funds with arbiter's
  help BEFORE the nLockTime expiry. At funding time, create a second
  pre-signed refund PSBT (buyer + arbiter → buyer) with a shorter
  nLockTime. This gives buyer recourse if seller disappears.
- **Buyer vanishes after funding:** The seller's auto-release nLockTime
  ensures funds are not locked forever.
- **nLockTime calibration:** Set long enough for shipping (e.g. 14-30 days
  for physical goods) but not so long that funds are locked indefinitely.
  App presents duration options based on listing category:
  digital (3 days), goods (14 days), services (30 days).

### §5.5 Arbiter selection

- Initially: project-run arbiter address (centralised but transparent)
- **Multisig arbiter (recommended):** Use a 2-of-3 multisig among trusted
  community members as the arbiter key. No single arbiter key compromise
  can steal escrowed funds. The arbiter "entity" requires 2 of 3 arbiter
  members to agree.
- Future: reputation-based arbiter marketplace (decentralised)
- Arbiter fee: small percentage deducted from escrow amount
- Arbiter's public key is published on-chain or in a well-known SMSG topic
- **Arbiter key security:** Each arbiter member should store their key in
  hardware (e.g. hardware wallet or HSM). Multi-party signing procedures
  required for dispute resolution.
- **Transparency:** Arbiter identities (or pseudonyms + reputation) should
  be publicly disclosed. Arbiter keys should be rotated periodically.

### §5.6 Reputation and reviews (future)

There is no built-in trust/rating system. Buyers can be scammed. Mitigations:

- **Short-term:** Clearly warn users the marketplace is trustless. Recommend
  arbitration for all trades above a threshold.
- **Encrypted review channel:** After a trade completes, both parties
  exchange an encrypted rating via `smsgsendanon` (visible only to them).
  The app stores reviews locally per address — not broadcast.
- **Future (Phase 5+):** Pseudonymous reputation system. Aggregate review
  scores published to `omega.reputation` topic, linked to seller addresses
  (not real identities). Challenge: sellers can create fresh addresses to
  escape bad reviews. Requires economic stake or deposit to be meaningful.

---

## §6 App Screens & Navigation

### Bottom navigation tabs

```
┌─────────┬──────────┬───────────┬────────────┬────────┐
│  Rooms  │ Trollbox │ Directory │ Marketplace│ Wallet │
└─────────┴──────────┴───────────┴────────────┴────────┘
```

### §6.1 Rooms tab
- List of joined rooms, sorted by last message
- Unread badge per room
- Tap → room chat view (message list + input)
- Long-press → room settings (leave, mute, notifications)
- FAB → "Create Room" flow

### §6.2 Trollbox tab
- Single flat chat stream
- Auto-scrolling to newest
- Character counter on input
- Rate limit indicator

### §6.3 Directory tab
- Searchable list of all public rooms (from blockchain scan)
- Filter: open/moderated, active/expired, member count
- "Join" button per room
- Room metadata: name, creation date, retention, flags

### §6.4 Marketplace tab
- Browse listings by category
- Search by keyword
- "New Listing" flow (compose + publish to topic)
- Listing detail → "Buy" initiates escrow flow
- "My Listings" / "My Purchases" sub-tabs

### §6.5 Wallet tab
- Balance display (confirmed / unconfirmed / locked)
- Send / Receive buttons
- Transaction history list
- Receive → QR code with `omega:` URI
- Send → address input + amount + fee estimate

### §6.6 Settings (drawer or gear icon)
- Daemon connection (host, port, credentials, Tor toggle)
- SMSG key management (backup, import, list)
- Wallet seed backup
- Notification preferences
- About / version

---

## §7 Technology Stack

| Layer | Technology | Rationale |
|-------|-----------|-----------|
| Language | Kotlin | Android-native, coroutine support |
| UI | Jetpack Compose | Modern declarative UI |
| Local DB | Room + **SQLCipher** | Encrypted local database (see §7.1) |
| Networking | OkHttp + Ktor | JSON-RPC client with TLS/cert pinning |
| ZMQ | JeroMQ (pure Java ZMQ) | No native lib dependency |
| Crypto | Tink / SpongyCastle | Key derivation, local encryption |
| Credentials | EncryptedSharedPreferences | Android Keystore-backed credential storage |
| Biometric | BiometricPrompt (API 28+) | Optional biometric unlock for credentials |
| QR codes | ZXing | Address sharing |
| Image loading | Coil | Marketplace thumbnails |
| DI | Hilt | Dependency injection |
| Tor | **tor-android** (embedded) | Built-in Tor, no Orbot dependency (see §7.2) |
| Background | WorkManager | Battery-aware background polling |
| KDF | **Argon2id** (via `com.lambdapioneer.argon2kt`) | Memory-hard key derivation for backups |
| Static analysis | CodeQL + Android Lint | Security scanning from Phase 1 |
| Dependency audit | OWASP Dependency-Check | CVE scanning of all transitive dependencies |

### §7.1 Local database encryption (SQLCipher)

All local data (messages, contacts, room state, transaction cache) is stored
in SQLite via Room. **The database MUST be encrypted** using SQLCipher:

- SQLCipher integrates with Room via `net.zetetic:android-database-sqlcipher`
- Database key derived from Android Keystore (device-bound)
- Optionally: user-provided passphrase on top of Keystore key
- If device is compromised, attacker cannot read message history, contacts,
  or transaction cache without the Keystore key
- First-launch: generate a random 256-bit key, store in Android Keystore
- Migration path: if user upgrades from unencrypted beta, migrate in-place

### §7.2 Embedded Tor (no Orbot dependency)

Relying on Orbot requires users to install and configure a separate app.
This is a usability and reliability problem.

**Solution:** Embed Tor directly using `tor-android` (Guardian Project):
- Tor binary bundled inside the APK
- App manages the Tor process lifecycle internally
- Simple toggle in settings: "Route all connections through Tor"
- When enabled: RPC, ZMQ, and all network traffic routed via SOCKS5 proxy
- Tor bootstrap status shown in connection screen
- No external app dependency
- **Circuit isolation:** Use separate Tor circuits per room/topic to prevent
  a malicious exit node from correlating traffic across rooms. Achieved via
  `IsolateSOCKSAuth` with unique credentials per circuit.
- **DNS leak prevention:** All DNS resolution MUST go through Tor (SOCKS5
  with remote DNS). No system DNS queries when Tor is enabled. Verify
  with integration test that no non-Tor traffic escapes.
- Regularly rotate circuits (new identity every 10 minutes or on demand)

### §7.3 Analytics and telemetry policy

**Zero third-party analytics.** No Google Analytics, Firebase, Crashlytics,
or any external tracking SDK. This is a privacy-first app.

- Crash reporting: **disabled by default**. Optional opt-in to self-hosted
  Sentry instance (if project runs one) or local crash log export.
- No usage telemetry, no event tracking, no user profiling
- All dependencies audited for embedded trackers before inclusion
- Only open-source libraries used
- **OWASP Dependency-Check** integrated in CI to scan all transitive
  dependencies for known CVEs. Block builds on HIGH/CRITICAL findings.
- Cryptographic libraries (Tink, SpongyCastle, Argon2kt) pinned to
  specific versions and updated quarterly

---

## §8 Daemon-Side Requirements

Before the app can function, the daemon needs:

| ID | Requirement | Status |
|----|------------|--------|
| D-01 | SMSG enabled and functional | 🟢 Done |
| D-02 | Topic channels (subscribe/publish) | 🟢 Done |
| D-03 | Trollbox RPC | 🟢 Done |
| D-04 | TRANSACTION_SMSG_ROOM (type 8) | 🟢 Done |
| D-05 | PSBT support | 🟢 Done |
| D-06 | ZMQ compiled in | 🟢 Done |
| D-07 | ZMQ SMSG topic (`zmqpubhashsmsg` / `zmqpubrawsmsg`) | 🟢 Built in 0.20.3 (R-09/R-10/R-11) |
| D-08 | `createmultisig` RPC | 🔴 Needs testing |
| D-09 | `addmultisigaddress` RPC | 🔴 Needs testing |
| D-10 | BIP157/158 filter serving | 🟡 Code present, needs testing |
| D-11 | Room metadata RPC (list rooms, get room info) | 🟢 Built in 0.20.3 (R-06/R-07) — `smsglistrooms`, `smsggetroominfo` |
| D-12 | `nSmsgRoomHeight` activation height set | 🟢 **RESOLVED: 3,200,000 mainnet, 100 testnet** |
| D-13 | TLS support for RPC | 🔴 Evaluate stunnel or built-in |
| D-14 | ZMQ TLS or tunnel documentation | 🔴 Document secure ZMQ setup |
| D-15 | `smsgzmqpush` — replaced by native ZMQ publisher | 🟢 Superseded — native `zmqpubhashsmsg` / `zmqpubrawsmsg` built |
| D-16 | Room key rotation tx (future `SMSG_ROOM_UPDATE`) | 🔴 Design needed |
| D-17 | SMSG code security audit (Particl port review) | 🔴 Required before launch |
| D-18 | PoW difficulty increase or per-message fee (anti-spam) | 🔴 Design needed |
| D-19 | RPC auth rate-limiting (brute-force protection) | 🔴 Evaluate |

### D-11: Room metadata RPC (built in 0.20.3)

```
smsglistrooms [flags_filter]
    → Returns all TRANSACTION_SMSG_ROOM txids with parsed CSmsgRoomTx fields
    → Backed by SmsgRoomIndex (LevelDB, BaseIndex pattern) — no full chain scan

smsggetroominfo <room_txid>
    → Returns room details: flags, pubkey, retention, height, confirmations,
      topic (omega.room.<txid[0:12]>), last_message_timestamp
```

Both built in `src/smsg/rpcsmessage.cpp`. Index in `src/index/smsgroomindex.h/.cpp`.

---

## §9 Development Phases

### Phase 1 — Foundation + Security Baseline (weeks 1-4)

Security is not a Phase 4 concern. Critical security infrastructure must
be in place from the first build.

| ID | Task | Priority |
|----|------|----------|
| P1-01 | Android project scaffold (Kotlin, Compose, Hilt, Room + SQLCipher) | MUST |
| P1-02 | RPC client library (JSON-RPC 2.0 over HTTP/TLS, TOFU cert pinning) | MUST |
| P1-03 | Daemon connection screen (host, port, auth, test button) | MUST |
| P1-04 | ZMQ subscriber service (background, reconnect, tunnelled only) | MUST |
| P1-05 | Wallet tab: balance, send, receive, tx history | MUST |
| P1-06 | Basic SMSG: send/receive direct messages | MUST |
| P1-07 | Local SQLite schema (encrypted via SQLCipher): messages, rooms, contacts, settings | MUST |
| P1-08 | Android Keystore credential storage (EncryptedSharedPreferences) | MUST |
| P1-09 | Embedded Tor (tor-android) with toggle in connection screen | MUST |
| P1-10 | CodeQL + Android Lint integration in CI pipeline | MUST |
| P1-11 | One-click daemon deployment script (`omega-deploy.sh`) | SHOULD |
| P1-12 | Integration tests for RPC client (mock daemon, TLS, auth) | MUST |
| P1-13 | OWASP Dependency-Check in CI pipeline | MUST |
| P1-14 | Tor DNS leak integration test | MUST |
| P1-15 | RPC rate-limiting / credential brute-force protection | SHOULD |

### Phase 2 — Rooms & Trollbox (weeks 5-8)

| ID | Task | Priority |
|----|------|----------|
| P2-01 | Trollbox tab: list + send + rate limit | MUST |
| P2-02 | Room creation flow (TRANSACTION_SMSG_ROOM via topic model) | MUST |
| P2-03 | Room chat view (message list via `smsggetmessages`, send, scroll) | MUST |
| P2-04 | Per-room keypair generation (fresh address per room) | MUST |
| P2-05 | Room directory tab (scan chain for room txs via `smsglistrooms`) | MUST |
| P2-06 | Room join flow (subscribe to topic for open; receive invite for private) | MUST |
| P2-07 | Room expiry logic (7-day timer, daemon-verified, auto-resubscribe) | MUST |
| P2-08 | Moderation: kick/ban protocol + client enforcement + co-moderator delegation | SHOULD |
| P2-09 | Room invite flow (encrypted key exchange via `smsgsendanon`) | SHOULD |
| P2-10 | Anonymous posting toggle per room | SHOULD |
| P2-11 | SMSG key backup/restore screen + room key backup | MUST |
| P2-12 | Room invite via QR code (in-person, zero network exposure) | SHOULD |
| P2-13 | In-app warning on joining open rooms (eavesdropping notice) | MUST |
| P2-14 | Jittered polling intervals (±20% random) | MUST |
| P2-15 | Security review checkpoint: room crypto, key handling, topic model | MUST |

### Phase 3 — Marketplace (weeks 9-12)

| ID | Task | Priority |
|----|------|----------|
| P3-01 | Marketplace topic subscription (`omega.marketplace`) | MUST |
| P3-02 | Browse listings UI (search, filter, categories) | MUST |
| P3-03 | Create listing flow (compose, publish to topic) | MUST |
| P3-04 | 2-of-3 multisig escrow address creation | MUST |
| P3-05 | PSBT signing flow (create, sign, exchange, finalise) | MUST |
| P3-06 | Purchase intent → escrow → release UI flow | MUST |
| P3-07 | Escrow timeout logic (nLockTime auto-release) | MUST |
| P3-08 | Dispute flow (arbiter involvement) | SHOULD |
| P3-09 | Listing expiry and refresh | SHOULD |
| P3-10 | Per-trade escrow topic (`omega.escrow.<trade_id>`) | SHOULD |
| P3-11 | PSBT size validation before SMSG send | MUST |
| P3-12 | PSBT field validation before signing (anti-tampering) | MUST |
| P3-13 | Fresh address per marketplace listing (enforced by app) | MUST |
| P3-14 | Paid SMSG required for listings (anti-spam) | MUST |
| P3-15 | Escrow refund PSBT (buyer + arbiter, shorter nLockTime) | MUST |
| P3-16 | Security review checkpoint: escrow flow, PSBT handling, arbiter key | MUST |

### Phase 4 — Hardening & Polish (weeks 13-16)

| ID | Task | Priority |
|----|------|----------|
| P4-01 | Push notifications (FCM-free foreground service) | SHOULD |
| P4-02 | Offline message queue (send when reconnected) | SHOULD |
| P4-03 | Notification channels per room | SHOULD |
| P4-04 | Message search (local SQLite FTS over encrypted DB) | SHOULD |
| P4-05 | Battery optimisation: adaptive polling via WorkManager | MUST |
| P4-06 | Trusted public node fallback mode (with privacy warning) | SHOULD |
| P4-07 | Wallet seed backup flow with screenshot warning | MUST |
| P4-08 | Encrypted wallet/SMSG key export (Argon2id + AES-256-GCM) | MUST |
| P4-09 | Automated UI tests | SHOULD |
| P4-10 | Full security audit: all crypto, all storage, all network paths | MUST |
| P4-11 | Biometric unlock option (BiometricPrompt) | SHOULD |

### Phase 5 — Future (post-launch)

| ID | Task | Priority |
|----|------|----------|
| P5-01 | BIP157/158 autonomous light wallet (no daemon required) | COULD |
| P5-02 | SMSG light client (partial bucket sync) | COULD |
| P5-03 | BitHome real estate integration (Nostr bridge) | COULD |
| P5-04 | iOS port (Kotlin Multiplatform or Swift rewrite) | COULD |
| P5-05 | Desktop Electron/Tauri companion app | COULD |
| P5-06 | Decentralised arbiter reputation system | COULD |
| P5-07 | Voice messages (compressed audio over SMSG paid) | COULD |
| P5-08 | File sharing (chunked over paid SMSG) | COULD |
| P5-09 | Room key rotation on-chain (SMSG_ROOM_UPDATE tx) | COULD |
| P5-10 | Multisig room ownership | COULD |
| P5-11 | DM session ratchet (PFS for direct messages) | COULD |
| P5-12 | Optional E2E encrypted mode for open rooms (symmetric key overlay) | COULD |
| P5-13 | Cover traffic (dummy SMSG heartbeats for traffic analysis resistance) | COULD |
| P5-14 | Pseudonymous reputation system (`omega.reputation` topic) | COULD |
| P5-15 | Cryptographic group membership for stronger moderation | COULD |

---

## §10 Privacy & Security Threat Model

| # | Threat | Severity | Mitigation |
|---|--------|----------|-----------|
| T-01 | Daemon operator sees messages | HIGH | User runs own daemon — no third party. Public node fallback has explicit privacy warning. |
| T-02 | Network observer correlates addresses | HIGH | Fresh address per room; `smsgsendanon` for DMs; no reuse across rooms |
| T-03 | Room member enumeration | MEDIUM | No member list broadcast; open rooms have no roster; private rooms invite-only |
| T-04 | User tracking across rooms | HIGH | Separate keypair per room (unlinkable); no global identity or profile |
| T-05 | IP address leakage | HIGH | Embedded Tor for all connections; daemon accessible only via .onion or LAN |
| T-06 | Message metadata (timing) | MEDIUM | SMSG bucket relay adds natural mixing delay (60s buckets); no read receipts |
| T-07 | Local device compromise — DB | HIGH | SQLCipher-encrypted database; Android Keystore-bound key |
| T-08 | Local device compromise — credentials | HIGH | EncryptedSharedPreferences; optional biometric gate (BiometricPrompt) |
| T-09 | Local device compromise — screenshots | MEDIUM | Screenshot warning during mnemonic/key display; `FLAG_SECURE` on sensitive screens |
| T-10 | Lost/stolen device | MEDIUM | Remote daemon unaffected; app credentials require Keystore (device-bound); no private keys on device |
| T-11 | Trollbox identity leakage | LOW | Warning shown before first trollbox post; address visible by design |
| T-12 | RPC interception (MITM) | HIGH | TLS with TOFU cert pinning; Tor .onion provides built-in authentication |
| T-13 | ZMQ interception | HIGH | ZMQ tunnelled through Tor/VPN/stunnel; never exposed on plain TCP to internet |
| T-14 | Mnemonic interception over RPC | HIGH | Prefer daemon-local seed generation; if RPC restore needed, TLS/Tor mandatory |
| T-15 | Third-party analytics leakage | MEDIUM | Zero analytics SDKs; all dependencies audited; no Firebase/Crashlytics |
| T-16 | Arbiter key compromise | HIGH | Multisig arbiter (2-of-3); nLockTime auto-release limits exposure window |
| T-17 | Room moderation key loss | MEDIUM | Mandatory key backup at creation; co-moderator delegation; key rotation (future) |
| T-18 | PSBT tampering in transit | MEDIUM | PSBT exchanged via encrypted SMSG (`smsgsendanon`); app validates PSBT fields before signing |
| T-19 | Stale room expiry (offline app) | LOW | Daemon-side verification via `smsggetmessages`; auto-resubscribe on new activity |
| T-20 | No PFS — long-term key compromise decrypts all past messages | HIGH | Per-room addresses limit blast radius; rotate SMSG keys periodically; DM ratchet (Phase 5+). See §2.0. |
| T-21 | Public room eavesdropping (passive subscription) | MEDIUM | Documented by design; in-app warning on join; optional E2E overlay for sensitive topics (Phase 5+) |
| T-22 | Spam flooding (rooms / marketplace) | MEDIUM | Paid messages for listings; app-side rate limit per address; future daemon PoW increase. See §2.0. |
| T-23 | Room invite exposes private key in SMSG network | MEDIUM | QR code preferred; short TTL for SMSG invites (1 day); ephemeral invite keys (future). See §4.6. |
| T-24 | Marketplace seller identity linking via address reuse | HIGH | Fresh address per listing enforced by app; fresh room per trade. See §5.1. |
| T-25 | Traffic analysis (polling timing correlation) | MEDIUM | Jittered polling intervals (±20% random); Tor circuit isolation; cover traffic (future). See below. |
| T-26 | Moderation bypass via new address | LOW | Advisory only; documented in app. Cryptographic group membership (research, Phase 5+). |
| T-27 | DNS leaks bypassing Tor | HIGH | Remote DNS via SOCKS5; no system DNS when Tor enabled; integration test verification. See §7.2. |
| T-28 | PSBT manipulation by counterparty | MEDIUM | Validate all PSBT fields before signing; compare against locally-stored trade params. See §5.3. |
| T-29 | Dependency supply chain (compromised library) | MEDIUM | OWASP Dependency-Check in CI; pinned versions; open-source only. See §7.3. |

### §10.1 Adversary model

| Adversary | Capability | Primary threats |
|-----------|-----------|-----------------|
| Passive network observer | Sees traffic patterns, IP addresses, message timing | T-05, T-06, T-25 |
| Malicious SMSG node | Subscribes to topics, records all public messages, correlates metadata | T-01, T-21, T-24 |
| Compromised daemon operator | Full access to RPC, wallet, SMSG keys | T-01, T-14, T-20 |
| Device malware / thief | Access to local storage, Android Keystore (if rooted) | T-07, T-08, T-09, T-10 |
| Counterparty in trade | Crafts malicious PSBTs, attempts to steal escrow | T-16, T-28 |
| Banned room user | Creates new addresses, re-joins rooms | T-26 |

### §10.2 Behavioural obfuscation (traffic analysis resistance)

Fixed polling intervals (15s, 30s) create detectable patterns. An attacker
monitoring network traffic can correlate exact timing with user activity.

**Mitigations:**
- **Jittered polling:** Add ±20% random jitter to all poll intervals.
  E.g. 15s ± 3s = 12-18s random uniform.
- **Cover traffic (optional, future):** When Tor is enabled, send periodic
  encrypted SMSG heartbeat messages (empty payload, self-addressed) at
  random intervals. Makes real messages indistinguishable from cover.
- **Burst suppression:** When opening a room, stagger message fetches
  rather than fetching all at once (avoids traffic spike = "user opened
  room X at time T").
- **Tor circuit rotation:** New circuits every 10 minutes; per-room
  circuit isolation via `IsolateSOCKSAuth`.

---

## §11 BitHome Integration Decision

**Decision: Keep separate. Revisit at Phase 5.**

BitHome is a Nostr-based real estate listing protocol (v0.1.0-alpha). It
uses `kind:30023` Nostr events, not SMSG. Integrating it now would:

- Add Nostr relay dependency (different P2P network)
- Mix privacy models (Nostr is pubkey-identity-centric)
- Build on an unstable specification
- Increase app complexity with no immediate user benefit

**Future bridge option (P5-03):** When BitHome matures, add an
`omega.realestate` SMSG topic that mirrors BitHome listings from Nostr
relays. This is a plugin adapter, not a core dependency. The marketplace
escrow (§5) works independently of listing source.

---

## §12 Open Questions

| ID | Question | Blocking | Action |
|----|----------|----------|--------|
| Q-01 | Maximum SMSG `days_retention` value? | P2-07 | Test `smsgsend ... true 30` then higher on live daemon |
| Q-02 | ~~Does `zmqpubsmsg` topic exist in daemon?~~ | ~~P1-04~~ | **RESOLVED: Built `zmqpubhashsmsg` + `zmqpubrawsmsg` in 0.20.3.** Needs testnet verification (R-21). |
| Q-03 | ~~`nSmsgRoomHeight` — what activation height?~~ | ~~P2-02~~ | **RESOLVED: 3,200,000 mainnet, 100 testnet.** Set in `chainparams.cpp:169,387`. |
| Q-04 | `createmultisig` — confirmed working? | P3-04 | Test on live daemon; if broken, implement local multisig address creation |
| Q-05 | ~~Room metadata RPCs — who builds them?~~ | ~~P2-05~~ | **RESOLVED: Built `smsgcreateroom`, `smsglistrooms`, `smsggetroominfo`, `smsgjoinroom` in 0.20.3.** |
| Q-06 | Arbiter selection — project-run or community? | P3-08 | Start with project-run 2-of-3 multisig arbiter; publish pubkeys in `omega.arbiters` topic |
| Q-07 | SMSG paid message fee rate — current value? | P2-07 | Retrieve via `smsggetfeerate` RPC; display to user before sending paid messages |
| Q-08 | ~~Android minimum API level~~ | ~~P1-01~~ | **RESOLVED: API 28 (Android 9.0)** — required for BiometricPrompt and modern Keystore |
| Q-09 | ~~Room OP_RETURN metadata format — what fields?~~ | ~~P2-02~~ | **RESOLVED: `smsgcreateroom` writes OP_RETURN with room name (max 64 chars). Topic derived from txid: `omega.room.<txid[0:12]>`.** |
| Q-10 | ~~`smsgzmqpush` — does it work for real-time?~~ | ~~P1-04~~ | **RESOLVED: Superseded by native `zmqpubhashsmsg` / `zmqpubrawsmsg` publishers.** `smsgzmqpush` remains for ad-hoc query. |
| Q-11 | Tor bootstrap time — acceptable for UX? | P1-09 | Test embedded tor-android cold start latency; target < 15 seconds |
| Q-12 | Known Particl SMSG bugs or CVEs? | D-17 | Review Particl issue tracker and changelogs for SMSG-related fixes |
| Q-13 | PoW difficulty increase — consensus change needed? | D-18 | Evaluate impact; may require hard fork or soft activation |
| Q-14 | Argon2id library for Android — which one? | P4-08 | Evaluate `argon2kt` (Kotlin wrapper) vs `com.lambdapioneer.argon2kt` |

---

## §13 Build Environment & Plan of Action

### §13.1 Build environment

**OS:** Ubuntu 24.04 LTS (Noble Numbat)

#### Android app build machine

```
Minimum: 8 GB RAM, 50 GB free disk, x86_64
Recommended: 16 GB RAM, SSD

Packages:
  sudo apt update && sudo apt install -y \
      openjdk-17-jdk git curl unzip wget

Android SDK/NDK (command-line tools):
  mkdir -p ~/android-sdk/cmdline-tools
  wget https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
  unzip commandlinetools-linux-*.zip -d ~/android-sdk/cmdline-tools/latest
  export ANDROID_HOME=~/android-sdk
  export PATH=$PATH:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools

  sdkmanager --licenses
  sdkmanager "platforms;android-34" "build-tools;34.0.0" \
             "platform-tools" "ndk;26.1.10909125"

Environment variables (~/.bashrc):
  export ANDROID_HOME=~/android-sdk
  export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
```

#### Omega daemon build machine (for deployment scripts & testing)

```
Same Ubuntu 24.04 LTS box or separate.

Daemon build dependencies (Dash 0.19 lineage):
  sudo apt install -y build-essential libtool autotools-dev automake \
      pkg-config bsdmainutils python3 libssl-dev libevent-dev \
      libboost-system-dev libboost-filesystem-dev libboost-test-dev \
      libboost-thread-dev libboost-chrono-dev libboost-program-options-dev \
      libzmq3-dev libminiupnpc-dev libdb5.3++-dev libdb5.3-dev \
      libsqlite3-dev qtbase5-dev qttools5-dev-tools qttools5-dev \
      libqrencode-dev libprotobuf-dev protobuf-compiler

Daemon build:
  cd /home/rob/omegacoin
  ./autogen.sh
  ./configure --with-zmq --enable-wallet
  make -j$(nproc)

Cross-compile for ARM64 (Raspberry Pi / VPS):
  sudo apt install -y g++-aarch64-linux-gnu
  # Use depends/ system:
  cd depends && make HOST=aarch64-linux-gnu -j$(nproc)
  cd .. && ./configure --prefix=$(pwd)/depends/aarch64-linux-gnu --with-zmq
  make -j$(nproc)
```

#### CI pipeline (GitHub Actions recommended)

```yaml
# .github/workflows/android.yml
runs-on: ubuntu-24.04
env:
  JAVA_VERSION: 17
  ANDROID_COMPILE_SDK: 34
  ANDROID_BUILD_TOOLS: 34.0.0
steps:
  - uses: actions/setup-java@v4
    with: { java-version: 17, distribution: temurin }
  - uses: android-actions/setup-android@v3
  - run: ./gradlew assembleDebug lint
  - run: ./gradlew dependencyCheckAnalyze   # OWASP
```

---

### §13.2 Plan of action — what to do and in what order

This is the concrete execution sequence. Each step has a clear
deliverable. Steps within a group can be parallelised.

#### WEEK 0 — Prerequisites (before writing any app code)

**All daemon prerequisites are now tracked in §14 (Omega 0.20.3 release
plan).** The 0.20.3 release must ship before mainnet block 3,200,000 and
must be validated on testnet (fork at block 100) first.

| # | Action | Deliverable | Status |
|---|--------|-------------|--------|
| 0-1 | Build ZMQ SMSG publisher (R-09/R-10/R-11 in §14) | `zmqpubhashsmsg` + `zmqpubrawsmsg` | ✅ Built |
| 0-2 | Test `days_retention` max on testnet (R-13) | Q-01 answered | 🔴 Test on testnet |
| 0-3 | Test `createmultisig` on testnet (R-14) | Q-04 answered | 🔴 Test on testnet |
| 0-4 | Set `nSmsgRoomHeight` in `chainparams.cpp` | **DONE** — 3,200,000 mainnet, 100 testnet | ✅ |
| 0-5 | Build `smsgcreateroom` RPC (R-05) | Room creation convenience | ✅ Built |
| 0-6 | Build `smsglistrooms` + `smsggetroominfo` RPCs (R-06/R-07) | Room directory backend | ✅ Built |
| 0-7 | Build room tx index (R-12) | Fast room queries | ✅ Built |
| 0-8 | Review Particl SMSG issue tracker (Q-12) | Known bugs listed | 🔴 Pending |
| 0-9 | Reset testnet, mine to block 100, run full test suite | All Qs answered | 🔴 Pending |

**Gate:** 0.20.3 must be released and all tests passing before Phase 1
app development begins. App development starts after 0.20.3-rc1 is tagged.

#### WEEK 1 — Project scaffold + build pipeline

| # | Action | Deliverable |
|---|--------|-------------|
| 1-1 | Create Android project: Kotlin, Compose, Hilt, Room+SQLCipher, Gradle | Compiles on Ubuntu 24.04; empty app runs on emulator |
| 1-2 | Set up GitHub repo + CI (GitHub Actions: build, lint, CodeQL, OWASP Dependency-Check) | Green CI on every push |
| 1-3 | Implement `OmegaRpcClient` class: JSON-RPC 2.0 over OkHttp, TLS, TOFU cert pinning | Unit tests pass against mock server |
| 1-4 | Implement `EncryptedPrefsStore`: Android Keystore credential storage | Integration test: store/retrieve/wipe credentials |
| 1-5 | Write `omega-deploy.sh` v0.1 (installs daemon + Tor on Ubuntu 24.04 VPS) | Script tested on a fresh Hetzner CX22 |

#### WEEK 2 — Connection + Tor + ZMQ

| # | Action | Deliverable |
|---|--------|-------------|
| 2-1 | Daemon connection screen (host, port, user, pass, Tor toggle, Test button) | User can configure and test a connection |
| 2-2 | Embed `tor-android`; Tor bootstrap; SOCKS5 proxy for OkHttp + JeroMQ | App connects to `.onion` daemon |
| 2-3 | DNS leak integration test (verify no non-Tor traffic) | Test in CI |
| 2-4 | ZMQ subscriber service: `hashblock`, `rawtx`, (optionally `smsg`) | Background service reconnects and logs events |
| 2-5 | Fallback polling service with jittered intervals (WorkManager) | Battery-safe polling when ZMQ unavailable |

#### WEEK 3 — Wallet tab

| # | Action | Deliverable |
|---|--------|-------------|
| 3-1 | Wallet tab: `getbalances` display, pull-to-refresh | Balance shown correctly |
| 3-2 | Receive screen: `getnewaddress` + QR code (ZXing) | QR displayed with `omega:` URI |
| 3-3 | Send screen: address input, amount, `estimatesmartfee`, `sendtoaddress` | Test send on testnet |
| 3-4 | Transaction history: `listtransactions` + local cache (SQLCipher) | Scrollable list, offline viewing |
| 3-5 | HD wallet onboarding: `upgradetohd` + `dumphdinfo` + mnemonic display with warnings | Full flow works on testnet daemon |

#### WEEK 4 — Basic SMSG + direct messages

| # | Action | Deliverable |
|---|--------|-------------|
| 4-1 | SMSG enable check (`smsggetinfo`), key registration (`smsgaddlocaladdress`) | App auto-enables SMSG on connect |
| 4-2 | Send DM: `smsgsendanon` UI (recipient address, message, send button) | Message arrives on test daemon |
| 4-3 | Receive DMs: `smsginbox` polling / ZMQ push → local SQLCipher storage | Incoming messages display in conversation view |
| 4-4 | Conversations list (group by sender address, unread badge) | Basic chat UX working |
| 4-5 | Phase 1 security review checkpoint | Checklist: TLS, Tor, SQLCipher, Keystore all verified |

#### WEEKS 5-6 — Trollbox + rooms

| # | Action | Deliverable |
|---|--------|-------------|
| 5-1 | Trollbox tab: `trollboxlist` + `trollboxsend`, rate limit, character counter | Matches omega-qt trollbox |
| 5-2 | Room creation flow: construct `TRANSACTION_SMSG_ROOM` (type 8), submit | Room tx confirmed on chain |
| 5-3 | Room directory: `smsglistrooms` → paginated list, join button | User can browse and join rooms |
| 5-4 | Room chat: `smsgsubscribe` topic, `smsggetmessages`, send via topic | Messages flow in open room |
| 5-5 | Per-room keypair generation (fresh address per room via `getnewaddress`) | Addresses not reused across rooms |
| 5-6 | Open room eavesdropping warning dialog on first join | User must acknowledge before seeing messages |

#### WEEKS 7-8 — Room moderation + invites + expiry

| # | Action | Deliverable |
|---|--------|-------------|
| 7-1 | Room expiry: track `last_message_timestamp`, daemon-verified, auto-resubscribe | Expired rooms greyed out; revived on new message |
| 7-2 | Kick/ban protocol: JSON mod commands, signature verification, client-side filter | Banned users' messages hidden |
| 7-3 | Room invite via `smsgsendanon` (short TTL) + QR code alternative | Both invite methods work |
| 7-4 | Co-moderator delegation (`mod_delegate` message) | Delegated moderator can kick/ban |
| 7-5 | SMSG key backup/restore screen + room key WIF export | User can back up and restore all keys |
| 7-6 | Anonymous posting toggle per room | Toggle in room settings |
| 7-7 | Phase 2 security review checkpoint | Room crypto, key handling, topic model reviewed |

#### WEEKS 9-12 — Marketplace + escrow

| # | Action | Deliverable |
|---|--------|-------------|
| 9-1 | Marketplace topic subscription (`omega.marketplace`) + browse UI | Listings display with search/filter |
| 9-2 | Create listing flow: fresh address, paid SMSG, compose + publish | Listing visible to other nodes |
| 9-3 | 2-of-3 multisig escrow: `createmultisig`, fund, refund PSBT | Escrow address created and funded on testnet |
| 9-4 | PSBT signing flow: create, validate fields, sign, exchange via `smsgsendanon`, finalise | Full escrow round-trip on testnet |
| 9-5 | nLockTime auto-release + refund PSBT (buyer+arbiter, shorter timelock) | Both timeout paths tested |
| 9-6 | Per-trade escrow topic (`omega.escrow.<trade_id>`) | Escrow messages separated from room chat |
| 9-7 | Phase 3 security review checkpoint | Escrow flow, PSBT validation, arbiter key reviewed |

#### WEEKS 13-16 — Hardening, polish, audit

| # | Action | Deliverable |
|---|--------|-------------|
| 13-1 | Battery optimisation pass: adaptive polling via WorkManager | Battery usage profiled and acceptable |
| 13-2 | Encrypted backup export (Argon2id + AES-256-GCM) | Backup/restore round-trip verified |
| 13-3 | Biometric unlock option | BiometricPrompt gates app launch |
| 13-4 | Message search (SQLite FTS5 over SQLCipher) | Full-text search across all conversations |
| 13-5 | Offline message queue (outbox, send on reconnect) | Messages queued and sent after reconnect |
| 13-6 | Full security audit: all crypto, storage, network paths, dependencies | Written audit report with findings |
| 13-7 | SMSG daemon code audit (D-17): review Particl port diffs | Written audit report |
| 13-8 | Automated UI tests (Espresso / Compose test) | Core user flows covered |
| 13-9 | `omega-deploy.sh` v1.0: polished, tested on Hetzner + Raspberry Pi | Documented, one-command deploy |

#### POST-LAUNCH — first actions

| # | Action |
|---|--------|
| L-1 | Publish to F-Droid (open-source, no Google Play dependency) |
| L-2 | Publish APK on GitHub Releases (signed, reproducible build) |
| L-3 | Set up community arbiter multisig (2-of-3, keys in hardware) |
| L-4 | Begin Phase 5 triage: prioritise BIP157/158 light wallet vs DM ratchet |

---

### §13.3 Repository structure (proposed)

```
omega-messenger/
├── app/                          # Android app module
│   ├── src/main/java/org/omega/messenger/
│   │   ├── data/                 # Repository, RPC client, SQLCipher DB
│   │   ├── domain/               # Use cases (SendMessage, JoinRoom, CreateEscrow)
│   │   ├── ui/                   # Compose screens per tab
│   │   │   ├── wallet/
│   │   │   ├── rooms/
│   │   │   ├── trollbox/
│   │   │   ├── directory/
│   │   │   ├── marketplace/
│   │   │   └── settings/
│   │   ├── service/              # ZMQ subscriber, Tor manager, WorkManager workers
│   │   └── crypto/               # Argon2id wrapper, PSBT validation
│   └── src/test/                 # Unit tests
├── rpc-client/                   # Standalone JSON-RPC library (reusable)
├── scripts/
│   └── omega-deploy.sh           # One-click VPS/RPi daemon deployment
├── .github/workflows/
│   └── android.yml               # CI: build, lint, CodeQL, OWASP
├── build.gradle.kts
└── README.md
```

---

## §14 Pre-Fork Release: Omega 0.20.3

**Mainnet fork height:** 3,200,000 (~1 month from now)
**Testnet fork height:** 100 (changed from 50 — requires testnet reset)
**Regtest fork height:** 1 (unchanged, already active)

### §14.1 Fork bundle at height 3,200,000 (mainnet)

All of these activate at the same block (`chainparams.cpp:167-172`):

| Feature | Param | Mainnet | Testnet | Regtest |
|---------|-------|---------|---------|---------|
| Schnorr signatures | `nSchnorrHeight` | 3,200,000 | 100 | 1 |
| Large script elements (4096 bytes) | `nLargeElementsHeight` | 3,200,000 | 100 | 1 |
| **SMSG room transactions (type 8)** | `nSmsgRoomHeight` | 3,200,000 | 100 | 1 |
| Confidential SMSG funding | `nConfidentialSmsgHeight` | 3,144,000 | 100 | 1 |
| HP masternodes (10000 OMEGA) | `nHPMasternodeHeight` | 3,200,000 | 100 | 1 |
| Fork enforcement (reject old nodes) | `nForkEnforcementHeight` | 3,190,000 | 90 | 1 |

**Testnet change applied:** `chainparams.cpp` line 385-390 updated from
50/50/50/50/500/490 to 100/100/100/100/100/90. This requires a testnet
reset (wipe `~/.omega/testnet3/` data directories on all testnet nodes).

### §14.2 What must ship in 0.20.3 (daemon-side, before fork)

These are the daemon features that must be built, tested, and released
before block 3,200,000. Ordered by dependency and priority.

#### Tier 1 — Consensus-critical (must merge first, test extensively)

| ID | Feature | Files affected | Status |
|----|---------|---------------|--------|
| R-01 | `TRANSACTION_SMSG_ROOM` validation already in place | `smsgroomtx.cpp`, `specialtxman.cpp`, `validation.cpp` | 🟢 Done |
| R-02 | Verify room tx rejection before activation height | `specialtxman.cpp:49` | 🔴 Test on testnet |
| R-03 | Verify room tx acceptance after activation height | end-to-end on testnet | 🔴 Test on testnet |
| R-04 | Verify old nodes (0.20.2) reject after `nForkEnforcementHeight` | peer test with mixed versions | 🔴 Test on testnet |

#### Tier 2 — New RPCs needed for the mobile app

| ID | Feature | Description | Status |
|----|---------|-------------|--------|
| R-05 | `smsgcreateroom` | Convenience RPC: generate room keypair, construct `TRANSACTION_SMSG_ROOM` special tx, broadcast. Params: `name`, `flags` (open/moderated), `retention_days`. Returns `{txid, room_address, room_pubkey, room_privkey_wif, topic}`. | 🟢 Built — `rpcsmessage.cpp` |
| R-06 | `smsglistrooms` | Scan blockchain index for all type 8 txs. Return array of `{txid, name, flags, pubkey, retention, height, topic}`. Optional filter by flags or active/expired. | 🟢 Built — `rpcsmessage.cpp`, uses `SmsgRoomIndex` |
| R-07 | `smsggetroominfo` | Given a room txid, return full room details: `{txid, name, flags, pubkey, retention, height, confirmations, topic, last_message_timestamp}`. | 🟢 Built — `rpcsmessage.cpp` |
| R-08 | `smsgjoinroom` | Given a room txid (open room): subscribe to topic, register room pubkey, store locally. For private rooms: accept a WIF key param. Convenience wrapper around `smsgsubscribe` + `smsgaddaddress`. | 🟢 Built — `rpcsmessage.cpp` |

#### Tier 3 — ZMQ SMSG publisher (needed for real-time mobile notifications)

| ID | Feature | Description | Status |
|----|---------|-------------|--------|
| R-09 | `CZMQPublishHashSmsgNotifier` | New ZMQ notifier class. Publishes SMSG message hash when a new message is stored to inbox. Config: `-zmqpubhashsmsg=tcp://...`. Follows existing `CZMQPublishHashBlockNotifier` pattern. | 🟢 Built — `zmqpublishnotifier.h/.cpp` |
| R-10 | `CZMQPublishRawSmsgNotifier` | Publishes full SMSG message (header + payload) on receive. Config: `-zmqpubrawsmsg=tcp://...`. | 🟢 Built — `zmqpublishnotifier.h/.cpp` |
| R-11 | Hook SMSG `ScanMessage` to fire ZMQ | In `smessage.cpp`, after a message is stored to inbox, calls `g_zmq_notification_interface->NotifySmsgReceived()` to trigger ZMQ. | 🟢 Built — `smessage.cpp:2849` |

#### Tier 4 — Room transaction index (for R-06 / R-07 performance)

| ID | Feature | Description | Status |
|----|---------|-------------|--------|
| R-12 | Room tx index | LevelDB index `'R' + txid → SmsgRoomIndexEntry` via `BaseIndex` pattern. Populated during block sync. Enables fast `smsglistrooms` / `smsggetroominfo`. | 🟢 Built — `index/smsgroomindex.h/.cpp`, always enabled |

#### Tier 5 — Testing and QA (answer all open questions)

| ID | Test | Answers | Status |
|----|------|---------|--------|
| R-13 | `smsgsend ... true 30` and `true 31` on testnet | Q-01: max `days_retention` | 🔴 |
| R-14 | `createmultisig 2 [pk1,pk2,pk3]` on testnet | Q-04: multisig working? | 🔴 |
| R-15 | `addmultisigaddress` on testnet | D-09: add to wallet | 🔴 |
| R-16 | `smsggetfeerate` on testnet | Q-07: current fee rate | 🔴 |
| R-17 | Full PSBT escrow round-trip on testnet | Escrow flow validated | 🔴 |
| R-18 | Room create → join → send → receive → expire on testnet | Full room lifecycle | 🔴 |
| R-19 | Fork transition: mine blocks 89→90→99→100→101 on testnet | Activation clean? | 🔴 |
| R-20 | Mixed-version peer test (0.20.2 node vs 0.20.3 node) | Old nodes disconnected after enforcement height? | 🔴 |
| R-21 | ZMQ SMSG: subscribe → send message → verify push received | Q-02: ZMQ SMSG works | 🔴 |
| R-22 | `listunspent` on testnet | D-08 in BLOCKCHAIN_READINESS.md | 🔴 |
| R-23 | `signrawtransactionwithwallet` on testnet | D-08 in BLOCKCHAIN_READINESS.md | 🔴 |

### §14.3 Testnet deployment plan

```
1. Build 0.20.3-rc1 on Ubuntu 24.04
   cd /home/rob/omegacoin
   ./autogen.sh && ./configure --with-zmq --enable-wallet && make -j$(nproc)

2. Reset testnet (ALL testnet nodes):
   omega-cli -testnet stop
   rm -rf ~/.omega/testnet3/blocks ~/.omega/testnet3/chainstate
   rm -rf ~/.omega/testnet3/evodb ~/.omega/testnet3/llmq
   rm -rf ~/.omega/testnet3/smsgstore ~/.omega/testnet3/smsg

3. Start testnet node 1 (miner):
   omegad -testnet -daemon \
     -zmqpubhashblock=tcp://127.0.0.1:17780 \
     -zmqpubrawtx=tcp://127.0.0.1:17780 \
     -zmqpubhashsmsg=tcp://127.0.0.1:17780

4. Start testnet node 2 (peer):
   omegad -testnet -daemon -addnode=<node1_ip>:17778 \
     -zmqpubhashsmsg=tcp://127.0.0.1:17781

5. Mine to block 89 (before fork enforcement):
   omega-cli -testnet generate 89
   → Verify: room txs rejected ("bad-tx-smsgroom-not-active")

6. Mine to block 100 (fork activation):
   omega-cli -testnet generate 11
   → Verify: room txs accepted
   → Run full test suite (R-13 through R-23)

7. Connect a 0.20.2 node at block 95+ :
   → Verify: old node disconnected after enforcement height 90
```

### §14.4 Release timeline

```
Week 1 (now):
  ├── Testnet chainparams change committed          ✅ DONE
  ├── Version bumped to 0.20.3                      ✅ DONE
  ├── Build R-05 (smsgcreateroom)                   ✅ DONE
  ├── Build R-06 (smsglistrooms) + R-12 (room tx index)  ✅ DONE
  ├── Build R-07 (smsggetroominfo)                  ✅ DONE
  ├── Build R-08 (smsgjoinroom)                     ✅ DONE
  └── Build R-09, R-10, R-11 (ZMQ SMSG publisher)  ✅ DONE

Week 2:
  ├── Deploy testnet, mine to block 100, begin testing
  └── Run full test suite R-13 through R-23

Week 3:
  ├── Run all tests R-13 through R-23
  ├── Fix any issues found
  ├── Update BLOCKCHAIN_READINESS.md with results
  └── Tag 0.20.3-rc1

Week 4 (release week):
  ├── Final round of testnet testing
  ├── Build release binaries (linux-x86_64, linux-aarch64)
  ├── Tag 0.20.3 final
  ├── Publish release notes
  └── Begin node upgrade campaign (must complete before block 3,190,000)
```

### §14.5 Questions resolved by testnet

After running the testnet test suite, the following open questions from
§12 will be answered:

| Question | Test | Expected resolution |
|----------|------|-------------------|
| Q-01 max `days_retention` | R-13 | Document actual max value |
| Q-02 ZMQ SMSG topic | R-21 | **BUILT** — `zmqpubhashsmsg` + `zmqpubrawsmsg`. Needs testnet R-21 test. |
| Q-03 `nSmsgRoomHeight` | — | **RESOLVED: 3,200,000 mainnet, 100 testnet** |
| Q-04 `createmultisig` | R-14 | Confirm working or fix |
| Q-05 Room metadata RPCs | R-05/R-06/R-07 | **BUILT** — `smsgcreateroom`, `smsglistrooms`, `smsggetroominfo`, `smsgjoinroom` |
| Q-07 SMSG fee rate | R-16 | Document value |
| Q-09 Room OP_RETURN format | R-05 | **RESOLVED** — OP_RETURN carries room name; topic = `omega.room.<txid[0:12]>` |
| Q-10 `smsgzmqpush` real-time | R-21 | **RESOLVED** — Superseded by native `zmqpubhashsmsg` / `zmqpubrawsmsg` |
| Q-12 Particl SMSG bugs | — | Review during week 3 |
| Q-13 PoW difficulty | — | Defer to post-fork; document current value |
