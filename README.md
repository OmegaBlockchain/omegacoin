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
