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

### Step 1 progress (2026-06-12)

- **ICMP cross-check** of the 285 DOWN hosts: 71 `host_alive` (VPS up, MN port closed — textbook
  phantoms), 214 `no_icmp`. Subnet clustering: 152.67.0.0/16 (Oracle) = 187 of 285; a handful of /16s
  cover the rest — consistent with bulk insider registration, not organic operators.
- **Live partition cross-check** against the chain: all 285 committed phantoms are currently
  registered valid MNs (0 stale); none of the 5 reachable nodes are in the list; 285 + 5 = 290 exact.
- **Heavier re-probe of the 214 `no_icmp` group** (5 attempts × 8 s handshake): **0 responded** —
  all 214 confirmed unreachable. Recorded in `phantom_probe_evidence.tsv` (`reprobe = down_5x8s`).
- **Remaining gap (downgraded):** all probes so far are from a SINGLE vantage point. Originally
  flagged as a hard prerequisite, but the architectural argument supersedes it:
  - The probed address is each MN's on-chain `service`/`addr` (providertx.h) — the *single canonical
    address the whole network uses*. There is no alternate path; the probe is functionally identical
    to what every node and quorum member does. "Unreachable from the probe" ≈ "unreachable from the
    network," and a masternode the network cannot reach cannot serve quorums/ChainLocks/InstantSend —
    it is functionally useless regardless of whether the host is powered on.
  - The only false-DOWN mode left is **asymmetric routing** (this host/AS route-broken to a node the
    rest of the internet reaches). With the quorum layer OFF and only 5 live MNs, even such a node
    contributes nothing today.
  - **Self-correcting safety valve:** ~~a PoSe-banned MN revives automatically via `ProUpServTx`~~
    **superseded by Step 4c:** the exclusion is now permanent (ProUpServTx revival and
    same-collateral ProRegTx replacement are both rejected for a listed proTxHash). A
    genuinely-operational false positive recovers by spending the collateral and re-registering
    under a new proTxHash — two transactions, no coordination required, collateral never at risk.
  - **Conclusion:** a second independent vantage point is now *recommended belt-and-suspenders*, not a
    blocker. The exclusion list is justified on functional-uselessness grounds with a cheap recovery.
    Note the recovery is heavier than originally stated (spend + re-register, not one ProUpServTx),
    which strengthens — not weakens — the case for the multi-vantage re-probe before release.

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
  reorg-safe. Banned phantoms drop from `GetMNPayee` from the next block: the payee of the purge
  block itself was already selected from the previous block's list, so the purge block can still pay
  one phantom one last time; every block after it pays only from the cleaned list.
- **Revival:** ~~a real `ProUpServTx` revives any wrongly-excluded node~~ **superseded by Step 4c** —
  the exclusion is permanent; a wrongly-excluded operator recovers by spending the collateral and
  re-registering under a new proTxHash. Collateral and funds are never at risk.

---

## Step 3 — Right-size the quorum layer (Path A) or defer it (Path B)

- **Path A (≥30 real):**
  - `AddLLMQ(LLMQ_25_67)` on mainnet; point `llmqTypeChainLocks` / `llmqTypeInstantSend` at it.
  - Enable SPORK_17 (you hold the key) **after** the purge block confirms.
  - Quorums form from the clean pool; the existing liveness sweep becomes self-sustaining and
    auto-catches future phantoms.
- **Path B (too few real):**
  - Do **NOT** enable SPORK_17 for the stock quorum types — a quorum that cannot reach `minSize`
    only spams failed DKG.
  - **Updated by Step 4c:** the custom small LLMQ originally deferred here is now implemented
    (`LLMQ_5_3`, sized for the 5 real MNs). SPORK_17 **is** to be signed, but only after the purge
    fork confirms and the 5 real MNs run 0.20.9, so `LLMQ_5_3` DKGs run from the cleaned pool.
  - The liveness sweep re-arms once `LLMQ_5_3` quorums finalise commitments.

---

## Step 4 — Spork key

- Confirmed: you hold the private key for `oLDnqdsFSFRZCjcBKsz8xeoYCn7SiateZd`, stored **offline**,
  so **no `vSporkAddresses` change** is needed.
- The key is intentionally not present in this node's wallet. When a spork must be broadcast
  (Path A only — to enable SPORK_17), it is signed from the offline key and pushed via
  `spork <name> <value>` / `sporkupdate` from a node temporarily holding it, or the signed message is
  relayed. No further verification needed on this node.

---

## Step 4b — Protocol-version bump + fork enforcement (clean cutover)

The purge changes `merkleRootMNList` (via `CSimplifiedMNListEntry::isValid = !IsBanned()`), so a block
produced post-purge is rejected by pre-purge nodes with `bad-cbtx-mnmerkleroot`. The original phantom
fix did **not** bump the protocol version, so pre- and post-fix binaries both reported 70232 and could
not be told apart by the existing fork-enforcement disconnect — risking a *connected-but-conflicting*
split. Fixed (2026-06-12):

- `PROTOCOL_VERSION` 70232 → **70233** (`src/version.h`).
- New `FORK_3300000_MIN_PROTO_VERSION = 70233` (`src/version.h`).
- New `consensus.nPhantomForkEnforcementHeight = 3299000` on mainnet (~1000 blocks / ~20h before the
  3,300,000 purge; 0 on testnet/devnet/regtest). `src/chainparams.cpp`, `src/consensus/params.h`.
- Two disconnect checks added in `src/net_processing.cpp` (inbound version handler + connected-peer
  sweep), mirroring the existing `FORK_3200000` pattern: peers below 70233 are dropped once the
  enforcement height is reached, so the network partitions by version **before** the merkle change.
- `MIN_PEER_PROTO_VERSION` left at 70232 deliberately — pre-purge peers stay connected until the
  enforcement height rather than being rejected network-wide immediately.
- Client/release version bumped to **0.20.9** (`configure.ac`). Requires re-running
  autogen/configure for the new version to compile in.

---

## Step 4c — Hardening at the same fork (implemented 2026-07-02)

The one-time purge alone left two resurrection paths, both closed at `nPhantomPurgeHeight`
in the same 0.20.9 fork (no extra fork event):

1. **Permanent exclusion (revival hole closed).** A `ProUpServTx` unconditionally revived a
   PoSe-banned MN, so whoever holds the 285 operator BLS keys could undo the whole purge for
   tx fees. From the purge height, `CheckProUpServTx` rejects any ProUpServTx whose
   proTxHash is listed (`bad-protx-phantom-banned`) — enforced in mempool and block
   validation via the shared `CheckSpecialTx` path — and `BuildNewListFromBlock` refuses to
   revive a listed hash (defence in depth). Additionally, a ProRegTx that references a
   listed phantom's **existing collateral** is rejected (`bad-protx-phantom-collateral`,
   same dual enforcement): without this, the in-place same-collateral replacement rule
   would re-register the phantom under a fresh, unlisted proTxHash without ever spending
   the collateral. Re-entry therefore always requires an on-chain collateral spend.
   Recovery for a hypothetical false positive: spend the collateral and re-register under
   a new proTxHash; funds are never at risk.
2. **PoSe re-armed with a right-sized quorum (re-registration defence).** New mainnet LLMQ
   type `LLMQ_5_3` (size 5, minSize 4, threshold 3, DKG every 24 blocks). It is enabled only
   from `nPhantomPurgeHeight` (see `IsQuorumTypeEnabledInternal`) so pre-fork block validity
   is untouched (commitment requirements are NOT spork-gated — an always-on type would have
   split 0.20.9 from 0.20.7 immediately), and it stays dormant until SPORK_17 is signed
   post-fork. Because pre-fork 0.20.7 binaries do not know the type at all and reject any
   block carrying a commitment for it, patched nodes mirror that rule: any `LLMQ_5_3`
   commitment (even a valid null one) in a block at or below the purge height is rejected
   (`bad-qc-commitment-type` in `GetCommitmentsFromBlock`) — otherwise a crafted null
   commitment could split patched from unpatched nodes *before* the coordinated fork.
   The purge height is asserted to be a multiple of the 24-block dkgInterval so the first
   DKG cycle starts exactly at the fork from the freshly purged 5-node list. Once quorums
   form, `HandleQuorumCommitment` stamps `nPoSeSuccessHeight` and the existing liveness
   sweep re-arms. `MIN_MASTERNODE_PROTO_VERSION` is bumped to 70233 so a lingering pre-fork
   node is marked a bad DKG member rather than stalling sessions.
3. **Sweep grace tightened.** From the purge height the new-registration grace drops from
   `nSuperblockCycle` (~30 days) to `nSuperblockCycle/8`, so a newly registered node has a
   shorter window before its first liveness check while still leaving a genuine operator
   ample time to appear in a DKG. The sweep only acts while quorums are demonstrably active
   (`quorumsActive` guard). A revived MN gets the same grace measured from
   `nPoSeRevivedHeight` so a genuine operator recovering via `ProUpServTx` is not re-banned
   before its first DKG opportunity.

The hard-coded exclusion list remains the authoritative, permanent mechanism; the
LLMQ_5_3 sweep is a supplementary automatic cleanup for future stragglers once the real
population is healthy. The exclusion list can be extended at a later fork if operational
monitoring shows it is warranted.

**Test hooks (regtest-only):** `-phantompurgeheight=<n>` and `-phantomexclude=<proTxHash>`
(repeatable; replaces the hard-coded list). Coverage: `feature_phantom_purge.py`
(purge bans, payee exclusion, height-gated ProUpServTx rejection, same-collateral
replacement rejection for listed MNs + allowance for non-listed MNs, non-listed revival,
reorg + `-reindex` determinism) and `evo_phantom_exclusions_tests.cpp` (list integrity).

**Post-fork operational steps:** confirm the purge log line reports `banned 285 of 285`,
then sign SPORK_17 from the offline key once the 5 real MNs run 0.20.9. Routine post-fork
monitoring of masternode registrations is recommended, as for any network.

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

_Spork key: held offline (Step 4) — no on-node action required until after the purge fork,
when SPORK_17 is signed to activate LLMQ_5_3 DKGs (Step 4c)._
