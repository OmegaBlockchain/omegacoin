# Omega Phantom-Masternode Remediation — Plan

_Status: draft for review. Author/date: RottenCoin, 2026-06-12._
_Scope: consensus-critical (deterministic MN list). Read the Consensus Risk Register before any deployment._

---

## Background (confirmed on the live node, 2026-06-12)

- **290** registered masternodes; only **~5** answer their P2P port. The 2018-era "fake VPS IP"
  registration bug (an original-developer inside job) let phantoms register and draw masternode
  rewards without ever running a node.
- The block-**3,200,000** fix (commit `0056435bd4b82a42ea80d4bed8cfdcfe309e022c`) adds an
  `nPoSeSuccessHeight` field plus a periodic liveness sweep in `BuildNewListFromBlock` that bans
  masternodes with no recent LLMQ quorum participation.
- **The patch code is correct but inert.** Verified on the running node:
  - `SPORK_17_QUORUM_DKG_ENABLED = 4070908800` (OFF — never enabled).
  - `quorum list` is empty for all mainnet LLMQ types (50_60, 400_60, 400_85).
  - No non-null quorum commitments in recent blocks.
  - Therefore `HandleQuorumCommitment` never runs → every MN's `nPoSeSuccessHeight` stays `-1`
    → the sweep's `quorumsActive` guard is always false → it bans nobody. `masternode count`
    is frozen at 290 with `pose_banned: 0`. This exactly matches the observed symptom.
- **Why it cannot self-heal:** even if SPORK_17 were enabled, the smallest mainnet quorum
  (`LLMQ_50_60`) needs 40 live members from a 50-member deterministic draw out of all 290.
  With ~5 real nodes, no quorum ever reaches `minSize`, no commitment finalises, no success is
  recorded, and the sweep still bans nobody. The phantoms saturate the selection pool of the very
  mechanism meant to remove them. (Same reason stock Dash PoSe never punished them for years.)

### Mechanism lever (confirmed in source)

- `CDeterministicMNList::GetMNPayee` selects via `ForEachMNShared(true, ...)` where
  `true` = onlyValid = `IsMNValid` = `!IsMNPoSeBanned` = `!IsBanned()`.
- So setting `nPoSeBanHeight` on a phantom (via the existing `CDeterministicMNState::BanIfNotBanned`)
  **immediately and deterministically** removes it from both payment selection and quorum selection.
- This is the lever the remedy uses.

---

## Step 0 — Establish the real masternode count (BLOCKING PREREQUISITE)

The entire strategy branches on this number, so it must be rigorous and reproducible — not a
best-effort parallel probe (the session's first parallel probe lost ~38 results to a job-control
race and is not authoritative).

- Probe of all 290 service addresses, **3 attempts each**, performing an actual Omega P2P
  version handshake (not just a TCP connect — an open port can be a firewall/honeypot false positive).
  Run via a bounded Python thread pool (`/tmp/mn_handshake_probe.py`) to avoid the bash job-control
  race; handshake logic validated against known-live peers and a known-dead address first.
- Cross-check against `getpeerinfo` over a multi-hour window and against `protx diff` payment activity.
- **Output:** evidence file mapping `{service, proTxHash} → reachable: yes/no}`
  (`phantom_probe_evidence.tsv`, committed alongside this plan).

### RESULT (2026-06-12)

```
TOTAL = 290    UP = 5    DOWN = 285
```

The 5 reachable masternodes (handshake completed):

| service                | proTxHash                                                          |
|------------------------|-------------------------------------------------------------------|
| 149.102.158.28:7777    | 67e38c78477a6488adfe0f04be0b1501a06660a61b270d0d76dfc103236d5372  |
| 167.86.91.191:7777     | bcc76445f94b3dd9f3ec0bdbccbdc3f62b5efa3334e1d5a583d75ae8b3f9d8f0  |
| 173.212.254.163:7777   | 24897ef4cb71442c8e01e520ce10560f8178db833cc338cd8f62ce4e972429a4  |
| 185.62.81.131:7777     | ee02bf91b6a9887fad8216a56a32ea72614baab43e4a6c8427300818a8dfb7b1  |
| 207.180.202.182:7777   | 52de5fb0f79c0316883faac0aac3e2d82530745b580be6902957fe173cb37fff  |

### Decision gate → **PATH B (resolved)**

- **≥ ~30 real MNs** → cleanup restores a working network; re-enable quorums → Path A. **Not applicable.**
- **~5–25 real MNs** → no quorum type can form (smallest non-test is `LLMQ_25_67`, `minSize = 22`);
  remedy is **exclusion-only** and the liveness sweep stays dormant until the real population grows
  → **Path B. SELECTED** (5 real ≪ 22).

> **Caveat for Step 1:** this 5/285 split is decisive for the Path A/B decision, but it was measured
> from a single network vantage point in one window. Before any proTxHash is baked into a consensus
> ban list (Step 1), re-probe from ≥2 independent vantage points across a multi-day window and
> intersect the DOWN sets, so a real-but-temporarily-unreachable node is never wrongly banned
> (the conservative-bias rule).

### Quorum size reference (from `src/llmq/params.h`)

| LLMQ type   | size | minSize | threshold |
|-------------|------|---------|-----------|
| llmq_25_67  | 25   | 22      | 17        |
| llmq_50_60  | 50   | 40      | 30        |
| llmq_60_75  | 60   | 50      | 45        |
| llmq_100_67 | 100  | 80      | 67        |
| llmq_400_60 | 400  | 300     | 240       |
| llmq_400_85 | 400  | 350     | 340       |
| llmq_devnet | 12   | 7       | 6         |

Smallest production-grade type needs **22** live members. With ~5 real nodes, **no** quorum can form.

---

## Step 1 — Build the phantom exclusion set

- Ban list = registered MNs that fail the Step 0 handshake across **all** attempts.
- **Conservative bias:** include a node only if it is unambiguously dead (no response on any attempt
  over the whole window). A borderline-reachable node is left alone. Rationale: a false negative
  (missing a phantom) is recoverable on a later cycle; a false positive (banning a real operator) is
  baked into consensus and is not.
- Freeze the result as a hard-coded `std::set<uint256>` of proTxHashes, with the evidence file
  checked in alongside for auditability.

---

## Step 2 — Consensus exclusion at a new fork height

- Add `consensus.nPhantomPurgeHeight` to chainparams (follow the existing `nHPMasternodeHeight`
  pattern). Choose a height with enough lead time for operators to upgrade
  (current tip ~3,247,000 + margin).
- In `BuildNewListFromBlock`, add a **one-time** block guarded by
  `if (nHeight == consensus.nPhantomPurgeHeight)`:
  - iterate the hard-coded set;
  - for each MN still present and not already banned, call the existing
    `newState->BanIfNotBanned(nHeight)` + `newList.UpdateMN(...)`.
- Reuses the exact mechanism the liveness sweep already uses → proven, serialises cleanly into the
  existing `dmn_S4/dmn_D4` (MN_V3_FORMAT) records, **no new DB migration required**.
- Exactly-once at a single fixed height (not `% period`) → fully deterministic, replay-safe,
  reorg-safe. Banned phantoms drop from `GetMNPayee` in the same block.
- **Revival unchanged:** a real `ProUpServTx` revives any wrongly-excluded node, so the action is not
  destructive to collateral or identity — it only halts rewards until the service is genuinely online.

---

## Step 3 — Right-size the quorum layer (Path A) or defer it (Path B)

- **Path A (≥30 real):**
  - `AddLLMQ(LLMQ_25_67)` on mainnet; point `llmqTypeChainLocks` / `llmqTypeInstantSend` at it.
  - Enable SPORK_17 (you hold the key) **after** the purge block confirms.
  - Quorums form from the clean pool; the existing liveness sweep becomes self-sustaining and
    auto-catches future phantoms.
- **Path B (too few real):**
  - Do **NOT** enable SPORK_17 — a quorum that cannot reach `minSize` only spams failed DKG.
  - The Step 2 exclusion is the complete remedy.
  - Document that the liveness sweep re-arms automatically once the real population exceeds the
    chosen quorum's `minSize`.
  - (Optional, out of scope here) a future custom small LLMQ could enable ChainLocks with a small set.

---

## Step 4 — Spork key

- Confirmed: you hold the private key for `oLDnqdsFSFRZCjcBKsz8xeoYCn7SiateZd`, stored **offline**,
  so **no `vSporkAddresses` change** is needed.
- The key is intentionally not present in this node's wallet. When a spork must be broadcast
  (Path A only — to enable SPORK_17), it is signed from the offline key and pushed via
  `spork <name> <value>` / `sporkupdate` from a node temporarily holding it, or the signed message is
  relayed. No further verification needed on this node.

---

## Step 5 — Checkpoints

- After the purge fork stabilises, add checkpoints at the last pre-fork block and at the purge block
  (same hardening applied to Agouti) to lock the cleaned MN set against deep reorg.

---

## Step 6 — Testing (mandatory before mainnet)

- **Regtest/devnet:** register N fake + M real MNs, run past `nPhantomPurgeHeight`; assert the fake
  set becomes `pose_banned` and stops appearing as payees, the real set is untouched, and a
  `ProUpServTx` revives a banned one.
- **Sync-from-zero** with the new binary: confirm the one-time action replays identically and
  `MigrateDBIfNeeded3` still behaves.
- **Fork agreement:** a node on the old binary and a node on the new binary must agree on the MN list
  up to the fork height, then diverge only at the purge block (expected hard-fork behaviour — must be
  coordinated with operators).

---

## Consensus Risk Register

- **CONSENSUS RISK – REVIEW REQUIRED:** Step 2 changes the deterministic MN list at a fixed height.
  Every node must run the patched binary by `nPhantomPurgeHeight` or the network forks. This is the
  central deployment hazard and requires an operator-communications plan.
- The hard-coded ban list is itself consensus data — a single wrong proTxHash bakes a wrong ban into
  the chain. Hence the conservative-bias rule (Step 1) and the checked-in evidence file.
- No serialization change (reuses MN_V3_FORMAT + `BanIfNotBanned`) → no new evoDB migration and no
  `b_b5` → `b_b6` bump.

---

## Immediate next actions

1. ~~Run the rigorous Step 0 probe~~ — **DONE 2026-06-12: 5 real / 285 phantom → Path B.**
2. Step 1: re-probe from ≥2 vantage points over a multi-day window; intersect DOWN sets to finalise
   the consensus ban list (conservative bias — only unambiguously-dead nodes).
3. Step 2: implement the one-time exclusion at `nPhantomPurgeHeight` (Path B — no quorum/spork work).
4. Steps 5–6: checkpoints + regtest/devnet tests before mainnet.

_Spork key: held offline (Step 4) — no on-node action required. Path B does not enable SPORK_17._
