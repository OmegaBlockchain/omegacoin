# SMSG RPC Improvements for Android App

Implementation plan for new SMSG RPC commands ported from particl-core.

## Phase 1 — Mobile Dashboard & Connection Health

### 1.1 `smsggetinfo`
- **Returns:** `{ enabled, active_wallet, enabled_wallets[] }`
- **Method:** Read `fSecMsgEnabled`, call `smsgModule.GetWalletName()`, iterate `m_vpwallets`
- **File:** `src/smsg/rpcsmessage.cpp`
- **Dependencies:** None — all methods exist in CSMSG

### 1.2 `smsgpeers`
- **Returns:** Array of `{ id, address, version, ignoreuntil, misbehaving, numwantsent, receivecounter, ignoredcounter }`
- **Method:** Call `smsgModule.GetNodesStats(node_id, result)`
- **File:** `src/smsg/rpcsmessage.cpp`
- **Dependencies:** `GetNodesStats()` already exists in CSMSG

### 1.3 `smsgaddresses`
- **Returns:** Array of `{ address, receive_enabled, receive_anon }`
- **Method:** Iterate `smsgModule.addresses` vector
- **File:** `src/smsg/rpcsmessage.cpp`
- **Dependencies:** `addresses` vector already public in CSMSG

### 1.4 `smsgzmqpush`
- **Returns:** `{ numsent }` — count of unread message IDs re-emitted
- **Method:** Iterate inbox DB (prefix `"im"`), filter by unread status and time range, return count
- **Note:** Simplified vs Particl (no CMainSignals::NewSecureMessage). Returns msgids for app to fetch.
- **File:** `src/smsg/rpcsmessage.cpp`
- **Dependencies:** `SecMsgDB`, `cs_smsgDB` lock, `SMSG_MASK_UNREAD`

### 1.5 `smsggetfeerate`
- **Returns:** Fee rate in satoshis per KB per day
- **Method:** Return constant `nMsgFeePerKPerDay` (50000) and `nFundingTxnFeePerK` (200000)
- **Note:** Omegacoin uses fixed fee rate, not blockchain-adjusted like Particl
- **File:** `src/smsg/rpcsmessage.cpp`

### 1.6 `smsggetdifficulty`
- **Returns:** `{ currentdifficulty }` — free message PoW difficulty
- **Method:** Return current PoW target from SMSG_SECONDS_IN_DAY-based calculation
- **File:** `src/smsg/rpcsmessage.cpp`

## Phase 2 — Full CRUD & Backup/Restore

### 2.1 `smsgremoveaddress`
- **Returns:** `{ result, reason? }`
- **Method:** Decode address, find in addrpkdb, erase. New `CSMSG::RemoveAddress()` method.
- **Files:** `src/smsg/smessage.h`, `src/smsg/smessage.cpp`, `src/smsg/rpcsmessage.cpp`

### 2.2 `smsgremoveprivkey`
- **Returns:** `{ result, reason? }`
- **Method:** Decode address, find in keyStore, erase. New `CSMSG::RemovePrivkey()` method.
- **Files:** `src/smsg/smessage.h`, `src/smsg/smessage.cpp`, `src/smsg/rpcsmessage.cpp`

### 2.3 `smsgdumpprivkey`
- **Returns:** WIF-encoded private key string
- **Method:** Decode address, read from keyStore. New `CSMSG::DumpPrivkey()` method.
- **Files:** `src/smsg/smessage.h`, `src/smsg/smessage.cpp`, `src/smsg/rpcsmessage.cpp`

### 2.4 `smsgfund`
- **Returns:** `{ txid?, fee, tx_vsize }`
- **Method:** Read stashed message from DB, call `FundMsg()`, submit if not testfee
- **File:** `src/smsg/rpcsmessage.cpp`
- **Dependencies:** `FundMsg()` exists. Uses outbox DB prefix `"qm"`.

### 2.5 `smsgimport`
- **Returns:** `{ msgid }`
- **Method:** Parse hex, validate, call `Store()` + `ScanMessage()`
- **File:** `src/smsg/rpcsmessage.cpp`

### 2.6 `smsgsetwallet`
- **Returns:** `{ result, wallet }`
- **Method:** Find wallet by name, call `SetActiveWallet()`
- **File:** `src/smsg/rpcsmessage.cpp`
- **Dependencies:** `SetActiveWallet()`, `m_vpwallets` exist in CSMSG

## Phase 3 — Debugging & Final Parity

### Duplicate / Overlap Analysis

Before adding more commands, cross-reference of existing Omega RPCs vs new RPCs:

| Existing Omega RPC | New RPC | Verdict |
|---|---|---|
| `smsglocalkeys whitelist\|all` — lists wallet keys + keyStore keys with address, pubkey, receive, anon, label | `smsgaddresses` — lists only `addresses` vector with address, receive_enabled, receive_anon | **NOT duplicate.** `smsglocalkeys` is heavyweight management (list + toggle), requires wallet. `smsgaddresses` is lightweight read-only, ideal for mobile. Both coexist in Particl. |
| `smsglocalkeys recv +/- addr` — toggles receive flag on/off | `smsgremoveaddress` — removes address entirely from PK db + addresses vector | **NOT duplicate.** Toggle vs delete — different operations. |
| `smsgsend` with `testfee=true` — creates + encrypts + estimates fee in one step | `smsgfund` — funds an already-stashed outbox message | **NOT duplicate.** Different workflow stage (create-and-fund vs fund-existing). |
| `smsgimportprivkey` — imports WIF key into SMSG keyStore | `smsgdumpprivkey` — exports key from keyStore as WIF | **NOT duplicate.** Inverse operations (import vs export). |
| `smsgaddaddress` — adds external address + pubkey to PK db (for sending to others) | `smsgaddlocaladdress` — adds local wallet address to SMSG receive list | **NOT duplicate.** External contact vs local receive. |

**Result: 0 true duplicates across all 30 commands.** All serve distinct purposes.

### Particl parity status

Particl has 31 SMSG RPCs. Omega now has 30 (18 original + 12 from Phase 1+2).
The only missing command is `smsgdebug`.

### 3.1 `smsgdebug`
- **Returns:** Varies by sub-command
- **Sub-commands:**
  - `clearbanned` — calls `smsgModule.ClearBanned()` (already exists in CSMSG)
  - `dumpids` — iterates all buckets, writes active message IDs to `smsg_ids.txt` in datadir
  - *(no args)* — returns summary info (bucket count, total messages, etc.)
- **Particl sub-commands NOT ported:** `dumpfundingtxids` (needs `ShowFundingTxns()` — not in Omega), `clearbestblock`/`setinvalidbestblock` (need `ReadBestBlock()`/`WriteBestBlock()` — not in Omega DB)
- **Method:** Implement supported sub-commands only. Skip Particl-specific ones that depend on missing infrastructure.
- **File:** `src/smsg/rpcsmessage.cpp`
- **Dependencies:** `ClearBanned()` exists. `dumpids` uses existing `buckets` map + `Retrieve()`.
- **Status:** IMPLEMENTED — all 31 Particl SMSG RPCs now present in Omegacoin.
