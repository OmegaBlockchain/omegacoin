# Omega Core 0.21.0

**Status: draft** — release planned for early December 2026. Update this file
before tagging.

Omega Core 0.21.0 is a security and secure-messaging release. It does **not**
change consensus rules: blocks and transactions are validated and relayed
exactly as before, `MIN_PEER_PROTO_VERSION` stays `70232`, and 0.21.0 nodes
sync the chain with 0.20.9 nodes.

It **does** change the secure-messaging (SMSG) network rules, and raises
`PROTOCOL_VERSION` to `70234`. 0.21.0 nodes exchange SMSG only with other
0.21.0 nodes; 0.20.x nodes keep messaging among themselves until they upgrade.
**Upgrade all masternodes and messaging users together** — SMSG reaches
across the network only through upgraded nodes.

## SMSG network changes (protocol 70234)

- **Topic messages (rooms) work.** Up to 0.20.9 a topic message carried its
  topic hint in the header `nonce`, which the proof of work then overwrote, so
  no node ever matched a topic message to its topic; nodes not subscribed to a
  topic also dropped its messages instead of relaying them. Now every node
  stores and relays topic messages like any other, and a subscriber finds its
  messages by trying its subscribed topic keys. Topic messages live 2 days on
  the network like other free messages (their lifetime used to be read from the
  PoW counter, i.e. random). Posting to a topic no longer requires subscribing to
  it.
- **Free-message proof of work is the documented 19 bits** (it was effectively
  ~16.2). New messages always carry the 19-bit proof; messages timestamped
  from 2026-12-01 00:00 UTC on must carry it, older ones stay valid.
- **Messages can no longer be blocked by a look-alike.** A message is
  identified for deduplication and sync by its timestamp and a hash of the
  whole message; it used to be its first 8 payload bytes, which a relay could
  copy into a junk message to have the real one refused everywhere.
- **SMSG only between 0.21.0 nodes** (`MIN_SMSG_PROTO_VERSION` 70234), so that
  old and new rules never meet and re-request each other's messages forever.

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
- A peer can no longer make the node send the same message index again and
  again: `smsgShow` is answered only for buckets this node advertised to that
  peer, once per advertisement (one small request could previously pull the
  same large bucket hundreds of times).
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
- Masternodes: please upgrade promptly — they carry most SMSG relay. Block
  relay and LLMQ participation are unaffected by the version bump.
- Downloads are published under the GitHub tag `v0.21.0` (the in-app update
  check parses the tag numerically).
