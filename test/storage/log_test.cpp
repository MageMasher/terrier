#include <unordered_map>
#include <vector>
#include "gtest/gtest.h"
#include "main/db_main.h"
#include "settings/settings_callbacks.h"
#include "settings/settings_manager.h"
#include "storage/data_table.h"
#include "storage/garbage_collector_thread.h"
#include "storage/write_ahead_log/log_manager.h"
#include "transaction/transaction_manager.h"
#include "type/transient_value_factory.h"
#include "util/catalog_test_util.h"
#include "util/storage_test_util.h"
#include "util/test_harness.h"
#include "util/transaction_test_util.h"

#define __SETTING_GFLAGS_DEFINE__      // NOLINT
#include "settings/settings_common.h"  // NOLINT
#include "settings/settings_defs.h"    // NOLINT
#undef __SETTING_GFLAGS_DEFINE__       // NOLINT

#define LOG_FILE_NAME "./test.log"

namespace terrier::storage {
class WriteAheadLoggingTests : public TerrierTest {
 protected:
  storage::LogManager *log_manager_;

  // Settings for log manager
  const uint64_t num_log_buffers_ = 100;
  const std::chrono::milliseconds log_serialization_interval_{10};
  const std::chrono::milliseconds log_persist_interval_{20};
  const uint64_t log_persist_threshold_ = (1 << 20);  // 1MB

  std::default_random_engine generator_;
  storage::RecordBufferSegmentPool pool_{2000, 100};
  storage::BlockStore block_store_{100, 100};

  const std::chrono::milliseconds gc_period_{10};
  storage::GarbageCollectorThread *gc_thread_;

  void SetUp() override {
    // Unlink log file incase one exists from previous test iteration
    unlink(LOG_FILE_NAME);
    log_manager_ = new LogManager(LOG_FILE_NAME, num_log_buffers_, log_serialization_interval_, log_persist_interval_,
                                  log_persist_threshold_, &pool_);
    TerrierTest::SetUp();
  }

  void TearDown() override {
    // Delete log file
    unlink(LOG_FILE_NAME);
    delete log_manager_;
    DedicatedThreadRegistry::GetInstance().TearDown();
    TerrierTest::TearDown();
  }

  storage::LogRecord *ReadNextRecord(storage::BufferedLogReader *in, const storage::BlockLayout &block_layout) {
    auto size = in->ReadValue<uint32_t>();
    byte *buf = common::AllocationUtil::AllocateAligned(size);
    auto record_type = in->ReadValue<storage::LogRecordType>();
    auto txn_begin = in->ReadValue<transaction::timestamp_t>();
    if (record_type == storage::LogRecordType::COMMIT) {
      auto txn_commit = in->ReadValue<transaction::timestamp_t>();
      // Okay to fill in null since nobody will invoke the callback.
      // is_read_only argument is set to false, because we do not write out a commit record for a transaction if it is
      // not read-only.
      return storage::CommitRecord::Initialize(buf, txn_begin, txn_commit, nullptr, nullptr, false, nullptr);
    }

    // TODO(Tianyu): Without a lookup mechanism this oid is not exactly meaningful. Implement lookup when possible
    auto database_oid = in->ReadValue<catalog::db_oid_t>();
    auto table_oid = in->ReadValue<catalog::table_oid_t>();
    auto tuple_slot = in->ReadValue<storage::TupleSlot>();

    if (record_type == storage::LogRecordType::DELETE) {
      return storage::DeleteRecord::Initialize(buf, txn_begin, database_oid, table_oid, tuple_slot);
    }

    // If code path reaches here, we have a REDO record.
    TERRIER_ASSERT(record_type == storage::LogRecordType::REDO, "Unknown record type during test deserialization");

    // Read in col_ids
    // IDs read individually since we can't guarantee memory layout of vector
    auto num_cols = in->ReadValue<uint16_t>();
    std::vector<storage::col_id_t> col_ids(num_cols);
    for (uint16_t i = 0; i < num_cols; i++) {
      const auto col_id = in->ReadValue<storage::col_id_t>();
      col_ids[i] = col_id;
    }

    // Initialize the redo record.
    auto initializer = storage::ProjectedRowInitializer::Create(block_layout, col_ids);
    auto *result = storage::RedoRecord::Initialize(buf, txn_begin, database_oid, table_oid, initializer);
    auto *record_body = result->GetUnderlyingRecordBodyAs<RedoRecord>();
    record_body->SetTupleSlot(tuple_slot);
    auto *delta = record_body->Delta();

    // Get an in memory copy of the record's null bitmap. Note: this is used to guide how the rest of the log file is
    // read in. It doesn't populate the delta's bitmap yet. This will happen naturally as we proceed column-by-column.
    auto bitmap_num_bytes = common::RawBitmap::SizeInBytes(num_cols);
    auto *bitmap_buffer = new uint8_t[bitmap_num_bytes];
    in->Read(bitmap_buffer, bitmap_num_bytes);
    auto *bitmap = reinterpret_cast<common::RawBitmap *>(bitmap_buffer);

    for (uint16_t i = 0; i < num_cols; i++) {
      if (!bitmap->Test(i)) {
        // Recall that 0 means null in our definition of a ProjectedRow's null bitmap.
        delta->SetNull(i);
        continue;
      }

      // The column is not null, so set the bitmap accordingly and get access to the column value.
      auto *column_value_address = delta->AccessForceNotNull(i);
      if (block_layout.IsVarlen(col_ids[i])) {
        // Read how many bytes this varlen actually is.
        const auto varlen_attribute_size = in->ReadValue<uint32_t>();
        // Allocate a varlen buffer of this many bytes.
        auto *varlen_attribute_content = common::AllocationUtil::AllocateAligned(varlen_attribute_size);
        // Fill the entry with the next bytes from the log file.
        in->Read(varlen_attribute_content, varlen_attribute_size);
        // Create the varlen entry depending on whether it can be inlined or not
        storage::VarlenEntry varlen_entry;
        if (varlen_attribute_size <= storage::VarlenEntry::InlineThreshold()) {
          varlen_entry = storage::VarlenEntry::CreateInline(varlen_attribute_content, varlen_attribute_size);
        } else {
          varlen_entry = storage::VarlenEntry::Create(varlen_attribute_content, varlen_attribute_size, true);
        }
        // The attribute value in the ProjectedRow will be a pointer to this varlen entry.
        auto *dest = reinterpret_cast<storage::VarlenEntry *>(column_value_address);
        // Set the value to be the address of the varlen_entry.
        *dest = varlen_entry;
      } else {
        // For inlined attributes, just directly read into the ProjectedRow.
        in->Read(column_value_address, block_layout.AttrSize(col_ids[i]));
      }
    }

    // Free the memory allocated for the bitmap.
    delete[] bitmap_buffer;

    return result;
  }
};

// This test uses the LargeTransactionTestObject to simulate some number of transactions with logging turned on, and
// then reads the logged out content to make sure they are correct
// NOLINTNEXTLINE
TEST_F(WriteAheadLoggingTests, LargeLogTest) {
  // Each transaction does 5 operations. The update-select ratio of operations is 50%-50%.
  log_manager_->Start();
  LargeTransactionTestObject tested = LargeTransactionTestObject::Builder()
                                          .SetMaxColumns(5)
                                          .SetInitialTableSize(1)
                                          .SetTxnLength(5)
                                          .SetUpdateSelectRatio({0.5, 0.5})
                                          .SetBlockStore(&block_store_)
                                          .SetBufferPool(&pool_)
                                          .SetGenerator(&generator_)
                                          .SetGcOn(true)
                                          .SetVarlenAllowed(true)
                                          .SetBookkeeping(true)
                                          .SetLogManager(log_manager_)
                                          .build();
  auto result = tested.SimulateOltp(100, 4);
  log_manager_->PersistAndStop();

  std::unordered_map<transaction::timestamp_t, RandomWorkloadTransaction *> txns_map;
  for (auto *txn : result.first) txns_map[txn->BeginTimestamp()] = txn;
  // At this point all the log records should have been written out, we can start reading stuff back in.
  storage::BufferedLogReader in(LOG_FILE_NAME);
  const auto &block_layout = tested.Layout();
  while (in.HasMore()) {
    storage::LogRecord *log_record = ReadNextRecord(&in, block_layout);
    if (log_record->TxnBegin() == transaction::timestamp_t(0)) {
      // TODO(Tianyu): This is hacky, but it will be a pain to extract the initial transaction. The LargeTransactionTest
      //  harness probably needs some refactor (later after wal is in).
      // This the initial setup transaction.
      delete[] reinterpret_cast<byte *>(log_record);
      continue;
    }

    auto it = txns_map.find(log_record->TxnBegin());
    if (it == txns_map.end()) {
      // Okay to write out aborted transaction's redos, just cannot be a commit
      EXPECT_NE(log_record->RecordType(), storage::LogRecordType::COMMIT);
      delete[] reinterpret_cast<byte *>(log_record);
      continue;
    }
    if (log_record->RecordType() == storage::LogRecordType::COMMIT) {
      EXPECT_EQ(log_record->GetUnderlyingRecordBodyAs<storage::CommitRecord>()->CommitTime(),
                it->second->CommitTimestamp());
      EXPECT_TRUE(it->second->Updates()->empty());  // All previous updates have been logged out previously
      txns_map.erase(it);
    } else {
      // This is leveraging the fact that we don't update the same tuple twice in a transaction with
      // bookkeeping turned on
      auto *redo = log_record->GetUnderlyingRecordBodyAs<storage::RedoRecord>();
      // TODO(Tianyu): The DataTable field cannot be recreated from oid_t yet (we also don't really have oids),
      // so we are not checking it
      auto update_it = it->second->Updates()->find(redo->GetTupleSlot());
      EXPECT_NE(it->second->Updates()->end(), update_it);
      EXPECT_TRUE(StorageTestUtil::ProjectionListEqualDeep(tested.Layout(), update_it->second, redo->Delta()));
      delete[] reinterpret_cast<byte *>(update_it->second);
      it->second->Updates()->erase(update_it);
    }
    delete[] reinterpret_cast<byte *>(log_record);
  }

  // Ensure that the only committed transactions which remain in txns_map are read-only, because any other committing
  // transaction will generate a commit record and will be erased from txns_map in the checks above, if log records are
  // properly being written out. If at this point, there is exists any transaction in txns_map which made updates, then
  // something went wrong with logging. Read-only transactions do not generate commit records, so they will remain in
  // txns_map.
  for (const auto &kv_pair : txns_map) {
    EXPECT_TRUE(kv_pair.second->Updates()->empty());
  }

  // We perform GC at the end because we need the transactions to compare against the deserialized logs, thus we can
  // only reclaim their resources after that's done
  gc_thread_ = new storage::GarbageCollectorThread(tested.GetTxnManager(), gc_period_);
  delete gc_thread_;

  for (auto *txn : result.first) delete txn;
  for (auto *txn : result.second) delete txn;
}

// This test simulates a series of read-only transactions, and then reads the generated log file back in to ensure that
// read-only transactions do not generate any log records, as they are not necessary for recovery.
// NOLINTNEXTLINE
TEST_F(WriteAheadLoggingTests, ReadOnlyTransactionsGenerateNoLogTest) {
  // Each transaction is read-only (update-select ratio of 0-100). Also, no need for bookkeeping.
  log_manager_->Start();
  LargeTransactionTestObject tested = LargeTransactionTestObject::Builder()
                                          .SetMaxColumns(5)
                                          .SetInitialTableSize(1)
                                          .SetTxnLength(5)
                                          .SetUpdateSelectRatio({0.0, 1.0})
                                          .SetBlockStore(&block_store_)
                                          .SetBufferPool(&pool_)
                                          .SetGenerator(&generator_)
                                          .SetGcOn(true)
                                          .SetBookkeeping(false)
                                          .SetLogManager(log_manager_)
                                          .build();

  auto result = tested.SimulateOltp(1000, 4);
  log_manager_->PersistAndStop();

  // Read-only workload has completed. Read the log file back in to check that no records were produced for these
  // transactions.
  int log_records_count = 0;
  storage::BufferedLogReader in(LOG_FILE_NAME);
  while (in.HasMore()) {
    storage::LogRecord *log_record = ReadNextRecord(&in, tested.Layout());
    if (log_record->TxnBegin() == transaction::timestamp_t(0)) {
      // (TODO) Currently following pattern from LargeLogTest of skipping the initial transaction. When the transaction
      // testing framework changes, fix this.
      delete[] reinterpret_cast<byte *>(log_record);
      continue;
    }

    log_records_count += 1;
    delete[] reinterpret_cast<byte *>(log_record);
  }

  // We perform GC at the end because we need the transactions to compare against the deserialized logs, thus we can
  // only reclaim their resources after that's done
  gc_thread_ = new storage::GarbageCollectorThread(tested.GetTxnManager(), gc_period_);
  delete gc_thread_;

  EXPECT_EQ(log_records_count, 0);
  for (auto *txn : result.first) delete txn;
  for (auto *txn : result.second) delete txn;
}
}  // namespace terrier::storage
