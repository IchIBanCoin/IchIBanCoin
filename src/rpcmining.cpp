// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"
#include "db.h"
#include "init.h"
#include "miner.h"
#include "bitcoinrpc.h"

using namespace json_spirit;
using namespace std;

Value getmininginfo(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getmininginfo\n"
            "Returns an object containing mining-related information.");

    /* Caches the results for 10 minutes */
    if((GetTime() - 600) > nLastWalletStakeTime) {
        pwalletMain->GetStakeWeight(*pwalletMain, nMinWeightInputs, nAvgWeightInputs, nMaxWeightInputs, nTotalStakeWeight);
        nLastWalletStakeTime = GetTime();
    }

    Object obj;
    obj.push_back(Pair("blocks",        (int)nBestHeight));
    obj.push_back(Pair("currentblocksize",(uint64_t)nLastBlockSize));
    obj.push_back(Pair("currentblocktx",(uint64_t)nLastBlockTx));
    obj.push_back(Pair("powdifficulty", (float)GetDifficulty()));
    obj.push_back(Pair("posdifficulty", (float)GetDifficulty(GetLastBlockIndex(pindexBest, true))));
    obj.push_back(Pair("powreward",     (float)(GetProofOfWorkReward(GetLastBlockIndex(pindexBest, false)->nHeight, (int64)NULL))/COIN));
    obj.push_back(Pair("posreward",     (float)(GetProofOfStakeReward(GetLastBlockIndex(pindexBest, true)->nHeight, (int64)NULL))/COIN));
    obj.push_back(Pair("networkhashps", getnetworkhashps(params, false)));
    obj.push_back(Pair("stakeweight",   (uint64_t)nTotalStakeWeight));
    obj.push_back(Pair("minweightinputs", (uint64_t)nMinWeightInputs));
    obj.push_back(Pair("avgweightinputs", (uint64_t)nAvgWeightInputs));
    obj.push_back(Pair("maxweightinputs", (uint64_t)nMaxWeightInputs));
    obj.push_back(Pair("stakemindepth", (int)nStakeMinDepth));
    obj.push_back(Pair("minstakinginput", ValueFromAmount(nMinStakingInputValue)));
    obj.push_back(Pair("stakecombine",  ValueFromAmount(nCombineThreshold)));
    obj.push_back(Pair("stakesplit",    ValueFromAmount(nSplitThreshold)));
    obj.push_back(Pair("pooledtx",      (uint64_t)mempool.size()));
    obj.push_back(Pair("testnet",       fTestNet));

    return(obj);
}

Value getcounters(const Array& params, bool fHelp) {

    if(fHelp || (params.size() != 0))
      throw(runtime_error("getcounters\n"
        "Returns an object containing performance counters."));

    Object obj;
    obj.push_back(Pair("block hash cache hits",       (uint64_t)nBlockHashCacheHits));
    obj.push_back(Pair("block hash cache misses",     (uint64_t)nBlockHashCacheMisses));
    obj.push_back(Pair("stake modifier cache hits",   (uint64_t)nModifierCacheHits));
    obj.push_back(Pair("stake modifier cache misses", (uint64_t)nModifierCacheMisses));
    obj.push_back(Pair("stake input cache hits",      (uint64_t)nInputCacheHits));
    obj.push_back(Pair("stake input cache misses",    (uint64_t)nInputCacheMisses));

    return(obj);
}


/* RPC getwork provides a miner with the current best block header to solve
 * and receives the result if available */
Value getwork(const Array& params, bool fHelp) {

    if(fHelp || (params.size() > 1))
      throw(runtime_error(
        "getwork [data]\n"
        "If [data] is not specified, returns formatted data to work on:\n"
        "  \"data\" : block header\n"
        "  \"target\" : hash target\n"
        "  \"algorithm\" : hashing algorithm expected (optional)\n"
        "If [data] is specified, verifies the PoW hash against target and returns true if successful."));

    if (vNodes.empty())
        throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Orbitcoin is not connected!");

    if (IsInitialBlockDownload())
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Orbitcoin is downloading blocks...");

    typedef map<uint256, pair<CBlock*, CScript> > mapNewBlock_t;
    static mapNewBlock_t mapNewBlock;    // FIXME: thread safety
    static vector<CBlock*> vNewBlock;
    static CReserveKey reservekey(pwalletMain);

    if (params.size() == 0)
    {
        // Update block
        static unsigned int nTransactionsUpdatedLast;
        static CBlockIndex* pindexPrev;
        static int64 nStart;
        static CBlock* pblock;
        if (pindexPrev != pindexBest ||
            (nTransactionsUpdated != nTransactionsUpdatedLast && GetTime() - nStart > 60))
        {
            if (pindexPrev != pindexBest)
            {
                // Deallocate old blocks since they're obsolete now
                mapNewBlock.clear();
                BOOST_FOREACH(CBlock* pblock, vNewBlock)
                    delete pblock;
                vNewBlock.clear();
            }

            // Clear pindexPrev so future getworks make a new block, despite any failures from here on
            pindexPrev = NULL;

            // Store the pindexBest used before CreateNewBlock, to avoid races
            nTransactionsUpdatedLast = nTransactionsUpdated;
            CBlockIndex* pindexPrevNew = pindexBest;
            nStart = GetTime();

            // Create new block
            pblock = CreateNewBlock(pwalletMain, false);
            if (!pblock)
                throw JSONRPCError(RPC_OUT_OF_MEMORY, "Out of memory");
            vNewBlock.push_back(pblock);

            // Need to update only after we know CreateNewBlock succeeded
            pindexPrev = pindexPrevNew;
        }

        // Update nTime
        pblock->UpdateTime(pindexPrev);
        pblock->nNonce = 0;

        // Update nExtraNonce
        static unsigned int nExtraNonce = 0;
        IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

        /* Save this block for the future use */
        mapNewBlock[pblock->hashMerkleRoot] = make_pair(pblock, pblock->vtx[0].vin[0].scriptSig);

        /* Prepare the block header for transmission */
        uint pdata[32];
        FormatDataBuffer(pblock, pdata);

        /* Get the current decompressed block target */
        uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

        Object result;
        result.push_back(Pair("data",   HexStr(BEGIN(pdata), fNeoScrypt ? (char *) &pdata[20] : END(pdata))));
        result.push_back(Pair("target", HexStr(BEGIN(hashTarget), END(hashTarget))));
        /* Optional */
        if(fNeoScrypt)
          result.push_back(Pair("algorithm", "neoscrypt"));
        else
          result.push_back(Pair("algorithm", "scrypt:1024,1,1"));

        return(result);

    } else {

        /* Data received */
        vector<unsigned char> vchData = ParseHex(params[0].get_str());

        /* Must be no less actual data than sent previously */
        if(vchData.size() < 80)
          throw(JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter"));
        CBlock* pdata = (CBlock*) &vchData[0];

        if(!fNeoScrypt) {
            uint i;
            /* nVersion and hashPrevBlock aren't needed */
            for(i = 9; i < 20; i++)
              /* Convert BE to LE */
              ((uint *) pdata)[i] = ByteReverse(((uint *) pdata)[i]);
        }

        /* Pick up the block contents saved previously */
        if(!mapNewBlock.count(pdata->hashMerkleRoot))
          return(false);
        CBlock* pblock = mapNewBlock[pdata->hashMerkleRoot].first;

        /* Replace with the data received */
        pblock->nTime = pdata->nTime;
        pblock->nNonce = pdata->nNonce;
        pblock->vtx[0].vin[0].scriptSig = mapNewBlock[pdata->hashMerkleRoot].second;

        /* Rebuild the merkle root */
        pblock->hashMerkleRoot = pblock->BuildMerkleTree();

        /* Legacy proof-of-work block signing;
         * signature must be empty for all NeoScrypt blocks */
        if(!fNeoScrypt) {
            if(!pblock->SignWorkBlock(*pwalletMain))
              throw(JSONRPCError(-100, "Failed to sign this proof-of-work block!"));
        }

        /* Verify the resulting hash against target */
        return(CheckWork(pblock, *pwalletMain, reservekey));
    }
}


Value getblocktemplate(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getblocktemplate [params]\n"
            "Returns data needed to construct a block to work on:\n"
            "  \"version\" : block version\n"
            "  \"previousblockhash\" : hash of current highest block\n"
            "  \"transactions\" : contents of non-coinbase transactions that should be included in the next block\n"
            "  \"coinbaseaux\" : data that should be included in coinbase\n"
            "  \"coinbasevalue\" : maximum allowable input to coinbase transaction, including the generation award and transaction fees\n"
            "  \"target\" : hash target\n"
            "  \"mintime\" : minimum timestamp appropriate for next block\n"
            "  \"curtime\" : current timestamp\n"
            "  \"mutable\" : list of ways the block template may be changed\n"
            "  \"noncerange\" : range of valid nonces\n"
            "  \"sigoplimit\" : limit of sigops in blocks\n"
            "  \"sizelimit\" : limit of block size\n"
            "  \"bits\" : compressed target of next block\n"
            "  \"height\" : height of the next block\n"
            "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.");

    std::string strMode = "template";
    if (params.size() > 0)
    {
        const Object& oparam = params[0].get_obj();
        const Value& modeval = find_value(oparam, "mode");
        if (modeval.type() == str_type)
            strMode = modeval.get_str();
        else if (modeval.type() == null_type)
        {
            /* Do nothing */
        }
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");
    }

    if (strMode != "template")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");

    if (vNodes.empty())
        throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Orbitcoin is not connected!");

    if (IsInitialBlockDownload())
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Orbitcoin is downloading blocks...");

    static CReserveKey reservekey(pwalletMain);

    // Update block
    static unsigned int nTransactionsUpdatedLast;
    static CBlockIndex* pindexPrev;
    static int64 nStart;
    static CBlock* pblock;
    if (pindexPrev != pindexBest ||
        (nTransactionsUpdated != nTransactionsUpdatedLast && GetTime() - nStart > 5))
    {
        // Clear pindexPrev so future calls make a new block, despite any failures from here on
        pindexPrev = NULL;

        // Store the pindexBest used before CreateNewBlock, to avoid races
        nTransactionsUpdatedLast = nTransactionsUpdated;
        CBlockIndex* pindexPrevNew = pindexBest;
        nStart = GetTime();

        // Create new block
        if(pblock)
        {
            delete pblock;
            pblock = NULL;
        }
        pblock = CreateNewBlock(pwalletMain, false);
        if (!pblock)
            throw JSONRPCError(RPC_OUT_OF_MEMORY, "Out of memory");

        // Need to update only after we know CreateNewBlock succeeded
        pindexPrev = pindexPrevNew;
    }

    // Update nTime
    pblock->UpdateTime(pindexPrev);
    pblock->nNonce = 0;

    Array transactions;
    map<uint256, int64_t> setTxIndex;
    int i = 0;
    CCoinsViewCache &view = *pcoinsTip;
    BOOST_FOREACH (CTransaction& tx, pblock->vtx)
    {
        uint256 txHash = tx.GetHash();
        setTxIndex[txHash] = i++;

        if (tx.IsCoinBase() || tx.IsCoinStake())
            continue;

        Object entry;

        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << tx;
        entry.push_back(Pair("data", HexStr(ssTx.begin(), ssTx.end())));

        entry.push_back(Pair("hash", txHash.GetHex()));

        Array deps;
        BOOST_FOREACH (const CTxIn &in, tx.vin)
        {
            if (setTxIndex.count(in.prevout.hash))
                deps.push_back(setTxIndex[in.prevout.hash]);
        }
        entry.push_back(Pair("depends", deps));

        int64_t nSigOps = tx.GetLegacySigOpCount();
        if (tx.HaveInputs(view))
        {
            entry.push_back(Pair("fee", (int64_t)(tx.GetValueIn(view) - tx.GetValueOut())));
            nSigOps += tx.GetP2SHSigOpCount(view);
        }
        entry.push_back(Pair("sigops", nSigOps));

        transactions.push_back(entry);
    }

    Object aux;
    aux.push_back(Pair("flags", HexStr(COINBASE_FLAGS.begin(), COINBASE_FLAGS.end())));

    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

    static Array aMutable;
    if (aMutable.empty())
    {
        aMutable.push_back("time");
        aMutable.push_back("transactions");
        aMutable.push_back("prevblock");
    }

    Object result;
    result.push_back(Pair("version", pblock->nVersion));
    result.push_back(Pair("previousblockhash", pblock->hashPrevBlock.GetHex()));
    result.push_back(Pair("transactions", transactions));
    result.push_back(Pair("coinbaseaux", aux));
    result.push_back(Pair("coinbasevalue", (int64_t)pblock->vtx[0].vout[0].nValue));
    result.push_back(Pair("target", hashTarget.GetHex()));
    result.push_back(Pair("mintime", (int64_t)(pindexPrev->GetMedianTimePast() + BLOCK_LIMITER_TIME + 1)));
    result.push_back(Pair("mutable", aMutable));
    result.push_back(Pair("noncerange", "00000000ffffffff"));
    result.push_back(Pair("sigoplimit", (int64_t)MAX_BLOCK_SIGOPS));
    result.push_back(Pair("sizelimit", (int64_t)MAX_BLOCK_SIZE));
    result.push_back(Pair("curtime", (int64_t)pblock->nTime));
    result.push_back(Pair("bits", HexBits(pblock->nBits)));
    result.push_back(Pair("height", (int64_t)(pindexPrev->nHeight+1)));

    return result;
}

Value submitblock(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "submitblock <hex data> [optional-params-obj]\n"
            "[optional-params-obj] parameter is currently ignored.\n"
            "Attempts to submit new block to network.\n"
            "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.");

    vector<unsigned char> blockData(ParseHex(params[0].get_str()));
    CDataStream ssBlock(blockData, SER_NETWORK, PROTOCOL_VERSION);
    CBlock block;
    try {
        ssBlock >> block;
    }
    catch (std::exception &e) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");
    }

    if(!fNeoScrypt) {
        if(!block.SignWorkBlock(*pwalletMain))
          throw(JSONRPCError(-100, "Failed to sign this proof-of-work block!"));
    }

    bool fAccepted = ProcessBlock(NULL, &block);
    if (!fAccepted)
        return "rejected";

    return Value::null;
}

Value getnetworkhashps(const Array& params, bool fHelp) {

    if(fHelp || (params.size() > 1))
      throw(runtime_error(
        "getnetworkhashps [blocks]\n"
        "Calculates estimated network hashes per second based on the last 50 PoW blocks.\n"
        "Pass in [blocks] to override the default value."));

    int lookup = (params.size() > 0) ? params[0].get_int() : 50;

    /* The genesis block only */
    if(!pindexBest) return(0);

    /* Range limit */
    if(lookup <= 0) lookup = 50;

    int i;
    CBlockIndex* pindexPrev = pindexBest;
    for(i = 0; (i < lookup); i++) {
        /* Hit the genesis block */
        if(!pindexPrev->pprev) {
            lookup = i + 1;
            break;
        }
        /* Move one block back */
        pindexPrev = pindexPrev->pprev;
        /* Don't count PoS blocks */
        if(pindexPrev->IsProofOfStake()) i--;
    }

    double timeDiff = pindexBest->GetBlockTime() - pindexPrev->GetBlockTime();
    double timePerBlock = timeDiff / lookup;

    return((boost::int64_t)(((double)GetDifficulty() * pow(2.0, 32)) / timePerBlock));
}

Value getstakegen(const Array& params, bool fHelp) {

    if(fHelp || params.size() != 0) throw runtime_error(
      "getstakegen\n"
      "Returns true or false.");

    return fStakeGen;
}

Value setstakegen(const Array& params, bool fHelp) {

    if(fHelp || params.size() != 1) throw runtime_error(
      "setstakegen <generate>\n"
      "<generate> is true or false to turn generation on or off.");

    /* The flag triggers the stake miner */
    if(params.size() > 0) fStakeGen = params[0].get_bool();

    return Value::null;
}

