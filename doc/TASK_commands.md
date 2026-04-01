# TASK: Update project docs and RPC mapping from live Omega daemon audit

## Background

A live audit of two Omega nodes running omega-qt with SMSG revealed that several
RPC method names differ from what was designed in PROJECT.md and TASKS.md.
Additionally, several new useful commands were discovered, and PSBT support was
confirmed as a better escrow mechanism than raw PST exchange.

This task updates all affected documentation and creates the definitive RPC
reference document. No app code is written yet — this is documentation and
specification only.

Source of truth: the full `omega-cli help` output audited on 2026-03-20.

---

## Pre-flight

```bash
cd /home/rob/omegacoin
git status
```

Must be on a clean branch or the `feat/guix-reproducible-builds` branch.
If dirty, stash or commit first. Do not proceed on a dirty tree.

Create a working branch if not already on one:

```bash
git checkout -b docs/rpc-audit-corrections 2>/dev/null || \
git checkout feat/guix-reproducible-builds
```

---

## Step 1: Create docs/ directory if it does not exist

```bash
mkdir -p /home/rob/omegacoin/docs
ls /home/rob/omegacoin/docs/
```

---

## Step 2: Create docs/smsg-protocol.md

Create this file at `/home/rob/omegacoin/docs/smsg-protocol.md` with the
following exact content:

```markdown
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
```

---

## Step 3: Create docs/rpc-interface.md

Create `/home/rob/omegacoin/docs/rpc-interface.md` with the following content:

```markdown
# Omega RPC Interface Reference

**Source:** Live audit of omega-cli help output, 2026-03-20.
**Confirmed on:** Two peered nodes running omega-qt with SMSG enabled.

---

## Address Format

TODO: Fill from chainparams.cpp
```bash
grep -E 'PUBKEY_ADDRESS|SCRIPT_ADDRESS|base58' \
    /home/rob/omegacoin/src/chainparams.cpp | head -20
```

---

## BIP44 Coin Type

TODO: Fill from chainparams.cpp
```bash
grep -i 'bip44\|cointype\|coin_type' \
    /home/rob/omegacoin/src/chainparams.cpp
```

---

## SMSG Methods

See docs/smsg-protocol.md for full SMSG reference.

Key corrections from original design:
- `smsggetbuckets` → `smsgbuckets`
- `smsgget` → `smsg`
- `smsgsend --anon` → `smsgsendanon`

---

## Wallet Methods Used by App

| Method | Purpose | Notes |
|--------|---------|-------|
| `getbalances` | Wallet balance | Returns confirmed + unconfirmed |
| `getnewaddress` | Fresh receive address | |
| `listunspent` | UTXO list | For UTXO sync worker |
| `listtransactions` | Tx history | |
| `estimatesmartfee` | Fee estimation | |
| `sendrawtransaction` | Broadcast signed tx | |
| `signrawtransactionwithkey` | Sign tx with explicit key | Used for escrow |
| `signrawtransactionwithwallet` | Sign tx with wallet key | Used for standard sends |
| `getwalletinfo` | Wallet state | |
| `importprivkey` | Import WIF key | |
| `dumpwallet` | Export wallet | |
| `upgradetohd` | BIP39 mnemonic restore | Native mnemonic support confirmed |
| `dumphdinfo` | HD wallet info | |

---

## Escrow Methods (PSBT — preferred over raw PST)

PSBT support confirmed. Use PSBT for escrow instead of manual raw
transaction exchange as originally designed in PROJECT.md §7.3.

| Method | Purpose |
|--------|---------|
| `createpsbt` | Create unsigned PSBT |
| `walletprocesspsbt` | Sign PSBT with wallet key |
| `combinepsbt` | Combine partially-signed PSBTs from multiple parties |
| `finalizepsbt` | Finalise and extract signed transaction |
| `decodepsbt` | Inspect PSBT contents |
| `analyzepsbt` | Check PSBT signing status |

**Escrow signing flow (revised from PROJECT.md):**
1. App constructs multisig address (`createmultisig`)
2. Buyer creates `createpsbt` spending the multisig UTXO
3. Buyer signs with `walletprocesspsbt` → partial PSBT
4. Partial PSBT sent to seller via SMSG
5. Seller signs with `walletprocesspsbt` → second partial PSBT
6. Either party calls `combinepsbt` + `finalizepsbt`
7. Broadcast via `sendrawtransaction`

This replaces the manual `signrawtransactionwithkey` exchange in PROJECT.md.

---

## Multisig / Escrow Methods

| Method | Purpose |
|--------|---------|
| `createmultisig` | Create P2SH multisig address |
| `addmultisigaddress` | Add multisig to wallet |
| `createrawtransaction` | Build raw tx (fallback if PSBT not used) |
| `signrawtransactionwithkey` | Sign with explicit key |
| `sendrawtransaction` | Broadcast |
| `decodescript` | Inspect redeem script |

---

## ZMQ

```bash
omega-cli getzmqnotifications
```

ZMQ infrastructure confirmed present. Run the above command and record
which topics are published. If `smsg` topic exists, P4-05 (push
notifications) requires app-side work only. If absent, requires daemon
addition.

TODO: paste output of `getzmqnotifications` here.

---

## Chain Info Methods

| Method | Purpose |
|--------|---------|
| `getblockchaininfo` | Chain state, sync status |
| `getblockcount` | Current height |
| `getblock` | Block data |
| `getbestblockhash` | Tip hash |
```

---

## Step 4: Update TASKS.md — P2-01 SmsgClient specification

Open `/home/rob/omegacoin/docs/../TASKS.md` — if TASKS.md is in the repo root,
open `/home/rob/omegacoin/TASKS.md`.

Find the section `### P2-01 · SMSG RPC wrapper`.

Replace the RPC method list inside it. Find this block:

```
- `send(toAddress: String, message: String, daysRetention: Int): String` → returns msg hash
- `getBuckets(): List<SmsgBucket>`
- `getMessage(hash: String): SmsgMessage`
- `addAddress(address: String, privateKeyWif: String)`
```

Replace with:

```
- `send(fromAddress: String, toAddress: String, message: String, daysRetention: Int): String`
  → `smsgsend` — returns msg hash. plaintext only.
- `sendAnon(toAddress: String, message: String): String`
  → `smsgsendanon` — encrypted send, sender anonymous
- `getBuckets(): List<SmsgBucket>`
  → `smsgbuckets` (NOT smsggetbuckets — that command does not exist)
- `getMessage(msgId: String): SmsgMessage`
  → `smsg` (NOT smsgget — that command does not exist)
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
```

---

## Step 5: Update PROJECT.md — escrow section

Open `/home/rob/omegacoin/PROJECT.md`.

Find section `### 7.3 Escrow Model` and locate the subsection
`**Escrow flow:**`.

After the existing flow list, add the following note:

```markdown
**Note — PSBT preferred over raw PST exchange:**
The Omega daemon has full PSBT support (`createpsbt`, `walletprocesspsbt`,
`combinepsbt`, `finalizepsbt`). The implementation in Phase 3 should use
PSBT rather than manual raw transaction construction and
`signrawtransactionwithkey`. PSBT handles partial signature serialisation
and combination cleanly and is the standard approach in all modern
Bitcoin-derived projects. See `docs/rpc-interface.md` for the revised
escrow signing flow.
```

Also find the paragraph beginning `**Escrow transaction construction**` in
`§8.5` and replace the bullet points:

Find:
```
1. `createrawtransaction` called locally or constructed manually using the bitcoin-script library
2. Signed locally with the user's escrow key
3. Partially-signed transaction (PST) shared with counterparty via SMSG
4. Counterparty adds their signature via the same mechanism
5. Fully-signed transaction broadcast via `sendrawtransaction`
```

Replace with:
```
1. App constructs multisig address from three pubkeys via `createmultisig`
2. Buyer creates unsigned PSBT via `createpsbt` spending the funded multisig UTXO
3. Buyer signs with `walletprocesspsbt` → partial PSBT (1-of-2 signatures)
4. Partial PSBT serialised and sent to seller via SMSG
5. Seller signs with `walletprocesspsbt` → second partial PSBT
6. Either party combines with `combinepsbt` + `finalizepsbt`
7. Fully-signed transaction broadcast via `sendrawtransaction`

PSBT is used in preference to manual `signrawtransactionwithkey` exchange
as it is the standard approach, handles serialisation cleanly, and allows
`analyzepsbt` to verify signing status at each step.
```

---

## Step 6: Update BLOCKCHAIN_READINESS.md — mark confirmed items

Open `/home/rob/omegacoin/BLOCKCHAIN_READINESS.md`.

### 6a. Section 1.1 — mark confirmed RPC methods

In the table under `### 1.1 RPC Methods Present`, update the following rows
to add a `✅ CONFIRMED` note in the Expected column:

- `smsgsend` → `✅ CONFIRMED — params: address_from address_to message paid_msg days_retention testfee`
- `smsginbox` → `✅ CONFIRMED`
- `smsgoutbox` → `✅ CONFIRMED`
- `smsgaddaddress` → `✅ CONFIRMED`
- `smsgimportprivkey` → `✅ CONFIRMED`
- `smsglocalkeys` → `✅ CONFIRMED`
- `smsgdisable` → `✅ CONFIRMED`
- `smsgenable` → `✅ CONFIRMED`
- `smsgscanbuckets` → `✅ CONFIRMED`

Update the `smsggetbuckets` row to:
```
| 1.1.2 | `smsgbuckets` | `omega-cli smsgbuckets` | ✅ CONFIRMED — NOTE: command is `smsgbuckets` not `smsggetbuckets` | 🔴 |
```

Update the `smsgget` row to:
```
| 1.1.3 | `smsg` | `omega-cli smsg <msgid>` | ✅ CONFIRMED — NOTE: command is `smsg` not `smsgget` | 🔴 |
```

### 6b. Section 1.4 — mark TTL confirmed

Find `### 1.4 TTL (Time-to-Live)` and update row 1.4.1:

```
| 1.4.1 | TTL parameter name and unit | ✅ CONFIRMED — parameter is `days_retention`, unit is days. 7-day: `smsgsend FROM TO "msg" false 7` | 🔴 |
```

### 6c. Section 1.5 — mark encryption confirmed

Find `### 1.5 SMSG Encryption` and update row 1.5.3:

```
| 1.5.3 | Encryption command | ✅ CONFIRMED — separate command `smsgsendanon "address_to" "message"`. NOT a flag on smsgsend. | 🔴 |
```

### 6d. Section 3.1 — mark wallet methods confirmed

Mark the following rows in the wallet methods table as `✅ CONFIRMED`:
`getbalances`, `getnewaddress`, `listtransactions`, `estimatesmartfee`,
`createrawtransaction`, `signrawtransactionwithkey`, `sendrawtransaction`,
`decoderawtransaction`, `getwalletinfo`, `importprivkey`, `dumpwallet`.

Note that `getbalance` (singular) may be replaced by `getbalances` (plural)
in this version. Update that row accordingly.

### 6e. Section 6 — ZMQ confirmed present

Find `### 6.1` and update:

```
| 6.1 | ZMQ compiled in | ✅ CONFIRMED — `getzmqnotifications` RPC present | 🟡 |
```

### 6f. Section 9 — mark documentation gaps resolved

Update the following rows in the documentation gaps table:

```
| 9.3 | Exact `smsgsend` RPC signature | ✅ RESOLVED — see docs/smsg-protocol.md |
| 9.7 | Anonymous sender SMSG | ✅ RESOLVED — separate command `smsgsendanon "to" "msg"` |
| 9.8 | TTL values | ✅ PARTIALLY RESOLVED — unit confirmed as days, max TTL needs testing |
| 9.9 | ZMQ SMSG topic | ⚠️ PENDING — ZMQ present, run `getzmqnotifications` to confirm SMSG topic |
```

---

## Step 7: Add smsg-protocol.md and rpc-interface.md to .gitignore exclusion

These are new tracked files — make sure they are not accidentally ignored:

```bash
grep -n 'docs/' /home/rob/omegacoin/.gitignore
```

If `docs/` is listed as ignored, add exceptions:

```bash
echo '!docs/smsg-protocol.md' >> /home/rob/omegacoin/.gitignore
echo '!docs/rpc-interface.md' >> /home/rob/omegacoin/.gitignore
```

---

## Step 8: Syntax and content check

```bash
cd /home/rob/omegacoin

echo "=== Checking docs created ==="
ls -la docs/

echo "=== Checking smsggetbuckets not in docs (must be zero) ==="
grep -rn 'smsggetbuckets' docs/ TASKS.md PROJECT.md \
    && echo "FAIL — old command name present" \
    || echo "PASS"

echo "=== Checking smsgget not in docs (must be zero) ==="
grep -rn '"smsgget"' docs/ TASKS.md PROJECT.md \
    && echo "FAIL — old command name present" \
    || echo "PASS"

echo "=== Checking smsgbuckets present in docs ==="
grep -rn 'smsgbuckets' docs/smsg-protocol.md \
    && echo "PASS" \
    || echo "FAIL — smsgbuckets missing"

echo "=== Checking smsgsendanon present ==="
grep -rn 'smsgsendanon' docs/smsg-protocol.md \
    && echo "PASS" \
    || echo "FAIL"

echo "=== Checking PSBT note in PROJECT.md ==="
grep -n 'PSBT' /home/rob/omegacoin/PROJECT.md \
    && echo "PASS" \
    || echo "FAIL — PSBT note missing"
```

All checks must PASS before committing.

---

## Step 9: Commit

```bash
cd /home/rob/omegacoin

git add docs/smsg-protocol.md
git add docs/rpc-interface.md
git add TASKS.md
git add PROJECT.md
git add BLOCKCHAIN_READINESS.md

git commit -m "docs: update RPC mapping from live Omega daemon audit

Live audit of two peered omega-qt nodes with SMSG confirmed:

Critical command name corrections:
- smsggetbuckets does not exist -> use smsgbuckets
- smsgget does not exist -> use smsg <msgid>
- smsgsend --anon flag does not exist -> use smsgsendanon

New commands discovered (not in original design):
- smsgaddlocaladdress (simpler key registration for wallet addresses)
- smsggetpubkey (key discovery for encrypted room invites)
- smsgview (room message fetch by address)
- smsgpurge (room expiry implementation)

TTL confirmed: days_retention parameter, unit is days.
Encryption confirmed: smsgsendanon is a separate command.
PSBT support confirmed: use PSBT for escrow (replaces raw PST exchange).
ZMQ confirmed present: getzmqnotifications exists.
HD wallet confirmed: upgradetohd + dumphdinfo present.

Files added:
- docs/smsg-protocol.md: complete SMSG RPC reference
- docs/rpc-interface.md: full RPC interface catalogue

Files updated:
- TASKS.md P2-01: SmsgClient method names corrected
- PROJECT.md §7.3, §8.5: PSBT escrow flow
- BLOCKCHAIN_READINESS.md: confirmed items marked

Ref: BLOCKCHAIN_READINESS.md §1.1, §1.4, §1.5, §3.1, §6, §9"
```

---

## Step 10: Report

```
RPC AUDIT COMPLETE
==================
Branch: <current branch>
Commit: <hash>

Files created:
  docs/smsg-protocol.md
  docs/rpc-interface.md

Files updated:
  TASKS.md
  PROJECT.md
  BLOCKCHAIN_READINESS.md

Critical corrections applied:
  smsggetbuckets -> smsgbuckets
  smsgget -> smsg
  smsgsend --anon -> smsgsendanon

Checks: ALL PASS / <list failures>

Remaining TODOs in docs/rpc-interface.md (require human input):
  1. Address version bytes — grep chainparams.cpp
  2. BIP44 coin type — grep chainparams.cpp
  3. ZMQ SMSG topic — run getzmqnotifications on live node
  4. Maximum SMSG TTL — test with days_retention > 7

Next step (human action required):
  On a live node run:
    omega-cli getzmqnotifications
    omega-cli smsgsend "ADDR_A" "ADDR_B" "test" false 7
    omega-cli smsgsendanon "ADDR_B" "encrypted test"
  Paste results to complete BLOCKCHAIN_READINESS.md §1.3, §1.4, §6.3
```

---

## Hard stops

- Any check in Step 8 reports FAIL
- `smsggetbuckets` or `"smsgget"` still present in any updated file
- docs/ directory not created
- PSBT note missing from PROJECT.md
