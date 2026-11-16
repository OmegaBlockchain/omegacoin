# Omega Core 0.21.0

**Status: draft** — release planned for early December 2026. Update this file
before tagging.

Omega Core 0.21.0 is a security and bug-fix release. It does **not** change
consensus rules or the network protocol: `PROTOCOL_VERSION` stays `70233`,
`MIN_PEER_PROTO_VERSION` stays `70232`, and 0.21.0 nodes interoperate with
0.20.9 nodes (older releases are already split off by the 0.20.9 fork). Upgrading is recommended for all nodes, and strongly
recommended for anyone using secure messaging (SMSG) or the trollbox.

## Fixes

### SMSG keys "disappear" after a restart (BUG-01)

Since 0.20.4, SMSG address registrations were lost on every restart: the
messaging page, the trollbox "From" list and `smsglocalkeys` no longer showed
an address after omega-qt/omegad was closed and opened again. Topic
subscriptions were not restored at start either.

The private keys were never lost — they are in the wallet — only the SMSG
registration in `smsg.ini` was not read at start and was then overwritten on
shutdown. After upgrading, re-register an affected address once with
`smsgaddlocaladdress <address>` (omega-qt RPC console or `omega-cli`); it now
persists.

Users on 0.20.4–0.20.9 can work around the bug by starting with
`-smsgsaddnewkeys=1`, which re-registers every wallet address (note: this
enables SMSG receiving on all wallet addresses).

### Messages from some nodes were never relayed (BUG-02)

A node whose clock is behind its peers' clocks (negative `timeoffset` in
`getnetworkinfo`) did not announce new messages to its peers: with an offset of
-1 second about half of its messages stayed on the node, with -2 or lower all
of them, including messages it should have relayed. Such a node still received
messages normally. Fixed by using the same clock for both sides of the
announcement check. Until upgraded, an affected node can push a stuck message
by reconnecting a peer (`disconnectnode "<peer>"`) or restarting.

### Secure messaging hardening

- `smsgimport` rejects a message whose header payload length does not match
  the data supplied (previously the node could read past the message buffer).
- A peer can no longer lock message buckets or inflate request counters with
  unsolicited `smsgHave` messages; only buckets this node asked the peer about
  are acted on.
- The purged-message check had an inverted condition; purged messages are
  now recognised by the cheap pre-check again.
- Trollbox: expired rows are pruned from the database; `trollboxlist` reads
  newest-first and stops at the requested count or the retention limit; the
  number of messages stored per bucket is capped.

### ZMQ

- If a ZMQ publisher address is already in use, the node now logs "ZMQ
  publishing is disabled" and keeps running instead of aborting with an
  assertion. Note: all ZMQ publishers are still enabled by default on
  `127.0.0.1:7780` and readable by any local process.

## Operator notes

- No reindex, no wallet upgrade and no configuration change is required.
- Masternodes: no protocol change; upgrade at your convenience.
- Downloads are published under the GitHub tag `v0.21.0` (the in-app update
  check parses the tag numerically).

## Not in this release

The SMSG proof-of-work difficulty for free messages (SEC-05) and storing/
relaying topic messages regardless of subscription (SEC-09) are network-
protocol changes that need a coordinated, version-gated upgrade; they are not
included.
