// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_PHANTOM_EXCLUSIONS_H
#define BITCOIN_EVO_PHANTOM_EXCLUSIONS_H

#include <uint256.h>

#include <set>

//
// Phantom masternode exclusion set. See phantom_exclusions.cpp for the
// hard-coded mainnet list and the full rationale, and
// PHANTOM_MN_REMEDIATION_PLAN.md for the remediation plan.
//
// Consensus rules built on this set (all gated at Consensus::nPhantomPurgeHeight):
//  - BuildNewListFromBlock bans every listed proTxHash exactly once at the
//    purge height (BanIfNotBanned).
//  - CheckProUpServTx rejects any ProUpServTx for a listed proTxHash
//    (bad-protx-phantom-banned), so a listed MN can never be revived.
//  - BuildNewListFromBlock additionally refuses to revive a listed proTxHash
//    (defence in depth should a ProUpServTx ever bypass CheckProUpServTx).
//

/** Returns the active exclusion set: the hard-coded mainnet list, or the
 *  regtest-injected set once SetPhantomExclusionsForTesting has been called. */
const std::set<uint256>& GetPhantomExclusions();

/** Returns true if proTxHash is in the active exclusion set. */
bool IsPhantomExcluded(const uint256& proTxHash);

/** Replaces the active exclusion set. Regtest only (-phantomexclude); must be
 *  called during chainparams construction, before validation threads start. */
void SetPhantomExclusionsForTesting(std::set<uint256> proTxHashes);

#endif // BITCOIN_EVO_PHANTOM_EXCLUSIONS_H
