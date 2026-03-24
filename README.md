Omega Core 0.20
===============

https://omegablockchain.net

What is Omega?
--------------

Omega is a peer-to-peer digital currency and blockchain platform launched on 2 January 2018.
It is forked from Dash Core and extends it with a number of protocol-level features including
on-chain encrypted messaging, Schnorr signatures, and an improved proof-of-work difficulty
algorithm. The network runs with 60-second block times and masternode-based governance.

- **Port:** 7777 (mainnet), 17778 (testnet)
- **Block time:** 60 seconds
- **Addresses start with:** `o`
- **Script addresses start with:** `7`
- **Genesis block:** 2 January 2018
- **Seed node:** seed.omegablockchain.net

What's New in 2025-2026
-----------------------

The following features and improvements have been implemented this development cycle,
starting from the Dash 19.3 base:

### Consensus Protocol Upgrades

- **Schnorr Signatures** — Full Schnorr signature consensus activated at block 3,205,000.
  Includes `OP_CHECKDATASIG` and `OP_CHECKDATASIGVERIFY` opcodes, enabling compact
  multi-party signing and cross-chain atomic swap constructions.

- **LWMA Difficulty Algorithm** — Zawy's LWMA (Linearly Weighted Moving Average) difficulty
  adjustment activated at block 3,220,000 with a 60-block window. Provides faster and more
  stable difficulty retargeting compared to the legacy algorithm, improving resistance to
  hash rate fluctuations.

- **Increased Script Element Size** — `MAX_SCRIPT_ELEMENT_SIZE` raised from 520 to 4096 bytes,
  enabling more complex script constructions.

- **Increased Standard Transaction Size** — `MAX_STANDARD_TX_SIZE` raised to 400 KB.

- **Legacy Fork Activations Buried** — Old soft fork activation logic (BIP65, BIP66, CSV)
  buried at their historical heights, simplifying validation.

### Secure Messaging (SMSG)

On-chain encrypted peer-to-peer messaging ported from Particl, integrated directly into
the Omega consensus layer:

- **SMSG Library** — Full secure messaging implementation (`src/smsg/`) including encrypted
  message storage, key management, and P2P relay.

- **TRANSACTION_SMSG_ROOM** — New transaction type (type 8) for funding decentralised
  messaging rooms on-chain.

- **Confidential SMSG Funding** — Confidential transaction support for SMSG room funding,
  activated at block 3,205,000 on mainnet.

- **SMSG Key Generation UI** — GUI button in the Qt wallet to generate and manage SMSG keys.

- **Messages Tab** — Dedicated messaging tab in the Qt wallet interface.

### Wallet & GUI

- **Masternode Wizard** — Step-by-step masternode setup wizard added to the Qt GUI.
  If an error occurs during registration, the wizard displays a structured error
  code with an explanation and suggested fix. See the error reference below.

- **Governance Tab** — Full governance interface in the Qt wallet. Browse proposals,
  vote Yes/No/Abstain with all your masternodes in one click, and create new budget
  proposals directly from the GUI. Supports wallets controlling multiple masternodes —
  each vote is cast with every masternode whose voting key is held in the wallet.
  See the Governance Guide below.

- **Rebranding** — Full Omega branding applied throughout: splash screen, colour scheme,
  icons, window titles, and copyright notices updated.

- **Qt Upgrade** — Qt upgraded from 5.12.11 to 5.15.18 for improved compatibility and
  security.

### Build System

- **Guix Reproducible Builds** — Full GNU Guix reproducible build support upgraded and
  working for Linux (x86\_64, aarch64) and Windows (x86\_64-w64-mingw32).

- **Dependency Cache Persistence** — Built dependency cache now stored at
  `~/.cache/guix-base-cache` by default, surviving `guix-clean` between builds.

- **Python 3.12 Compatibility** — Build scripts updated to support Python 3.12.

- **Seeds Updated** — DNS seed list refreshed.

Governance Guide
----------------

Omega uses a decentralised governance system where masternode owners vote on budget
proposals. Proposals that receive enough yes votes are funded automatically via
superblock payments.

### Key Parameters

| Parameter             | Mainnet Value |
|-----------------------|---------------|
| Superblock cycle      | ~16,616 blocks (~30 days) |
| Proposal collateral   | 1 OMEGA (non-refundable) |
| Collateral confirmations | 6 |
| Passing threshold     | (Yes − No) ≥ (Total masternodes / 10) |

### Viewing Proposals

Open the **Governance** tab in the Qt wallet. The table shows all active proposals
with title, start/end dates, payment amount, yes/no/abstain vote counts, and
current voting status (passing or votes still needed).

- **Filter** proposals by title using the search box.
- **Double-click** a proposal to see its full JSON data.
- **Right-click** for a context menu to open the proposal URL, copy the hash, or vote.

### Voting on Proposals

Select a proposal in the table and click **Vote Yes**, **Vote No**, or **Vote Abstain**.

The wallet uses `gobject vote-many` internally, which finds every registered
masternode whose voting key is held in this wallet and casts a vote with each one.
If you operate multiple masternodes from the same wallet, all of them vote in a
single action. The result message shows how many masternodes voted successfully
and how many failed.

The wallet must be **unlocked** to vote (Settings → Unlock Wallet).

### Creating a Proposal

Click **Create Proposal** to open the proposal dialog. Fill in:

| Field           | Description |
|-----------------|-------------|
| Name            | Short identifier (a–z, 0–9, hyphens, underscores; max 40 characters) |
| URL             | Link to a page describing the proposal in detail |
| Payment count   | Number of monthly superblock payments (1–100) |
| Amount (OMEGA)  | Amount per payment |
| Payment address | Omega address that will receive the funds |

Proposal submission is a two-stage process:

1. **Prepare** — Click "Prepare Proposal". The wallet creates and broadcasts a
   collateral transaction (1 OMEGA). The dialog displays the collateral transaction
   hash and begins monitoring confirmations.

2. **Submit** — After the collateral reaches 6 confirmations, the "Submit Proposal"
   button becomes active. Click it to broadcast the proposal to the network.
   Masternode owners can then vote on it from the Governance tab.

### Governance Error Messages

Errors in the governance tab follow the format shown in the result dialog.
Common issues and solutions:

| Error | Meaning | Fix |
|-------|---------|-----|
| Wallet not available | No wallet loaded or wallet model missing | Open or create a wallet first |
| Please select a proposal to vote on | No row selected in the table | Click on a proposal before voting |
| Wallet is locked | Voting requires an unlocked wallet | Settings → Unlock Wallet |
| Can't find masternode by proTxHash | The masternode is no longer in the valid set | Ensure the masternode is registered and not banned |
| Failure to sign | The wallet does not hold the voting key | Import the voting key or use the wallet that registered the masternode |
| Voted successfully 0 time(s) | No masternodes with voting keys found | Ensure this wallet holds at least one masternode voting key |
| Invalid proposal data | One or more proposal fields are invalid | Check name, URL, amount, and address format |
| Collateral requires at least 6 confirmations | The collateral transaction has not confirmed yet | Wait and try the Submit step again |
| Must wait for client to sync | Blockchain is not fully synchronised | Wait for sync to complete |
| Error making collateral transaction | Insufficient funds for the proposal fee | Ensure the wallet has at least 1 OMEGA plus a small transaction fee |

Masternode Wizard Error Reference
---------------------------------

Errors displayed by the masternode wizard follow the format
`[MNW-xxx] Description / Suggestion`. The table below lists every code.

### Key & Address Generation (MNW-2xx)

| Code    | Meaning                                    | Suggested Fix |
|---------|--------------------------------------------|---------------|
| MNW-210 | Could not read BLS key data.               | Try generating keys again. If this persists, restart the wallet. |
| MNW-211 | BLS key generation returned incomplete data. | Try generating keys again. If this persists, restart the wallet. |

### Collateral & Funding (MNW-3xx)

| Code    | Meaning                                    | Suggested Fix |
|---------|--------------------------------------------|---------------|
| MNW-301 | Collateral address reuses the owner or voting address. | Go back and generate fresh addresses. |
| MNW-302 | Collateral is not held in a standard (P2PKH) address. | Send the collateral to a regular wallet address, not a multisig or script address. |
| MNW-303 | Collateral transaction is missing, already spent, or the wrong amount. | Ensure the wallet has an unspent output of exactly the required collateral amount. |
| MNW-304 | Payout address reuses the owner or voting address. | Go back and generate fresh addresses. |
| MNW-305 | Payout address is not a valid standard address. | Ensure the payout address is a regular (P2PKH or P2SH) address. |
| MNW-306 | One or more required keys are missing.     | Go back and regenerate keys and addresses. |
| MNW-307 | BLS operator key uses an incompatible scheme. | Regenerate the BLS key pair. |
| MNW-310 | Not enough funds to complete the registration. | The wallet needs the collateral amount plus a small fee. Check balance and wait for pending transactions to confirm. |
| MNW-311 | Collateral found but no additional funds for the transaction fee. | Send a small amount (at least 0.001 OMEGA) to any address in this wallet and wait for it to confirm. |
| MNW-312 | Network fee could not be estimated.        | Wait for the blockchain to sync fully, then try again. |

### Network & Duplicates (MNW-4xx)

| Code    | Meaning                                    | Suggested Fix |
|---------|--------------------------------------------|---------------|
| MNW-401 | IP address is already registered to another masternode. | Use a different IP address, or update the existing masternode instead of registering a new one. |
| MNW-402 | Owner key or operator key is already in use. | Go back and generate a new set of keys. |
| MNW-403 | Platform Node ID is already registered.    | Assign a unique Platform Node ID. |
| MNW-404 | Port number is not allowed for this network. | Use the default port shown in the wizard. |
| MNW-405 | IP address is not a valid, publicly routable IPv4 address. | Enter the public IP of your server (not a LAN or localhost address). |

### Wallet & Signing (MNW-5xx)

| Code    | Meaning                                    | Suggested Fix |
|---------|--------------------------------------------|---------------|
| MNW-501 | Registration transaction could not be signed. | Ensure the wallet is unlocked and contains the private key for the owner address. |
| MNW-502 | Wallet could not sign the registration transaction. | Unlock the wallet (Settings > Unlock Wallet), then try again. |
| MNW-510 | Wallet is locked.                          | Unlock the wallet (Settings > Unlock Wallet), then try again. |
| MNW-511 | Wallet was closed while the wizard was open. | Close the wizard and reopen it. |
| MNW-512 | Wallet does not hold the private key for the owner address. | The owner address must belong to this wallet. |

### Protocol (MNW-6xx)

| Code    | Meaning                                    | Suggested Fix |
|---------|--------------------------------------------|---------------|
| MNW-601 | Registration data could not be processed by the network. | This may indicate a version mismatch. Ensure the wallet is up to date. |
| MNW-602 | Unsupported masternode registration version. | Update the wallet to the latest release. |
| MNW-603 | Masternode type is not supported on this network. | Ensure you are on the correct network and the wallet is up to date. |

### Catch-all

| Code    | Meaning                                    | Suggested Fix |
|---------|--------------------------------------------|---------------|
| MNW-900 | Unexpected error (raw detail is shown).    | Check that the blockchain is fully synced and the wallet is unlocked. If the problem persists, copy the error and ask for help. |

Building
--------

### Linux / macOS

```bash
./autogen.sh
./configure
make -j$(nproc)
```

### Reproducible builds via GNU Guix

```bash
# All platforms (Linux x86_64, Linux aarch64, Windows x86_64)
./contrib/guix/guix-build

# Single platform
HOSTS="x86_64-w64-mingw32" ./contrib/guix/guix-build
HOSTS="x86_64-linux-gnu" ./contrib/guix/guix-build
HOSTS="aarch64-linux-gnu" ./contrib/guix/guix-build
```

Built dependency tarballs are cached at `~/.cache/guix-base-cache` and source tarballs
at `~/.cache/guix-sources`, so they survive `guix-clean`.

### Dependencies

```bash
cd depends
make HOST=x86_64-linux-gnu -j$(nproc)
```

License
-------

Omega Core is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see https://opensource.org/licenses/MIT.

Omega Core is derived from Dash Core, which is derived from Bitcoin Core.

Development Process
-------------------

The `master` branch is the stable release branch. Development work is done in feature branches
and merged when stable.

Issues and contributions: https://github.com/OmegaBlockchain/omega/issues
