// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <validation.h>

#include <chain.h>
#include <chainparams.h>
#include <clientversion.h>
#include <flatfile.h>
#include <hash.h>
#include <pow.h>
#include <primitives/block.h>
#include <shutdown.h>
#include <streams.h>
#include <tinyformat.h>
#include <undo.h>
#include <util/system.h>
#include <util/translation.h>

#include <set>

static FlatFileSeq BlockFileSeq()
{
    return FlatFileSeq(GetBlocksDir(), "blk", BLOCKFILE_CHUNK_SIZE);
}

static FlatFileSeq UndoFileSeq()
{
    return FlatFileSeq(GetBlocksDir(), "rev", UNDOFILE_CHUNK_SIZE);
}

FILE* OpenBlockFile(const FlatFilePos& pos, bool fReadOnly)
{
    return BlockFileSeq().Open(pos, fReadOnly);
}

static FILE* OpenUndoFile(const FlatFilePos& pos, bool fReadOnly)
{
    return UndoFileSeq().Open(pos, fReadOnly);
}

fs::path GetBlockPosFilename(const FlatFilePos& pos)
{
    return BlockFileSeq().FileName(pos);
}

static bool WriteBlockToDisk(const CBlock& block, FlatFilePos& pos, const CMessageHeader::MessageStartChars& messageStart)
{
    CAutoFile fileout(OpenBlockFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("WriteBlockToDisk: OpenBlockFile failed");

    unsigned int nSize = GetSerializeSize(block, fileout.GetVersion());
    fileout << messageStart << nSize;

    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0)
        return error("WriteBlockToDisk: ftell failed");
    pos.nPos = (unsigned int)fileOutPos;
    fileout << block;

    return true;
}

bool ReadBlockFromDisk(CBlock& block, const FlatFilePos& pos, const Consensus::Params& consensusParams)
{
    block.SetNull();

    CAutoFile filein(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("ReadBlockFromDisk: OpenBlockFile failed for %s", pos.ToString());

    try {
        filein >> block;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s at %s", __func__, e.what(), pos.ToString());
    }

    if (!CheckProofOfWork(block.GetHash(), block.nBits, consensusParams))
        return error("ReadBlockFromDisk: Errors in block header at %s", pos.ToString());

    return true;
}

bool ReadBlockFromDisk(CBlock& block, const CBlockIndex* pindex, const Consensus::Params& consensusParams)
{
    FlatFilePos blockPos;
    {
        LOCK(cs_main);
        blockPos = pindex->GetBlockPos();
    }

    if (!ReadBlockFromDisk(block, blockPos, consensusParams))
        return false;

    if (block.GetHash() != pindex->GetBlockHash())
        return error("ReadBlockFromDisk(CBlock&, CBlockIndex*): GetHash() doesn't match index for %s at %s",
                     pindex->ToString(), pindex->GetBlockPos().ToString());
    return true;
}

static bool UndoWriteToDisk(const CBlockUndo& blockundo, FlatFilePos& pos, const uint256& hashBlock, const CMessageHeader::MessageStartChars& messageStart)
{
    CAutoFile fileout(OpenUndoFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s: OpenUndoFile failed", __func__);

    unsigned int nSize = GetSerializeSize(blockundo, fileout.GetVersion());
    fileout << messageStart << nSize;

    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0)
        return error("%s: ftell failed", __func__);
    pos.nPos = (unsigned int)fileOutPos;
    fileout << blockundo;

    CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
    hasher << hashBlock;
    hasher << blockundo;
    fileout << hasher.GetHash();

    return true;
}

bool UndoReadFromDisk(CBlockUndo& blockundo, const CBlockIndex* pindex)
{
    FlatFilePos pos = pindex->GetUndoPos();
    if (pos.IsNull()) {
        return error("%s: no undo data available", __func__);
    }

    CAutoFile filein(OpenUndoFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("%s: OpenUndoFile failed", __func__);

    uint256 hashChecksum;
    CHashVerifier<CAutoFile> verifier(&filein);
    try {
        verifier << pindex->pprev->GetBlockHash();
        verifier >> blockundo;
        filein >> hashChecksum;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }

    if (hashChecksum != verifier.GetHash())
        return error("%s: Checksum mismatch", __func__);

    return true;
}

void UnlinkPrunedFiles(const std::set<int>& setFilesToPrune)
{
    for (std::set<int>::iterator it = setFilesToPrune.begin(); it != setFilesToPrune.end(); ++it) {
        FlatFilePos pos(*it, 0);
        fs::remove(BlockFileSeq().FileName(pos));
        fs::remove(UndoFileSeq().FileName(pos));
        LogPrintf("Prune: %s deleted blk/rev (%05u)\n", __func__, *it);
    }
}

// -----------------------------------------------------------------------
// BlockManager storage methods
// -----------------------------------------------------------------------

void BlockManager::FlushUndoFile(int block_file, bool finalize)
{
    FlatFilePos undo_pos_old(block_file, m_blockfile_info[block_file].nUndoSize);
    if (!UndoFileSeq().Flush(undo_pos_old, finalize)) {
        AbortNode("Flushing undo file to disk failed. This is likely the result of an I/O error.");
    }
}

void BlockManager::FlushBlockFile(bool fFinalize, bool finalize_undo)
{
    LOCK(m_cs_last_blockfile);
    FlatFilePos block_pos_old(m_last_blockfile, m_blockfile_info[m_last_blockfile].nSize);
    if (!BlockFileSeq().Flush(block_pos_old, fFinalize)) {
        AbortNode("Flushing block file to disk failed. This is likely the result of an I/O error.");
    }
    if (!fFinalize || finalize_undo) FlushUndoFile(m_last_blockfile, finalize_undo);
}

bool BlockManager::FindBlockPos(FlatFilePos& pos, unsigned int nAddSize, unsigned int nHeight, uint64_t nTime, bool fKnown)
{
    LOCK(m_cs_last_blockfile);

    unsigned int nFile = fKnown ? pos.nFile : m_last_blockfile;
    if (m_blockfile_info.size() <= nFile) {
        m_blockfile_info.resize(nFile + 1);
    }

    bool finalize_undo = false;
    if (!fKnown) {
        while (m_blockfile_info[nFile].nSize + nAddSize >= MAX_BLOCKFILE_SIZE) {
            finalize_undo = (m_blockfile_info[nFile].nHeightLast == (unsigned int)::ChainActive().Tip()->nHeight);
            nFile++;
            if (m_blockfile_info.size() <= nFile) {
                m_blockfile_info.resize(nFile + 1);
            }
        }
        pos.nFile = nFile;
        pos.nPos = m_blockfile_info[nFile].nSize;
    }

    if ((int)nFile != m_last_blockfile) {
        if (!fKnown) {
            LogPrintf("Leaving block file %i: %s\n", m_last_blockfile, m_blockfile_info[m_last_blockfile].ToString());
        }
        FlushBlockFile(!fKnown, finalize_undo);
        m_last_blockfile = nFile;
    }

    m_blockfile_info[nFile].AddBlock(nHeight, nTime);
    if (fKnown)
        m_blockfile_info[nFile].nSize = std::max(pos.nPos + nAddSize, m_blockfile_info[nFile].nSize);
    else
        m_blockfile_info[nFile].nSize += nAddSize;

    if (!fKnown) {
        bool out_of_space;
        size_t bytes_allocated = BlockFileSeq().Allocate(pos, nAddSize, out_of_space);
        if (out_of_space) {
            return AbortNode("Disk space is too low!", _("Disk space is too low!"));
        }
        if (bytes_allocated != 0 && fPruneMode) {
            m_check_for_pruning = true;
        }
    }

    m_dirty_fileinfo.insert(nFile);
    return true;
}

bool BlockManager::FindUndoPos(CValidationState& state, int nFile, FlatFilePos& pos, unsigned int nAddSize)
{
    pos.nFile = nFile;

    LOCK(m_cs_last_blockfile);

    pos.nPos = m_blockfile_info[nFile].nUndoSize;
    m_blockfile_info[nFile].nUndoSize += nAddSize;
    m_dirty_fileinfo.insert(nFile);

    bool out_of_space;
    size_t bytes_allocated = UndoFileSeq().Allocate(pos, nAddSize, out_of_space);
    if (out_of_space) {
        return AbortNode(state, "Disk space is too low!", _("Disk space is too low!"));
    }
    if (bytes_allocated != 0 && fPruneMode) {
        m_check_for_pruning = true;
    }

    return true;
}

bool BlockManager::WriteUndoDataForBlock(const CBlockUndo& blockundo, CValidationState& state, CBlockIndex* pindex, const CChainParams& chainparams)
{
    if (pindex->GetUndoPos().IsNull()) {
        FlatFilePos _pos;
        if (!FindUndoPos(state, pindex->nFile, _pos, ::GetSerializeSize(blockundo, CLIENT_VERSION) + 40))
            return error("ConnectBlock(): FindUndoPos failed");
        if (!UndoWriteToDisk(blockundo, _pos, pindex->pprev->GetBlockHash(), chainparams.MessageStart()))
            return AbortNode(state, "Failed to write undo data");
        if (_pos.nFile < m_last_blockfile && static_cast<uint32_t>(pindex->nHeight) == m_blockfile_info[_pos.nFile].nHeightLast) {
            FlushUndoFile(_pos.nFile, true);
        }

        pindex->nUndoPos = _pos.nPos;
        pindex->nStatus |= BLOCK_HAVE_UNDO;
        m_dirty_blockindex.insert(pindex);
    }

    return true;
}

FlatFilePos BlockManager::SaveBlockToDisk(const CBlock& block, int nHeight, const CChainParams& chainparams, const FlatFilePos* dbp)
{
    unsigned int nBlockSize = ::GetSerializeSize(block, CLIENT_VERSION);
    FlatFilePos blockPos;
    if (dbp != nullptr)
        blockPos = *dbp;
    if (!FindBlockPos(blockPos, nBlockSize + 8, nHeight, block.GetBlockTime(), dbp != nullptr)) {
        error("%s: FindBlockPos failed", __func__);
        return FlatFilePos();
    }
    if (dbp == nullptr) {
        if (!WriteBlockToDisk(block, blockPos, chainparams.MessageStart())) {
            AbortNode("Failed to write block");
            return FlatFilePos();
        }
    }
    return blockPos;
}

uint64_t BlockManager::CalculateCurrentUsage()
{
    LOCK(m_cs_last_blockfile);

    uint64_t retval = 0;
    for (const CBlockFileInfo& file : m_blockfile_info) {
        retval += file.nSize + file.nUndoSize;
    }
    return retval;
}

void BlockManager::PruneOneBlockFile(const int fileNumber)
{
    AssertLockHeld(cs_main);
    LOCK(m_cs_last_blockfile);

    for (const auto& entry : m_block_index) {
        CBlockIndex* pindex = entry.second;
        if (pindex->nFile == fileNumber) {
            pindex->nStatus &= ~BLOCK_HAVE_DATA;
            pindex->nStatus &= ~BLOCK_HAVE_UNDO;
            pindex->nFile = 0;
            pindex->nDataPos = 0;
            pindex->nUndoPos = 0;
            m_dirty_blockindex.insert(pindex);

            auto range = m_blocks_unlinked.equal_range(pindex->pprev);
            while (range.first != range.second) {
                std::multimap<CBlockIndex*, CBlockIndex*>::iterator _it = range.first;
                range.first++;
                if (_it->second == pindex) {
                    m_blocks_unlinked.erase(_it);
                }
            }
        }
    }

    m_blockfile_info[fileNumber].SetNull();
    m_dirty_fileinfo.insert(fileNumber);
}

CBlockFileInfo* BlockManager::GetBlockFileInfo(size_t n)
{
    LOCK(m_cs_last_blockfile);
    return &m_blockfile_info.at(n);
}

void BlockManager::FindFilesToPruneManual(std::set<int>& setFilesToPrune, int nManualPruneHeight)
{
    assert(fPruneMode && nManualPruneHeight > 0);

    LOCK2(cs_main, m_cs_last_blockfile);
    if (::ChainActive().Tip() == nullptr)
        return;

    unsigned int nLastBlockWeCanPrune = std::min((unsigned)nManualPruneHeight, ::ChainActive().Tip()->nHeight - MIN_BLOCKS_TO_KEEP);
    int count = 0;
    for (int fileNumber = 0; fileNumber < m_last_blockfile; fileNumber++) {
        if (m_blockfile_info[fileNumber].nSize == 0 || m_blockfile_info[fileNumber].nHeightLast > nLastBlockWeCanPrune)
            continue;
        PruneOneBlockFile(fileNumber);
        setFilesToPrune.insert(fileNumber);
        count++;
    }
    LogPrintf("Prune (Manual): prune_height=%d removed %d blk/rev pairs\n", nLastBlockWeCanPrune, count);
}

void BlockManager::FindFilesToPrune(std::set<int>& setFilesToPrune, uint64_t nPruneAfterHeight)
{
    LOCK2(cs_main, m_cs_last_blockfile);
    if (::ChainActive().Tip() == nullptr || nPruneTarget == 0) {
        return;
    }
    if ((uint64_t)::ChainActive().Tip()->nHeight <= nPruneAfterHeight) {
        return;
    }

    unsigned int nLastBlockWeCanPrune = ::ChainActive().Tip()->nHeight - MIN_BLOCKS_TO_KEEP;
    uint64_t nCurrentUsage = CalculateCurrentUsage();
    uint64_t nBuffer = BLOCKFILE_CHUNK_SIZE + UNDOFILE_CHUNK_SIZE;
    uint64_t nBytesToPrune;
    int count = 0;

    if (nCurrentUsage + nBuffer >= nPruneTarget) {
        if (::ChainstateActive().IsInitialBlockDownload()) {
            nBuffer += nPruneTarget / 10;
        }

        for (int fileNumber = 0; fileNumber < m_last_blockfile; fileNumber++) {
            nBytesToPrune = m_blockfile_info[fileNumber].nSize + m_blockfile_info[fileNumber].nUndoSize;

            if (m_blockfile_info[fileNumber].nSize == 0)
                continue;

            if (nCurrentUsage + nBuffer < nPruneTarget)
                break;

            if (m_blockfile_info[fileNumber].nHeightLast > nLastBlockWeCanPrune)
                continue;

            PruneOneBlockFile(fileNumber);
            setFilesToPrune.insert(fileNumber);
            nCurrentUsage -= nBytesToPrune;
            count++;
        }
    }

    LogPrint(BCLog::PRUNE, "Prune: target=%dMiB actual=%dMiB diff=%dMiB max_prune_height=%d removed %d blk/rev pairs\n",
             nPruneTarget / 1024 / 1024, nCurrentUsage / 1024 / 1024,
             ((int64_t)nPruneTarget - (int64_t)nCurrentUsage) / 1024 / 1024,
             nLastBlockWeCanPrune, count);
}
