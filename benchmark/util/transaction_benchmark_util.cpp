#include "util/transaction_benchmark_util.h"
#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>
#include "common/allocator.h"
#include "transaction/transaction_util.h"
#include "util/catalog_test_util.h"

namespace terrier {
RandomWorkloadTransaction::RandomWorkloadTransaction(LargeTransactionBenchmarkObject *test_object)
    : test_object_(test_object),
      txn_(test_object->txn_manager_.BeginTransaction()),
      aborted_(false),
      start_time_(txn_->StartTime()),
      commit_time_(UINT64_MAX),
      buffer_(common::AllocationUtil::AllocateAligned(test_object->row_initializer_.ProjectedRowSize())) {}

RandomWorkloadTransaction::~RandomWorkloadTransaction() {
  if (!test_object_->gc_on_) delete txn_;
  delete[] buffer_;
}

template <class Random>
void RandomWorkloadTransaction::RandomUpdate(Random *generator) {
  if (aborted_) return;
  const storage::TupleSlot updated = *(RandomTestUtil::UniformRandomElement(test_object_->inserted_tuples_, generator));
  std::vector<storage::col_id_t> update_col_ids =
      StorageTestUtil::ProjectionListRandomColumns(test_object_->layout_, generator);
  storage::ProjectedRowInitializer initializer =
      storage::ProjectedRowInitializer::Create(test_object_->layout_, update_col_ids);

  auto *const record = txn_->StageWrite(CatalogTestUtil::test_db_oid, CatalogTestUtil::test_table_oid, initializer);
  record->SetTupleSlot(updated);

  StorageTestUtil::PopulateRandomRow(record->Delta(), test_object_->layout_, 0.0, generator);
  auto result = test_object_->table_.Update(txn_, updated, *(record->Delta()));
  aborted_ = !result;
}

template <class Random>
void RandomWorkloadTransaction::RandomInsert(Random *generator) {
  if (aborted_) return;
  auto *const redo =
      txn_->StageWrite(CatalogTestUtil::test_db_oid, CatalogTestUtil::test_table_oid, test_object_->row_initializer_);
  StorageTestUtil::PopulateRandomRow(redo->Delta(), test_object_->layout_, 0.0, generator);
  const storage::TupleSlot inserted = test_object_->table_.Insert(txn_, *(redo->Delta()));
  redo->SetTupleSlot(inserted);
}

template <class Random>
void RandomWorkloadTransaction::RandomSelect(Random *generator) {
  if (aborted_) return;
  const storage::TupleSlot selected =
      *(RandomTestUtil::UniformRandomElement(test_object_->inserted_tuples_, generator));
  auto *select_buffer = buffer_;
  storage::ProjectedRow *select = test_object_->row_initializer_.InitializeRow(select_buffer);
  test_object_->table_.Select(txn_, selected, select);
}

void RandomWorkloadTransaction::Finish() {
  if (aborted_)
    test_object_->txn_manager_.Abort(txn_);
  else
    commit_time_ = test_object_->txn_manager_.Commit(txn_, TestCallbacks::EmptyCallback, nullptr);
}

LargeTransactionBenchmarkObject::LargeTransactionBenchmarkObject(const std::vector<uint8_t> &attr_sizes,
                                                                 uint32_t initial_table_size, uint32_t txn_length,
                                                                 std::vector<double> operation_ratio,
                                                                 storage::BlockStore *block_store,
                                                                 storage::RecordBufferSegmentPool *buffer_pool,
                                                                 std::default_random_engine *generator, bool gc_on,
                                                                 storage::LogManager *log_manager)
    : txn_length_(txn_length),
      operation_ratio_(std::move(operation_ratio)),
      generator_(generator),
      layout_({attr_sizes}),
      table_(block_store, layout_, storage::layout_version_t(0)),
      txn_manager_(buffer_pool, gc_on, log_manager),
      gc_on_(gc_on),
      abort_count_(0) {
  // Bootstrap the table to have the specified number of tuples
  PopulateInitialTable(initial_table_size, generator_);
}

LargeTransactionBenchmarkObject::~LargeTransactionBenchmarkObject() {
  if (!gc_on_) delete initial_txn_;
}

// Caller is responsible for freeing the returned results if bookkeeping is on.
uint64_t LargeTransactionBenchmarkObject::SimulateOltp(uint32_t num_transactions, uint32_t num_concurrent_txns) {
  common::WorkerPool thread_pool(num_concurrent_txns, {});
  std::vector<RandomWorkloadTransaction *> txns;
  std::function<void(uint32_t)> workload;
  std::atomic<uint32_t> txns_run = 0;
  if (gc_on_) {
    // Then there is no need to keep track of RandomWorkloadTransaction objects
    workload = [&](uint32_t) {
      for (uint32_t txn_id = txns_run++; txn_id < num_transactions; txn_id = txns_run++) {
        RandomWorkloadTransaction txn(this);
        SimulateOneTransaction(&txn, txn_id);
      }
    };
  } else {
    txns.resize(num_transactions);
    // Either for correctness checking, or to cleanup memory afterwards, we need to retain these
    // test objects
    workload = [&](uint32_t) {
      for (uint32_t txn_id = txns_run++; txn_id < num_transactions; txn_id = txns_run++) {
        txns[txn_id] = new RandomWorkloadTransaction(this);
        SimulateOneTransaction(txns[txn_id], txn_id);
      }
    };
  }

  MultiThreadTestUtil::RunThreadsUntilFinish(&thread_pool, num_concurrent_txns, workload);

  // We only need to deallocate, and return, if gc is on, this loop is a no-op
  for (RandomWorkloadTransaction *txn : txns) {
    if (txn->aborted_) abort_count_++;
    delete txn;
  }
  // This result is meaningless if bookkeeping is not turned on.
  return abort_count_;
}

void LargeTransactionBenchmarkObject::SimulateOneTransaction(terrier::RandomWorkloadTransaction *txn, uint32_t txn_id) {
  std::default_random_engine thread_generator(txn_id);

  auto insert = [&] { txn->RandomInsert(&thread_generator); };
  auto update = [&] { txn->RandomUpdate(&thread_generator); };
  auto select = [&] { txn->RandomSelect(&thread_generator); };
  RandomTestUtil::InvokeWorkloadWithDistribution({insert, update, select}, operation_ratio_, &thread_generator,
                                                 txn_length_);
  txn->Finish();
}

template <class Random>
void LargeTransactionBenchmarkObject::PopulateInitialTable(uint32_t num_tuples, Random *generator) {
  initial_txn_ = txn_manager_.BeginTransaction();

  for (uint32_t i = 0; i < num_tuples; i++) {
    auto *const redo =
        initial_txn_->StageWrite(CatalogTestUtil::test_db_oid, CatalogTestUtil::test_table_oid, row_initializer_);
    StorageTestUtil::PopulateRandomRow(redo->Delta(), layout_, 0.0, generator);
    const storage::TupleSlot inserted = table_.Insert(initial_txn_, *(redo->Delta()));
    redo->SetTupleSlot(inserted);
    inserted_tuples_.emplace_back(inserted);
  }
  txn_manager_.Commit(initial_txn_, TestCallbacks::EmptyCallback, nullptr);
}
}  // namespace terrier
