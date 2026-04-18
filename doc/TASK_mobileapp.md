# TASK: Omega Secure Messenger — Android App

**Created:** 2026-03-30
**Last revised:** 2026-04-18 (daemon-less mode expanded; wallet lifecycle RPCs aligned; topic constraints corrected; sketch IA folded in)
**Target platform:** Android 9.0+ (API 28+)
**Blockchain:** Omega 0.20.x (Dash 0.19 lineage)
**Core transport:** SMSG (SecureMessage protocol)
**Priority:** Maximum user privacy

---

## §0 Architecture Decision: Dual-Mode Access

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

### Reality check: own-daemon-only adoption will be weak

If the app requires self-hosting before a user can even join trollbox or
reply in a room, adoption will be poor. Assume most users will not run their
own daemon initially. The product therefore needs a first-class daemon-less
mode for social features, while keeping an own-daemon mode for users who want
full wallet control, trading, and the strongest privacy.

### Chosen rollout: daemon-less messaging first, own daemon for full sovereignty

**Mode A — Shared-node daemon-less mode (default onboarding)**

```
┌──────────────────────┐    HTTPS / WSS / Tor    ┌─────────────────────────┐
│   Android App        │◄───────────────────────►│ Shared Omega Node(s)    │
│   (Kotlin/Compose)   │                         │                         │
│                      │                         │ - SMSG relay / index    │
│   - UI               │                         │ - Room directory        │
│   - Local SQLite     │                         │ - Topic / bucket feed   │
│   - SMSG key vault   │                         │ - No wallet exposed     │
│   - Local decrypt    │                         │                         │
└──────────────────────┘                         └─────────────────────────┘
```

**Mode B — Companion daemon mode (full feature set)**

```
┌──────────────────────┐          ┌─────────────────────────┐
│   Android App        │  RPC     │   Omega Daemon          │
│   (Kotlin/Compose)   │◄────────►│   (user's VPS / home    │
│                      │  ZMQ     │    server / Tor hidden   │
│   - UI               │◄─────── │    service)              │
│   - Local SQLite     │          │                         │
│   - Key management   │          │   - Full node           │
│   - PSBT orchestration │        │   - SMSG enabled        │
│                      │          │   - Wallet              │
└──────────────────────┘          └─────────────────────────┘
```

**Mode C — Future autonomous mode** adds an on-device wallet and multi-peer
light sync so users can trade without running their own daemon.

**Feature matrix**

| Capability | Shared-node daemon-less | Own daemon | Future autonomous |
|------------|--------------------------|------------|-------------------|
| Trollbox / open rooms | Yes | Yes | Yes |
| Direct messages / private-room participation | Yes, with device-held SMSG keys | Yes | Yes |
| Room directory / join rooms | Yes | Yes | Yes |
| Room creation / moderation txs | No | Yes | Later |
| Wallet send / receive | No | Yes | Yes |
| Marketplace browse | Yes | Yes | Yes |
| Marketplace listing / escrow / dispute | No | Yes | Later |
| Anchoring / paid SMSG / funded on-chain actions | No | Yes | Later |
| Privacy | Lowest; shared node sees metadata | Highest | High, but still peer-dependent |

**Default policy:** onboarding starts in shared-node mode. When a user taps a
wallet, marketplace, or room-creation action that needs sovereignty, the app
should explain the trade-off and offer either "Connect my daemon" or "Wait
for autonomous wallet mode".

**Privacy properties:**
- Shared-node mode keeps the app usable without self-hosting, but leaks more
  metadata to the node operator
- Own-daemon mode keeps wallet and messaging under the user's control
- Tor support is strongly recommended for both modes
- Node can run on the same LAN (zero external exposure) in own-daemon mode

### Adoption barrier mitigation

Running a daemon is a high barrier. Mitigations:

1. **Shared-node quick start** — ship with a curated list of community nodes
   (plus custom node entry) so a user can reach trollbox and rooms on first
   launch.
2. **One-click deployment script** — ship `omega-deploy.sh` that provisions
   a VPS (e.g. cheapest Hetzner/OVH) with Omega daemon, Tor hidden service,
   firewall, and auto-updates. User pastes a single command.
3. **Raspberry Pi image** — pre-built SD card image with Omega + Tor,
   discoverable via LAN mDNS.
4. **In-app setup / upgrade wizard** — walks the user through both node
   choices: "Use shared node now" or "Connect my own daemon".
5. **Clear feature gating** — if a user stays daemon-less, the app still
   provides trollbox, room browsing, room participation, and messaging, but
   explains that wallet, room creation, anchoring, and escrow require either
   their own daemon or a future autonomous wallet.

### Connection security

| Method | Transport |
|--------|-----------|
| Shared node | HTTPS / WebSocket / long-poll over Tor preferred; no wallet RPC; rotate on failure |
| LAN | RPC + ZMQ over TLS (self-signed cert, TOFU-pinned) |
| Internet | RPC + ZMQ over Tor hidden service (.onion) |
| Fallback | RPC + ZMQ over WireGuard/VPN to home network |

**CRITICAL: in own-daemon mode, ZMQ must be tunnelled.** ZMQ uses plain TCP by
default. If the daemon is exposed to the internet, ZMQ traffic can be
intercepted, leaking transaction and message metadata. All ZMQ connections
MUST go through one of:
- Tor (`.onion` address — inherently encrypted)
- WireGuard/VPN tunnel
- stunnel or socat TLS wrapper (LAN with untrusted segments)
- Same-host loopback only (bind ZMQ to `127.0.0.1`, access via SSH tunnel)

**Credential storage:**
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

The app stores encrypted connection profiles:
- Shared-node mode:
  `{mode:"shared", apiBaseUrl, wsUrl, tlsCertHash, torEnabled, nodePolicyHash}`
- Own-daemon mode:
  `{mode:"daemon", host, rpcPort, zmqPort, rpcUser, rpcPassword, tlsCertHash, torEnabled}`

### Shared-node gateway boundary

In daemon-less mode, the phone is a **light messaging client**. It MUST NOT be
given raw Omega RPC credentials or direct access to a community node's ZMQ
ports.

Required boundary:
- Mobile app ↔ shared-node gateway: HTTPS + WebSocket, Tor-capable
- Shared-node gateway ↔ Omega node(s): local RPC / ZMQ inside the trusted
  backend network only
- Outgoing daemon-less messages: client constructs the SMSG envelope locally;
  gateway injects it via backend-local mechanisms such as `smsgimport` or a
  dedicated relay hook
- Incoming daemon-less messages: gateway streams candidate encrypted envelopes,
  topic items, or room updates; client decrypts locally
- Wallet send/receive, anchoring, room creation, listing publication, and
  escrow remain disabled in shared-node mode

### Shared-node gateway policy

- Expose only minimal messaging APIs: health, room directory, topic read,
  direct/private message ingress-egress, trollbox, marketplace browse, pubkey
  lookup, and invite sync
- Authenticate app sessions without raw RPC credentials: device generates a
  gateway-auth keypair in Android Keystore, completes an onboarding challenge
  (operator policy may use PoW, CAPTCHA, invite code, or attestation), then
  receives a short-lived signed session token scoped to messaging APIs only
- Typical backend-internal reads behind that API: `getblockchaininfo`,
  `getblockcount`, `getblockhash`, `getrawtransaction` (metadata only),
  `smsggetinfo`, `smsggetmessages`, `smsglistrooms`, `trollboxlist`,
  `getzmqnotifications`
- Never expose raw RPC credentials, wallet RPC, daemon admin endpoints, or ZMQ
  ports to the handset
- Treat node RPC as **backend-internal only**; mobile clients call the gateway,
  not the daemon
- Prefer WebSocket push fed by local ZMQ. Polling is fallback only.
- Apply per-token, per-IP, and per-Tor-circuit rate limits, caching, abuse
  bans, and response size caps. Example starting point: ~60 read requests/min
  and ~10 send operations/min per authenticated session, tuned by deployment
- Rotate session tokens periodically and support "request new token" / revoke
  flow without changing the user's messaging keys
- Shape WebSocket traffic to reduce metadata leakage: heartbeat frames at a
  steady cadence, padded response chunks, batched delivery windows (for example
  up to 2 seconds), and no one-frame-per-message push pattern
- Serve cursor/diff-friendly room and trollbox APIs plus cached OP_RETURN room
  names so shared-node clients do not need raw transaction lookups for normal
  browsing
- Apply extra abuse friction on public-room join/subscribe bursts in
  shared-node mode (rate limit plus optional PoW/captcha/invite challenge)
- Distribute community shared nodes via a signed manifest fetched by the app.
  Users must also be able to add a custom gateway URL manually. Optional DNS
  TXT discovery can be explored later, but signed manifests remain the default
- Offer an optional high-privacy public-room mode that subscribes to a small
  cover set of rooms and filters locally, with clear bandwidth/battery warning
- Deny heavy or dangerous backend operations to external callers:
  `wallet*`, `dump*`, `import*`, `sign*`, `getrawmempool`, `scantxoutset`,
  `rescanblockchain`, and raw transaction broadcast. `sendrawtransaction`
  remains own-daemon / autonomous-wallet only.

---

## §1 Wallet Implementation

### Decision: messaging must work without daemon ownership

Most users will start in a no-daemon mode. The app must therefore separate
"messaging access" from "wallet / trading access":

- **Shared-node daemon-less mode** is the default path for first-time users.
  It must unlock trollbox, room directory, room participation, and direct
  messaging without forcing self-hosting.
- **Companion-daemon mode** unlocks wallet operations, room creation,
  anchoring, marketplace listing / buying, escrow, and the best privacy.
- **Future autonomous light-wallet mode** removes the remaining wallet
  dependency for users who want non-custodial trading without running a
  daemon.

### §1.0 Daemon-less messaging mode (default onboarding)

In this mode the phone does not control a full Omega wallet. It is a
messaging client first. The app should be useful immediately without asking
the user to provision a VPS or keep a Raspberry Pi online.

**Local responsibilities:**
- Generate and store SMSG keys on device (identity, per-room, per-trade)
- Encrypt/decrypt direct-message and private-room payloads on device
- Cache joined rooms, room invites, contacts, message history, and decrypt
  state in SQLCipher
- Maintain a sync cursor for recent SMSG buckets / topics
- Retry outgoing messages from a local outbox when connectivity returns
- Support encrypted export/import of the device SMSG key vault for backup and
  device migration

**Shared-node responsibilities:**
- Serve trollbox and open-room topic traffic
- Serve room directory / room metadata
- Return recent bucket payloads or equivalent message candidates so the app
  can attempt local decryption
- Accept outgoing **client-built** SMSG envelopes for relay into the network
- Provide WebSocket / long-poll / polling-friendly notification that a new
  bucket or room message is available

**Release rules:**
- Never upload wallet seed, wallet private keys, or wallet passphrase to a
  shared node
- Do not import SMSG private keys into a community node just to make
  receiving easier
- Do not persist user plaintext on the shared node beyond the transport needs
  of the protocol
- Direct-message and private-room content must be encrypted/decrypted on
  device before this mode is considered complete
- Treat shared-node mode as messaging-only: all funded on-chain actions
  remain disabled until the user connects their own daemon or Phase 5
  autonomous wallet lands

**Shared-node mode includes:**
- Trollbox read/write
- Directory browsing + join open rooms
- Open-room posting
- Direct messages
- Private-room participation after invite import
- Marketplace browsing / read-only listing detail

**Shared-node mode excludes:**
- Wallet send/receive
- Room creation / room moderation transactions
- Listing publication / escrow / dispute resolution
- Anchoring / paid SMSG / other funded on-chain actions

**Required client work:**
- Port SMSG envelope creation/decryption to Kotlin/Rust or shared native code
- Implement bucket-window sync worker + local decrypt queue
- Add multi-node failover and health scoring
- Add encrypted SMSG key-vault export/import for device migration and manual
  backup to file (user passphrase protected; never uploaded to shared node)
- Add clear UI mode badges and locked-action explanations
- Keep gateway-auth identity separate from messaging identities so a rotated
  gateway token does not rotate or expose SMSG keys

### §1.1 Wallet architecture (own-daemon / sovereign mode)

When a user connects their own daemon, the Android app becomes a **thin wallet
UI** that delegates all wallet operations to the daemon's built-in wallet via
RPC. Wallet creation, encryption, unlock, signing, and backup all remain
daemon-side.

```
App (Kotlin)                         Daemon
─────────────                        ──────
listwallets/createwallet/loadwallet ─► wallet RPC
getbalances/getnewaddress/listtransactions ─► wallet RPC
sendtoaddress/estimatesmartfee ────────────► wallet RPC
walletpassphrase/walletlock ───────────────► wallet RPC
walletcreatefundedpsbt/walletprocesspsbt ─► wallet RPC
ZMQ subscribe (rawtx, hashblock) ◄───────── ZMQ push
```

**Operational rule:** in companion-daemon mode, the daemon remains the only
holder of wallet private keys in Phases 1-4. The app may validate, review,
cache, and route PSBTs, but it does not independently store wallet keys or
implement local signing.

**Wallet lifecycle & security RPCs the app should wrap:**

| RPC | Use in app |
|-----|------------|
| `listwalletdir` / `listwallets` | Discover wallets on the daemon and detect whether one is already loaded |
| `createwallet` / `loadwallet` | Create or load the wallet the mobile app will control |
| `encryptwallet` | Initial wallet encryption if a new wallet was created without a passphrase |
| `walletpassphrase` / `walletlock` | Just-in-time unlock for sending, room creation, anchoring, and signing |
| `walletpassphrasechange` | Settings flow for changing the daemon wallet passphrase |
| `backupwallet` | Standard backup path; app triggers it but the daemon writes the file |
| `dumpwallet` | Expert-only text export; hidden behind strong warnings |
| `smsgsetwallet` | Re-bind SMSG after a wallet switch in multi-wallet environments |

**Escrow / multisig / PSBT RPCs the app should wrap:**

| RPC | Use in app |
|-----|------------|
| `createmultisig` | Define the 2-of-3 escrow script and address |
| `addmultisigaddress` | Teach each participant wallet about the escrow script so it can watch/sign it |
| `walletcreatefundedpsbt` | Build payout / refund / timeout PSBTs from watched escrow UTXOs |
| `decodepsbt` / `analyzepsbt` | Inspect and validate the PSBT before any signing step |
| `walletprocesspsbt` | Add wallet metadata and signatures |
| `combinepsbt` | Merge independently signed PSBT variants when needed |
| `finalizepsbt` / `sendrawtransaction` | Finalise and broadcast the fully signed transaction |

**Address-index note:** RPCs such as `getaddressbalance` and
`listaddressbalances` require `addressindex=1`. Phase 1-4 mobile flows should
not depend on them. If later features use them for monitoring or diagnostics,
document the requirement clearly and provide a wallet-native fallback.

**What the app stores locally:**
- Transaction cache (SQLite) — for offline viewing
- Address book — labels, room associations
- Connection profiles — shared node and/or daemon credentials
- SMSG key vault for daemon-less mode — encrypted at rest
- Room / marketplace / anchor metadata caches
- Backup-state flags (e.g. "seed backup confirmed", "wallet backup reminded") — **not** secret material

**What the app does NOT store:**
- Wallet private keys in companion-daemon mode (held by daemon wallet)
- Wallet passphrase
- Mnemonic in default companion-daemon mode; if future autonomous/expert flows
  ever display it locally, erase it immediately after confirmation
- UTXOs (queried live from daemon)
- Full blockchain data

### §1.2 HD wallet & seed management

Omega has native BIP39 mnemonic support (`upgradetohd` RPC, `dumphdinfo`).
This subsection applies when the user has connected their own daemon, or
later when the autonomous light wallet exists.

**SECURITY NOTE — do not fetch mnemonic over RPC by default:**
`dumphdinfo` returns the HD seed, mnemonic, and mnemonic passphrase over RPC.
That is too sensitive for routine mobile onboarding. The default design is:

- Generate or restore the seed on the daemon host itself, not in the phone UI
- Retrieve/display the mnemonic only out-of-band on the daemon host (for
  example via local console or SSH session running `omega-cli dumphdinfo`)
- Let the phone verify only non-secret state such as `getwalletinfo.hdseedid`
  and wallet readiness
- If a deployment insists on remote mnemonic transport, treat it as expert-only
  break-glass mode behind explicit danger warnings, never the default flow

**SECURITY NOTE — wallet passphrase transport:**
`walletpassphrase` sends the wallet passphrase over the RPC channel. That is
acceptable only with TLS/Tor and short unlock windows. Policy:

- Unlock just-in-time for send / room-create / anchor / sign actions only
- Default unlock timeout should be short (for example 30-60 seconds)
- Re-lock with `walletlock` immediately after the action completes
- Recommend a dedicated "mobile-controlled" daemon wallet with limited funds
- Future advanced option: external signer / hardware wallet integration for
  higher-value PSBT flows

**Onboarding flow (new wallet):**
1. App connects to daemon (over TLS or Tor)
2. Calls `listwallets` / `listwalletdir` — if no wallet is loaded, offers
   `createwallet` or `loadwallet`
3. If the selected wallet is unencrypted, offers either:
   - `createwallet` with a passphrase from the start, or
   - `encryptwallet` immediately after creation
4. Calls `getwalletinfo` — checks `"hdseedid"` field
5. If no HD seed:
   - **Recommended:** app instructs user to generate the HD seed on the daemon
     host and record the mnemonic there:
     `omega-cli upgradetohd ""`
     followed by
     `omega-cli dumphdinfo`
   - encrypted wallet → the daemon-host command must include the wallet
     passphrase parameter (or the app must issue the equivalent short-lived
     RPC call), for example:
     `omega-cli upgradetohd "" "" "<walletpassphrase>"`
     then `walletlock` immediately after backup is recorded
6. App polls `getwalletinfo` until `hdseedid` is present
7. App records only a non-secret backup-state acknowledgement:
   - "Seed created and backed up on daemon host"
   - optional user confirmation step / checklist
8. App never persists the mnemonic and should not automatically request it from
   the daemon over RPC

**Expert-mode fallback (not default):**
- For users who fully understand the risk, the app may expose remote
  `upgradetohd` / `dumphdinfo` under an advanced warning sheet
- This mode must require TLS/Tor, `FLAG_SECURE`, no screenshots, no logging,
  and explicit acknowledgement that the seed crossed the network

**Onboarding flow (restore from backup):**
1. Fresh daemon install
2. App ensures the target wallet exists and is loaded via `createwallet`
   or `loadwallet`
3. **Recommended:** user restores directly on the daemon host:
   - unencrypted wallet:
     `omega-cli upgradetohd "mnemonic words here"`
   - encrypted wallet:
     `omega-cli upgradetohd "mnemonic words here" "" "<walletpassphrase>"`
4. If the wallet is encrypted, the restore command must include the wallet
   passphrase parameter; re-lock immediately after restore completes
5. App monitors progress and shows rescan state; the mnemonic itself never
   needs to cross the app connection in the normal flow
6. **Expert fallback:** remote restore via RPC may exist behind the same
   explicit danger gate as above, but should not be the recommended path
7. Triggers `rescanblockchain` to recover transaction history
8. Re-registers SMSG keys via `smsgaddlocaladdress`
9. Forces a fresh daemon backup after the HD upgrade completes

**Daemon-backed backup and export:**
- Primary backup path: `backupwallet <destination>` on the daemon
- Advanced/manual export: `dumpwallet <filename>` in expert mode only
- After `upgradetohd`, `importprivkey`, `smsgimportprivkey`, or room-key
  creation, the app should remind the user to create a fresh backup
- For non-wallet SMSG material (e.g. a room WIF), export explicitly via
  `smsgdumpprivkey` with the same warning treatment as mnemonic display
- **Do not** invent a second app-managed encrypted wallet-backup format in
  Phase 1-4; it duplicates daemon functionality and widens key-handling risk
- In daemon-less messaging mode this rule does **not** apply to the SMSG-only
  key vault: the app must provide its own encrypted backup/restore file
  because no daemon wallet exists to back up those messaging keys

### §1.3 Future: autonomous daemon-less wallet + SMSG light sync (Phase 5+)

Shared-node messaging solves onboarding, but it does not solve sovereignty.
The long-term path is to let a user start in daemon-less messaging mode and
later upgrade to a fully non-custodial phone wallet without ever running a
daemon.

**Target architecture:**
1. Verify headers locally and track reorgs
2. Connect to Omega peers advertising `NODE_COMPACT_FILTERS`
3. Download compact block filters (GCS) via `getcfilters` P2P message
4. Match filters locally against wallet scripts / addresses
5. Download only matching blocks and maintain a local UTXO / tx index
6. Derive keys locally:
   - mainnet external: `m/44'/5'/0'/0/i`
   - mainnet change: `m/44'/5'/0'/1/i`
   - testnet/regtest external: `m/44'/1'/0'/0/i`
   - testnet/regtest change: `m/44'/1'/0'/1/i`
7. Construct and sign transactions locally (app holds keys)
8. Broadcast via multiple public peers over Tor
9. Run SMSG partial-bucket sync in parallel for DMs / private rooms

**Validation requirement:** Phase 5 must verify that locally derived addresses
match Omega's existing HD wallet behaviour and network prefixes before any
autonomous send/receive release.

**Recommended delivery order:**
- Milestone A: shared-node messaging-only mode
- Milestone B: read-only light wallet (balances / history, no spending)
- Milestone C: local send / receive and change handling
- Milestone D: on-chain room creation, anchoring, and paid SMSG
- Milestone E: marketplace listing, escrow, and dispute flows from the
  on-device wallet

**Trade-offs vs own daemon:**
- Higher battery, CPU, and storage cost on the phone
- Public peers can still rate-limit, censor, or delay data
- Background sync is less reliable than an always-on daemon + ZMQ
- Privacy is better than wallet-on-public-node hosting, but still benefits
  from Tor and peer rotation

**Required work:**
- BIP157/158 client protocol in Kotlin/Rust
- Header sync + reorg handling
- Local key derivation (BIP32/39/44)
- Transaction construction and signing
- SMSG bucket sync + local envelope encrypt/decrypt
- Peer selection, health scoring, and failover
- Migration path from shared-node message-only profiles to autonomous wallet
  profiles

**Estimated effort:** 4-8 months additional. Treat this as the main
post-launch adoption track, not an optional experiment.

### §1.4 Lightning Network

Omega does not have a Lightning Network implementation. The HTLC and
channel infrastructure from Bitcoin's LN is not ported. **Lightning is not
an option for Omega 0.20.x.** If instant micro-payments are needed, use
InstantSend (already supported via Dash's LLMQ infrastructure).

---

## §2 SMSG Integration — Core Messaging

Unless explicitly marked otherwise, the RPC mappings in §2-§5 describe
companion-daemon mode. Shared-node mode uses the same user-facing workflows,
but must move SMSG key custody and direct/private-message encryption/decryption
onto the device.

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
- Rotate DM keys automatically on a schedule (for example every 30 days or
  after N messages). The app sends a signed key-rotation notice to active
  contacts and keeps the old key in receive-only grace mode until the
  counterparty migrates
- Treat that rotation policy as a **launch requirement**, not optional polish.
  Every DM / private-room header and send composer must surface an explicit
  status badge such as `CONFIDENTIAL · NO PFS · ROTATES 30D` until a ratchet
  overlay actually exists
- In own-daemon mode, retire superseded keys with `smsgremoveprivkey` after
  the grace window. In daemon-less mode, remove them from the local encrypted
  key vault only after backup / migration checks pass
- Private-room key rotation needs future protocol support (`SMSG_ROOM_UPDATE`
  or equivalent). Until then, rotation is manual room recreation / re-invite
- Long-term: investigate a session-based ratchet for DMs. Until that lands,
  the spec must describe current SMSG confidentiality honestly and not imply
  forward secrecy

### §2.0a Identity model and key lifecycle

- **Base identifier:** Omega/SMSG address backed by a secp256k1 key. No global
  username is required for messaging.
- **Per-room pseudonym:** one fresh sender address per room. Never reuse a DM,
  room, trollbox, marketplace, or escrow address across contexts.
- **Trollbox persona:** disposable pseudonym only. Treat as publicly linkable.
- **Private room ownership:** the room key is separate from day-to-day posting
  identities and must be backed up explicitly.
- **Rotation:** DM addresses rotate automatically by policy; room and trollbox
  sender addresses rotate on demand or when the user leaves/rejoins. Private
  room ownership keys rotate only when future room-update support exists.
  Marketplace and escrow addresses are single-use by policy.
- **Recovery:** own-daemon mode recovers from the wallet seed; daemon-less mode
  recovers from an encrypted SMSG key-vault export plus room-key WIF exports.
- **Device migration:** supported for daemon-less messaging keys, but must use
  an encrypted export/import flow. Never sync private keys through a shared
  node account.

### §2.0b Daemon-less transport contract

Shared-node mode needs more than "use SMSG". Required behaviour:

- **Key exchange:** recipient pubkeys are resolved through pubkey lookup,
  contact import, or QR/invite exchange. Private-room access is granted only
  by encrypted invite or out-of-band key transfer.
- **Message egress:** the client builds the raw SMSG envelope locally. The
  shared-node gateway injects that ciphertext into the network via
  backend-local mechanisms such as `smsgimport` or an equivalent relay hook.
  The gateway must not hold the user's long-term messaging private keys.
- **Message ingress:** the gateway returns candidate encrypted envelopes,
  topic items, room metadata, and push notifications. The device performs the
  final decryption and decides whether a payload belongs to the user.
- **Message storage:** shared nodes may store protocol ciphertext / public topic
  payloads only as required for relay. Decrypted content lives only in the
  device's SQLCipher database.
- **Replay / duplicate handling:** each app payload should include a
  `client_msgid`, `created_at`, and sender sequence value inside the encrypted
  body. Clients still key storage by daemon `msgid` / message hash, but also
  keep a local duplicate cache keyed by the app-level identifiers to drop
  failover duplicates, bucket replays, or delayed re-injection attacks.
- **Spam control:** baseline SMSG PoW stays in force, but daemon-less mode also
  requires gateway quotas, local mute/report controls, and abuse throttling.

### §2.0c Privacy defaults and future ratchet

- Direct messages, invites, private-room posts, and escrow coordination are
  encrypted by default.
- Only trollbox and open-room topic posts are public. These flows MUST carry a
  persistent public warning banner before send.
- UI must make message visibility obvious: locked badge for encrypted DMs /
  private rooms, open/public badge for trollbox and open rooms, and a forced
  room-type choice when creating any new room in wallet-capable mode
- DM and private-room surfaces must also show the current security posture:
  `CONFIDENTIAL`, `NO PFS`, and rotation window / next-rotation countdown
- The app must never market current SMSG DMs as "forward-secret". They are
  confidential, but not PFS.
- Mandatory 30-day DM rotation plus explicit warning copy is the minimum
  launch posture. A future app-layer prekey + ratchet overlay remains
  desirable, but the spec must not rely on it for launch safety.

**Proof-of-work (spam prevention):**
Free messages require PoW: 19 bits of difficulty (`smessage.cpp:3992` —
2 zero bytes + 3 zero bits). This is ~500K hash iterations, taking
milliseconds on modern hardware. **Insufficient against determined
spammers.** A GPU can generate thousands of valid messages per second.

**Anti-spam strategy (app + daemon):**
- Trollbox: 30-second rate limit enforced by daemon, plus shared-node
  per-session quotas and abuse bans
- Public rooms: app-side rate limiting per address (e.g. 1 msg / 5 seconds).
  Client-side filter: if > N messages from one address in T seconds, mute.
  Shared-node gateways must also enforce send quotas, join / subscribe
  throttles, and optional onboarding PoW / challenge gates for abusive room
  join patterns.
- Marketplace: current daemon rejects `topic + paid`. Use free topic
  listings, strict per-seller quotas (for example 5 listings / day from the
  app UI), default listing anchoring before final publish confirmation, local
  mute/report controls, aggressive expiry pruning, and optional discovery-node
  PoW / small-bond friction where a project-operated cache or relay exists.
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

Full authoritative list (with argument signatures) lives in
`doc/rpc_commands.md`. The tables below are the subset used by the mobile
app, grouped by function.

**Boundary rule:** these are daemon RPCs used by either:
- the user's own daemon connection, or
- backend-internal shared-node services

Daemon-less mobile clients must not receive raw access to these RPCs.

**Node control & diagnostics**

| Command | Purpose | Status |
|---------|---------|--------|
| `smsgenable` | Enable SMSG on daemon (optional `walletname`) | 🟢 |
| `smsgdisable` | Disable SMSG | 🟢 |
| `smsggetinfo` | Module status & statistics — use for health checks and connection screen | 🟢 |
| `smsgpeers` | List SMSG-capable peers (optional `node_id`) | 🟢 |
| `smsgsetwallet` | Bind SMSG module to a named wallet (multi-wallet support) | 🟢 |
| `smsgoptions` | View or change runtime options (`list` / `set optname value`) | 🟢 |
| `smsggetfeerate` | Current paid-message fee rate (OMEGA/kB) — show to user before sending paid messages | 🟢 |
| `smsggetdifficulty` | Current SMSG PoW difficulty — used for UX estimate of free-message send latency | 🟢 |
| `getzmqnotifications` | Verify which ZMQ endpoints the daemon actually has enabled before subscribing | 🟢 |
| `smsgscanchain` | Scan blockchain for SMSG public keys (resumable). Used on first-time setup or re-key. | 🟢 |
| `smsgscanbuckets` | Re-process stored buckets (decrypt newly importable messages) | 🟢 |
| `smsgbuckets` | List message buckets; `dump` writes `smsgstore/buckets_dump.json` (non-destructive) | 🟢 |
| `smsgclearbuckets` | Permanently delete all smsgstore .dat files (`confirm=true` required) — admin/maintenance only | 🟢 |
| `smsgzmqpush` | Re-publish stored messages over ZMQ within optional time range — used by the app for resync after long offline periods | 🟢 |

**Key & address management**

| Command | Purpose | Status |
|---------|---------|--------|
| `smsglocalkeys` | List / modify watched local addresses (`whitelist`, `all`, `wallet`, `recv +/- addr`, `anon +/- addr`) | 🟢 |
| `smsgaddaddress` | Add external address + pubkey (enables sending to a new recipient) | 🟢 |
| `smsgaddlocaladdress` | Add wallet address to SMSG watch list | 🟢 |
| `smsgremoveaddress` | Remove a watched address from the SMSG key store | 🟢 |
| `smsgaddresses` | List all addresses in the SMSG key store — used for the app's SMSG identity picker | 🟢 |
| `smsggetpubkey` | Get SMSG pubkey for an address | 🟢 |
| `smsgimportprivkey` | Import raw SMSG private key (WIF) | 🟢 |
| `smsgremoveprivkey` | Remove SMSG private key by address | 🟢 |
| `smsgdumpprivkey` | Export SMSG private key (WIF) — **mandatory warning before display; never log** | 🟢 |

**Send & receive**

| Command | Purpose | Status |
|---------|---------|--------|
| `smsgsend` | Send direct encrypted, paid, or topic message. Topic broadcasts use `address_to=""`; `topic + paid` is currently invalid. | 🟢 |
| `smsgsendanon` | Send anonymous direct encrypted message. Direct-only, deprecated in help text, and payload-capped at 512 bytes. | 🟢 |
| `smsginbox` | Read inbox (`mode`: `count`/`clear`/`all`/`unread`; `filter`) | 🟢 |
| `smsgoutbox` | Read outbox | 🟢 |
| `smsgview` | View messages by address/label with sort & date filters | 🟢 |
| `smsg` | Fetch a single message by `msgid` (with optional decode options) | 🟢 |
| `smsgpurge` | Delete a message from local store by ID | 🟢 |
| `smsgfund` | Broadcast fee transaction for a paid message (if `testfee` deferred at send time) | 🟢 |
| `smsgimport` | Import a raw hex-encoded SMSG message — used for message sync between user's devices (cross-device inbox mirror) | 🟢 |

**Trollbox**

| Command | Purpose | Status |
|---------|---------|--------|
| `trollboxsend` | Send trollbox message (global channel, optional `paid` flag for priority) | 🟢 |
| `trollboxlist` | List the most recent trollbox messages (default 20). Current daemon has no cursor / `since` pagination; gateway should wrap it with a windowed API. | 🟢 |

**Topic channels**

| Command | Purpose | Status |
|---------|---------|--------|
| `smsgsubscribe` | Subscribe to a topic channel (imports shared key) | 🟢 |
| `smsgunsubscribe` | Unsubscribe from a topic | 🟢 |
| `smsglisttopics` | List subscribed topics | 🟢 |
| `smsggetmessages` | Retrieve the newest indexed messages for a subscribed topic (default 50, newest first). Current daemon lacks `since` / `before` cursoring, so gateway/app must maintain cursors until a richer RPC exists. | 🟢 |

**Messaging rooms (new in 0.20.3)**

| Command | Purpose | Status |
|---------|---------|--------|
| `smsgcreateroom` | Create room (type 8 special tx). Args: `name, flags, retention_days` | 🟢 |
| `smsglistrooms` | List indexed rooms from `SmsgRoomIndex` (flags/pubkey/retention/height/topic; room name currently requires raw-tx OP_RETURN decode unless a future verbose mode is added) | 🟢 |
| `smsggetroominfo` | Get room details by txid (`room_address`, flags, retention, confirmations, topic) | 🟢 |
| `smsgjoinroom` | Join room (subscribe topic + register key; optional `privkey_wif` for private rooms) | 🟢 |

**Current daemon gaps the app/gateway must plan around:**
- `smsggetmessages` returns newest-first windows only. A cursor / `since`
  variant is still needed for efficient historical sync and unread tracking.
- `trollboxlist` likewise needs a `since` / `max` variant or gateway cursor for
  low-bandwidth operation.
- `smsglistrooms` would benefit from a verbose flag that returns the parsed room
  name directly, avoiding repeated `getrawtransaction` lookups.
- Unread badges should come from local cursors today; a future gateway
  aggregate or daemon RPC such as `smsggetunread` would be cleaner.

**Message anchoring (new in 0.20.4 — see §2.4)**

| Command | Purpose | Status |
|---------|---------|--------|
| `anchormsg` | Queue a SHA-256 message hash for batch anchoring | 🟢 |
| `anchorcommit` | Commit queued hashes to the blockchain via `OP_RETURN` | 🟢 |
| `verifymsg` | Check whether a message hash has been anchored on-chain | 🟢 |
| `getmsgproof` | Return Merkle inclusion proof for an anchored hash | 🟢 |

### §2.2 Message types used by the app

| Use case | RPC command | Transport / privacy | Retention model |
|----------|------------|---------------------|-----------------|
| Public room message | `smsgsend` with `topic` | Topic shared-key broadcast; readable by all subscribers; not anonymous | Room `retention_days` policy (1-31 days, default 31) |
| Private DM | `smsgsendanon` | Direct encrypted DM; anonymous sender; 512-byte payload cap | Daemon free-message TTL |
| Room invite | direct `smsgsend` preferred, `smsgsendanon` fallback | Encrypted direct message; use `smsgsend` if TTL control or larger payload is needed | Usually 1 day for explicit invite sends |
| Marketplace listing | `smsgsend` with `topic` | Topic shared-key broadcast; effectively public to subscribers | Listing JSON expiry + optional topic `retention_days` hint |
| Escrow PSBT exchange | direct `smsgsend` | Encrypted direct message between fresh trade addresses; 24 KB free / 512 KB paid | Free by default, paid direct SMSG if size/reliability requires it |
| Trollbox | `trollboxsend` | None | 24 hours |

### §2.3 ZMQ real-time notifications

The daemon must be configured with ZMQ endpoints:

```ini
# omega.conf
zmqpubhashblock=tcp://127.0.0.1:7780
zmqpubrawtx=tcp://127.0.0.1:7780
zmqpubhashsmsg=tcp://127.0.0.1:7780  # SHA256(full_msg) on inbox receipt
zmqpubrawsmsg=tcp://127.0.0.1:7780   # full SMSG bytes on inbox receipt
```

These examples are intentionally loopback-only. Do **not** bind ZMQ to
`0.0.0.0` on internet-facing hosts. Remote/mobile access must traverse Tor,
WireGuard, stunnel, or an SSH tunnel instead of exposing raw ZMQ publicly.

The app subscribes to ZMQ topics for push-style notifications:
- `hashblock` — new block (update balance, confirmations)
- `rawtx` — new transaction (instant balance update)
- `hashsmsg` — new SMSG message hash (lightweight ping; app then polls `smsginbox` / `smsggetmessages`)
- `rawsmsg` — full SMSG bytes on receipt (topic hash visible in cleartext `nonce[0..3]`; use for topic routing)

**Shared-node mode:** the handset does not connect to daemon ZMQ directly.
Instead, the gateway subscribes to local ZMQ and fans out a narrowed
WebSocket/long-poll event stream for new blocks, trollbox updates, room
messages, and candidate encrypted envelopes.

**Fallback + mobile background delivery** (if ZMQ SMSG topic unavailable, or
Android background limits suspend normal sync):

Battery-aware strategy:
- **Live room / live DM mode:** when the user opts into real-time background
  delivery for an active conversation, run a foreground service with a
  persistent notification and keep the own-daemon ZMQ or shared-node WebSocket
  path alive
- **Normal background mode:** use WorkManager for periodic catch-up and use
  one-off `setExpedited()` jobs for reconnect / push-hint catch-up where the
  platform allows it
- **Doze / battery saver reality:** delivery becomes best-effort. The app must
  explicitly tell the user when Android is delaying sync rather than implying
  instant delivery

Polling table:

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
- Shared-node mode should prefer HTTPS/Tor WebSocket push backed by gateway
  heartbeats and padded batches over short-interval polling

### §2.4 Message anchoring (tamper-evident records)

Omega 0.20.4 introduces a batched message-hash anchoring layer
(`src/smsg/msganchor.{h,cpp}`, `rpcsmessage.cpp:2998+`). Up to 127
SHA-256 hashes are aggregated into a Merkle tree; the root is committed
on-chain in a single `OP_RETURN` transaction. Each anchored hash can
later be proven via a Merkle branch without revealing any other hash in
the batch.

**Why this matters for the mobile app:**

SMSG messages are ephemeral and have transport- / message-type-specific
retention limits. Anchoring gives the app a persistent, tamper-evident
record of selected messages without storing their content on-chain. Only
the 32-byte hash is committed — the content stays in SMSG or in the app's
local encrypted store.

**RPC interface (summary):**

| Command | Purpose |
|---------|---------|
| `anchormsg msghash ( prevhash )` | Queue a SHA-256 message hash. Optional `prevhash` links to a previous revision (versioned records). Returns `{queued, status, pending}` where `status ∈ queued\|pending\|confirmed\|full`. |
| `anchorcommit` | Commit all queued hashes to chain via `OP_RETURN`. Requires an unlocked, funded wallet. Returns `{txid, root, count}`. |
| `verifymsg msghash` | Check whether a hash is on-chain. Returns `{anchored, pending, txid, root}`. |
| `getmsgproof msghash` | Return the Merkle inclusion proof: `{hash, anchored, pending, txid, root, index, branch[]}`. |

**Mobile app integration points:**

| Use case | Flow | Priority |
|----------|------|----------|
| Marketplace listing receipts | On publish, hash the canonical listing JSON → `anchormsg` → surface txid+root in the listing metadata so any buyer can `verifymsg` the exact listing text existed at time T | SHOULD (Phase 3) |
| Escrow agreement finalisation | Before funding, both parties sign the agreed trade terms (JSON), hash it, `anchormsg`, include the anchor txid in the PSBT description. Provides tamper-evident proof of what was agreed if a dispute arises | MUST (Phase 3) |
| Room policy snapshots | Owner hashes the current rules/moderator set and anchors periodically — room members can audit policy history | COULD (Phase 5) |
| Contract signalling | Anchored message chains using `prevhash` form an append-only audit trail (versioned listings, multi-round negotiations) | COULD (Phase 5) |
| DM receipts (opt-in) | User can tap "anchor this message" to produce an on-chain timestamp + proof. Counterparty's client verifies automatically | COULD (Phase 5) |

**Cost model:**

- One OP_RETURN transaction per batch (standard wallet fee, ~0.0001 OMEGA)
- Up to 127 hashes per batch → per-message anchoring cost is ~1/127 of
  a transaction fee (sub-satoshi economically)
- The app should **batch aggressively**: queue with `anchormsg`, then
  call `anchorcommit` on a timer (e.g. every 10 minutes) or when the
  queue reaches the 127-hash cap (`status = full`).
- Display "pending anchor (N messages queued)" indicator in the UI
  until the next commit lands.

**Privacy properties:**

- Anchoring leaks **only** the fact that a batch of N message hashes was
  committed, plus the wallet that paid the fee. It does not reveal
  message content, participants, or topic.
- For maximum unlinkability, use a dedicated "anchoring" wallet address
  that is not reused for marketplace or room payments.
- `prevhash` creates a linkable chain between versions — acceptable for
  public listing histories, but avoid for private messages.

**Verification by third parties:**

Anyone holding a candidate `msghash` can:
1. `verifymsg <msghash>` → confirms `anchored: true` and returns the
   anchor txid + Merkle root.
2. `getmsgproof <msghash>` → returns the Merkle branch.
3. Independently verify: `MerkleRoot(hash, branch, index) == root` and
   that `root` appears in the OP_RETURN output of the anchor tx at the
   reported block height.

This works even if the verifier does not have the SMSG message itself —
only the hash. Ideal for receipts, bills of lading, and proof-of-existence
claims that precede any content disclosure.

**Daemon requirements:**

- Wallet must be unlocked when `anchorcommit` is invoked
- Funded with at least enough OMEGA for one standard tx fee
- App should surface errors: `PREPARE`, `WALLET_UNAVAILABLE`,
  `CREATE_TRANSACTION`, `BROADCAST`, `NOT_INITIALIZED`, `DATABASE`
  (from `AnchorCommitError` enum in `msganchor.h`)

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
- In shared-node mode, trollbox uses a separate gateway channel / quota bucket
  from DMs and room messaging to reduce abuse spillover
- In shared-node mode, posting cannot rely on handing raw `trollboxsend` to the
  phone. The gateway needs a dedicated relay path for client-originated
  trollbox messages.

### §3.4 Privacy note

Trollbox messages include the sender's address. Users should be warned that
trollbox participation reveals their address to the network. For anonymous
discussion, use direct DMs or private rooms. Open/public rooms are only
pseudonymous via fresh room-specific sender addresses.

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
- **Private room** (no `SMSG_ROOM_OPEN`): Join by invite only; not shown in the
  default public directory UI

**Creation UX requirement:** wallet-capable room creation must force the user
to choose between:
- `Open` — visible to everyone; topic messages are public
- `Private` — invite-only; messages decrypt only for invited members
The choice cannot be hidden behind advanced settings.

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
- `room_address` from `smsgcreateroom` / `smsggetroominfo` is still useful
  for private-room traffic and metadata, but open-room broadcast should use
  the topic itself, not direct encryption to the room key

**Sending a public room message (open room):**
```
App → smsgsend <room_local_sender> "" "message" false 1 false false false
      "omega.room.<short_txid>" "" <room_retention_days>
```
All nodes subscribed to the topic can read the message. No room private key
is needed. Messages are visible to anyone who subscribes, so the app should
use one fresh sender address per room.

**Shared-node nuance:** the above `smsgsend` form applies directly in
own-daemon mode. In daemon-less mode, the phone must build the topic message
locally and hand it to the gateway's relay/injection endpoint rather than
calling raw daemon RPC.

**Current limitation:** topic messages cannot be anonymous. `smsgsendanon`
has no topic parameter, and the daemon rejects anonymous topic sends. Open
room posting is therefore **pseudonymous**, not anonymous: one fresh sender
address per room, rotated when the user leaves/rejoins or wants a new persona.

**Private (invite-only) room messages:**
For rooms WITHOUT `SMSG_ROOM_OPEN` flag, the room private key is
distributed only to invited members (via encrypted invite). Messages are
sent to the room address via `smsgsend` — only holders of the room
private key can decrypt.

| Room type | Message transport | Who can read |
|-----------|------------------|--------------|
| Open | Topic channel (`omega.room.<id>`) | Anyone subscribed |
| Open + Moderated | Topic channel | Anyone subscribed (kicked users filtered client-side) |
| Private (invite-only) | Point-to-point to room address | Only members with room private key; join by invite |

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

All room creation transactions are on-chain and publicly queryable. The app
should not full-scan the chain from mobile code. Shared-node clients should
consume a gateway-cached room directory with parsed names. Own-daemon mode can
use the daemon's room index first, then fall back to extra metadata lookups
only when needed.

**Important limitation:** current `TRANSACTION_SMSG_ROOM` design does not
support true stealth rooms. A private room can be hidden from the app's public
directory UX, but its existence is still inferable from on-chain metadata. If
fully hidden room identifiers are required, that needs a future protocol
change.

1. Call `smsglistrooms` to get indexed room txids, flags, pubkey, retention,
   height, and derived topic
2. Shared-node mode: receive the cached human-readable room name from the
   gateway's parsed OP_RETURN index
3. Own-daemon fallback only: if the UI still needs a name and no daemon-side
   cache/verbose RPC exists, call `getrawtransaction <txid> 1` and parse the
   OP_RETURN room name
4. Display: room name if recovered, otherwise topic / short txid fallback,
   plus retention period, flags, and last activity
5. User taps "Join":
   - Open room → `smsgsubscribe "omega.room.<short_txid>"`
   - Open room → show a blocking warning banner: "Messages in this room are
     public to network subscribers"
   - Private room → request invite from owner (out-of-band or via DM)
6. App stores joined room in local database

**Directory policy:**
- Public directory shows open / moderated rooms by default
- Private rooms are accessed by invite, QR, or direct txid deep link
- The app should not index or search private rooms in the default public room
  browser even though their creation tx may exist on-chain

**Performance note:** Avoid full-chain room scans on mobile. Mitigations:
- Cache room list in local SQLite; only scan new blocks since last check
- Rely on daemon-side `smsglistrooms` index results for the primary list
- Shared-node mode should never issue raw `getrawtransaction` calls for room
  names. Use the gateway's parsed-name cache.
- Own-daemon mode uses `getrawtransaction` only as a temporary fallback for
  uncached names / detail views. This may require `txindex=1` unless a future
  verbose `smsglistrooms` mode ships or the daemon caches parsed names
- Paginate directory results (50 rooms per page)
- Background refresh via WorkManager (not on every tab open)
- Clock skew between devices and daemon: use daemon's `getblockchaininfo`
  timestamp as authoritative time source, not device clock, for all
  expiry and retention calculations
- Render room names and listing titles as plain text only: HTML-escape,
  disable autolink, and never interpret user-supplied metadata as markup

### §4.5 Room expiry

Rooms carry an explicit `retention_days` policy set at creation time via
`smsgcreateroom "name" ( flags retention_days )`. Current daemon range is
**1-31 days**, default **31**. For open/topic rooms this is an app-visible
retention policy, not a paid-topic storage purchase.

**Important:** `retention_days` is a freshness / discovery policy, not a
cryptographic deletion guarantee. Current Omega builds do not guarantee that
all subscribed nodes prune every copy of a room message exactly when the UI
marks the room stale.

**Implementation (client + daemon hybrid):**
- App stores each room's configured `retention_days`
- App tracks last seen message timestamp per room in local SQLite
- Periodically queries daemon via `smsggetmessages <topic> 1` to get the
  actual latest message timestamp (guards against stale local state)
- If `now - last_message_timestamp > room.retention_days`, room is marked
  "stale" / "expired" in the UI
- Expired rooms remain in directory but greyed out
- Room owner can revive by sending a new message (resets activity timer)
- Old messages purged locally via `smsgpurge`
- Shared-node gateways must also prune expired cached plaintext / derived
  indices and keep only the minimum ciphertext / metadata needed for relay
- If a new message arrives on an expired room topic, the room is
  automatically un-expired and the user is notified
- Auto-resubscribe to expired rooms if daemon receives a new message
  (daemon keeps topic subscription active even when app marks it expired)
- If longer-than-31-day retention is needed, that requires future daemon
  work (paid-topic or archive design), not `topic + paid` on current builds
- If the user needs durable proof that a specific room message existed after
  expiry, the app should offer `anchormsg` / `anchorcommit` for that message
  hash rather than pretending room TTL can be extended network-wide today
- If hard network-wide deletion semantics are required, that also needs future
  protocol work; the current release should describe expiry honestly as
  best-effort retention plus local purge

### §4.6 Moderation (room owner powers)

| Action | Implementation |
|--------|---------------|
| **Invite** | Owner sends encrypted invite via direct `smsgsend` (preferred) or QR code; `smsgsendanon` is fallback only for small payloads |
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
- **Remote invite:** Use direct encrypted `smsgsend` from a throwaway
  sender address with `days_retention=1`, so TTL is explicit and the
  invite is not forced into the 512-byte anonymous-message cap.
- **Fallback:** `smsgsendanon` is acceptable only when the invite payload
  is small enough and the daemon's default free-message TTL is acceptable.
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
- Open-room posting is pseudonymous only: one fresh sender address per room
- `smsgsendanon` remains for DMs and small direct invites, not room topics
- Room member list is not broadcast (only the owner knows invitees for
  private rooms; open rooms have no member list at all)
- No read receipts, no typing indicators, no presence status

---

## §5 Marketplace with Escrow

All publish / buy / escrow flows in this section require a wallet-capable
mode. Shared-node daemon-less users may browse listings, but creation,
purchasing, anchoring, and dispute resolution stay locked until the user
connects their own daemon or Phase 5 autonomous wallet lands.

**Scope limit:** the escrow design in this document is for **OMEGA-settled
trades only**. Fiat rails, third-party chains, and off-platform payment
escrows are out of scope for the first release and should not be implied by
the UI.

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
  "contact_room": "<optional room_txid>",
  "expiry": 1712000000,
  "seller_address": "<omega_address>"
}
```

Listings are public and browseable by all nodes subscribed to the
marketplace topic. In transport terms they use the topic shared key, so
they are effectively public to any subscriber.

**Address privacy:** If a seller reuses the same address across listings,
an observer can link all their listings (and transaction history) to one
identity. **The app MUST generate a fresh address for each listing.**
The `seller_address` field uses a one-time address; `contact_room` should
be treated as an optional seller-managed room for public Q&A or support.
Actual escrow coordination uses fresh trade-specific direct-message addresses.

**Listing anti-spam (current protocol constraint):** topic messages cannot be
paid on current Omega builds. Marketplace listings therefore **cannot** use
`paid_msg=true` while published to `omega.marketplace`. Phase 3 must rely on:
- fresh seller address per listing
- strict per-seller listing quotas / cooldowns (for example 5 listings / day
  from the app UI)
- default listing-hash anchoring before the final publish confirmation screen
- local mute / report / hide controls
- optional proof-of-work / small-bond friction for any project-operated public
  listing cache or relay
- aggressive local pruning of expired listings so stale spam does not linger

### §5.2 Purchase flow (2-of-3 multisig escrow via PSBT)

Omega has confirmed PSBT support. The escrow uses a 2-of-3 multisig:
**buyer + seller + arbiter**.

```
Step 1: Buyer signals intent
        → smsgsendanon <seller_address> { "type": "buy", "listing_id": "..." }

Step 2: Agree on arbiter
        → Both parties exchange arbiter's public key and fresh trade-specific
          sender/recipient addresses via encrypted SMSG

Step 3: Create multisig escrow address
        → App calls: createmultisig 2 ["buyer_pubkey","seller_pubkey","arbiter_pubkey"]
        → Returns: { "address": "oEscrow...", "redeemScript": "..." }
        → Each participant also calls:
          addmultisigaddress 2 ["buyer_pubkey","seller_pubkey","arbiter_pubkey"]
          so their wallet can watch and later sign the escrow UTXO

Step 4: Anchor the agreement (tamper-evident record — see §2.4)
        → Canonical JSON of trade terms:
          { listing_id, price, buyer_addr, seller_addr, arbiter_addr,
            escrow_addr, timeout, nLockTime, created_at }
        → hash = SHA-256(canonical_json)
        → anchormsg <hash>
        → anchorcommit  (both parties do this independently or via one
          side; anchor txid is exchanged via smsgsendanon)
        → Both clients store canonical_json + anchor_txid locally.

Step 5: Buyer funds escrow
        → sendtoaddress "oEscrow..." <amount>

Step 6: Seller delivers goods/service

Step 7: Release — buyer signs PSBT releasing funds to seller
        → walletcreatefundedpsbt [{"txid":"...","vout":0}] [{"seller_addr": amount}]
          0 {"includeWatching":true}
        → decodepsbt / analyzepsbt before any signing
        → walletprocesspsbt <psbt>  (buyer signs)
        → Send partial PSBT to seller via direct encrypted smsgsend
          using fresh trade-specific addresses
        → Seller calls walletprocesspsbt (seller signs — 2 of 3 reached)
        → combinepsbt if signatures were collected on separate copies
        → finalizepsbt → sendrawtransaction

Dispute: If buyer and seller disagree, arbiter + aggrieved party
         sign to release funds appropriately. The arbiter fetches the
         anchored agreement via verifymsg + getmsgproof to confirm the
         terms both sides originally accepted — neither side can rewrite
         history.
```

### §5.3 PSBT size and reliability

PSBTs can be large (especially with multiple inputs or complex scripts).
SMSG message limits:
- Free messages: 24 KB max
- Paid messages: 512 KB max

A typical 2-of-3 multisig PSBT is ~500-2000 bytes (well within limits).
However, for safety:

- **Anonymous SMSG is too small for general PSBT transport:** `smsgsendanon`
  is capped at 512 bytes, so escrow PSBT exchange should use direct
  encrypted `smsgsend` between fresh trade addresses instead.
- **Size check:** App validates PSBT size before sending. If > 20 KB,
  use direct paid SMSG (`smsgsend ... true <days>`) rather than anonymous SMSG.
- **Chunked messages (future):** If PSBTs ever exceed 24 KB (unlikely for
  escrow), implement chunked SMSG delivery with reassembly. Deferred.
- **Conversation threading:** Keep escrow messages organised in the app DB
  by `trade_id` / counterparty / listing, but transport them as direct
  encrypted SMSG or an invite-only private room, not a public `omega.escrow.*`
  topic.
- **PSBT validation (anti-tampering):** Before signing any PSBT received
  from the counterparty, the app MUST validate:
  - Validation order is fixed:
    `analyzepsbt` → `decodepsbt` → deterministic compare against anchored
    trade template → enable signing
  - Reconstruct the expected transaction template from the anchored trade
    terms: seller payout, refund path, arbiter fee, and any explicitly
    allowed buyer-owned change output
  - Output addresses and amounts exactly match that expected template
  - No unexpected inputs or outputs are present
  - Change, if any, returns only to a buyer-controlled address that was
    already committed in the trade agreement
  - Input amounts do not exceed the funded escrow UTXO set known locally
  - The PSBT was not modified in transit (compare against locally-stored
    trade parameters and escrow funding txid/vout)
  - Any mismatch in output script, amount, fee sink, input count, or change
    destination is a hard reject and must keep the signing UI disabled
  - Use `decodepsbt` / `analyzepsbt` before `walletprocesspsbt`
  - Never blindly sign a PSBT constructed by an untrusted party
  - This validation gate is a release blocker for marketplace launch

**Escrow reliability contingency:** `createmultisig` and
`addmultisigaddress` remain release blockers until validated on testnet. If
they prove unreliable, the fallback path is descriptor-derived escrow:
- derive `wsh(sortedmulti(2,...))` via `getdescriptorinfo` / `deriveaddresses`
- construct the unsigned PSBT via `createpsbt`
- attach UTXO metadata via `utxoupdatepsbt`
- continue signing / inspection with `walletprocesspsbt`, `decodepsbt`,
  `analyzepsbt`, `combinepsbt`, and `finalizepsbt`
This contingency must be tested before Phase 3 is considered complete.

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
- **No seller-only fast-release in v1:** a seller delivery claim by itself is
  not enough to shorten the timeout. Any early release path still requires
  buyer cooperation or arbiter participation; otherwise it is too easy to
  abuse for physical-goods fraud.
- **nLockTime calibration:** Set long enough for shipping (e.g. 14-30 days
  for physical goods) but not so long that funds are locked indefinitely.
  App presents duration options based on listing category:
  digital (3 days), goods (14 days), services (30 days).

### §5.5 Arbiter selection

- Users should choose an arbiter per trade from a public registry, not rely on
  one hidden hard-coded project operator.
- **Bootstrap model:** the project may publish an initial signed registry in a
  well-known topic such as `omega.arbiters`, but the app must let users pick
  any compatible arbiter entry or enter a custom arbiter key manually.
- **Multisig arbiter (recommended):** each arbiter entry should itself be a
  2-of-3 multisig among trusted community members. No single arbiter-key
  compromise can steal escrowed funds. The arbiter "entity" requires 2 of 3
  arbiter members to agree.
- Arbiter fee: small percentage deducted from escrow amount
- Publish arbiter metadata with pubkey/script, fee policy, contact method,
  optional bond statement, and reputation summary
- Time-locked refund / release paths remain the primary guarantee; arbitration
  is the backup path when buyer and seller cannot cooperate
- **Arbiter key security:** Each arbiter member should store their key in
  hardware (e.g. hardware wallet or HSM). Multi-party signing procedures
  required for dispute resolution.
- **Transparency:** Arbiter identities (or pseudonyms + reputation) should
  be publicly disclosed. Arbiter keys should be rotated periodically.
- Future: pseudonymous arbiter reputation feed and dispute-history summaries so
  users can avoid low-quality arbiters without revealing real identities

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

### §6.0 UX safety defaults

- Primary language is user-facing: "message", "trade", and "backup", not raw
  RPC names or blockchain jargon
- Txids, confirmation counts, and anchor hashes live behind advanced detail
  sheets, not in the primary happy path
- DMs and private-room flows default to encrypted mode
- DM and private-room headers must show a status line such as
  `CONFIDENTIAL · NO PFS · ROTATES 30D` until a ratchet overlay exists
- Trollbox and open-room composers must show a persistent public warning banner
  before send
- The app must never imply trollbox or open rooms are private
- Sensitive screens (mnemonic display, room-key export, WIF/private-key export,
  passphrase entry, PSBT approval) must use `FLAG_SECURE`, suppress ordinary
  screen recording/screenshot capture, and auto-clear after ~30 seconds of
  inactivity
- No remotely synced theme/profile customisation that could add avoidable
  fingerprinting surface

### Bottom navigation tabs

```
┌─────────┬──────────┬───────────┬────────────┬────────┐
│  Rooms  │ Trollbox │ Directory │ Marketplace│ Wallet │
└─────────┴──────────┴───────────┴────────────┴────────┘
```

This IA should follow the first sketch in `/home/rob/Trollbox_app_sketch`:
one tab per primary workflow, a compact header with live status subtitle,
and a settings gear in the header rather than a drawer-first navigation model.

### §6.1 Rooms tab
- List of joined rooms, sorted by last message
- Unread badge per room
- Visibility badge per room (`LOCKED`, `PUBLIC`, `PUBLIC+MODERATED`)
- Tap → room chat view (message list + input)
- Long-press → room settings (leave, mute, notifications)
- FAB → "Create Room" flow when wallet-capable mode is active; otherwise show
  "Connect your own daemon to create rooms"

### §6.2 Trollbox tab
- Single flat chat stream
- Auto-scrolling to newest
- Character counter on input
- Rate limit indicator
- Top banner clearly states `PUBLIC · EPHEMERAL · 24H TTL`
- Persistent warning that sender address is visible

### §6.3 Directory tab
- Searchable list of indexed rooms by room name, topic, or txid
- Filter: open/moderated, active/expired, member count
- "Join" button per room
- Room metadata: topic / txid, retention, visibility badge, flags, and name when recovered
  from the room tx OP_RETURN

### §6.4 Marketplace tab
- Browse listings by category
- Search by keyword
- "New Listing" flow (compose + publish to topic) only when wallet-capable
  mode is active
- Listing detail → "Buy" initiates escrow flow only when wallet-capable mode
  is active
- Listing detail should surface seller address, escrow model, and anchor proof
- "My Listings" / "My Purchases" sub-tabs

### §6.5 Wallet tab
- If own-daemon or future autonomous wallet mode is active: balance display
  (confirmed / unconfirmed / locked in escrow), send / receive buttons,
  transaction history, QR receive, and fee-aware send form
- If shared-node mode is active: show a locked-state card,
  "Messaging mode active. Connect your own daemon to unlock wallet, room
  creation, anchoring, and marketplace escrow."

### §6.6 Settings (gear icon)
- Access mode selector (shared node / my daemon)
- Shared-node profile, privacy warning, failover controls, session reset, and
  optional high-privacy public-room mode
- Daemon connection (host, port, credentials, Tor toggle)
- SMSG key management (backup, import, list)
- Wallet seed backup
- Wallet encryption / unlock / backup controls (`walletpassphrase`,
  `walletlock`, `backupwallet`, expert `dumpwallet`)
- Notification preferences
- About / version

---

## §7 Technology Stack

| Layer | Technology | Rationale |
|-------|-----------|-----------|
| Language | Kotlin | Android-native, coroutine support |
| UI | Jetpack Compose | Modern declarative UI |
| Local DB | Room + **SQLCipher** | Encrypted local database (see §7.1) |
| Networking | OkHttp + Ktor | Shared-node HTTPS/WebSocket API plus own-daemon JSON-RPC over TLS |
| ZMQ | JeroMQ (pure Java ZMQ) | No native lib dependency |
| Crypto | Tink / SpongyCastle | Key derivation, local encryption |
| Credentials | EncryptedSharedPreferences | Android Keystore-backed credential storage |
| Biometric | BiometricPrompt (API 28+) | Optional biometric unlock for credentials |
| QR codes | ZXing | Address sharing |
| Image loading | Coil | Marketplace thumbnails |
| DI | Hilt | Dependency injection |
| Tor | **tor-android** (embedded) | Built-in Tor, no Orbot dependency (see §7.2) |
| Background | WorkManager + foreground service | Battery-aware catch-up plus persistent live-sync option for active rooms/DMs |
| Static analysis | CodeQL + Android Lint | Security scanning from Phase 1 |
| Dependency audit | OWASP Dependency-Check | CVE scanning of all transitive dependencies |

### §7.1 Local database encryption (SQLCipher)

All local data (messages, contacts, room state, transaction cache) is stored
in SQLite via Room. **The database MUST be encrypted** using SQLCipher:

- SQLCipher integrates with Room via `net.zetetic:android-database-sqlcipher`
- Database key derived from Android Keystore (device-bound). Prefer StrongBox-
  backed keys when the device offers StrongBox; otherwise fall back to normal
  hardware-backed / TEE Keystore
- Optional user-provided passphrase on top of the Keystore key, offered during
  first-run security setup
- If device is compromised, attacker cannot read message history, contacts,
  or transaction cache without the Keystore key
- First-launch: generate a random 256-bit key, store in Android Keystore
- Migration path: if user upgrades from unencrypted beta, migrate in-place
- If the user enables the passphrase overlay, biometrics may unlock only the
  Keystore-wrapped component; they do not replace the user passphrase entirely
- Sensitive buffers (wallet passphrases, exported WIFs, room keys, PSBT blobs)
  should use mutable byte arrays / native buffers where practical and be wiped
  immediately after use. Avoid logging or retaining long-lived immutable copies

### §7.2 Embedded Tor (no Orbot dependency)

Relying on Orbot requires users to install and configure a separate app.
This is a usability and reliability problem.

**Solution:** Embed Tor directly using `tor-android` (Guardian Project):
- Tor binary bundled inside the APK
- App manages the Tor process lifecycle internally
- Simple toggle in settings: "Route all connections through Tor"
- When enabled: RPC, ZMQ, and all network traffic routed via SOCKS5 proxy
- Tor bootstrap status shown in connection screen with progress and clear
  "network still starting" copy; cold start may take 10-30 seconds
- No external app dependency
- Reuse cached Tor state / consensus data where safe to reduce cold-start pain
- **Circuit isolation:** Use separate Tor circuits per room/topic to prevent
  a malicious exit node from correlating traffic across rooms. Achieved via
  `IsolateSOCKSAuth` with unique credentials per circuit.
- **DNS leak prevention:** All DNS resolution MUST go through Tor (SOCKS5
  with remote DNS). No system DNS queries when Tor is enabled. Verify
  with integration test that no non-Tor traffic escapes.
- Regularly rotate circuits (new identity every 10 minutes or on demand)
- When the user disables Tor, clearnet mode is allowed only with strict TLS
  TOFU pinning and explicit UI that privacy is reduced

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
- Cryptographic libraries (Tink, SpongyCastle) pinned to
  specific versions and updated quarterly

---

## §8 Daemon-Side Requirements

Own-daemon mode, and any community node that serves daemon-less users,
depends on the daemon capabilities below:

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
| D-20 | Message anchoring RPCs (`anchormsg`, `anchorcommit`, `verifymsg`, `getmsgproof`) | 🟢 Built in 0.20.4 — `smsg/msganchor.{h,cpp}`, `rpcsmessage.cpp` |
| D-21 | Anchor batch size / timing tuning on testnet | 🔴 Measure on testnet under load |
| D-22 | `smsggetmessages` cursor / `since` / `before` support for efficient history sync | 🔴 Design needed |
| D-23 | `smsglistrooms` verbose name decode (return parsed room name directly) | 🔴 Design needed |
| D-24 | Aggregate unread-count RPC or equivalent indexed backend support | 🔴 Design needed |
| D-25 | `trollboxlist` cursor / `since` / `max` support | 🔴 Design needed |
| D-26 | `smsgaddresses` filter mode (`local|external|all`) | 🔴 Nice-to-have daemon improvement |
| D-27 | Document address-index-dependent RPCs (`getaddressbalance`, `listaddressbalances`) and fallback path when `addressindex=1` is absent | 🔴 Required documentation |
| D-28 | Descriptor-based escrow fallback (`getdescriptorinfo`, `deriveaddresses`, `createpsbt`, `utxoupdatepsbt`) if multisig convenience RPCs misbehave | 🔴 Required contingency design |

### §8.1 Shared-node gateway requirements (daemon-less mode)

| ID | Requirement | Status |
|----|-------------|--------|
| G-01 | External interface is HTTPS/WebSocket only. No raw RPC or ZMQ exposed to mobile clients. | 🔴 Required |
| G-02 | Gateway never stores user wallet seeds, wallet private keys, or imported SMSG private keys. | 🔴 Required |
| G-03 | Outgoing daemon-less messages use client-built envelopes and backend-local injection (`smsgimport` or equivalent relay hook). | 🔴 Design + implementation required |
| G-04 | Internal RPC allowlist / denylist enforced. Wallet, dump, import, sign, and heavy scan endpoints blocked from external callers. | 🔴 Required |
| G-05 | Local ZMQ → WebSocket bridge for block / topic / candidate-message push. | 🔴 Required |
| G-06 | Per-IP / per-session / per-Tor-circuit rate limits, quotas, caching, and abuse controls. | 🔴 Required |
| G-07 | Public directory lists only open / moderated rooms by default. Private rooms are invite-only UX. | 🔴 Required |
| G-08 | Shared node retains ciphertext / public topic payloads only as needed for relay. No long-term plaintext cache. | 🔴 Required |
| G-09 | Multi-node failover with health scoring and duplicate suppression. | 🔴 Required |
| G-10 | Device-auth keypair + short-lived signed session tokens + onboarding challenge. Messaging identities stay separate from gateway-auth identity. | 🔴 Required |
| G-11 | WebSocket padding / heartbeat / batched delivery to reduce traffic analysis leakage (for example steady ~2s cadence and padded minimum frame size). | 🔴 Required |
| G-12 | Signed shared-node manifest, custom-node override, and automatic failover. | 🔴 Required |
| G-13 | Session rotation / revoke flow without rotating the user's SMSG keys. | 🔴 Required |
| G-14 | Gateway diff / cursor API for room timelines and trollbox until daemon pagination improves. | 🔴 Required |
| G-15 | Gateway parses OP_RETURN room names and caches them; shared-node clients must not call raw tx RPC for room-directory labels. | 🔴 Required |
| G-16 | Shared-node room-join / subscribe throttling and optional abuse challenge (PoW / captcha / invite code) for public-room spam control. | 🔴 Required |
| G-17 | Optional high-privacy broad-subscription mode for public rooms (subscribe to a small cover set, filter locally). | 🔴 Should support |

### D-11: Room metadata RPC (built in 0.20.3)

```
smsglistrooms [flags_filter]
    → Returns all TRANSACTION_SMSG_ROOM txids with parsed CSmsgRoomTx fields
    → Backed by SmsgRoomIndex (LevelDB, BaseIndex pattern) — no full chain scan

smsggetroominfo <room_txid>
    → Returns room details: flags, pubkey, room_address, retention, height,
      confirmations, topic (omega.room.<txid[0:12]>)
    → Room name still comes from the room tx OP_RETURN, so fetch raw tx if the
      UI needs the human-readable name
```

Both built in `src/smsg/rpcsmessage.cpp`. Index in `src/index/smsgroomindex.h/.cpp`.

**Current deployment note:** shared-node clients should consume cached parsed
room names from the gateway. In own-daemon mode, a temporary fallback to
`getrawtransaction` may still require `txindex=1` unless the daemon caches
parsed names or a future verbose `smsglistrooms` lands.

---

## §9 Development Phases

### Phase 1 — Foundation + Security Baseline (weeks 1-4)

Security is not a Phase 4 concern. Critical security infrastructure must
be in place from the first build.

| ID | Task | Priority |
|----|------|----------|
| P1-01 | Android project scaffold (Kotlin, Compose, Hilt, Room + SQLCipher) | MUST |
| P1-02 | Transport client abstraction (shared-node messaging API + JSON-RPC 2.0 over HTTP/TLS for own daemon) | MUST |
| P1-03 | Mode selector + connection screen (shared node or own daemon) | MUST |
| P1-04 | Sync service abstraction (foreground service / polling / WebSocket for shared node; ZMQ for own daemon) | MUST |
| P1-05 | Wallet tab: upgrade card in shared-node mode; full balance/send/receive in wallet-capable mode | MUST |
| P1-06 | Basic SMSG: trollbox, room messaging, and direct messages | MUST |
| P1-07 | Local SQLite schema (encrypted via SQLCipher): messages, rooms, contacts, settings | MUST |
| P1-08 | Android Keystore credential storage (EncryptedSharedPreferences, StrongBox when available) | MUST |
| P1-09 | Embedded Tor (tor-android) with toggle for both shared-node and own-daemon connections | MUST |
| P1-10 | CodeQL + Android Lint integration in CI pipeline | MUST |
| P1-11 | One-click daemon deployment script (`omega-deploy.sh`) | SHOULD |
| P1-12 | Integration tests for RPC client (mock daemon, TLS, auth) | MUST |
| P1-13 | OWASP Dependency-Check in CI pipeline | MUST |
| P1-14 | Tor DNS leak integration test | MUST |
| P1-15 | RPC rate-limiting / credential brute-force protection | SHOULD |
| P1-16 | Shared-node gateway contract: HTTPS/WebSocket API, internal RPC allowlist / denylist, no raw daemon exposure | MUST |
| P1-17 | Shared-node WebSocket bridge backed by backend-local ZMQ | MUST |
| P1-18 | Shared-node gateway rate limits, caching, abuse controls, and response caps | MUST |
| P1-19 | Shared-node authentication: device-auth keypair, onboarding challenge, short-lived signed session tokens | MUST |
| P1-20 | Shared-node traffic shaping: heartbeat frames, padded responses, batched push delivery | MUST |
| P1-21 | Own-daemon HD onboarding without default remote mnemonic fetch; `hdseedid` readiness + backup acknowledgement flow | MUST |
| P1-22 | Sensitive-screen policy: `FLAG_SECURE`, export-screen timeout, and best-effort secret-buffer wiping | MUST |
| P1-23 | Android background-delivery model: foreground service for live rooms/DMs, expedited catch-up jobs, clear delayed-delivery UX | MUST |

### Phase 2 — Rooms & Trollbox (weeks 5-8)

| ID | Task | Priority |
|----|------|----------|
| P2-01 | Trollbox tab: list + send + rate limit | MUST |
| P2-02 | Room creation flow (TRANSACTION_SMSG_ROOM via topic model) | MUST |
| P2-03 | Room chat view (message list via `smsggetmessages`, send, scroll) | MUST |
| P2-04 | Per-room keypair generation (fresh address per room) | MUST |
| P2-05 | Room directory tab (indexed via `smsglistrooms`, with raw-tx name decode as needed) | MUST |
| P2-06 | Room join flow (subscribe to topic for open; receive invite for private) | MUST |
| P2-07 | Room expiry logic (`retention_days`, daemon-verified, auto-resubscribe) | MUST |
| P2-08 | Moderation: kick/ban protocol + client enforcement + co-moderator delegation | SHOULD |
| P2-09 | Room invite flow (QR preferred; direct encrypted `smsgsend` for remote invite) | SHOULD |
| P2-10 | Room sender-address privacy controls (fresh per-room address, disclosure warning, rotation) | SHOULD |
| P2-11 | SMSG key backup/restore screen + room key backup | MUST |
| P2-12 | Room invite via QR code (in-person, zero network exposure) | SHOULD |
| P2-13 | In-app warning on joining open rooms (eavesdropping notice) | MUST |
| P2-14 | Jittered polling intervals (±20% random) | MUST |
| P2-15 | Security review checkpoint: room crypto, key handling, topic model | MUST |
| P2-16 | Daemon-less message sync worker: bucket polling / long-poll + local decrypt queue | MUST |
| P2-17 | Device-side SMSG key vault for no-daemon users (generate, import, export, backup prompts) | MUST |
| P2-18 | Shared-node failover + rate-limit handling | SHOULD |
| P2-19 | Locked-action UX: explain why create-room / wallet / escrow require own daemon or future autonomous wallet | MUST |
| P2-20 | Identity bundle + per-room pseudonym rotation + daemon-less device migration flow | MUST |
| P2-21 | Encryption-default UX rules: public banners for trollbox/open rooms, no false privacy affordances | MUST |
| P2-22 | Automatic DM key rotation + grace-period retirement (`smsgremoveprivkey` in own-daemon mode) | MUST |
| P2-23 | Replay / duplicate cache using `client_msgid` + timestamps + local cursor state | MUST |
| P2-24 | Room-name escaping, plain-text rendering, and public/private lock badges | MUST |
| P2-25 | Local cursor + delta sync for rooms/trollbox, with gateway diff/unread aggregation until richer daemon RPCs exist | MUST |
| P2-26 | Shared-node room-join throttling / abuse challenge for public-room spam control | SHOULD |
| P2-27 | Shared-node room-name cache integration; no direct raw-tx label lookups in shared-node mode | MUST |

### Phase 3 — Marketplace (weeks 9-12)

| ID | Task | Priority |
|----|------|----------|
| P3-01 | Marketplace topic subscription (`omega.marketplace`) | MUST |
| P3-02 | Browse listings UI (search, filter, categories) | MUST |
| P3-03 | Create listing flow (compose, publish to topic) | MUST |
| P3-04 | 2-of-3 multisig escrow address creation | MUST |
| P3-05 | PSBT flow (add multisig script, create with `walletcreatefundedpsbt`, inspect, sign, combine, finalise) | MUST |
| P3-06 | Purchase intent → escrow → release UI flow | MUST |
| P3-07 | Escrow timeout logic (nLockTime auto-release) | MUST |
| P3-08 | Dispute flow (arbiter involvement) | SHOULD |
| P3-09 | Listing expiry and refresh | SHOULD |
| P3-10 | Per-trade encrypted thread using fresh trade addresses (or invite-only private room) | SHOULD |
| P3-11 | PSBT size validation before SMSG send | MUST |
| P3-12 | PSBT field validation before signing (anti-tampering) | MUST |
| P3-13 | Fresh address per marketplace listing (enforced by app) | MUST |
| P3-14 | Marketplace anti-spam without paid topics: strict quotas, default listing anchor, mute/report, expiry pruning, optional PoW/bond on public caches | MUST |
| P3-15 | Escrow refund PSBT (buyer + arbiter, shorter nLockTime) | MUST |
| P3-16 | Anchor the trade-terms JSON before funding (`anchormsg` + `anchorcommit`); store anchor txid in both clients | MUST |
| P3-17 | Anchor verification on dispute: arbiter fetches anchored terms via `verifymsg` + `getmsgproof` | MUST |
| P3-18 | Anchor-commit scheduler: batch `anchormsg` calls, commit every 10 minutes or at `status=full` | SHOULD |
| P3-19 | Security review checkpoint: escrow flow, PSBT handling, arbiter key, anchor-chain integrity | MUST |
| P3-20 | Arbiter registry UI: signed/public arbiter list + custom arbiter override | MUST |
| P3-21 | Exact PSBT template reconstruction before signing, including allowed change outputs only | MUST |
| P3-22 | Marketplace scope guardrails: OMEGA-only escrow copy and disabled fiat/off-chain settlement claims | MUST |
| P3-23 | Paid-direct-SMSG and `smsgfund` integration tests for oversized escrow payload fallback | SHOULD |
| P3-24 | Descriptor-based escrow fallback validated if `createmultisig` / `addmultisigaddress` misbehave on testnet | MUST |

### Phase 4 — Hardening & Polish (weeks 13-16)

| ID | Task | Priority |
|----|------|----------|
| P4-01 | Live background delivery for active rooms/DMs (FCM-free foreground service + reconnect handling) | MUST |
| P4-02 | Offline message queue (send when reconnected) | SHOULD |
| P4-03 | Notification channels per room | SHOULD |
| P4-04 | Message search (local SQLite FTS over encrypted DB) | SHOULD |
| P4-05 | Battery optimisation: adaptive polling via WorkManager | MUST |
| P4-06 | Shared-node daemon-less mode polish: curated node list, failover, privacy warning, and mode badges | MUST |
| P4-07 | Wallet seed / key backup UX: daemon-host backup acknowledgement by default; screenshot warning only for expert or autonomous local-seed display | MUST |
| P4-08 | Daemon-assisted backup/export flow (`backupwallet`, expert `dumpwallet`, `smsgdumpprivkey`) | MUST |
| P4-09 | Automated UI tests | SHOULD |
| P4-10 | Full security audit: all crypto, all storage, all network paths | MUST |
| P4-11 | Biometric unlock option (BiometricPrompt) | SHOULD |
| P4-12 | Plain-language UX pass: hide txids, confirmations, hashes, and RPC jargon behind advanced drawers | SHOULD |
| P4-13 | Shared-node retention audit: verify no long-term plaintext cache and correct TTL/local purge behaviour | MUST |
| P4-14 | Shared-node session rotation / revoke UX + signed shared-node manifest refresh + custom-node override | MUST |
| P4-15 | Fuzz testing for SMSG parsing / envelope decode paths (daemon and app-side implementation) | MUST |
| P4-16 | Concurrency / race tests for wallet send + PSBT signing flows | MUST |
| P4-17 | Traffic-analysis hardening review: verify padding, batching, Tor isolation, and low-rate cover-traffic option | SHOULD |
| P4-18 | Optional high-privacy broad-subscription public-room mode (subscribe to 5-10 rooms, filter locally) | SHOULD |

### Phase 5 — Future (post-launch)

| ID | Task | Priority |
|----|------|----------|
| P5-01 | BIP157/158 autonomous light wallet upgrade (no daemon required) | SHOULD |
| P5-02 | Autonomous SMSG multi-peer sync (reduce dependence on shared-node APIs) | SHOULD |
| P5-03 | BitHome real estate integration (Nostr bridge) | COULD |
| P5-04 | iOS port (Kotlin Multiplatform or Swift rewrite) | COULD |
| P5-05 | Desktop Electron/Tauri companion app | COULD |
| P5-06 | Decentralised arbiter reputation system | COULD |
| P5-07 | Voice messages (compressed audio over SMSG paid) | COULD |
| P5-08 | File sharing (chunked over paid SMSG) | COULD |
| P5-09 | Room key rotation on-chain (SMSG_ROOM_UPDATE tx) | COULD |
| P5-10 | Multisig room ownership | COULD |
| P5-11 | DM session ratchet (PFS for direct messages and private rooms) | SHOULD |
| P5-12 | Optional E2E encrypted mode for open rooms (symmetric key overlay) | COULD |
| P5-13 | Cover traffic (dummy SMSG heartbeats for traffic analysis resistance) | COULD |
| P5-14 | Pseudonymous reputation system (`omega.reputation` topic) | COULD |
| P5-15 | Cryptographic group membership for stronger moderation | COULD |
| P5-16 | Opt-in DM receipts via `anchormsg` ("tap to anchor this message") | COULD |
| P5-17 | Room policy snapshots anchored on-chain (auditable rule history) | COULD |
| P5-18 | Versioned listings & multi-round negotiations using `prevhash` chains | COULD |
| P5-19 | Room creation, anchoring, and paid SMSG in autonomous wallet mode | SHOULD |
| P5-20 | Marketplace listing + escrow from autonomous wallet mode | SHOULD |
| P5-21 | Stealth-room design (unlisted / hidden room identifiers) | COULD |
| P5-22 | Stronger room-retention / deletion semantics at protocol level | COULD |
| P5-23 | Prekey-bundle / ratchet service design for stronger DM PFS | SHOULD |
| P5-24 | Expanded broad-subscription + cover-traffic automation for high-risk users | COULD |

---

## §10 Privacy & Security Threat Model

| # | Threat | Severity | Mitigation |
|---|--------|----------|-----------|
| T-01 | Shared node / daemon operator sees metadata or censors traffic | HIGH | Tor by default, multi-node failover, signed shared-node manifest, never upload wallet seed, never import SMSG private keys into shared node, own daemon for highest privacy. |
| T-02 | Network observer correlates addresses | HIGH | Fresh address per room; `smsgsendanon` for DMs; no reuse across rooms |
| T-03 | Room member enumeration | MEDIUM | No member list broadcast; open rooms have no roster; private rooms invite-only |
| T-04 | User tracking across rooms | HIGH | Separate keypair per room (unlinkable); no global identity or profile |
| T-05 | IP address leakage | HIGH | Embedded Tor for shared-node and own-daemon connections; self-hosted daemon accessible only via .onion or LAN |
| T-06 | Message metadata (timing) | MEDIUM | SMSG bucket relay adds natural mixing delay (60s buckets); no read receipts; gateway heartbeat/batching/padding in shared-node mode |
| T-07 | Local device compromise — DB | HIGH | SQLCipher-encrypted database; StrongBox/Keystore-wrapped key, optional user passphrase overlay, and minimal plaintext lifetime |
| T-08 | Local device compromise — credentials | HIGH | EncryptedSharedPreferences; optional biometric gate (BiometricPrompt) |
| T-09 | Local device compromise — screenshots / recordings | MEDIUM | `FLAG_SECURE`, export-screen timeout, suppressed ordinary screen capture, and explicit consent on sensitive export screens |
| T-10 | Lost/stolen device | MEDIUM | Remote daemon wallet unaffected in sovereign mode; daemon-less mode still stores SMSG keys locally, so enforce SQLCipher, Keystore, biometric gate, and backup guidance |
| T-11 | Trollbox identity leakage | LOW | Warning shown before first trollbox post; address visible by design |
| T-12 | RPC interception (MITM) | HIGH | TLS with TOFU cert pinning; Tor .onion provides built-in authentication |
| T-13 | ZMQ interception | HIGH | ZMQ tunnelled through Tor/VPN/stunnel; never exposed on plain TCP to internet |
| T-14 | Mnemonic interception over RPC | HIGH | Default flow never fetches mnemonic to the phone; generate/restore and display backup words on daemon host. Expert remote seed transport only behind explicit danger gate and TLS/Tor |
| T-15 | Third-party analytics leakage | MEDIUM | Zero analytics SDKs; all dependencies audited; no Firebase/Crashlytics |
| T-16 | Arbiter key compromise | HIGH | Multisig arbiter (2-of-3); nLockTime auto-release limits exposure window |
| T-17 | Room moderation key loss | MEDIUM | Mandatory key backup at creation; co-moderator delegation; key rotation (future) |
| T-18 | PSBT tampering in transit | MEDIUM | PSBT exchanged via direct encrypted SMSG between fresh trade addresses; app validates PSBT fields before signing |
| T-19 | Stale room expiry (offline app) | LOW | Daemon-side verification via `smsggetmessages`; auto-resubscribe on new activity |
| T-20 | No PFS — long-term key compromise decrypts all past messages | HIGH | Launch requirement: explicit `NO PFS` badge + mandatory 30-day DM rotation. Per-room addresses limit blast radius; ratchet remains future work. See §2.0. |
| T-21 | Public room eavesdropping (passive subscription) | MEDIUM | Documented by design; in-app warning on join; optional E2E overlay for sensitive topics (Phase 5+) |
| T-22 | Spam flooding (rooms / marketplace) | MEDIUM | App-side rate limit per address, room-join throttles/challenges on shared nodes, marketplace quotas, default listing anchor, mute/report controls, and optional PoW/bond friction on public caches. See §2.0. |
| T-23 | Room invite exposes private key in SMSG network | MEDIUM | QR code preferred; short TTL for SMSG invites (1 day); ephemeral invite keys (future). See §4.6. |
| T-24 | Marketplace seller identity linking via address reuse | HIGH | Fresh address per listing and fresh trade-specific direct-message addresses. See §5.1. |
| T-25 | Traffic analysis (polling timing correlation) | MEDIUM | Jittered polling intervals (±20% random), WebSocket heartbeat/padding/batching, Tor circuit isolation, optional cover traffic (future). See below. |
| T-26 | Moderation bypass via new address | LOW | Advisory only; documented in app. Shared-node join throttles/challenges slow abuse but do not solve Sybil attacks. Cryptographic group membership remains future research. |
| T-27 | DNS leaks bypassing Tor | HIGH | Remote DNS via SOCKS5; no system DNS when Tor enabled; integration test verification. See §7.2. |
| T-28 | PSBT manipulation by counterparty | MEDIUM | `analyzepsbt` → `decodepsbt` → anchored-template compare before any signing; reject on any mismatch. See §5.3. |
| T-29 | Dependency supply chain (compromised library) | MEDIUM | OWASP Dependency-Check in CI; pinned versions; open-source only. See §7.3. |
| T-30 | Shared node sees plaintext if daemon-less mode falls back to relay-side encryption | HIGH | Release gate: direct/private-room messaging on shared nodes only after client-side SMSG envelope support; public-room traffic explicitly marked public |
| T-31 | Shared node outage or censorship | MEDIUM | Multiple community nodes, health scoring, automatic failover, local outbox retry, and own-daemon upgrade path |
| T-32 | Shared node accidentally exposes raw daemon RPC | HIGH | Gateway boundary: HTTPS/WebSocket only; internal RPC allowlist / denylist; no raw credentials shipped to clients |
| T-33 | Duplicate / replay delivery from multi-node failover or bucket rescan | MEDIUM | Deduplicate by `msgid` / message hash, track sync cursors, and treat repeated ciphertext as idempotent |
| T-34 | Private-room metadata leak via on-chain room creation tx | MEDIUM | Default directory hides private rooms; users join by invite only; true stealth rooms deferred to future protocol work |
| T-35 | Shared-node abuse or enumeration without strong client auth | HIGH | Device-auth keypair, onboarding challenge, short-lived signed session tokens, per-token quotas, rapid revoke/rotation |
| T-36 | WebSocket frame size / cadence reveals active room or DM state | MEDIUM | Fixed-size or padded chunks, batched updates, steady heartbeats, avoid one-frame-per-message push |
| T-37 | Shared node infers joined public rooms over time | MEDIUM | Separate gateway-auth identity from SMSG identities, rotate gateway sessions, optional broad-subscription privacy mode, own-daemon mode for strongest protection |
| T-38 | User-generated room/listing metadata rendered unsafely in UI | MEDIUM | Plain-text rendering only, HTML escaping, no autolink, and defensive validation on both daemon and client |
| T-39 | Android background execution limits delay messages or create false "real-time" expectations | MEDIUM | Foreground service for active rooms/DMs, expedited catch-up work, and explicit delayed-delivery UX when Doze/battery saver intervenes |

### §10.1 Adversary model

| Adversary | Capability | Primary threats |
|-----------|-----------|-----------------|
| Passive network observer | Sees traffic patterns, IP addresses, message timing | T-05, T-06, T-25 |
| Malicious SMSG node | Subscribes to topics, records all public messages, correlates metadata | T-01, T-21, T-24 |
| Compromised daemon operator | Full access to RPC, wallet, SMSG keys | T-01, T-14, T-20 |
| Device malware / thief | Access to local storage, Android Keystore (if rooted) | T-07, T-08, T-09, T-10 |
| Counterparty in trade | Crafts malicious PSBTs, attempts to steal escrow | T-16, T-28 |
| Banned room user | Creates new addresses, re-joins rooms | T-26 |
| Malicious shared-node gateway | Tries to enumerate users, room interests, or traffic patterns | T-01, T-35, T-36, T-37 |

### §10.2 Behavioural obfuscation (traffic analysis resistance)

Fixed polling intervals (15s, 30s) create detectable patterns. An attacker
monitoring network traffic can correlate exact timing with user activity.

**Mitigations:**
- **Jittered polling:** Add ±20% random jitter to all poll intervals.
  E.g. 15s ± 3s = 12-18s random uniform.
- **Padded push:** Shared-node WebSocket responses should be padded to a small
  fixed-size bucket (for example at least ~1 KB) and emitted on a steady
  heartbeat (for example every ~2 seconds) instead of "exactly one frame when
  a message arrives".
- **Batching:** Coalesce multiple updates for up to a short window (for example
  2 seconds) before delivery, reducing room-specific timing spikes.
- **Cover traffic (optional, future):** When Tor is enabled, send periodic
  encrypted SMSG heartbeat messages (empty payload, self-addressed) at
  random intervals. Makes real messages indistinguishable from cover.
- **Burst suppression:** When opening a room, stagger message fetches
  rather than fetching all at once (avoids traffic spike = "user opened
  room X at time T").
- **Tor circuit rotation:** New circuits every 10 minutes; per-room
  circuit isolation via `IsolateSOCKSAuth`.
- **Gateway-auth rotation:** rotate or refresh gateway session identities
  independently of messaging keys so long-lived room subscriptions are harder
  to correlate at one backend.
- **High-privacy public-room mode:** optionally subscribe to a small cover set
  of public rooms (for example 5-10) and filter locally. This increases
  bandwidth cost but reduces obvious room-membership signals to the shared
  node.

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
| Q-06 | Arbiter selection — project-run or community? | P3-08 | Bootstrap with a project-published signed arbiter registry, but let users choose any compatible arbiter or enter one manually |
| Q-07 | SMSG paid message fee rate — current value? | P2-07 | Retrieve via `smsggetfeerate` RPC; display to user before sending paid messages |
| Q-08 | ~~Android minimum API level~~ | ~~P1-01~~ | **RESOLVED: API 28 (Android 9.0)** — required for BiometricPrompt and modern Keystore |
| Q-09 | ~~Room OP_RETURN metadata format — what fields?~~ | ~~P2-02~~ | **RESOLVED: `smsgcreateroom` writes OP_RETURN with room name (max 64 chars). Topic derived from txid: `omega.room.<txid[0:12]>`.** |
| Q-10 | ~~`smsgzmqpush` — does it work for real-time?~~ | ~~P1-04~~ | **RESOLVED: Superseded by native `zmqpubhashsmsg` / `zmqpubrawsmsg` publishers.** `smsgzmqpush` remains for ad-hoc query. |
| Q-11 | Tor bootstrap time — acceptable for UX? | P1-09 | Test embedded tor-android cold start latency; target < 15 seconds |
| Q-12 | Known Particl SMSG bugs or CVEs? | D-17 | Review Particl issue tracker and changelogs for SMSG-related fixes |
| Q-13 | PoW difficulty increase — consensus change needed? | D-18 | Evaluate impact; may require hard fork or soft activation |

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
| 1-6 | Write shared-node gateway contract: HTTPS/WebSocket surface, internal RPC allowlist/denylist, no raw daemon exposure, signed manifest format | Reviewed API spec committed |
| 1-7 | Design shared-node auth bootstrap: device-auth keypair, onboarding challenge, short-lived session token, revoke/rotate flow | Threat model + API contract committed |
| 1-8 | Implement StrongBox-aware key storage, `FLAG_SECURE` policy, and secret-buffer wipe helpers | Sensitive-screen baseline committed |

#### WEEK 2 — Connection + Tor + ZMQ

| # | Action | Deliverable |
|---|--------|-------------|
| 2-1 | Daemon connection screen (host, port, user, pass, Tor toggle, Test button) | User can configure and test a connection |
| 2-2 | Embed `tor-android`; Tor bootstrap; SOCKS5 proxy for OkHttp + JeroMQ | App connects to `.onion` daemon |
| 2-3 | DNS leak integration test (verify no non-Tor traffic) | Test in CI |
| 2-4 | ZMQ subscriber service: `hashblock`, `rawtx`, (optionally `smsg`) | Background service reconnects and logs events |
| 2-5 | Fallback polling + catch-up service with jittered intervals (WorkManager + expedited reconnect jobs) | Battery-safe polling when ZMQ unavailable |
| 2-6 | Shared-node backend bridge: local ZMQ → WebSocket, quotas, caching, auth checks, padded frames, and batched push delivery | Shared-node push path works without raw RPC exposure |
| 2-7 | Foreground-service live sync mode for active room/DM sessions | Persistent notification + real-time background delivery works |
| 2-8 | Tor bootstrap progress UI + cached-state reuse + reduced-privacy clearnet warning path | Tor startup UX is explicit and understandable |

#### WEEK 3 — Wallet tab

| # | Action | Deliverable |
|---|--------|-------------|
| 3-1 | Wallet tab: `getbalances` display, pull-to-refresh | Balance shown correctly |
| 3-2 | Receive screen: `getnewaddress` + QR code (ZXing) | QR displayed with `omega:` URI |
| 3-3 | Send screen: address input, amount, `estimatesmartfee`, `sendtoaddress` | Test send on testnet |
| 3-4 | Transaction history: `listtransactions` + local cache (SQLCipher) | Scrollable list, offline viewing |
| 3-5 | HD wallet onboarding: `listwallets`/`createwallet`/`loadwallet` + `upgradetohd` with daemon-host backup flow + `hdseedid` confirmation (no default remote mnemonic fetch) | Full flow works on testnet daemon |

#### WEEK 4 — Basic SMSG + direct messages

| # | Action | Deliverable |
|---|--------|-------------|
| 4-1 | SMSG enable check (`smsggetinfo`), key registration (`smsgaddlocaladdress`) | App auto-enables SMSG on connect |
| 4-2 | Send DM UI: own-daemon via `smsgsendanon`, shared-node via relay/injection path | Message arrives in both access modes |
| 4-3 | Receive DMs: own-daemon `smsginbox` / ZMQ, shared-node candidate-envelope sync → local SQLCipher storage | Incoming messages display in conversation view |
| 4-4 | Conversations list (group by sender address, unread badge) | Basic chat UX working |
| 4-5 | Phase 1 security review checkpoint | Checklist: TLS, Tor, SQLCipher, Keystore all verified |
| 4-6 | Identity bundle: fresh per-room / DM pseudonyms, encrypted key-vault export/import, replay cache, public-vs-private send warnings | Daemon-less identities rotate and migrate safely |
| 4-7 | DM/private-room security badge: `CONFIDENTIAL · NO PFS · ROTATES 30D` + mandatory rotation schedule | Users see current confidentiality limits clearly |

#### WEEKS 5-6 — Trollbox + rooms

| # | Action | Deliverable |
|---|--------|-------------|
| 5-1 | Trollbox tab: read via `trollboxlist`, send via daemon RPC or shared-node relay path, rate limit, character counter | Matches omega-qt trollbox UX |
| 5-2 | Room creation flow: construct `TRANSACTION_SMSG_ROOM` (type 8), submit | Room tx confirmed on chain |
| 5-3 | Room directory: `smsglistrooms` → paginated list, cached names, plain-text rendering, join button | User can browse and join rooms |
| 5-4 | Room chat: `smsgsubscribe` topic, `smsggetmessages`, local/gateway cursors, send via topic broadcast form or shared-node relay path | Messages flow in open room |
| 5-5 | Per-room keypair generation (fresh address per room via `getnewaddress`) | Addresses not reused across rooms |
| 5-6 | Open room eavesdropping warning dialog on first join | User must acknowledge before seeing messages |
| 5-7 | Public directory policy: show open rooms by default, private rooms by invite/txid only | Private rooms hidden from default browse/search UX |
| 5-8 | Public/private badges and lock-state cues across room list + chat composer | Visibility model obvious before every send |
| 5-9 | Shared-node room-name cache + join throttling / abuse challenge integration | Shared-node room browse/join path avoids raw-tx lookups and obvious spam loops |

#### WEEKS 7-8 — Room moderation + invites + expiry

| # | Action | Deliverable |
|---|--------|-------------|
| 7-1 | Room expiry: track last activity + room `retention_days`, daemon-verified, auto-resubscribe | Expired rooms greyed out; revived on new message |
| 7-2 | Kick/ban protocol: JSON mod commands, signature verification, client-side filter | Banned users' messages hidden |
| 7-3 | Room invite via direct encrypted `smsgsend` (1-day TTL) + QR code alternative | Both invite methods work |
| 7-4 | Co-moderator delegation (`mod_delegate` message) | Delegated moderator can kick/ban |
| 7-5 | SMSG key backup/restore screen + encrypted local key-vault export file + room key WIF export | User can back up and restore all keys |
| 7-6 | Room sender-address disclosure warning + automatic DM rotation / retirement controls | Privacy model clear in room settings |
| 7-7 | Phase 2 security review checkpoint | Room crypto, key handling, topic model reviewed |
| 7-8 | Room-retention honesty pass: stale/expired copy, local purge, shared-node plaintext retention checks | UI does not promise hard deletion |

#### WEEKS 9-12 — Marketplace + escrow

| # | Action | Deliverable |
|---|--------|-------------|
| 9-1 | Marketplace topic subscription (`omega.marketplace`) + browse UI | Listings display with search/filter |
| 9-2 | Create listing flow: fresh address, compose + free topic publish + quota / anchor / anti-spam controls | Listing visible to other nodes |
| 9-3 | 2-of-3 multisig escrow: `createmultisig`, fund, refund PSBT | Escrow address created and funded on testnet |
| 9-4 | PSBT flow: `addmultisigaddress`, `walletcreatefundedpsbt`, exact-template validation, sign, exchange via direct encrypted `smsgsend`, finalise | Full escrow round-trip on testnet |
| 9-5 | nLockTime auto-release + refund PSBT (buyer+arbiter, shorter timelock) | Both timeout paths tested |
| 9-6 | Per-trade encrypted thread keyed locally by `trade_id` / peer addresses | Escrow messages separated from room chat |
| 9-7 | Anchor trade terms before funding: hash canonical JSON → `anchormsg` → `anchorcommit` | Anchor txid surfaced in UI; `verifymsg` returns `anchored=true` |
| 9-8 | Anchor-commit scheduler service (batch window + `status=full` trigger) | Queue + commit tested under load |
| 9-9 | Listing receipt: anchor listing hash; `getmsgproof` wired into listing detail view | Buyers can verify listing existed at time T |
| 9-10 | Arbiter registry + custom arbiter entry flow | Buyer and seller can choose arbiter explicitly |
| 9-11 | `smsgfund` test path for paid direct SMSG fallback | Oversized escrow payload path verified |
| 9-12 | Descriptor-based escrow fallback test (`getdescriptorinfo` / `deriveaddresses` / `createpsbt` / `utxoupdatepsbt`) | Escrow remains viable if multisig convenience RPCs fail |
| 9-13 | Phase 3 security review checkpoint | Escrow flow, PSBT validation, arbiter key, anchor integrity reviewed |

#### WEEKS 13-16 — Hardening, polish, audit

| # | Action | Deliverable |
|---|--------|-------------|
| 13-1 | Battery optimisation pass: adaptive polling via WorkManager | Battery usage profiled and acceptable |
| 13-2 | Daemon-assisted backup/export UX (`backupwallet`, expert `dumpwallet`, room-key export) | Backup/restore round-trip verified |
| 13-3 | Biometric unlock option | BiometricPrompt gates app launch |
| 13-4 | Message search (SQLite FTS5 over SQLCipher) | Full-text search across all conversations |
| 13-5 | Offline message queue (outbox, send on reconnect) | Messages queued and sent after reconnect |
| 13-6 | Full security audit: all crypto, storage, network paths, dependencies | Written audit report with findings |
| 13-7 | SMSG daemon code audit (D-17): review Particl port diffs | Written audit report |
| 13-8 | Automated UI tests (Espresso / Compose test) | Core user flows covered |
| 13-9 | `omega-deploy.sh` v1.0: polished, tested on Hetzner + Raspberry Pi | Documented, one-command deploy |
| 13-10 | Fuzz SMSG parsing / envelope decode paths and fix crashes | Fuzz corpus + CI job committed |
| 13-11 | Concurrency tests for send / sign / timeout wallet flows | Race conditions identified or ruled out |
| 13-12 | Shared-node manifest refresh + custom-node override + session-rotation UX | Failover and auth lifecycle verified |
| 13-13 | Optional high-privacy public-room mode (subscribe to 5-10 rooms, filter locally) | Shared node sees less precise room-interest metadata |

#### POST-LAUNCH — first actions

| # | Action |
|---|--------|
| L-1 | Publish to F-Droid (open-source, no Google Play dependency) |
| L-2 | Publish APK on GitHub Releases (signed, reproducible build) |
| L-3 | Set up community arbiter multisig (2-of-3, keys in hardware) |
| L-4 | Begin daemon-less adoption track: BIP157/158 wallet + autonomous SMSG sync before DM ratchet |

---

### §13.3 Repository structure (proposed)

```
omega-messenger/
├── app/                          # Android app module
│   ├── src/main/java/org/omega/messenger/
│   │   ├── data/                 # Repository, RPC client, shared-node client, SQLCipher DB
│   │   ├── domain/               # Use cases (SendMessage, JoinRoom, CreateEscrow)
│   │   ├── ui/                   # Compose screens per tab
│   │   │   ├── wallet/
│   │   │   ├── rooms/
│   │   │   ├── trollbox/
│   │   │   ├── directory/
│   │   │   ├── marketplace/
│   │   │   └── settings/
│   │   ├── service/              # ZMQ subscriber, shared-node sync, Tor manager, WorkManager workers
│   │   ├── crypto/               # SMSG envelope code, local encryption helpers, PSBT validation
│   │   └── lightwallet/          # BIP157 filters, headers, local wallet state (Phase 5+)
│   └── src/test/                 # Unit tests
├── rpc-client/                   # Standalone JSON-RPC library for own-daemon mode
├── shared-node-client/           # Shared-node messaging transport + failover policy
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
| R-06 | `smsglistrooms` | Scan blockchain index for all type 8 txs. Return array of `{txid, version, flags, flags_readable, pubkey, retention_days, max_members, height, topic}`. Optional filter by flags only. Room name still requires raw-tx OP_RETURN decode if the UI wants it. | 🟢 Built — `rpcsmessage.cpp`, uses `SmsgRoomIndex` |
| R-07 | `smsggetroominfo` | Given a room txid, return full room details: `{txid, version, flags, flags_readable, pubkey, room_address, retention_days, max_members, height, confirmations, topic}`. | 🟢 Built — `rpcsmessage.cpp` |
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
   omega-cli -testnet generatetoaddress 89 <miner_address>
   → Verify: room txs rejected ("bad-tx-smsgroom-not-active")

6. Mine to block 100 (fork activation):
   omega-cli -testnet generatetoaddress 11 <miner_address>
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
