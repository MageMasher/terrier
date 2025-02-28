#pragma once

#include <queue>
#include <utility>
#include <vector>
#include "common/container/concurrent_blocking_queue.h"
#include "common/dedicated_thread_task.h"
#include "storage/record_buffer.h"
#include "storage/write_ahead_log/log_record.h"

namespace terrier::storage {

/**
 * Task that processes buffers handed over by transactions and serializes them into consumer buffers
 */
class LogSerializerTask : public DedicatedThreadTask {
 public:
  /**
   * @param serialization_interval Interval time for when to trigger serialization
   * @param buffer_pool buffer pool to use to release serialized buffers
   * @param empty_buffer_queue pointer to queue to pop empty buffers from
   * @param filled_buffer_queue pointer to queue to push filled buffers to
   * @param disk_log_writer_thread_cv pointer to condition variable to notify consumer when a new buffer has handed over
   */
  explicit LogSerializerTask(const std::chrono::milliseconds serialization_interval,
                             RecordBufferSegmentPool *buffer_pool,
                             common::ConcurrentBlockingQueue<BufferedLogWriter *> *empty_buffer_queue,
                             common::ConcurrentQueue<storage::SerializedLogs> *filled_buffer_queue,
                             std::condition_variable *disk_log_writer_thread_cv)
      : run_task_(false),
        serialization_interval_(serialization_interval),
        buffer_pool_(buffer_pool),
        filled_buffer_(nullptr),
        empty_buffer_queue_(empty_buffer_queue),
        filled_buffer_queue_(filled_buffer_queue),
        disk_log_writer_thread_cv_(disk_log_writer_thread_cv) {}

  /**
   * Runs main disk log writer loop. Called by thread registry upon initialization of thread
   */
  void RunTask() override {
    run_task_ = true;
    LogSerializerTaskLoop();
  }

  /**
   * Signals task to stop. Called by thread registry upon termination of thread
   */
  void Terminate() override {
    // If the task hasn't run yet, yield the thread until it's started
    while (!run_task_) std::this_thread::yield();
    TERRIER_ASSERT(run_task_, "Cant terminate a task that isnt running");
    run_task_ = false;
  }

  /**
   * Hands a (possibly partially) filled buffer to the serializer task to be serialized
   * @param buffer_segment the (perhaps partially) filled log buffer ready to be consumed
   */
  void AddBufferToFlushQueue(RecordBufferSegment *const buffer_segment) {
    common::SpinLatch::ScopedSpinLatch guard(&flush_queue_latch_);
    flush_queue_.push(buffer_segment);
  }

 private:
  // Flag to signal task to run or stop
  bool run_task_;
  // Interval for serialization
  const std::chrono::milliseconds serialization_interval_;

  // Used to release processed buffers
  RecordBufferSegmentPool *buffer_pool_;

  // TODO(Tianyu): Might not be necessary, since commit on txn manager is already protected with a latch
  // TODO(Tianyu): benchmark for if these should be concurrent data structures, and if we should apply the same
  //  optimization we applied to the GC queue.
  // Latch to protect flush queue
  common::SpinLatch flush_queue_latch_;
  // Stores unserialized buffers handed off by transactions
  std::queue<RecordBufferSegment *> flush_queue_;

  // Current buffer we are serializing logs to
  BufferedLogWriter *filled_buffer_;
  // Commit callbacks for commit records currently in filled_buffer
  std::vector<std::pair<transaction::callback_fn, void *>> commits_in_buffer_;

  // The queue containing empty buffers. Task will dequeue a buffer from this queue when it needs a new buffer
  common::ConcurrentBlockingQueue<BufferedLogWriter *> *empty_buffer_queue_;
  // The queue containing filled buffers. Task should push filled serialized buffers into this queue
  common::ConcurrentQueue<SerializedLogs> *filled_buffer_queue_;

  // Condition variable to signal disk log consumer task thread that a new full buffer has been pushed to the queue
  std::condition_variable *disk_log_writer_thread_cv_;

  /**
   * Main serialization loop. Calls Process every interval. Processes all the accumulated log records and
   * serializes them to log consumer tasks.
   */
  void LogSerializerTaskLoop();

  /**
   * Process all the accumulated log records and serialize them to log consumer tasks. It's important that we serialize
   * the logs in order to ensure that a single transaction's logs are ordered. Only a single thread can serialize the
   * logs (without more sophisticated ordering checks).
   */
  void Process();

  /**
   * Serialize out the task buffer to the current serialization buffer
   * @param buffer_to_serialize the iterator to the redo buffer to be serialized
   */
  void SerializeBuffer(IterableBufferSegment<LogRecord> *buffer_to_serialize);

  /**
   * Serialize out the record to the log
   * @param record the redo record to serialise
   */
  void SerializeRecord(const LogRecord &record);

  /**
   * Serialize the data pointed to by val to current serialization buffer
   * @tparam T Type of the value
   * @param val The value to write to the buffer
   */
  template <class T>
  void WriteValue(const T &val) {
    WriteValue(&val, sizeof(T));
  }

  /**
   * Serialize the data pointed to by val to current serialization buffer
   * @param val the value
   * @param size size of the value to serialize
   */
  void WriteValue(const void *val, uint32_t size);

  /**
   * Returns the current buffer to serialize logs to
   * @return buffer to write to
   */
  BufferedLogWriter *GetCurrentWriteBuffer();

  /**
   * Hand over the current buffer and commit callbacks for commit records in that buffer to the log consumer task
   */
  void HandFilledBufferToWriter();
};
}  // namespace terrier::storage
