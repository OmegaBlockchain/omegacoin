// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXORPHANAGE_H
#define BITCOIN_TXORPHANAGE_H

#include <net.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <sync.h>

#include <map>
#include <set>

/** Tracks orphan transactions (failed on TX_MISSING_INPUTS).
 *  Because orphans are indistinguishable from bad transactions with
 *  non-existent inputs, entries are heavily limited in count and duration.
 */
class TxOrphanage {
public:
    /** Add a new orphan transaction */
    bool AddTx(const CTransactionRef& tx, NodeId peer) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Check if we already have an orphan transaction */
    bool HaveTx(const uint256& txid) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Extract a transaction from a peer's work set.
     *  Returns nullptr (more=false) if nothing to process.
     *  Otherwise returns the transaction, removes it from the work set,
     *  and sets originator and more. */
    CTransactionRef GetTxToReconsider(NodeId peer, NodeId& originator, bool& more) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Queue orphans whose parents appear in block as candidates for reconsideration */
    void SetCandidatesByBlock(const CBlock& block) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Erase an orphan by txid */
    int EraseTx(const uint256& txid) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Erase all orphans announced by a peer (e.g. after disconnection) */
    void EraseForPeer(NodeId peer) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Erase all orphans included in or invalidated by a new block */
    void EraseForBlock(const CBlock& block) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Return true if a peer has more orphans in its work set */
    bool HaveMoreWork(NodeId peer) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Enforce size limit, evicting random or expired entries as needed */
    void LimitOrphans(unsigned int max_orphans_size) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Add any orphans that list tx as a parent into a peer's work set */
    void AddChildrenToWorkSet(const CTransaction& tx, NodeId peer) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Return number of entries in the orphanage */
    size_t Size() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        return m_orphans.size();
    }

protected:
    mutable Mutex m_mutex;

    struct OrphanTx {
        CTransactionRef tx;
        NodeId fromPeer;
        int64_t nTimeExpire;
        size_t list_pos;
        size_t nTxSize;
    };

    std::map<uint256, OrphanTx> m_orphans GUARDED_BY(m_mutex);

    std::map<NodeId, std::set<uint256>> m_peer_work_set GUARDED_BY(m_mutex);

    using OrphanMap = decltype(m_orphans);

    struct IteratorComparator
    {
        template<typename I>
        bool operator()(const I& a, const I& b) const
        {
            return &(*a) < &(*b);
        }
    };

    std::map<COutPoint, std::set<OrphanMap::iterator, IteratorComparator>> m_outpoint_to_orphan_it GUARDED_BY(m_mutex);

    std::vector<OrphanMap::iterator> m_orphan_list GUARDED_BY(m_mutex);

    size_t m_orphan_tx_size GUARDED_BY(m_mutex){0};

    int _EraseTx(const uint256& txid) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
};

#endif // BITCOIN_TXORPHANAGE_H
