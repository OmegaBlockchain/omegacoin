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
