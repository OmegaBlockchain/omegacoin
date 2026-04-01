# Trollbox — Communal Chat for Omega Core

## Overview

A public chatroom ("Trollbox") built on top of the existing SMSG protocol. All Omega nodes
share a single well-known keypair so every node can decrypt every Trollbox message. Messages
propagate through the normal SMSG bucket/inventory system — no new P2P messages needed.

---

## Design Decision: Why a Hardcoded Keypair Is Required

**Q: Can we do this without an address/private-key/public-key pair?**

**A: No.** The SMSG protocol encrypts every message to a specific recipient public key
(ECDH + AES-256-CBC). There is no unencrypted broadcast mode. To make a "public" channel
we need one fixed keypair that every node knows:

| Component | Value | Purpose |
|-----------|-------|---------|
| Private key | Hardcoded in `smessage.h` | All nodes import it → all can decrypt |
| Public key | Derived from private key | Sender encrypts to this |
| Address | Derived from public key | Used as `addressTo` in `CSMSG::Send()` |

The "encryption" is structurally present (SMSG demands it) but provides no secrecy — every
node holds the key. This is intentional: the Trollbox is a public chat.

### Why not add an unencrypted message type?
- Would require changes to the P2P message protocol, validation, and storage layers.
- Risk of breaking existing SMSG peers or creating incompatible forks.
- The hardcoded-key approach reuses 100 % of existing infrastructure with zero protocol changes.

---

## Anti-Spam & Anti-DDoS Strategy

Multiple layers, combining existing SMSG protections with Trollbox-specific limits:

### Layer 1 — Existing SMSG Protections (free)
| Protection | How it works |
|-----------|--------------|
| **Proof-of-Work** | `CSMSG::SetHash()` — brute-force ~19-bit PoW hash per message. Costs CPU time (~50-500 ms) making mass spam expensive. |
| **Bucket TTL** | Messages auto-expire. `SMSG_RETENTION = 31 days` (we shorten Trollbox to 24 h). |
| **Peer misbehavior scoring** | Invalid/malformed messages increment peer misbehavior → ban at threshold. |
| **Peer ignore timeout** | Peers that fail to deliver are ignored for 90 seconds. |

### Layer 2 — Trollbox-Specific Protections (new)
| Protection | Detail |
|-----------|--------|
| **Message size cap** | 256 characters max (vs 24 KB normal SMSG). Limits payload spam. |
| **Client-side rate limit** | 1 message per 30 seconds per node. Enforced in GUI + RPC. Cooldown timer shown to user. |
| **Identified senders** | Messages signed with sender's SMSG address → enables mute/ignore. Anonymous sending disabled for Trollbox. |
| **Local mute list** | Right-click "Mute sender" — address stored in `smsg.ini`, messages from muted addresses hidden locally. |
| **Short retention** | Trollbox messages expire after 24 hours (vs 31 days for normal SMSG). Limits persistent spam and DB growth. |
| **Max display buffer** | GUI shows last 200 messages. Older messages scrolled out (still in DB until TTL expires). |

### Layer 3 — Paid Highlight Mode (implemented)
| Protection | Detail |
|-----------|--------|
| **Paid Trollbox message** | User checks "Paid (red highlight)" checkbox. Message sent as paid SMSG (1-day retention). Paid messages display in **red** in the chat. Standard messages display in black. The fee is determined by SMSG protocol (`nMsgFeePerKPerDay × size × days`). |

### Layer 4 — Optional Future Hardening
| Protection | Detail |
|-----------|--------|
| **Difficulty scaling** | Increase PoW difficulty if > N messages per bucket (adaptive). |
| **Masternode-only relay** | Only masternodes relay Trollbox messages — reduces amplification surface. |

---

## Architecture

### Message Flow

```
User types message in Trollbox tab
        │
        ▼
MessagingPage::onTrollboxSendClicked()
  - Rate-limit check (30 s cooldown)
  - Size check (≤ 256 chars)
        │
        ▼
CSMSG::Send(userAddr, TROLLBOX_ADDRESS, message)
  - Encrypts to hardcoded Trollbox pubkey
  - Proof-of-Work computed (SetHash)
  - Stored in local bucket
  - Propagated to peers via smsgInv
        │
        ▼
All peers receive via normal SMSG bucket sync
  - smsgInv → smsgShow → smsgHave → smsgWant → smsgMsg
        │
        ▼
Each peer's CSMSG::ScanMessage()
  - Tries Trollbox private key → decrypt succeeds
  - Detects addressTo == TROLLBOX_ADDRESS → store with "tb" DB prefix
  - Fires NotifySecMsgTrollboxChanged signal
        │
        ▼
MessagingPage Trollbox tab
  - Signal triggers refresh
  - Shows message in chat-style scrolling view
  - Sender address shown (truncated), timestamp, message text
```

### Database Storage

| Prefix | Format | Purpose |
|--------|--------|---------|
| `"tb"` | `tb` + timestamp + msgHash | Trollbox messages (separate from inbox `"im"`) |

Separate prefix prevents Trollbox messages from cluttering the user's inbox.

### Constants (new in `smessage.h`)

```cpp
// Trollbox well-known keypair (all nodes share this — public chat, no secrecy)
const char* const TROLLBOX_PRIVKEY_HEX = "<64-hex-char-private-key>";
const CAmount TROLLBOX_MAX_MSG_BYTES = 256;          // max message length (chars)
const unsigned int TROLLBOX_RETENTION = 24 * 60 * 60; // 24 hours
const int TROLLBOX_RATE_LIMIT_SECS = 30;              // min seconds between sends
const int TROLLBOX_MAX_DISPLAY = 200;                  // max messages in GUI buffer
```

> **Note**: The actual private key will be generated once at development time using
> `omegad -rpccommand` or `openssl ecparam`, then hardcoded. Since it's public knowledge
> by design, there is no security concern in committing it to source.

---

## Implementation Plan

### Phase 1: Core Infrastructure

**Files modified**: `smessage.h`, `smessage.cpp`, `db.h`, `db.cpp`

| Step | Task | Detail |
|------|------|--------|
| 1.1 | Generate Trollbox keypair | Create one secp256k1 keypair. Derive Omega address. Hardcode private key hex, public key, and address as constants in `smessage.h`. |
| 1.2 | Auto-import on SMSG start | In `CSMSG::Start()`, after `LoadKeyStore()`, import the Trollbox private key into `keyStore` and add the Trollbox address to `addresses` vector with receive=true. Skip if already present. |
| 1.3 | DB storage with "tb" prefix | Add `ReadTrollbox()` / `WriteTrollbox()` / `EraseTrollbox()` to `SecMsgDB`. Same pattern as inbox (`"im"`) but with `"tb"` prefix. |
| 1.4 | Trollbox message detection | In `CSMSG::ScanMessage()` (or `Decrypt()`/`Store()`): when `addressTo == trollboxAddress`, store with `"tb"` prefix instead of `"im"`. |
| 1.5 | Signal for new messages | Add `NotifySecMsgTrollboxChanged` boost signal (same pattern as `NotifySecMsgInboxChanged`). Fire it when a Trollbox message is stored. |
| 1.6 | Retention enforcement | In `CSMSG::SecureMsgWalletKeyChanged()` / bucket cleanup: Trollbox messages expire after `TROLLBOX_RETENTION` (24 h) instead of `SMSG_RETENTION` (31 days). |
| 1.7 | Rate limiting | Add `nLastTrollboxSend` timestamp to `CSMSG`. Check elapsed time ≥ `TROLLBOX_RATE_LIMIT_SECS` before allowing send. Return error code if too soon. |

### Phase 2: GUI — Trollbox Tab

**Files modified**: `messagingpage.h`, `messagingpage.cpp`, `messagingpage.ui`

| Step | Task | Detail |
|------|------|--------|
| 2.1 | Add Trollbox tab to UI form | New tab in `tabWidget` after Keys tab. Contains: `QTextBrowser` (chat display, read-only, HTML-formatted), `QComboBox` "From:" address selector + `QCheckBox` "Paid (red highlight)" above the input, `QLineEdit` (message input, max 256 chars) + `QPushButton` ("Send") + `QLabel` (cooldown timer). |
| 2.2 | Chat display widget | `QTextBrowser` with `setOpenExternalLinks(false)`. Messages rendered as HTML blocks: `<span style="color:gray">[HH:MM]</span> <b>Oxxxx...xxxx:</b> message text`. Auto-scroll to bottom on new message. |
| 2.3 | Send handler | `onTrollboxSendClicked()`: validate length ≤ 256, check rate limit, call `CSMSG::Send(userAddr, trollboxAddr, msg)`, clear input on success. On rate-limit violation, show remaining seconds on cooldown label. |
| 2.4 | Load existing messages | `updateTrollboxList()`: read all `"tb"` entries from DB, decrypt each, populate `QTextBrowser`. Called on tab switch and on signal. Cap at `TROLLBOX_MAX_DISPLAY` most recent. |
| 2.5 | Signal connection | Connect `NotifySecMsgTrollboxChanged` → set `fTrollboxChanged = true` → `updateScheduled()` refreshes on 3-second timer (same pattern as inbox/outbox). |
| 2.6 | Cooldown timer display | `QTimer` (1-second interval) updates cooldown label: "Wait 23s..." → "Wait 22s..." → hidden when ready. Send button disabled during cooldown. |
| 2.7 | Context menu | Right-click on chat area: "Copy Message", "Copy Sender Address", "Mute Sender". |

### Phase 3: Mute List & Settings

**Files modified**: `smessage.h`, `smessage.cpp`, `messagingpage.cpp`

| Step | Task | Detail |
|------|------|--------|
| 3.1 | Mute list storage | `std::set<CKeyID> trollboxMuteList` in `CSMSG`. Persisted in `smsg.ini` under `[trollbox_mute]` section. |
| 3.2 | Mute/unmute functions | `CSMSG::TrollboxMute(address)` / `TrollboxUnmute(address)`. Add to set, save ini. |
| 3.3 | Filter muted on display | `updateTrollboxList()` skips messages from addresses in mute list. |
| 3.4 | "Mute Sender" context menu | Right-click → "Mute Sender" adds sender to mute list, refreshes chat. |
| 3.5 | Muted senders management | Optional: small "Manage Muted" button showing list of muted addresses with "Unmute" option. |

### Phase 4: RPC Interface

**Files modified**: `rpcsmessage.cpp`

| Step | Task | Detail |
|------|------|--------|
| 4.1 | `trollboxsend` RPC | `trollboxsend "message"` — sends Trollbox message from first available SMSG address. Respects rate limit. |
| 4.2 | `trollboxlist` RPC | `trollboxlist [count]` — returns last N Trollbox messages as JSON array: `[{time, from, text}, ...]`. Default count=50. |
| 4.3 | `trollboxmute` RPC | `trollboxmute "address"` — add address to mute list. |
| 4.4 | `trollboxunmute` RPC | `trollboxunmute "address"` — remove address from mute list. |

### Phase 5: Polish & Hardening

| Step | Task | Detail |
|------|------|--------|
| 5.1 | `-notrollbox` flag | Command-line option to disable Trollbox entirely. Trollbox messages are not stored or displayed. The Trollbox tab shows "Trollbox is disabled" with no send functionality. |
| 5.2 | Unread indicator | Tab title shows "Trollbox (3)" with unread count when tab is not active. Clears when user switches to Trollbox tab. |
| 5.3 | Sender coloring | Deterministic color per sender address (hash address → HSL hue). Makes it easier to follow conversations. |
| 5.4 | Notification sound | Optional beep/notification on new Trollbox message (off by default, toggle in settings). |

---

## File Change Summary

| File | Changes |
|------|---------|
| `src/smsg/smessage.h` | Trollbox constants, `nLastTrollboxSend`, `trollboxMuteList`, `NotifySecMsgTrollboxChanged` signal |
| `src/smsg/smessage.cpp` | Auto-import keypair in `Start()`, Trollbox detection in `ScanMessage()`/`Store()`, rate limit check, mute list I/O, retention logic |
| `src/smsg/db.h` | `ReadTrollbox()`, `WriteTrollbox()`, `EraseTrollbox()`, `NextTrollbox()` |
| `src/smsg/db.cpp` | Implementations of `"tb"` prefix DB operations |
| `src/qt/forms/messagingpage.ui` | New Trollbox tab with QTextBrowser, QLineEdit, QPushButton, QLabel |
| `src/qt/messagingpage.h` | Trollbox slots, timer, mute list members, signal connection |
| `src/qt/messagingpage.cpp` | `setupTrollboxTab()`, `updateTrollboxList()`, `onTrollboxSendClicked()`, cooldown timer, context menu, mute handler |
| `src/smsg/rpcsmessage.cpp` | `trollboxsend`, `trollboxlist`, `trollboxmute`, `trollboxunmute` RPCs + command table entries |

---

## Security Considerations

| Threat | Mitigation |
|--------|-----------|
| **Message flood / DDoS** | PoW per message + 30s rate limit + 256-char cap + 24h TTL |
| **Large payload attack** | 256-byte message cap (vs 24 KB normal SMSG) |
| **Network amplification** | Normal SMSG bucket protocol — nodes only fetch messages they don't have |
| **Offensive content** | Mute list per user; `-notrollbox` to opt out entirely |
| **Sybil spam (multiple addresses)** | PoW cost per message regardless of sender identity |
| **Key compromise** | Key is public by design — no secret to compromise |
| **Replay attacks** | SMSG timestamps + bucket deduplication prevent replay |
| **Memory exhaustion** | GUI caps at 200 messages; DB entries auto-expire at 24h |

---

## Implementation Order

**Recommended**: Phase 1 → Phase 2 → Phase 4 → Phase 3 → Phase 5

Phase 4 (RPC) before Phase 3 (mute) because RPCs allow testing the backend without GUI,
and the mute list is a polish feature that can come after core functionality works.
