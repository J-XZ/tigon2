#pragma once

#include "kv/engine/region_allocator.h"
#include "kv/engine/kv_messages.h"
#include "kv/kv_store.h"
#include "common/LockfreeQueue.h"

#include <memory>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <string_view>
#include <vector>

namespace star {
class CXL_EBR;
class SCCManager;
}

namespace tigonkv::engine {

class KVPartition;
}

namespace star { class MPSCRingBuffer; }

namespace tigonkv::engine {

// Process-local assembly over persistent dual-region state.  Only partitions
// owned by config.node_id are materialized locally; remote transport is added
// by kv_messages in M5 without changing this ownership boundary.
class KVEngine {
 public:
  static std::unique_ptr<KVEngine> Open(const Config &config, bool reset);
  ~KVEngine();

  Status Put(std::string_view key, std::string_view value);
  GetResult Get(std::string_view key);
  Status Delete(std::string_view key);
  Status MoveOut(std::string_view key);
  ScanResult Scan(std::string_view start_key, uint64_t limit);
  CasResult CompareExchange(std::string_view key, std::string_view expected,
                            std::string_view desired);
  IncrementResult Increment(std::string_view key, int64_t delta);
  MemoryStats Memory() const;
  Status Checkpoint();
  // Foreground cooperative path (Tigon Worker::process_request analogue):
  // serve deferred inbound requests from this worker's lock-free queue.
  // The dedicated inbound demuxer is the sole MPSC consumer — FG never
  // contends for the recv lock.
  void PollTransport();
  // Bind the calling thread as foreground worker `worker_id` for CXL_EBR TLS
  // and Dispatcher-style per-worker request queue drainage.
  // Must be invoked once per worker thread before shared access (matches
  // core/Executor thread_init_ebr_meta).
  void BindWorker(uint32_t worker_id);
  uint32_t PartitionForKey(std::string_view key) const;
  uint32_t OwnerForKey(std::string_view key) const;
  uint64_t NetworkTxBytes() const { return network_tx_bytes_.load(std::memory_order_relaxed); }
  uint64_t NetworkRxBytes() const { return network_rx_bytes_.load(std::memory_order_relaxed); }
  RuntimeStats EngineRuntime() const;

 private:
  // Mirrors star::LockfreeQueue used by IncomingDispatcher → Worker::in_queue.
  // Capacity sized for bursty ScanItem/request fan-in without huge RSS.
  using WorkerRequestQueue = star::LockfreeQueue<KvMessage, 8192>;

  KVEngine(const Config &config, std::unique_ptr<DualRegionMappedPool> pool,
           std::unique_ptr<star::CXL_EBR> ebr,
           std::unique_ptr<star::SCCManager> scc);
  KVPartition *OwnedPartition(std::string_view key) const;
  KVPartition *VisiblePartition(std::string_view key) const;
  uint32_t OwnerForPartition(uint32_t partition) const;
  Status Forward(KvMessageType type, std::string_view key, std::string_view value,
                 std::string *response_value);
  CasResult ForwardCompareExchange(std::string_view key, std::string_view expected,
                                   std::string_view desired);
  Status AwaitResponse(uint64_t request_id, std::string *response_value);
  ScanResult ScanOwnedPartitions(std::string_view start_key, uint64_t limit);
  Status AwaitScan(uint64_t request_id, std::vector<ScanItem> *items);
  // Demuxer path: apply responses / queue requests. Never sends.
  void DemuxTransportMessage(const KvMessage &message);
  // Foreground path: serve a queued request (may Send).
  void ServeTransportRequest(const KvMessage &message);
  void ServeDeferredRequests(int max_count);
  void ServeScanRequest(const KvMessage &message);
  void SendTransportMessage(const KvMessage &message);
  void EnforceMigrationBudget(KVPartition &partition);
  void StartInboundDemuxer();
  void StopInboundDemuxer();
  void InboundDemuxerLoop();

  Config config_;
  std::unique_ptr<DualRegionMappedPool> pool_;
  std::unique_ptr<star::CXL_EBR> ebr_;
  std::unique_ptr<star::SCCManager> scc_;
  std::vector<std::unique_ptr<KVPartition>> partitions_;
  star::MPSCRingBuffer *rings_ = nullptr;
  // Sole MPSC consumer — mirrors Tigon IncomingDispatcher.  Never serves
  // Put/Get/Scan and never SendTransportMessage (avoids full-ring circular wait).
  std::thread inbound_demuxer_;
  std::atomic<bool> inbound_demuxer_stop_{false};
  uint32_t inbound_demuxer_worker_id_ = 0;
  std::mutex response_mutex_;
  std::unordered_map<uint64_t, KvMessage> responses_;
  struct PendingScan {
    StatusCode status = StatusCode::kOk;
    bool done = false;
    std::vector<ScanItem> items;
  };
  std::mutex pending_scan_mutex_;
  std::unordered_map<uint64_t, PendingScan> pending_scans_;
  // Demuxer (SPSC producer) → per-worker LockfreeQueue → FG PollTransport.
  // Nested serve (TlsRequestServeDepth != 0) must not pop/serve — OLC safety.
  std::vector<std::unique_ptr<WorkerRequestQueue>> worker_request_queues_;
  // Soft cap on concurrent remote Scan send+await slots (ring backpressure).
  static constexpr uint32_t kMaxInflightScanRpcs = 8;
  std::mutex scan_rpc_mutex_;
  std::condition_variable scan_rpc_cv_;
  uint32_t inflight_scan_rpcs_ = 0;
  struct PendingCas {
    uint32_t source_node = 0;
    std::string key;
    std::string expected;
  };
  std::mutex pending_cas_mutex_;
  std::unordered_map<uint64_t, PendingCas> pending_cas_;
  std::atomic<uint64_t> network_tx_bytes_{0};
  std::atomic<uint64_t> network_rx_bytes_{0};
  std::atomic<uint64_t> shared_gets_{0};
  std::atomic<uint64_t> shared_puts_{0};
  std::atomic<uint64_t> shared_deletes_{0};
  std::atomic<uint64_t> shared_swcc_flushes_{0};
  std::atomic<uint64_t> migration_in_{0};
  std::atomic<uint64_t> migration_out_{0};
};

}  // namespace tigonkv::engine
