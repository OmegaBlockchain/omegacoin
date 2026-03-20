# SMSG Protocol — Omega RPC Reference

**Source:** Live audit of omega-cli help output, 2026-03-20.
**Status:** Confirmed working on two peered nodes running omega-qt with SMSG.

---

## RPC Method Reference

### smsgbuckets

```
smsgbuckets ( stats|dump )
```

Returns the list of SMSG message buckets from the node.
- No arguments: returns bucket list with hashes
- `stats`: returns statistics
- `dump`: returns full bucket contents

**App usage:** SMSG poll worker (TASKS.md P2-02) uses this to diff against
local cache and detect new messages. Poll every 60 seconds.

**Note:** Previously documented as `smsggetbuckets` — that command does NOT
exist in Omega. Use `smsgbuckets`.

---

### smsg

```
smsg "msgid" ( options )
```

Retrieves a single SMSG message by its message ID (hash).

**App usage:** After detecting new bucket entries via `smsgbuckets`, fetch
each new message individually with `smsg <msgid>`.

**Note:** Previously documented as `smsgget` — that command does NOT exist
in Omega. Use `smsg`.

---

### smsgsend

```
smsgsend "address_from" "address_to" "message" ( paid_msg days_retention testfee )
```

Sends an unencrypted SMSG message.

**Parameters:**
- `address_from`: sender's SMSG address (must be in local keyring)
- `address_to`: recipient's SMSG address
- `message`: plaintext message body
- `paid_msg`: boolean, default false
- `days_retention`: TTL in **days** (not seconds), default varies
- `testfee`: boolean, default false

**7-day retention (room messages):**
```bash
omega-cli smsgsend "ADDR_FROM" "ADDR_TO" "message text" false 7
```

**App usage:** All public room messages, moderation messages, marketplace
listings, purchase intents, escrow PSBTs.

---

### smsgsendanon

```
smsgsendanon "address_to" "message"
```

Sends an encrypted anonymous SMSG message. Sender address is not revealed.
Message is encrypted to the recipient's public key.

**App usage:** Encrypted room messages (TASKS.md P2-10). The sender's
identity is not included in the message envelope.

**Note:** This is a SEPARATE command from `smsgsend`, not a flag. Previously
designed as `smsgsend` with an `--anon` flag — that does not exist. Use
`smsgsendanon` for all encrypted sends.

---

### smsgaddaddress

```
smsgaddaddress "address" "pubkey"
```

Adds an address and its public key to the SMSG keyring, enabling the node
to receive messages sent to that address.

**App usage:** P2-04 (per-room keypair manager) — registers each new room
keypair with the node.

---

### smsgaddlocaladdress

```
smsgaddlocaladdress "address"
```

Adds a wallet address to the SMSG keyring. The node derives the public key
from the wallet automatically. Simpler than `smsgaddaddress` when the key
is already in the wallet.

**App usage:** When the user's primary messaging address is a wallet address,
use this instead of `smsgaddaddress`.

---

### smsggetpubkey

```
smsggetpubkey "address"
```

Returns the public key for an address that has been seen in SMSG traffic.
Nodes broadcast their public keys when they send messages.

**App usage (important):** When constructing an encrypted room invite
(TASKS.md P2-10), the app needs the invitee's public key. Rather than
exchanging keys out of band, call `smsggetpubkey` on the invitee's address.
This only works if the invitee has previously sent at least one SMSG message
(their key will be known to the network).

---

### smsgimportprivkey

```
smsgimportprivkey "privkey" ( "label" )
```

Imports a WIF-encoded private key into the SMSG keyring. Used for restoring
messaging keypairs from backup.

**App usage:** P2-11 (key backup screen) — WIF import path.

---

### smsginbox

```
smsginbox ( "mode" "filter" )
```

Returns received messages from the SMSG inbox.

---

### smsgoutbox

```
smsgoutbox ( "mode" "filter" )
```

Returns sent messages from the SMSG outbox.

---

### smsglocalkeys

```
smsglocalkeys ( whitelist|all|wallet|recv +/- "address"|anon +/- "address" )
```

Lists SMSG keys registered on this node. Also used to manage whitelist
and anonymous receive settings.

**App usage:** Verify key registration after `smsgaddaddress`.

---

### smsgview

```
smsgview ( "address/label"|(asc/desc|-from yyyy-mm-dd|-to yyyy-mm-dd) )
```

Views messages for a specific address with optional date filtering.
Useful for debugging and room message fetching by address.

---

### smsgscanbuckets

```
smsgscanbuckets
```

Triggers a rescan of all message buckets. Use after importing a key to
retrieve messages that arrived before the key was registered.

---

### smsgscanchain

```
smsgscanchain
```

Scans the blockchain for SMSG messages. Used after wallet restore.

---

### smsgpurge

```
smsgpurge "msgid"
```

Removes a specific message from local storage. Useful for implementing
room expiry — expired room messages can be purged locally.

---

### smsgoptions

```
smsgoptions ( list with_description|set "optname" "value" )
```

Get or set SMSG configuration options.

---

### smsgenable / smsgdisable

```
smsgenable ( "walletname" )
smsgdisable
```

Enable or disable SMSG on the node.

---

## Command Mapping: Designed vs Actual

| Designed in TASKS.md | Actual Omega command | Action required |
|---------------------|---------------------|-----------------|
| `smsggetbuckets` | `smsgbuckets` | Update all app code |
| `smsgget <hash>` | `smsg <msgid>` | Update all app code |
| `smsgsend ... --anon` | `smsgsendanon "to" "msg"` | Separate method in SmsgClient |
| `smsgsend` | `smsgsend` | ✅ same — confirm TTL param name |
| `smsgaddaddress` | `smsgaddaddress` | ✅ same |
| `smsgimportprivkey` | `smsgimportprivkey` | ✅ same |
| `smsginbox` | `smsginbox` | ✅ same |
| `smsgoutbox` | `smsgoutbox` | ✅ same |
| `smsglocalkeys` | `smsglocalkeys` | ✅ same |
| `smsgdisable` | `smsgdisable` | ✅ same |
| `smsgenable` | `smsgenable` | ✅ same |
| `smsgscanbuckets` | `smsgscanbuckets` | ✅ same |
| *(not in design)* | `smsgaddlocaladdress` | 🆕 use for wallet addresses |
| *(not in design)* | `smsggetpubkey` | 🆕 use for encrypted invites |
| *(not in design)* | `smsgview` | 🆕 use for room fetch by address |
| *(not in design)* | `smsgpurge` | 🆕 use for room expiry |

---

## TTL Confirmation

`days_retention` parameter is in **days**, not seconds.

For room messages (7-day TTL):
```bash
omega-cli smsgsend "FROM" "TO" "body" false 7
```

Maximum TTL: needs testing — run:
```bash
omega-cli smsgsend "FROM" "TO" "test" false 30
# if that succeeds, try higher values
```

---

## Encryption Model

Public room messages: `smsgsend` — plaintext, visible to all nodes.
Encrypted room messages: `smsgsendanon` — encrypted to recipient pubkey,
ciphertext only visible to non-holders.

Key discovery: `smsggetpubkey "address"` — works only if the address has
previously sent at least one SMSG message on the network.
