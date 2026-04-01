# TASK: Fix SMSG configuration on both Omega nodes

## Background — what went wrong and why

During live testing on two nodes (Linux host + Windows VM bridged network),
SMSG failed to send with a series of errors. Root causes identified:

1. **SMSG not bound to wallet on startup** — `smsgenable` must be called with
   the wallet name explicitly after every restart. Without it, SMSG is globally
   enabled but has no wallet attached, so it cannot sign or receive.

2. **No local address in SMSG wallet_keys** — `smsglocalkeys` showed
   `wallet_keys: []`. SMSG needs at least one address registered from the
   local wallet to sign outgoing messages.

3. **scanIncoming disabled** — `smsgoptions` showed `scanIncoming: false`,
   meaning SMSG was not scanning incoming transactions for new keys.

4. **Addresses swapped** — `address_from` in `smsgsend` must be an address
   whose private key lives in the local wallet (`ismine: true`). Using the
   remote node's address as sender causes "Unknown private key for from
   address".

5. **Public key not exchanged** — before sending to a recipient, their public
   key must be known to the sending node via `smsgaddaddress`.

This task fixes all five issues permanently on both nodes and verifies
end-to-end SMSG messaging works.

---

## This task runs on TWO machines

- **Linux node** — `/home/rob/omegacoin/` — wallet name `lin`
- **Windows VM node** — bridged network, wallet name TBD (confirm below)

Run each step on the machine indicated. Steps marked **[BOTH]** run on both.

---

## Pre-flight [BOTH]

Confirm both nodes are running and synced:

```bash
omega-cli getblockchaininfo | grep -E 'blocks|headers|verificationprogress'
omega-cli getconnectioncount
omega-cli getpeerinfo | grep addr
```

Both nodes must show:
- Same block height
- At least 1 peer connection (each other)
- `verificationprogress` close to 1.0

If not peered, on Linux:
```bash
omega-cli addnode "WINDOWS_VM_IP:7777" add
```

On Windows:
```bash
omega-cli addnode "LINUX_HOST_IP:7777" add
```

---

## Step 1: Fix omega.conf on Linux node [LINUX]

Open or create `~/.omega/omega.conf`. Add or verify these lines are present:

```ini
# Wallet
wallet=lin

# SMSG
smsg=1

# ZMQ (for future Phase 4 push notifications)
zmqpubhashblock=tcp://127.0.0.1:28332
zmqpubrawtx=tcp://127.0.0.1:28332
```

Save the file. Do not restart yet — complete all config steps first.

**Check current conf:**

```bash
cat ~/.omega/omega.conf
```

If the file does not exist:

```bash
mkdir -p ~/.omega
cat > ~/.omega/omega.conf << 'EOF'
wallet=lin
smsg=1
zmqpubhashblock=tcp://127.0.0.1:28332
zmqpubrawtx=tcp://127.0.0.1:28332
EOF
```

---

## Step 2: Fix omega.conf on Windows node [WINDOWS]

On the Windows VM, open or create:
```
%APPDATA%\Omega\omega.conf
```

Find the wallet name first:
```bash
omega-cli listwallets
```

Add or verify:
```ini
# Replace "wallet_name" with actual wallet name from listwallets
wallet=<wallet_name>
smsg=1
```

---

## Step 3: Disable and re-enable SMSG with wallet binding [LINUX]

```bash
omega-cli smsgdisable
omega-cli smsgenable "lin"
```

Expected response:
```json
{
  "result": "Enabled secure messaging.",
  "wallet": "lin"
}
```

If wallet name is different, replace "lin" with the correct name from:
```bash
omega-cli listwallets
```

---

## Step 4: Same on Windows node [WINDOWS]

```bash
omega-cli smsgdisable
omega-cli smsgenable "<wallet_name>"
```

Confirm response shows correct wallet name.

---

## Step 5: Enable scanIncoming [BOTH]

```bash
omega-cli smsgoptions set scanIncoming true
```

Verify:
```bash
omega-cli smsgoptions list
```

All three options should now show:
```json
{
  "name": "newAddressRecv",  "value": true
  "name": "newAddressAnon",  "value": true
  "name": "scanIncoming",    "value": true
}
```

---

## Step 6: Get or create a local SMSG signing address [LINUX]

Check current state:
```bash
omega-cli smsglocalkeys
```

Look at `wallet_keys` section. If it is empty `[]`, the Linux node has no
local signing address. Create and register one:

```bash
# Get a fresh address from the wallet
LINUX_ADDR=$(omega-cli getnewaddress)
echo "Linux SMSG address: $LINUX_ADDR"

# Register it with SMSG
omega-cli smsgaddlocaladdress "$LINUX_ADDR"

# Verify it appears in wallet_keys
omega-cli smsglocalkeys
```

`wallet_keys` must now show the address with its public key.

If `smsglocalkeys` already shows an address in `wallet_keys`, use that
existing address — do not create a new one. Note it as `LINUX_ADDR`.

---

## Step 7: Same on Windows node [WINDOWS]

```bash
omega-cli smsglocalkeys
```

If `wallet_keys` is empty:
```bash
omega-cli getnewaddress
# copy the output, then:
omega-cli smsgaddlocaladdress "<new_address>"
omega-cli smsglocalkeys
```

Note the Windows wallet_keys address as `WINDOWS_ADDR`.

---

## Step 8: Exchange public keys between nodes

Each node needs to know the other's public key before sending.

### 8a. Get public keys [BOTH]

On Linux:
```bash
omega-cli smsglocalkeys
# Note the public_key value for your wallet_keys address
# This is LINUX_PUBKEY
```

On Windows:
```bash
omega-cli smsglocalkeys
# Note the public_key value for your wallet_keys address
# This is WINDOWS_PUBKEY
```

### 8b. Register Windows public key on Linux node [LINUX]

```bash
omega-cli smsgaddaddress "WINDOWS_ADDR" "WINDOWS_PUBKEY"
```

Expected:
```json
{"result": "Public key added to db."}
```

Verify:
```bash
omega-cli smsggetpubkey "WINDOWS_ADDR"
```

### 8c. Register Linux public key on Windows node [WINDOWS]

```bash
omega-cli smsgaddaddress "LINUX_ADDR" "LINUX_PUBKEY"
```

Verify:
```bash
omega-cli smsggetpubkey "LINUX_ADDR"
```

---

## Step 9: Verify address ownership [BOTH]

Confirm each node is using its own address as sender:

On Linux:
```bash
omega-cli getaddressinfo "LINUX_ADDR"
# Must show "ismine": true
```

On Windows:
```bash
omega-cli getaddressinfo "WINDOWS_ADDR"
# Must show "ismine": true
```

If either shows `"ismine": false` — STOP. The wrong address is being used.
Go back to Step 6/7 and use `getnewaddress` to get an address that belongs
to that wallet.

---

## Step 10: Test send Linux → Windows [LINUX]

```bash
omega-cli smsgsend "LINUX_ADDR" "WINDOWS_ADDR" "hello from linux"
```

Expected:
```json
{
  "result": "Sent",
  "msgid": "..."
}
```

If result is "Sent", wait 30 seconds then check on Windows:

```bash
# On Windows
omega-cli smsginbox
```

Should show the message with body "hello from linux".

---

## Step 11: Test send Windows → Linux [WINDOWS]

```bash
omega-cli smsgsend "WINDOWS_ADDR" "LINUX_ADDR" "hello from windows"
```

Wait 30 seconds then check on Linux:

```bash
# On Linux
omega-cli smsginbox
```

---

## Step 12: Test encrypted send [LINUX]

```bash
omega-cli smsgsendanon "WINDOWS_ADDR" "this is encrypted"
```

On Windows after 30 seconds:
```bash
omega-cli smsginbox
```

The message should arrive and decrypt correctly. If a third node existed
without `WINDOWS_ADDR` in its keyring, it would see only ciphertext —
this confirms encryption is actually working.

---

## Step 13: Test bucket propagation [BOTH]

Immediately after sending a message:

```bash
# On Linux
omega-cli smsgbuckets

# On Windows (within 60 seconds)
omega-cli smsgbuckets
```

Bucket hashes must match between both nodes. This confirms P2P SMSG
propagation is working — which is what the Android app poll worker
depends on.

---

## Step 14: Make SMSG wallet binding survive restart

The `smsgdisable` + `smsgenable "wallet"` cycle must happen on every
daemon restart. Add a startup script to handle this automatically.

### On Linux — create startup script:

```bash
cat > ~/.omega/smsg-enable.sh << 'EOF'
#!/bin/bash
# Wait for daemon to be ready
sleep 5
omega-cli smsgdisable 2>/dev/null || true
omega-cli smsgenable "lin"
omega-cli smsgoptions set scanIncoming true
echo "SMSG enabled for wallet lin"
EOF
chmod +x ~/.omega/smsg-enable.sh
```

Run this script after every daemon start:
```bash
~/.omega/smsg-enable.sh
```

**Note for Android app (TASKS.md P1-03, P2-01):** The app's node health
check should verify SMSG is enabled and bound after connecting. If
`smsglocalkeys` returns empty `wallet_keys`, the app should call
`smsgenable` with the configured wallet name before attempting any
SMSG operations. Add this to the `NodeManager` startup sequence.

---

## Step 15: Document confirmed values

After successful testing, fill in these values — they are needed before
Android app development starts:

```bash
# On Linux node
omega-cli smsglocalkeys
# Record: LINUX_ADDR and LINUX_PUBKEY

omega-cli getzmqnotifications
# Record: which ZMQ topics are active

grep -E 'PUBKEY_ADDRESS|SCRIPT_ADDRESS' \
    /home/rob/omegacoin/src/chainparams.cpp | head -10
# Record: address version bytes

grep -i 'bip44\|cointype' \
    /home/rob/omegacoin/src/chainparams.cpp | head -5
# Record: BIP44 coin type
```

Paste all output into `docs/rpc-interface.md` under the TODO sections.

---

## Step 16: Update docs/smsg-protocol.md

Add the following note to the top of `docs/smsg-protocol.md` under a new
section `## Known Quirks`:

```markdown
## Known Quirks — Confirmed by Live Testing 2026-03-20

### SMSG wallet binding requires explicit enable after restart

`smsg=1` in `omega.conf` enables SMSG globally but does NOT bind it to
a wallet. After every daemon restart, you must run:

```bash
omega-cli smsgdisable
omega-cli smsgenable "wallet_name"
```

Without this, all SMSG operations return "Wallet is not enabled" or
"Wallet unset" even though `smsgoptions list` shows SMSG as enabled.

**Android app impact (TASKS.md P1-03):** NodeManager must call
`smsgenable` with the wallet name as part of the node connection
startup sequence. Verify `smsglocalkeys` shows non-empty `wallet_keys`
before allowing any SMSG operations.

### address_from must be ismine: true

The `address_from` parameter in `smsgsend` must be an address whose
private key exists in the local wallet (`getaddressinfo` returns
`"ismine": true`). Using a remote node's address as sender causes:
"Unknown private key for from address"

**Android app impact (TASKS.md P2-08):** The compose screen must only
allow sending from addresses in the local wallet_keys list, never from
smsg_keys (contact keys).

### wallet_keys vs smsg_keys

`smsglocalkeys` returns two lists:
- `wallet_keys`: addresses with private keys in local wallet — can SEND
- `smsg_keys`: addresses added as contacts (public key only) — can only RECEIVE TO

Only `wallet_keys` addresses can be used as `address_from` in `smsgsend`.

### scanIncoming defaults to false

Must be explicitly enabled:
```bash
omega-cli smsgoptions set scanIncoming true
```

Without this, SMSG does not scan incoming transactions for new public
keys automatically.

### Public key must be registered before first send

Before sending to a recipient for the first time, their public key must
be registered:
```bash
omega-cli smsgaddaddress "recipient_address" "recipient_pubkey"
```

Get their public key from `smsglocalkeys` on their node, or via
`smsggetpubkey` if they have previously sent a message to the network.
```

---

## Step 17: Commit documentation updates

```bash
cd /home/rob/omegacoin

git add docs/smsg-protocol.md
git add docs/rpc-interface.md 2>/dev/null || true

git commit -m "docs: add SMSG quirks from live two-node testing

Confirmed by live testing on Linux host + Windows VM (bridged network):

- SMSG wallet binding requires explicit smsgenable after every restart
- wallet_keys vs smsg_keys distinction (only wallet_keys can send)
- scanIncoming defaults to false, must be set explicitly
- address_from must be ismine:true in local wallet
- Public key must be registered before first send

Added Known Quirks section to docs/smsg-protocol.md.
Added startup script ~/.omega/smsg-enable.sh for wallet binding.

Android app impact documented for TASKS.md P1-03, P2-08.

Ref: BLOCKCHAIN_READINESS.md §1.2, §1.6"
```

---

## Summary of fixes

| Problem | Fix |
|---------|-----|
| Wallet not enabled | `smsgdisable` + `smsgenable "wallet_name"` |
| scanIncoming false | `smsgoptions set scanIncoming true` |
| Empty wallet_keys | `getnewaddress` + `smsgaddlocaladdress` |
| Public key not in database | `smsgaddaddress "addr" "pubkey"` |
| Wrong address_from | Use address where `getaddressinfo` shows `ismine: true` |
| Doesn't survive restart | `wallet=name` in omega.conf + startup script |

---

## Hard stops

- Either node shows 0 peers after addnode — fix peering before testing SMSG
- `getaddressinfo` shows `ismine: false` for your sender address — wrong address
- `smsgsend` returns "Sent" but `smsginbox` on recipient shows nothing after
  60 seconds — bucket propagation broken, check `smsgbuckets` on both nodes
- `smsgbuckets` hashes differ after 60 seconds — P2P SMSG not propagating,
  check firewall and peer connection
