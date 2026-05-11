// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_HEADERSSYNC_H
#define BITCOIN_HEADERSSYNC_H

#include <arith_uint256.h>
#include <consensus/params.h>
#include <primitives/block.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <vector>

class CBlockIndex;

/** Maximum simultaneous peers running presync during initial block download. */
static constexpr int MAX_PRESYNC_PEERS = 3;

/**
 * Interval (in headers) between stored hash commitments during presync.
 * One 32-byte commitment per 512 headers → ~200 KB for a 3.2M-block chain.
 */
static constexpr int HEADER_PRESYNC_COMMITMENT_PERIOD = 512;

/**
 * Per-peer state machine for IBD header presync.
 *
 * PRESYNC: Validate proof-of-work and accumulate chainwork for each incoming
 *          header without inserting anything into the block index.  A hash
 *          commitment is stored every HEADER_PRESYNC_COMMITMENT_PERIOD blocks.
 *          Terminates when:
 *            (a) peer demonstrates chainwork >= minimum_required_work → REDOWNLOAD
 *            (b) peer runs out of headers before reaching minimum work → failure
 *
 * REDOWNLOAD: Re-request the same header chain from the peer, verify each
 *             header against stored commitments, then submit validated batches
 *             to ProcessNewBlockHeaders.  Commitments are freed as they are
 *             consumed.  Completes when the peer has no more headers → FINAL.
 *
 * FINAL: All headers have been accepted into the block index.  The object
 *        should be destroyed by the caller immediately.
 */
class HeadersSyncState
{
public:
    enum class Phase { PRESYNC, REDOWNLOAD, FINAL };

    /** Result returned from ProcessNextHeaders(). */
    struct Result {
        bool success{true};
        /**
         * When success == false, misbehaving == true means the peer sent
         * provably invalid data (bad PoW or commitment mismatch).  The caller
         * should call Misbehaving(100).  When misbehaving == false the peer
         * may simply have an insufficient-work chain; disconnect only.
         */
        bool misbehaving{false};
        /** Send GETHEADERS using next_locator_hash as the tip locator. */
        bool request_more{false};
        uint256 next_locator_hash;
        /** Non-empty only during REDOWNLOAD; submit these to ProcessNewBlockHeaders. */
        std::vector<CBlockHeader> validated;
    };

    /**
     * @param id               Peer node-id (int64_t in this codebase).
     * @param params           Consensus parameters.
     * @param chain_start      Block index entry whose GetBlockHash() is the
     *                         prev-hash of the first expected incoming header.
     *                         Equals pindexStart used in the GETHEADERS request.
     * @param min_work         Minimum nChainWork the peer must demonstrate.
     */
    HeadersSyncState(int64_t id,
                     const Consensus::Params& params,
                     const CBlockIndex* chain_start,
                     const arith_uint256& min_work);

    Phase          GetPhase()      const { return m_phase; }
    int64_t        GetId()         const { return m_id; }
    const uint256& GetStartHash()  const { return m_presync_start_hash; }
    int            GetPresyncHeight() const { return m_presync_height; }

    /** Process an incoming batch of headers from the peer. */
    Result ProcessNextHeaders(const std::vector<CBlockHeader>& headers,
                              bool full_message);

private:
    /** Validate one header's PoW and append its proof to running_work. */
    bool PresyncPoW(const CBlockHeader& hdr,
                    const uint256& expected_prev,
                    arith_uint256& running_work) const;

    int64_t                  m_id;
    const Consensus::Params& m_params;
    Phase                    m_phase{Phase::PRESYNC};
    arith_uint256            m_min_work;
    const CBlockIndex*       m_chain_start{nullptr};

    // --- Presync state ---
    uint256       m_presync_start_hash;   // GetBlockHash() of chain_start
    arith_uint256 m_presync_chainwork;    // accumulated work including chain_start
    int           m_presync_height{0};    // height of last presync header
    uint256       m_presync_tip_hash;     // hash of last presync header

    struct Commitment {
        int     height;
        uint256 hash;
    };
    std::vector<Commitment> m_commitments;

    // --- Redownload state ---
    int           m_redownload_height{0};
    std::size_t   m_redownload_commitment_idx{0};
    uint256       m_redownload_prev_hash;
    arith_uint256 m_redownload_chainwork;

    /** Upper bound on the number of commitments we will store for this peer.
     *  Derived from elapsed time since chain_start; prevents memory DoS when
     *  an attacker serves an implausibly long low-work chain. */
    uint64_t m_max_commitments{0};
};

#endif // BITCOIN_HEADERSSYNC_H
