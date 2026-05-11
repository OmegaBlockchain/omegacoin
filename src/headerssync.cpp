// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <headerssync.h>

#include <chain.h>
#include <logging.h>
#include <pow.h>
#include <util/time.h>

HeadersSyncState::HeadersSyncState(int64_t id,
                                   const Consensus::Params& params,
                                   const CBlockIndex* chain_start,
                                   const arith_uint256& min_work)
    : m_id(id)
    , m_params(params)
    , m_min_work(min_work)
    , m_chain_start(chain_start)
{
    assert(chain_start != nullptr);
    m_presync_start_hash = chain_start->GetBlockHash();
    m_presync_tip_hash   = m_presync_start_hash;
    m_presync_chainwork  = chain_start->nChainWork;
    m_presync_height     = chain_start->nHeight;

    // Bound the number of commitments we will store to prevent memory DoS.
    // Uses the expected block interval (nPowTargetSpacing) rather than the MTP
    // fastest-rate (6 blocks/s) because Omega uses LWMA, not fixed-window PoW.
    const int64_t elapsed = GetTime() -
                            static_cast<int64_t>(chain_start->GetMedianTimePast());
    const int64_t max_headers =
        (std::max<int64_t>(0, elapsed) + MAX_FUTURE_BLOCK_TIME + 7200) /
        params.nPowTargetSpacing + 1;
    m_max_commitments = static_cast<uint64_t>(max_headers) /
                        HEADER_PRESYNC_COMMITMENT_PERIOD + 1;
    LogPrint(BCLog::NET, "presync peer=%d: max_commitments=%u\n",
             m_id, m_max_commitments);
}

bool HeadersSyncState::PresyncPoW(const CBlockHeader& hdr,
                                  const uint256& expected_prev,
                                  arith_uint256& running_work) const
{
    if (hdr.hashPrevBlock != expected_prev) return false;
    if (!CheckProofOfWork(hdr.GetHash(), hdr.nBits, m_params)) return false;

    arith_uint256 bnTarget;
    bool fNeg, fOvf;
    bnTarget.SetCompact(hdr.nBits, &fNeg, &fOvf);
    if (fNeg || fOvf || bnTarget == 0) return false;
    running_work += (~bnTarget / (bnTarget + 1)) + 1;
    return true;
}

HeadersSyncState::Result HeadersSyncState::ProcessNextHeaders(
    const std::vector<CBlockHeader>& headers,
    bool full_message)
{
    Result result;
    if (headers.empty()) {
        result.success = false;
        return result;
    }

    if (m_phase == Phase::PRESYNC) {
        arith_uint256 work   = m_presync_chainwork;
        uint256       prev   = m_presync_tip_hash;
        int           height = m_presync_height;

        for (const CBlockHeader& hdr : headers) {
            if (!PresyncPoW(hdr, prev, work)) {
                LogPrint(BCLog::NET, "presync peer=%d invalid header at height %d\n",
                         m_id, height + 1);
                result.success     = false;
                result.misbehaving = true;
                return result;
            }
            ++height;
            prev = hdr.GetHash();

            if (height % HEADER_PRESYNC_COMMITMENT_PERIOD == 0) {
                m_commitments.push_back({height, prev});
                if (m_max_commitments > 0 &&
                    m_commitments.size() > m_max_commitments) {
                    LogPrint(BCLog::NET,
                             "presync peer=%d exceeded max commitments "
                             "(%u) at height %d\n",
                             m_id, m_max_commitments, height);
                    result.success = false;
                    return result;
                }
            }
        }

        m_presync_chainwork = work;
        m_presync_tip_hash  = prev;
        m_presync_height    = height;

        // Transition to REDOWNLOAD as soon as minimum work is demonstrated –
        // do not wait for the peer to run out of headers.  Waiting until a
        // short message arrives means a peer with a long high-work chain never
        // transitions to REDOWNLOAD (Fix 3 / finding #3).
        if (m_presync_chainwork >= m_min_work) {
            m_phase                     = Phase::REDOWNLOAD;
            m_redownload_height         = m_chain_start->nHeight;
            m_redownload_commitment_idx = 0;
            m_redownload_prev_hash      = m_presync_start_hash;
            m_redownload_chainwork      = m_chain_start->nChainWork;

            result.request_more      = true;
            result.next_locator_hash = m_presync_start_hash;
            LogPrint(BCLog::NET,
                     "presync peer=%d min_work reached at height %d "
                     "(%zu commitments), starting redownload\n",
                     m_id, m_presync_height, m_commitments.size());
            return result;
        }

        if (!full_message) {
            // Peer has no more headers and never demonstrated sufficient work.
            LogPrint(BCLog::NET, "presync peer=%d insufficient work at height %d\n",
                     m_id, m_presync_height);
            result.success = false;
            return result;
        }

        // Peer has more headers; request next batch.
        result.request_more      = true;
        result.next_locator_hash = m_presync_tip_hash;
        return result;
    }

    // REDOWNLOAD phase: validate against stored commitments then pass headers
    // on to the caller for submission to ProcessNewBlockHeaders.
    assert(m_phase == Phase::REDOWNLOAD);

    uint256       prev       = m_redownload_prev_hash;
    int           height     = m_redownload_height;
    arith_uint256 work       = m_redownload_chainwork;
    std::size_t   commit_idx = m_redownload_commitment_idx;

    for (const CBlockHeader& hdr : headers) {
        if (hdr.hashPrevBlock != prev) {
            LogPrint(BCLog::NET,
                     "redownload peer=%d non-continuous header at height %d\n",
                     m_id, height + 1);
            result.success     = false;
            result.misbehaving = true;
            return result;
        }
        if (!CheckProofOfWork(hdr.GetHash(), hdr.nBits, m_params)) {
            LogPrint(BCLog::NET, "redownload peer=%d PoW failure at height %d\n",
                     m_id, height + 1);
            result.success     = false;
            result.misbehaving = true;
            return result;
        }

        ++height;
        prev = hdr.GetHash();

        // Verify against stored commitment if one falls on this height.
        if (commit_idx < m_commitments.size() &&
            m_commitments[commit_idx].height == height) {
            if (m_commitments[commit_idx].hash != prev) {
                LogPrint(BCLog::NET,
                         "redownload peer=%d commitment mismatch at height %d\n",
                         m_id, height);
                result.success     = false;
                result.misbehaving = true;
                return result;
            }
            ++commit_idx;
        }

        arith_uint256 bnTarget;
        bool fNeg, fOvf;
        bnTarget.SetCompact(hdr.nBits, &fNeg, &fOvf);
        if (!fNeg && !fOvf && bnTarget != 0)
            work += (~bnTarget / (bnTarget + 1)) + 1;

        result.validated.push_back(hdr);
    }

    m_redownload_prev_hash      = prev;
    m_redownload_height         = height;
    m_redownload_chainwork      = work;
    m_redownload_commitment_idx = commit_idx;

    if (!full_message) {
        // Peer has no more headers; redownload complete.
        m_commitments.clear();
        m_commitments.shrink_to_fit();
        m_phase = Phase::FINAL;
        return result;
    }

    // Request next redownload batch.
    result.request_more      = true;
    result.next_locator_hash = prev;
    return result;
}
