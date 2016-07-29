////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2016, goatpig.                                              //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //                                      
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "Blockchain.h"
#include "lmdb_wrapper.h"
#include "BDM_supportClasses.h"
#include "BlockDataMap.h"
#include "Progress.h"
#include "bdmenums.h"
#include "ThreadSafeClasses.h"

#include <future>
#include <atomic>
#include <condition_variable>
#include <exception>

#ifndef _BLOCKCHAINSCANNER_H
#define _BLOCKCHAINSCANNER_H

typedef function<void(BDMPhase, double, unsigned, unsigned)> ProgressCallback;

class ScanningException : public runtime_error
{
private:
   const unsigned badHeight_;

public:
   ScanningException(unsigned badHeight, const string &what = "")
      : runtime_error(what), badHeight_(badHeight)
   { }
};


////////////////////////////////////////////////////////////////////////////////
struct BlockDataBatch
{
   const unsigned end_;

   promise<bool> scanUtxosPromise;
   shared_future<bool> doneScanningUtxos_;

   mutex parseTxinMutex_;
   exception_ptr exceptionPtr_;

   unsigned highestProcessedHeight_;
   
   //keep a reference to the file mmaps used by this object since we don't copy 
   //the data, just point at it.
   map<unsigned, BlockFileMapPointer> fileMaps_;

   //only for addresses and utxos we track
   map<BinaryData, map<unsigned, StoredTxOut>> utxos_;
   map<BinaryData, StoredScriptHistory> ssh_;
   vector<StoredTxOut> spentTxOuts_;

   map<unsigned, BlockData> blocks_;

   //to synchronize pulling block data
   atomic<unsigned> *blockCounter_;

   ////
   BlockDataBatch(unsigned end, atomic<unsigned>* counter) :
      end_(end), blockCounter_(counter)
   {
      highestProcessedHeight_ = 0;
      doneScanningUtxos_ = scanUtxosPromise.get_future();
   }

   void flagUtxoScanDone(void) 
   { 
      scanUtxosPromise.set_value(true); 
   }
};

////////////////////////////////////////////////////////////////////////////////
struct BatchLink
{
   vector<shared_ptr<BlockDataBatch>> batchVec_;
   shared_ptr<BatchLink> next_;

   mutex readyToWrite_;
   BinaryData topScannedBlockHash_;

   unsigned start_;
   unsigned end_;
};

////////////////////////////////////////////////////////////////////////////////
class BlockchainScanner
{
private:

   struct TxFilterResults
   {
      BinaryData hash_;

      //map<blockId, set<tx offset>>
      map<uint32_t, set<uint32_t>> filterHits_;

      bool operator < (const TxFilterResults& rhs) const
      {
         return hash_ < rhs.hash_;
      }
   };

   Blockchain* blockchain_;
   LMDBBlockDatabase* db_;
   ScrAddrFilter* scrAddrFilter_;
   BlockDataLoader blockDataLoader_;

   const unsigned nBlockFilesPerBatch_;
   const unsigned nBlocksLookAhead_ = 10;
   const unsigned totalThreadCount_;
   const unsigned totalBlockFileCount_;

   BinaryData topScannedBlockHash_;

   ProgressCallback progress_ = 
      [](BDMPhase, double, unsigned, unsigned)->void{};
   bool reportProgress_ = false;

   //only for relevant utxos
   map<BinaryData, map<unsigned, StoredTxOut>> utxoMap_;

   unsigned startAt_ = 0;

   mutex resolverMutex_;

private:
   void scanBlockData(shared_ptr<BlockDataBatch>,
      const set<TxOutScriptRef>&);
   
   void accumulateDataBeforeBatchWrite(vector<shared_ptr<BlockDataBatch>>&);
   void writeBlockData(shared_ptr<BatchLink>);
   void processAndCommitTxHints(
      const vector<shared_ptr<BlockDataBatch>>& batchVec);
   void preloadUtxos(void);

   uint32_t check_merkle(uint32_t startHeight);

   void getFilterHitsThread(
      const set<BinaryData>& hashSet,
      atomic<int>& counter,
      map<uint32_t, set<TxFilterResults>>& resultMap);

   void processFilterHitsThread(
      map<uint32_t, map<uint32_t, 
      set<const TxFilterResults*>>>& filtersResultMap,
      TransactionalSet<BinaryData>& missingHashes,
      atomic<int>& counter, map<BinaryData, BinaryData>& results);


public:
   BlockchainScanner(Blockchain* bc, LMDBBlockDatabase* db,
      ScrAddrFilter* saf,
      BlockFiles& bf,
      unsigned threadcount, unsigned batchSize, 
      ProgressCallback prg, bool reportProgress) :
      blockchain_(bc), db_(db), scrAddrFilter_(saf),
      totalThreadCount_(threadcount),
      blockDataLoader_(bf.folderPath(), true, true, true),
      progress_(prg), reportProgress_(reportProgress),
      totalBlockFileCount_(bf.fileCount()),
      nBlockFilesPerBatch_(batchSize)
   {}

   void scan(uint32_t startHeight);
   void scan_nocheck(uint32_t startHeight);

   void undo(Blockchain::ReorganizationState& reorgState);
   void updateSSH(bool);
   void resolveTxHashes();

   const BinaryData& getTopScannedBlockHash(void) const
   {
      return topScannedBlockHash_;
   }
};

#endif