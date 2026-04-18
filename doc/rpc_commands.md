# Omega Core — RPC Command Reference

All commands invoked via `omega-cli <command> [args]` or JSON-RPC.

---

## Blockchain

| Command | Arguments | Description |
|---------|-----------|-------------|
| `getblockchaininfo` | — | Return general blockchain state: chain, height, headers, difficulty, fork deployments. |
| `getblockcount` | — | Return the height of the most-work fully-validated chain. |
| `getbestblockhash` | — | Return the hash of the best (tip) block. |
| `getbestchainlock` | — | Return the best ChainLock known to this node. |
| `getblockhash` | `height` | Return the hash of the block at the given height. |
| `getblock` | `blockhash ( verbosity )` | Return a block, optionally with full transaction data. `verbosity` 0=hex, 1=summary, 2=full. |
| `getblockheader` | `blockhash ( verbose )` | Return the block header for a given hash. |
| `getblockheaders` | `blockhash ( count verbose )` | Return an array of consecutive block headers starting from `blockhash`. |
| `getblockhashes` | `high low` | Return an array of block hashes for blocks with timestamps in `[low, high]`. |
| `getblockstats` | `hash_or_height ( stats )` | Compute per-block statistics for a given block height or hash. |
| `getblockfilter` | `blockhash ( filtertype )` | Return the compact block filter for a block (BIP 157/158). |
| `getchaintips` | `( count branchlen )` | Return information about all known chain tips: active, valid, invalid, headers-only. |
| `getchaintxstats` | `( nblocks blockhash )` | Return statistics about the total number and rate of transactions in the chain. |
| `getdifficulty` | — | Return the current proof-of-work target as a multiple of the minimum difficulty. |
| `getmempoolancestors` | `txid ( verbose )` | Return all in-mempool ancestors of a transaction. |
| `getmempooldescendants` | `txid ( verbose )` | Return all in-mempool descendants of a transaction. |
| `getmempoolentry` | `txid` | Return mempool data for a given transaction. |
| `getmempoolinfo` | — | Return current mempool statistics (size, bytes, fees). |
| `getrawmempool` | `( verbose )` | Return all transaction IDs (or full objects) in the mempool. |
| `getmerkleblocks` | `filter blockhash ( count )` | Return serialised merkleblock objects matching a Bloom filter, starting from `blockhash`. |
| `getspecialtxes` | `blockhash ( type count skip verbosity )` | Return special transactions (ProTx, CbTx, etc.) in a block, optionally filtered by type. |
| `gettxout` | `txid n ( include_mempool )` | Return details about an unspent transaction output. |
| `gettxoutsetinfo` | `( hash_type )` | Return statistics about the UTXO set (total UTXOs, total amount, hash). |
| `gettxoutproof` | `txids ( blockhash )` | Return a hex-encoded proof that a transaction was included in a block. |
| `verifytxoutproof` | `proof` | Verify a proof returned by `gettxoutproof` and return the transactions it proves. |
| `getspentinfo` | `json` | Return the txid and input index that spent a given output `{txid, index}`. Requires `spentindex=1`. |
| `pruneblockchain` | `height` | Prune the blockchain up to the specified height or timestamp. |
| `savemempool` | — | Flush the mempool to disk. |
| `verifychain` | `( checklevel nblocks )` | Verify the blockchain database. `checklevel` 0–4; `nblocks` 0=all. |
| `preciousblock` | `blockhash` | Treat a block as if it were received before others at the same height (reorg trigger). |
| `scantxoutset` | `action scanobjects` | Scan the UTXO set for matching descriptors or addresses. |
| `invalidateblock` | `blockhash` | Mark a block as invalid and disconnect it. *(hidden)* |
| `reconsiderblock` | `blockhash` | Remove the invalid flag from a block and re-process it. *(hidden)* |
| `waitfornewblock` | `( timeout )` | Wait until a new block arrives or the timeout (ms) elapses. *(hidden)* |
| `waitforblock` | `blockhash ( timeout )` | Wait until a specific block is in the best chain or timeout. *(hidden)* |
| `waitforblockheight` | `height ( timeout )` | Wait until the best chain reaches the given height or timeout. *(hidden)* |
| `syncwithvalidationinterfacequeue` | — | Wait until the validation interface callback queue is drained. *(hidden)* |
| `dumptxoutset` | `path` | Write the current UTXO set snapshot to a file. *(hidden)* |

---

## Address Index

Requires `addressindex=1` in `omega.conf`.

| Command | Arguments | Description |
|---------|-----------|-------------|
| `getaddressmempool` | `addresses` | Return all mempool deltas (in/out) for a set of addresses. |
| `getaddressutxos` | `addresses` | Return all UTXOs for a set of addresses. |
| `getaddressdeltas` | `addresses` | Return all blockchain deltas (credits and debits) for a set of addresses. |
| `getaddresstxids` | `addresses` | Return all transaction IDs for a set of addresses. |
| `getaddressbalance` | `addresses` | Return the confirmed balance for a set of addresses. |

---

## Mining & Generating

| Command | Arguments | Description |
|---------|-----------|-------------|
| `getnetworkhashps` | `( nblocks height )` | Estimate the network hashes per second over the last `nblocks`. |
| `getmininginfo` | — | Return mining-related information: hashrate, difficulty, networkhashps, blockvalue. |
| `prioritisetransaction` | `txid fee_delta` | Modify the effective fee of a transaction for mining priority. |
| `getblocktemplate` | `( template_request )` | Return data needed to construct a block, or submit a proposal. |
| `submitblock` | `hexdata ( dummy )` | Attempt to submit a new block. |
| `submitheader` | `hexdata` | Attempt to submit a new block header only (SPV mining). |
| `generatetoaddress` | `nblocks address ( maxtries )` | Mine `nblocks` blocks to the given address. |
| `generatetodescriptor` | `num_blocks descriptor ( maxtries )` | Mine blocks to the given output descriptor. *(hidden in production)* |
| `generateblock` | `address transactions` | Mine a single block containing specific transactions. *(hidden in production)* |
| `estimatesmartfee` | `conf_target ( estimate_mode )` | Estimate the fee rate (OMEGA/kB) for a transaction to confirm in `conf_target` blocks. |
| `estimaterawfee` | `conf_target ( threshold )` | Return raw fee estimates; lower-level than `estimatesmartfee`. *(hidden)* |

---

## Network

| Command | Arguments | Description |
|---------|-----------|-------------|
| `getconnectioncount` | — | Return the number of active peer connections. |
| `ping` | — | Request a ping from all peers; results visible in `getpeerinfo` `pingtime`. |
| `getpeerinfo` | — | Return detailed information about each connected peer. |
| `addnode` | `node command` | Add, remove, or attempt a connection to a peer. `command`: `add`/`remove`/`onetry`. |
| `disconnectnode` | `( address nodeid )` | Disconnect a peer by IP/port or node ID. |
| `getaddednodeinfo` | `( node )` | Return info about nodes added via `addnode`. |
| `getnettotals` | — | Return total bytes sent/received and current time. |
| `getnetworkinfo` | — | Return P2P network state: version, subversion, connections, local addresses, services. |
| `setban` | `subnet command ( bantime absolute )` | Add or remove a banned subnet. |
| `listbanned` | — | List all manually banned subnets. |
| `clearbanned` | — | Clear all banned subnets. |
| `cleardiscouraged` | — | Clear all discouraged peers. |
| `setnetworkactive` | `state` | Enable (`true`) or disable (`false`) all peer-to-peer networking. |
| `getnodeaddresses` | `( count )` | Return known peer addresses from the address manager. |

---

## Control

| Command | Arguments | Description |
|---------|-----------|-------------|
| `getrpcinfo` | — | Return runtime info about the RPC server (active commands, logpath). |
| `help` | `( command subcommand )` | List all RPC commands, or return help for a specific command. |
| `stop` | `( wait )` | Request a graceful shutdown of the daemon. |
| `uptime` | — | Return the number of seconds the daemon has been running. |
| `debug` | — | Toggle debug categories or return current debug state. |
| `getmemoryinfo` | `( mode )` | Return memory usage statistics for the node process. |
| `logging` | `( include exclude )` | Get or set the active logging categories. |

---

## Utility

| Command | Arguments | Description |
|---------|-----------|-------------|
| `validateaddress` | `address` | Return validity and metadata for an Omega address. |
| `createmultisig` | `nrequired keys` | Create a P2SH multisig address and redeem script. |
| `deriveaddresses` | `descriptor ( range )` | Derive addresses from an output descriptor. |
| `getdescriptorinfo` | `descriptor` | Validate a descriptor and return its checksum and expanded form. |
| `verifymessage` | `address signature message` | Verify a signed message produced by `signmessage`. |
| `signmessagewithprivkey` | `privkey message` | Sign a message with a raw private key (WIF). |
| `echo` | `arg0 … arg9` | Echo arguments back; used for testing RPC passthrough. *(hidden)* |
| `setmocktime` | `timestamp` | Override the internal clock (regtest only). *(hidden)* |
| `mockscheduler` | `delta_time` | Advance the scheduler clock by `delta_time` seconds. *(hidden)* |

---

## Raw Transactions & PSBT

| Command | Arguments | Description |
|---------|-----------|-------------|
| `getrawtransaction` | `txid ( verbose blockhash )` | Return the raw hex or decoded form of a transaction. |
| `createrawtransaction` | `inputs outputs ( locktime )` | Create an unsigned raw transaction from inputs and outputs. |
| `decoderawtransaction` | `hexstring` | Decode and display all fields of a serialised transaction. |
| `decodescript` | `hexstring` | Decode a hex-encoded script and return its type and addresses. |
| `sendrawtransaction` | `hexstring ( maxfeerate instantsend bypasslimits )` | Broadcast a serialised signed transaction to the network. |
| `combinerawtransaction` | `txs` | Combine partially signed raw transactions into one. |
| `signrawtransactionwithkey` | `hexstring privkeys ( prevtxs sighashtype )` | Sign a raw transaction using supplied private keys. |
| `testmempoolaccept` | `rawtxs ( maxfeerate )` | Test whether transactions would be accepted to the mempool. |
| `fundrawtransaction` | `hexstring ( options )` | Add inputs to a raw transaction to fund it from the wallet. |
| `createpsbt` | `inputs outputs ( locktime )` | Create an unsigned PSBT. |
| `decodepsbt` | `psbt` | Decode and display all fields of a PSBT. |
| `combinepsbt` | `txs` | Combine multiple partially-signed PSBTs into one. |
| `finalizepsbt` | `psbt ( extract )` | Finalise a PSBT and optionally extract the complete signed transaction. |
| `converttopsbt` | `hexstring ( permitsigdata )` | Convert a raw transaction to PSBT format. |
| `utxoupdatepsbt` | `psbt ( descriptors )` | Update a PSBT with UTXO data from the UTXO set. |
| `joinpsbts` | `txs` | Join multiple PSBTs with different inputs into one. |
| `analyzepsbt` | `psbt` | Analyse a PSBT: per-input signing status, estimated fees. |

---

## ZMQ

| Command | Arguments | Description |
|---------|-----------|-------------|
| `getzmqnotifications` | — | Return the list of active ZMQ notification endpoints. |

---

## Dash / Omega Protocol

| Command | Arguments | Description |
|---------|-----------|-------------|
| `mnsync` | — | Return masternode sync status, or trigger a sync reset. |
| `spork` | `command` | Show all spork values, or activate/deactivate a spork (if authorised). |
| `sporkupdate` | `name value` | Broadcast a spork update signed by the spork key. |
| `mnauth` | `nodeId proTxHash publicKey` | Manually authenticate a masternode connection. *(hidden)* |

---

## Masternode

| Command | Subcommand | Description |
|---------|------------|-------------|
| `masternode` | `count` | Return masternode counts by status (enabled, banned, total). |
| `masternode` | `current` | Return info about the masternode scheduled to receive the next payment. |
| `masternode` | `winner` | Return info about the masternode scheduled for the next block's payment. |
| `masternode` | `winners ( count filter )` | Return the last/next N masternode payment winners. |
| `masternode` | `payments ( blockhash count )` | Return masternode payment details for recent blocks. |
| `masternode` | `list ( mode filter )` | Equivalent to `masternodelist`. |
| `masternode` | `status ( proTxHash )` | Return the status of the local masternode, or any masternode by `proTxHash`. |
| `masternode` | `outputs` | List wallet UTXOs suitable as masternode collateral. |
| `masternode` | `sign message` | Sign a message with the masternode's private key. |
| `masternode` | `verify address sig message` | Verify a message signed by a masternode. |
| `masternodelist` | `( mode filter )` | Return a map of registered masternodes with status, address, and PoSe fields. |

---

## Evo / ProTx

All subcommands of `protx`:

| Subcommand | Description |
|------------|-------------|
| `register` | Create and broadcast a ProRegTx to register a new deterministic masternode. |
| `register_fund` | Fund the collateral and register in one transaction. |
| `register_prepare` | Prepare (but do not broadcast) a ProRegTx; returns hex for external signing. |
| `register_submit` | Broadcast a previously prepared ProRegTx. |
| `register_legacy` | Register using the legacy (non-HD) key scheme. |
| `update_service` | Broadcast a ProUpServTx to update IP/port or operator payout address. |
| `update_service_hpmn` | Broadcast a ProUpServTx for a High Performance masternode with Platform fields. |
| `update_registrar` | Broadcast a ProUpRegTx to update voting key, operator key, or payout address. |
| `update_registrar_legacy` | ProUpRegTx using legacy key scheme. |
| `revoke` | Broadcast a ProUpRevTx to voluntarily revoke a masternode. |
| `diff` | Return the deterministic masternode list difference between two block heights. |
| `info` | Return detailed registration data for a masternode by proTxHash. |
| `list` | List registered masternodes from the DML; filterable by type and state. |

`bls` subcommands:

| Subcommand | Description |
|------------|-------------|
| `bls generate` | Generate a new BLS private/public key pair. |
| `bls fromsecret` | Derive the public key and ID from a BLS secret key. |
| `bls blsctreate` | Create a BLS commitment transaction. |

---

## Quorums

All subcommands of `quorum`:

| Subcommand | Description |
|------------|-------------|
| `list` | List all active LLMQ quorums. |
| `info` | Return detailed info about a specific quorum. |
| `dkgstatus` | Return DKG session status for each LLMQ type. |
| `sign` | Sign a message hash with this node's quorum share. |
| `verify` | Verify a threshold signature against a quorum. |
| `hasrecsig` | Check whether a recovered signature exists for an `(llmqType, id, msgHash)` tuple. |
| `getrecsig` | Return the recovered threshold signature for a given tuple. |
| `isconflicting` | Check whether two signing requests conflict. |
| `memberof` | Return which quorums the local masternode is a member of. |
| `rotationinfo` | Return LLMQ rotation state for the best quorum snapshot. |
| `selectquorum` | Return the quorum selected for a given `llmqType` and `id`. |
| `dkgsessionstatus` | Return the DKG session phase status for all LLMQ types. |

| Command | Arguments | Description |
|---------|-----------|-------------|
| `verifychainlock` | `blockHash signature ( blockHeight )` | Verify a ChainLock signature for a block. |
| `verifyislock` | `id txid signature ( maxHeight )` | Verify an InstantSend lock signature for a transaction. |

---

## Governance

| Command | Arguments | Description |
|---------|-----------|-------------|
| `getgovernanceinfo` | — | Return current governance parameters (superblock cycle, budget, etc.). |
| `getsuperblockbudget` | `index` | Return the total budget available for a given superblock index. |
| `gobject` | `subcommand` | Manage governance objects: `check`, `prepare`, `submit`, `deserialize`, `count`, `get`, `getvotes`, `getcurrentvotes`, `list`, `diff`, `vote-alias`, `vote-conf`, `vote-many`. |
| `voteraw` | `tx_hash tx_index gov_hash signal outcome time sig` | Broadcast a raw pre-signed governance vote. |

---

## CoinJoin

| Command | Arguments | Description |
|---------|-----------|-------------|
| `getpoolinfo` | — | Return the legacy CoinJoin pool status (deprecated; use `getcoinjoininfo`). |
| `getcoinjoininfo` | — | Return detailed CoinJoin mixing status and configuration. |
| `coinjoin` | `command` | Control the CoinJoin mixer: `start`, `stop`, `reset`. |

---

## Wallet

| Command | Arguments | Description |
|---------|-----------|-------------|
| `abandontransaction` | `txid` | Mark an unconfirmed transaction as abandoned (not rebroadcast). |
| `abortrescan` | — | Abort a running wallet rescan. |
| `addmultisigaddress` | `nrequired keys ( label )` | Add a P2SH multisig address to the wallet. |
| `backupwallet` | `destination` | Write a backup copy of the wallet file to `destination`. |
| `createwallet` | `wallet_name ( disable_private_keys blank passphrase avoid_reuse load_on_startup )` | Create and load a new wallet. |
| `dumphdinfo` | — | Return the HD seed, mnemonic, and extended public key for this wallet. |
| `dumpprivkey` | `address` | Return the private key (WIF) for a wallet address. |
| `dumpwallet` | `filename` | Export all wallet keys and metadata to a file. |
| `encryptwallet` | `passphrase` | Encrypt the wallet with a passphrase. |
| `getaddressesbylabel` | `label` | Return all addresses assigned to a label. |
| `getaddressinfo` | `address` | Return detailed metadata for a wallet address (ismine, keys, scripts). |
| `getbalance` | `( dummy minconf addlocked include_watchonly avoid_reuse )` | Return the total available balance. |
| `getbalances` | — | Return the wallet balance broken down by trusted/untrusted/immature. |
| `getnewaddress` | `( label )` | Generate a new receiving address, optionally under a label. |
| `getrawchangeaddress` | — | Return a new address for receiving change (not shown in UI). |
| `getreceivedbyaddress` | `address ( minconf addlocked )` | Return the total received amount at an address. |
| `getreceivedbylabel` | `label ( minconf addlocked )` | Return the total received amount for all addresses under a label. |
| `gettransaction` | `txid ( include_watchonly )` | Return detailed wallet information about a transaction. |
| `getunconfirmedbalance` | — | Return the unconfirmed balance (deprecated; see `getbalances`). |
| `getwalletinfo` | — | Return wallet metadata: name, version, balance, keypool, HD status. |
| `importaddress` | `address ( label rescan p2sh )` | Import an address for watch-only tracking (no spending key). |
| `importelectrumwallet` | `filename ( index )` | Import keys from an Electrum wallet file. |
| `importmulti` | `requests ( options )` | Import multiple keys, scripts, or addresses in one call. |
| `importprivkey` | `privkey ( label rescan )` | Import a private key (WIF) into the wallet. |
| `importprunedfunds` | `rawtransaction txoutproof` | Import funds from a pruned node using a raw transaction and its proof. |
| `importpubkey` | `pubkey ( label rescan )` | Import a public key for watch-only tracking. |
| `importwallet` | `filename` | Import keys from a wallet export file (see `dumpwallet`). |
| `keypoolrefill` | `( newsize )` | Refill the keypool. |
| `listaddressbalances` | `( minamount )` | Return a map of addresses to confirmed balances, optionally filtered by minimum amount. |
| `listaddressgroupings` | — | Return groups of addresses that share common inputs (change detection). |
| `listlabels` | `( purpose )` | Return all wallet labels. |
| `listlockunspent` | — | Return all temporarily locked (unspendable) outputs. |
| `listreceivedbyaddress` | `( minconf addlocked include_empty include_watchonly address_filter )` | List amounts received by each address. |
| `listreceivedbylabel` | `( minconf addlocked include_empty include_watchonly )` | List amounts received by each label. |
| `listsinceblock` | `( blockhash target_confirmations include_watchonly include_removed )` | Return all transactions in blocks since a given block hash. |
| `listtransactions` | `( label count skip include_watchonly )` | Return the most recent transactions affecting the wallet. |
| `listunspent` | `( minconf maxconf addresses include_unsafe query_options )` | Return unspent transaction outputs available to spend. |
| `listwalletdir` | — | List wallets available in the wallet directory. |
| `listwallets` | — | List currently loaded wallets. |
| `loadwallet` | `filename ( load_on_startup )` | Load a wallet from file. |
| `lockunspent` | `unlock ( transactions )` | Lock or unlock specific UTXOs from spending. |
| `removeprunedfunds` | `txid` | Remove a transaction imported via `importprunedfunds`. |
| `rescanblockchain` | `( start_height stop_height )` | Rescan the blockchain for wallet transactions. |
| `sendmany` | `dummy amounts ( minconf addlocked comment subtractfeefrom use_is use_cj conf_target estimate_mode )` | Send to multiple addresses in one transaction. |
| `sendtoaddress` | `address amount ( comment comment_to subtractfeefromamount use_is use_cj conf_target estimate_mode avoid_reuse )` | Send OMEGA to an address. |
| `setcoinjoinrounds` | `rounds` | Set the number of CoinJoin mixing rounds. |
| `setcoinjoinamount` | `amount` | Set the target CoinJoin mixing amount. |
| `setlabel` | `address label` | Assign a label to an address. |
| `settxfee` | `amount` | Set the transaction fee per kB. |
| `setwalletflag` | `flag ( value )` | Set or clear a wallet flag (`avoid_reuse`, etc.). |
| `signmessage` | `address message` | Sign a message with the private key of a wallet address. |
| `signrawtransactionwithwallet` | `hexstring ( prevtxs sighashtype )` | Sign a raw transaction using wallet keys. |
| `unloadwallet` | `( wallet_name load_on_startup )` | Unload a wallet. |
| `upgradewallet` | `( version )` | Upgrade the wallet database to the latest version. |
| `upgradetohd` | `( mnemonic mnemonicpassphrase walletpassphrase rescan )` | Upgrade a non-HD wallet to HD using an optional BIP39 mnemonic. |
| `walletlock` | — | Lock the wallet (clear decryption keys from memory). |
| `walletpassphrasechange` | `oldpassphrase newpassphrase` | Change the wallet encryption passphrase. |
| `walletpassphrase` | `passphrase timeout ( mixingonly )` | Unlock the wallet for `timeout` seconds. `mixingonly=true` restricts to CoinJoin. |
| `walletprocesspsbt` | `psbt ( sign sighashtype bip32derivs )` | Update a PSBT with wallet key data and optionally sign it. |
| `walletcreatefundedpsbt` | `inputs outputs ( locktime options bip32derivs )` | Create and fund a PSBT from the wallet. |
| `wipewallettxes` | `( keep_confirmed )` | Remove unconfirmed wallet transactions from the wallet DB. |
| `fundrawtransaction` | `hexstring ( options )` | Add wallet inputs to a raw transaction to cover its outputs. |
| `instantsendtoaddress` | *(same as sendtoaddress)* | Send via InstantSend (hidden alias for `sendtoaddress` with `use_is=true`). *(hidden)* |

---

## SMSG — Node Control

| Command | Arguments | Description |
|---------|-----------|-------------|
| `smsgenable` | `( walletname )` | Enable the SMSG module, optionally binding to a named wallet. |
| `smsgdisable` | — | Disable the SMSG module. |
| `smsgoptions` | `( list\|set optname value )` | View or change SMSG runtime options. |
| `smsggetinfo` | — | Return current SMSG module status and statistics. |
| `smsgpeers` | `( node_id )` | List SMSG-capable peers, or inspect a single peer by node ID. |
| `smsgsetwallet` | `walletname` | Bind the SMSG module to a different loaded wallet. |
| `smsgdebug` | `( command arg1 )` | Internal debug commands for SMSG development. |

## SMSG — Key & Address Management

| Command | Arguments | Description |
|---------|-----------|-------------|
| `smsglocalkeys` | `( whitelist\|all\|wallet\|recv +/- address\|anon +/- address )` | List or modify the set of local addresses monitored by SMSG. |
| `smsgaddaddress` | `address pubkey` | Register an external public key so messages can be sent to that address. |
| `smsgaddlocaladdress` | `address` | Add a local wallet address to the SMSG watch list. |
| `smsgremoveaddress` | `address` | Remove a watched address from the SMSG key store. |
| `smsgimportprivkey` | `privkey ( label )` | Import a raw private key into the SMSG key store. |
| `smsgremoveprivkey` | `address` | Remove a private key from the SMSG key store by its address. |
| `smsgdumpprivkey` | `address` | Export the SMSG private key for an address (WIF). |
| `smsggetpubkey` | `address` | Return the SMSG public key associated with an address. |
| `smsgaddresses` | — | List all addresses currently held in the SMSG key store. |

## SMSG — Sending & Receiving

| Command | Arguments | Description |
|---------|-----------|-------------|
| `smsgsend` | `address_from address_to message ( paid_msg days_retention testfee fromfile decodehex topic parent_msgid retention_days )` | Send an encrypted SMSG message. Supports paid messages, topic channels, and parent message references. |
| `smsgsendanon` | `address_to message` | Send an anonymous (no sender address) encrypted message. |
| `smsginbox` | `( mode filter )` | List messages in the inbox. `mode`: `count`/`clear`/`all`/`unread`. |
| `smsgoutbox` | `( mode filter )` | List messages in the outbox. |
| `smsgview` | `( address/label \| asc/desc \| -from yyyy-mm-dd \| -to yyyy-mm-dd )` | View messages for an address with optional sort and date filters. |
| `smsg` | `msgid ( options )` | Retrieve a single message by its message ID. |
| `smsgpurge` | `msgid` | Delete a message from the local store by ID. |
| `smsgfund` | `msgid ( testfee )` | Fund an existing paid message by broadcasting the fee transaction. |
| `smsgimport` | `hex` | Import a raw hex-encoded SMSG message into the local store. |

## SMSG — Network & Storage

| Command | Arguments | Description |
|---------|-----------|-------------|
| `smsgscanchain` | — | Scan the blockchain for SMSG public keys. Resumes from last saved height. |
| `smsgscanbuckets` | — | Re-process all stored SMSG buckets (decrypt any newly importable messages). |
| `smsgbuckets` | `( stats\|dump )` | Show or dump the P2P message bucket state. |
| `smsgclearbuckets` | `confirm` | Delete all P2P message buckets from local storage. |
| `smsgzmqpush` | `( timefrom timeto )` | Re-publish stored messages over ZMQ within an optional time range. |
| `smsggetfeerate` | — | Return the current SMSG paid-message fee rate (OMEGA/kB). |
| `smsggetdifficulty` | — | Return the current SMSG proof-of-work difficulty. |

## SMSG — Trollbox

| Command | Arguments | Description |
|---------|-----------|-------------|
| `trollboxsend` | `message ( paid )` | Broadcast a message to the global Trollbox channel. Pass `true` for a paid (priority) message. |
| `trollboxlist` | `( count )` | List the most recent Trollbox messages (default 20). |

## SMSG — Topic Channels

| Command | Arguments | Description |
|---------|-----------|-------------|
| `smsgsubscribe` | `topic` | Subscribe to a topic channel (e.g. `omega.listings.uk`). Imports the shared key. |
| `smsgunsubscribe` | `topic` | Unsubscribe from a topic channel and remove its shared key. |
| `smsglisttopics` | — | List all currently subscribed topic channels. |
| `smsggetmessages` | `topic ( count )` | Retrieve the newest messages for a subscribed topic (default 20). |

## SMSG — Messaging Rooms

| Command | Arguments | Description |
|---------|-----------|-------------|
| `smsgcreateroom` | `name ( flags retention_days )` | Create a new on-chain messaging room. `flags`: 1=public, 2=moderated. |
| `smsglistrooms` | `( flags_filter )` | List known messaging rooms, optionally filtered by flags bitmask. |
| `smsggetroominfo` | `room_txid` | Return full metadata for a room identified by its funding txid. |
| `smsgjoinroom` | `room_txid ( privkey_wif )` | Join an existing room, optionally supplying an invite private key. |

## SMSG — Message Anchoring

Batch-commits message SHA-256 hashes to the blockchain via `OP_RETURN` (max 127 per batch, Merkle root on-chain).

| Command | Arguments | Description |
|---------|-----------|-------------|
| `anchormsg` | `msghash ( prevhash )` | Queue a SHA-256 message hash for batch anchoring. Optional `prevhash` links to a previous revision. Returns `queued`, `status`, `pending` count. |
| `anchorcommit` | — | Commit all queued hashes to the blockchain. Requires unlocked, funded wallet. Returns `txid`, `root`, `count`. |
| `verifymsg` | `msghash` | Check whether a message hash has been confirmed on-chain. Returns `anchored`, `pending`, `txid`, `root`. |
| `getmsgproof` | `msghash` | Return the Merkle inclusion proof for an anchored hash. Returns `hash`, `anchored`, `txid`, `root`, `index`, `branch[]`. |
