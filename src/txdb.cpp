// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include "txdb.h"

#include "chainparams.h"
#include "hash.h"
#include "main.h"
#include "uint256.h"

#include <stdint.h>

using namespace std;

void static BatchWriteCoins(CLevelDBBatch &batch, const uint256 &hash, const CCoins &coins) {
    if (coins.IsPruned())
        batch.Erase(make_pair('c', hash));
    else
        batch.Write(make_pair('c', hash), coins);
}

void static BatchWriteHashBestChain(CLevelDBBatch &batch, const uint256 &hash) {
    batch.Write('B', hash);
}

CCoinsViewDB::CCoinsViewDB(size_t nCacheSize, bool fMemory, bool fWipe) : db(GetDataDir() / "chainstate", nCacheSize, fMemory, fWipe) {
}

bool CCoinsViewDB::GetCoins(const uint256 &txid, CCoins &coins) { 
    return db.Read(make_pair('c', txid), coins); 
}

bool CCoinsViewDB::SetCoins(const uint256 &txid, const CCoins &coins) {
    CLevelDBBatch batch;
    BatchWriteCoins(batch, txid, coins);
    return db.WriteBatch(batch);
}

bool CCoinsViewDB::HaveCoins(const uint256 &txid) {
    return db.Exists(make_pair('c', txid)); 
}

CBlockIndex *CCoinsViewDB::GetBestBlock() {
    uint256 hashBestChain;
    if (!db.Read('B', hashBestChain))
        return NULL;
    std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.find(hashBestChain);
    if (it == mapBlockIndex.end())
        return NULL;
    return it->second;
}

bool CCoinsViewDB::SetBestBlock(CBlockIndex *pindex) {
    CLevelDBBatch batch;
    BatchWriteHashBestChain(batch, pindex->GetBlockHash()); 
    return db.WriteBatch(batch);
}

bool CCoinsViewDB::BatchWrite(const std::map<uint256, CCoins> &mapCoins, CBlockIndex *pindex) {
    printf("Committing %u changed transactions to coin database...\n", (unsigned int)mapCoins.size());

    CLevelDBBatch batch;
    for (std::map<uint256, CCoins>::const_iterator it = mapCoins.begin(); it != mapCoins.end(); it++)
        BatchWriteCoins(batch, it->first, it->second);
    if (pindex)
        BatchWriteHashBestChain(batch, pindex->GetBlockHash());

    return db.WriteBatch(batch);
}

CBlockTreeDB::CBlockTreeDB(size_t nCacheSize, bool fMemory, bool fWipe) : CLevelDB(GetDataDir() / "blocks" / "index", nCacheSize, fMemory, fWipe) {
    if (!Read('S', salt)) {
        salt = GetRandHash();
        Write('S', salt);
    }
}

bool CBlockTreeDB::WriteBlockIndex(const CDiskBlockIndex& blockindex)
{
    return Write(make_pair('b', blockindex.GetBlockHash()), blockindex);
}

bool CBlockTreeDB::ReadBestInvalidWork(CBigNum& bnBestInvalidWork)
{
    return Read('I', bnBestInvalidWork);
}

bool CBlockTreeDB::WriteBestInvalidWork(const CBigNum& bnBestInvalidWork)
{
    return Write('I', bnBestInvalidWork);
}

bool CBlockTreeDB::WriteBlockFileInfo(int nFile, const CBlockFileInfo &info) {
    return Write(make_pair('f', nFile), info);
}

bool CBlockTreeDB::ReadBlockFileInfo(int nFile, CBlockFileInfo &info) {
    return Read(make_pair('f', nFile), info);
}

bool CBlockTreeDB::WriteLastBlockFile(int nFile) {
    return Write('l', nFile);
}

bool CBlockTreeDB::WriteReindexing(bool fReindexing) {
    if (fReindexing)
        return Write('R', '1');
    else
        return Erase('R');
}

bool CBlockTreeDB::ReadReindexing(bool &fReindexing) {
    fReindexing = Exists('R');
    return true;
}

bool CBlockTreeDB::ReadLastBlockFile(int &nFile) {
    return Read('l', nFile);
}

bool CCoinsViewDB::GetStats(CCoinsStats &stats) {
    CLevelDBIterator *pcursor = db.NewIterator();
    pcursor->Seek('c');

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    stats.hashBlock = GetBestBlock()->GetBlockHash();
    ss << stats.hashBlock;
    int64_t nTotalAmount = 0;
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, uint256> key;
        CCoins coins;
        if (pcursor->GetKey(key) && key.first == 'c') {
            if (pcursor->GetValue(coins)) {
                stats.nTransactions++;
                for (unsigned int i=0; i<coins.vout.size(); i++) {
                    const CTxOut &out = coins.vout[i];
                    if (!out.IsNull()) {
                        stats.nTransactionOutputs++;
                        ss << VARINT(i+1);
                        ss << out;
                        nTotalAmount += out.nValue;
                    }
                }
                stats.nSerializedSize += 32 + pcursor->GetKeySize();
                ss << VARINT(0);
            } else {
                return error("CCoinsViewDB::GetStats() : unable to read value");
            }
        } else {
            break;
        }
        pcursor->Next();
    }
    delete pcursor;
    stats.nHeight = GetBestBlock()->nHeight;
    stats.hashSerialized = ss.GetHash();
    stats.nTotalAmount = nTotalAmount;
    return true;
}

bool CBlockTreeDB::ReadTxIndex(const uint256 &txid, CDiskTxPos &pos) {
    return Read(make_pair('t', txid), pos);
}

bool CBlockTreeDB::WriteTxIndex(const std::vector<std::pair<uint256, CDiskTxPos> >&vect) {
    CLevelDBBatch batch;
    for (std::vector<std::pair<uint256,CDiskTxPos> >::const_iterator it=vect.begin(); it!=vect.end(); it++)
        batch.Write(make_pair('t', it->first), it->second);
    return WriteBatch(batch);
}

bool CBlockTreeDB::ReadAddrIndex(uint160 addrid, std::vector<CExtDiskTxPos> &list) {
    CLevelDBIterator *iter = NewIterator();
    uint64_t lookupid;
    {
        CHashWriter ss(SER_GETHASH, 0);
        ss << salt;
        ss << addrid;
        lookupid = ss.GetHash().Get64(0);
    }
    iter->Seek(make_pair('a', lookupid));
    while (iter->Valid()) {
        std::pair<std::pair<char, uint64_t>, CExtDiskTxPos> key;
        if (iter->GetKey(key) && key.first.first == 'a' && key.first.second == lookupid) {
            list.push_back(key.second);
        } else {
            break;
        }
        iter->Next();
    }
    return true;
}

bool CBlockTreeDB::AddAddrIndex(const std::vector<std::pair<uint160, CExtDiskTxPos> > &list) {
    unsigned char foo[0];
    CLevelDBBatch batch;
    for (std::vector<std::pair<uint160, CExtDiskTxPos> >::const_iterator it=list.begin(); it!=list.end(); it++) {
        CHashWriter ss(SER_GETHASH, 0);
        ss << salt;
        ss << it->first;
        batch.Write(make_pair(make_pair('a', ss.GetHash().Get64(0)), it->second), FLATDATA(foo));
    }
    return WriteBatch(batch);
}

bool CBlockTreeDB::WriteFlag(const std::string &name, bool fValue) {
    return Write(std::make_pair('F', name), fValue ? '1' : '0');
}

bool CBlockTreeDB::ReadFlag(const std::string &name, bool &fValue) {
    char ch;
    if (!Read(std::make_pair('F', name), ch))
        return false;
    fValue = ch == '1';
    return true;
}

bool CBlockTreeDB::LoadBlockIndexGuts()
{
    CLevelDBIterator *pcursor = NewIterator();

    pcursor->Seek(make_pair('b', uint256(0)));

    // Load mapBlockIndex
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, uint256> key;
        if (pcursor->GetKey(key) && key.first == 'b') {
            CDiskBlockIndex diskindex;
            if (pcursor->GetValue(diskindex)) {
                // Construct block index object
                CBlockIndex* pindexNew = InsertBlockIndex(diskindex.GetBlockHash());
                pindexNew->pprev          = InsertBlockIndex(diskindex.hashPrev);
                pindexNew->nHeight        = diskindex.nHeight;
                pindexNew->nFile          = diskindex.nFile;
                pindexNew->nDataPos       = diskindex.nDataPos;
                pindexNew->nUndoPos       = diskindex.nUndoPos;
                pindexNew->nVersion       = diskindex.nVersion;
                pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
                pindexNew->nTime          = diskindex.nTime;
                pindexNew->nBits          = diskindex.nBits;
                pindexNew->nNonce         = diskindex.nNonce;
                pindexNew->nStatus        = diskindex.nStatus;
                pindexNew->nTx            = diskindex.nTx;

                // Watch for genesis block
                if (pindexGenesisBlock == NULL && diskindex.GetBlockHash() == Params().HashGenesisBlock())
                    pindexGenesisBlock = pindexNew;

                if (!pindexNew->CheckIndex())
                    return error("LoadBlockIndex() : CheckIndex failed: %s", pindexNew->ToString().c_str());

                pcursor->Next();
            } else {
                return error("LoadBlockIndex() : failed to read value");
            }
        } else {
            break;
        }
    }
    delete pcursor;

    return true;
}
