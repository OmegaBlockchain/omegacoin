# Blockchain Readiness Checklist

Tracks which Omega daemon capabilities have been confirmed on live nodes.

**Last audit:** 2026-04-01 — testnet nodes running omegad 0.20.3 with SMSG, topic channels, and room RPCs.

Status legend: 🟢 confirmed | 🟡 partially confirmed | 🔴 not yet tested | ⚠️ pending

---

## §1 SMSG

### 1.1 RPC Methods Present

| ID | Method | Test command | Expected | Status |
|----|--------|-------------|----------|--------|
| 1.1.1 | `smsgsend` | `omega-cli smsgsend` | ✅ CONFIRMED — params: address_from address_to message paid_msg days_retention testfee | 🟢 |
| 1.1.2 | `smsgbuckets` | `omega-cli smsgbuckets` | ✅ CONFIRMED — `smsgbuckets` lists buckets; `smsgbuckets dump` writes `smsgstore/buckets_dump.json` (non-destructive). Use `smsgclearbuckets true` for deletion. | 🟢 |
| 1.1.3 | `smsg` | `omega-cli smsg <msgid>` | ✅ CONFIRMED — NOTE: command is `smsg` not `smsgget` | 🟢 |
| 1.1.4 | `smsgsendanon` | `omega-cli smsgsendanon` | ✅ CONFIRMED — separate command, NOT a flag on smsgsend | 🟢 |
| 1.1.5 | `smsginbox` | `omega-cli smsginbox` | ✅ CONFIRMED | 🟢 |
| 1.1.6 | `smsgoutbox` | `omega-cli smsgoutbox` | ✅ CONFIRMED | 🟢 |
| 1.1.7 | `smsgaddaddress` | `omega-cli smsgaddaddress` | ✅ CONFIRMED | 🟢 |
| 1.1.8 | `smsgimportprivkey` | `omega-cli smsgimportprivkey` | ✅ CONFIRMED | 🟢 |
| 1.1.9 | `smsglocalkeys` | `omega-cli smsglocalkeys` | ✅ CONFIRMED | 🟢 |
| 1.1.10 | `smsgdisable` | `omega-cli smsgdisable` | ✅ CONFIRMED | 🟢 |
| 1.1.11 | `smsgenable` | `omega-cli smsgenable` | ✅ CONFIRMED | 🟢 |
| 1.1.12 | `smsgscanbuckets` | `omega-cli smsgscanbuckets` | ✅ CONFIRMED | 🟢 |
| 1.1.13 | `smsgaddlocaladdress` | `omega-cli smsgaddlocaladdress` | ✅ CONFIRMED — new, use for wallet addresses | 🟢 |
| 1.1.14 | `smsggetpubkey` | `omega-cli smsggetpubkey` | ✅ CONFIRMED — new, use for encrypted invites | 🟢 |
| 1.1.15 | `smsgview` | `omega-cli smsgview` | ✅ CONFIRMED — new, use for room fetch by address | 🟢 |
| 1.1.16 | `smsgpurge` | `omega-cli smsgpurge` | ✅ CONFIRMED — new, use for room expiry | 🟢 |
| 1.1.17 | `smsgsubscribe` | `omega-cli smsgsubscribe "omega.room.<id>"` | ✅ CONFIRMED — subscribe to topic channel | 🟢 |
| 1.1.18 | `smsgunsubscribe` | `omega-cli smsgunsubscribe "omega.room.<id>"` | ✅ CONFIRMED — unsubscribe from topic | 🟢 |
| 1.1.19 | `smsglisttopics` | `omega-cli smsglisttopics` | ✅ CONFIRMED — list all subscribed topics | 🟢 |
| 1.1.20 | `smsggetmessages` | `omega-cli smsggetmessages "omega.room.<id>"` | ✅ CONFIRMED — fetch messages for topic; default limit 50, silent truncation (WARN-07) | 🟢 |
| 1.1.21 | `smsgcreateroom` | `omega-cli smsgcreateroom "name" flags retention_days` | ✅ CONFIRMED — creates type-8 special tx, auto-subscribes to `omega.room.<txid[0:12]>` topic | 🟢 |
| 1.1.22 | `smsglistrooms` | `omega-cli smsglistrooms` | ✅ CONFIRMED — returns indexed rooms from SmsgRoomIndex (LevelDB) | 🟢 |
| 1.1.23 | `smsggetroominfo` | `omega-cli smsggetroominfo <txid>` | ✅ CONFIRMED — full room details by txid | 🟢 |
| 1.1.24 | `smsgjoinroom` | `omega-cli smsgjoinroom <txid>` | ✅ CONFIRMED — wrapper: subscribes to topic + registers room pubkey | 🟢 |
| 1.1.25 | `smsgclearbuckets` | `omega-cli smsgclearbuckets true` | ✅ BUILT — permanently deletes all .dat files in smsgstore/; `confirm=true` mandatory. CRIT-01 fix. | 🟡 Built, pending testnet verification |
| 1.1.26 | `trollboxsend` | `omega-cli trollboxsend "message"` | ✅ CONFIRMED — max 256 chars, 30s rate limit, 24h retention | 🟢 |
| 1.1.27 | `trollboxlist` | `omega-cli trollboxlist 200` | ✅ CONFIRMED — returns up to N trollbox messages | 🟢 |

### 1.2 Message Delivery

| ID | Check | Status |
|----|-------|--------|
| 1.2.1 | Messages propagate between two peered nodes | 🟢 confirmed |

### 1.3 Message Routing

| ID | Check | Notes | Status |
|----|-------|-------|--------|
| 1.3.1 | Messages routable by address | | 🔴 |

### 1.4 TTL (Time-to-Live)

| ID | Check | Expected | Status |
|----|-------|----------|--------|
| 1.4.1 | TTL parameter name and unit | ✅ CONFIRMED — parameter is `days_retention`, unit is days. 7-day: `smsgsend FROM TO "msg" false 7` | 🟢 |
| 1.4.2 | Maximum TTL value | Needs testing — try `days_retention` 30, then higher | 🔴 |

### 1.5 SMSG Encryption

| ID | Check | Expected | Status |
|----|-------|----------|--------|
| 1.5.1 | Plaintext send works | `smsgsend` — confirmed | 🟢 |
| 1.5.2 | Encrypted send works | `smsgsendanon` — confirmed present | 🟡 |
| 1.5.3 | Encryption command | ✅ CONFIRMED — separate command `smsgsendanon "address_to" "message"`. NOT a flag on smsgsend. | 🟢 |

---

## §2 Blockchain

### 2.1 Sync

| ID | Check | Status |
|----|-------|--------|
| 2.1.1 | `getblockchaininfo` returns sync progress | 🔴 |

---

## §3 Wallet

### 3.1 Wallet RPC Methods

| ID | Method | Status |
|----|--------|--------|
| 3.1.1 | `getbalances` (plural) | ✅ CONFIRMED |
| 3.1.2 | `getnewaddress` | ✅ CONFIRMED |
| 3.1.3 | `listtransactions` | ✅ CONFIRMED |
| 3.1.4 | `listunspent` | 🔴 |
| 3.1.5 | `estimatesmartfee` | ✅ CONFIRMED |
| 3.1.6 | `createrawtransaction` | ✅ CONFIRMED |
| 3.1.7 | `signrawtransactionwithkey` | ✅ CONFIRMED |
| 3.1.8 | `signrawtransactionwithwallet` | 🔴 |
| 3.1.9 | `sendrawtransaction` | ✅ CONFIRMED |
| 3.1.10 | `decoderawtransaction` | ✅ CONFIRMED |
| 3.1.11 | `getwalletinfo` | ✅ CONFIRMED |
| 3.1.12 | `importprivkey` | ✅ CONFIRMED |
| 3.1.13 | `dumpwallet` | ✅ CONFIRMED |
| 3.1.14 | `upgradetohd` | ✅ CONFIRMED — native BIP39 mnemonic support confirmed |
| 3.1.15 | `dumphdinfo` | ✅ CONFIRMED |
| 3.1.16 | `getbalance` (singular) | ⚠️ may be replaced by `getbalances` (plural) in this version |

---

## §4 PSBT

| ID | Check | Status |
|----|-------|--------|
| 4.1 | `createpsbt` | ✅ CONFIRMED — PSBT support confirmed |
| 4.2 | `walletprocesspsbt` | ✅ CONFIRMED |
| 4.3 | `combinepsbt` | ✅ CONFIRMED |
| 4.4 | `finalizepsbt` | ✅ CONFIRMED |
| 4.5 | `decodepsbt` | ✅ CONFIRMED |
| 4.6 | `analyzepsbt` | ✅ CONFIRMED |

---

## §5 Multisig

| ID | Check | Status |
|----|-------|--------|
| 5.1 | `createmultisig` | 🔴 |
| 5.2 | `addmultisigaddress` | 🔴 |

---

## §6 ZMQ

| ID | Check | Expected | Status |
|----|-------|----------|--------|
| 6.1 | ZMQ compiled in | ✅ CONFIRMED — `getzmqnotifications` RPC present | 🟢 |
| 6.2 | ZMQ topics published | ✅ CONFIRMED — `hashblock`, `rawtx`, `hashsmsg`, `rawsmsg` all fire on testnet node configured with `-zmqpubhash*` / `-zmqpubraw*` flags | 🟢 |
| 6.3 | SMSG ZMQ topics | ✅ CONFIRMED — `zmqpubhashsmsg` publishes SHA256(full_msg) on inbox receipt; `zmqpubrawsmsg` publishes full SMSG bytes. Topic hash in cleartext `nonce[0..3]`. Config: `-zmqpubhashsmsg=tcp://...` `-zmqpubrawsmsg=tcp://...` | 🟢 |

---

## §7 Address Format

| ID | Check | Value | Status |
|----|-------|-------|--------|
| 7.1 | Base58 prefix for P2PKH (mainnet) | `115` (decimal) — addresses start with `o` | 🟢 confirmed |
| 7.2 | Base58 prefix for P2SH (mainnet) | `15` (decimal) | 🟢 confirmed |
| 7.3 | BIP44 coin type | `5` (comment says "Dash BIP44") | 🟢 confirmed |
| 7.4 | WIF prefix (SECRET_KEY) | `125` (decimal) | 🟢 confirmed |
| 7.5 | BIP32 xpub prefix | `{0x04, 0x88, 0xB2, 0x1E}` — standard xpub | 🟢 confirmed |

---

## §8 Network

| ID | Check | Value | Status |
|----|-------|-------|--------|
| 8.1 | Two nodes peer successfully | — | 🟢 confirmed |
| 8.2 | Default P2P port (mainnet) | `7777` | 🟢 confirmed |
| 8.3 | Default RPC port (mainnet) | `7778` | 🟢 confirmed |
| 8.4 | Default P2P port (testnet) | `17778` | 🟢 confirmed |
| 8.5 | Default RPC port (testnet) | `17778` | 🟢 confirmed |

---

## §9 Documentation Gaps

| ID | Gap | Resolution |
|----|-----|------------|
| 9.1 | Address version bytes | ✅ RESOLVED — P2PKH=115, P2SH=15, WIF=125 (see §7) |
| 9.2 | BIP44 coin type | ✅ RESOLVED — coin type 5 (see §7.3) |
| 9.3 | Exact `smsgsend` RPC signature | ✅ RESOLVED — see docs/smsg-protocol.md |
| 9.4 | SMSG message structure (fields) | 🔴 |
| 9.5 | SmsgBucket structure | 🔴 |
| 9.6 | PSBT escrow flow | ✅ RESOLVED — see docs/rpc-interface.md |
| 9.7 | Anonymous sender SMSG | ✅ RESOLVED — separate command `smsgsendanon "to" "msg"` |
| 9.8 | TTL values | ✅ PARTIALLY RESOLVED — unit confirmed as days, max TTL needs testing |
| 9.9 | ZMQ SMSG topic | ✅ RESOLVED — Native ZMQ publishers `zmqpubhashsmsg` + `zmqpubrawsmsg` built in 0.20.3. Fires on inbox message receipt via `g_zmq_notification_interface->NotifySmsgReceived()`. Needs testnet verification (R-21). |
