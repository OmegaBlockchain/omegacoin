# Omega Companion App — Project Specification

This document describes the architecture and design of the Omega companion app.

---

## §1 Overview

The Omega companion app provides a user-friendly interface for the Omega
blockchain, including wallet management, SMSG messaging, and a peer-to-peer
marketplace with escrow.

---

## §7 Marketplace

### 7.1 Overview

Marketplace listings are published as SMSG messages to a well-known room address.

### 7.2 Listing Format

TODO

### 7.3 Escrow Model

Escrow is implemented as a 2-of-3 multisig transaction. The three keyholders
are: buyer, seller, and arbitrator.

**Escrow flow:**
1. Seller publishes listing via SMSG
2. Buyer signals purchase intent via SMSG
3. Buyer funds multisig escrow address
4. Seller ships / delivers
5. Buyer confirms receipt → releases escrow to seller
6. On dispute, arbitrator votes 2-of-3

**Note — PSBT preferred over raw PST exchange:**
The Omega daemon has full PSBT support (`createpsbt`, `walletprocesspsbt`,
`combinepsbt`, `finalizepsbt`). The implementation in Phase 3 should use
PSBT rather than manual raw transaction construction and
`signrawtransactionwithkey`. PSBT handles partial signature serialisation
and combination cleanly and is the standard approach in all modern
Bitcoin-derived projects. See `docs/rpc-interface.md` for the revised
escrow signing flow.

---

## §8 Implementation Details

### 8.1 SMSG Transport

All messaging uses SMSG. See `docs/smsg-protocol.md`.

### 8.2 Wallet Integration

See `docs/rpc-interface.md` — Wallet Methods.

### 8.3 Key Management

TODO

### 8.4 Room Protocol

TODO

### 8.5 Escrow Transaction Construction

Escrow transactions are constructed and signed using PSBT:

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
