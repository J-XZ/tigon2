#pragma once

#include "kv/engine/region_allocator.h"
#include "kv/engine/kv_messages.h"
#include "kv/kv_store.h"

#include <memory>
#include <atomic>
#include <deque>
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
  // Foreground workers call this between operations; synchronous forwarding
  // also polls it while waiting so no dedicated service core is required.
  void PollTransport();
  // Bind the calling thread as foreground worker `worker_id` for CXL_EBR TLS.
  // Must be invoked once per worker thread before shared access (matches
  // core/Executor thread_init_ebr_meta).
  void BindWorker(uint32_t worker_id);
  uint32_t PartitionForKey(std::string_view key) const;
  uint32_t OwnerForKey(std::string_view key) const;
  uint64_t NetworkTxBytes() const { return network_tx_bytes_.load(std::memory_order_relaxed); }
  uint64_t NetworkRxBytes() const { return network_rx_bytes_.load(std::memory_order_relaxed); }
  RuntimeStats EngineRuntime() const;

 private:
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
  void HandleTransportMessage(const KvMessage &message);
  void ServeScanRequest(const KvMessage &message);
  void SendTransportMessage(const KvMessage &message);
  void EnforceMigrationBudget(KVPartition &partition);

  Config config_;
  std::unique_ptr<DualRegionMappedPool> pool_;
  std::unique_ptr<star::CXL_EBR> ebr_;
  std::unique_ptr<star::SCCManager> scc_;
  std::vector<std::unique_ptr<KVPartition>> partitions_;
  star::MPSCRingBuffer *rings_ = nullptr;
  // The node inbound transport is MPSC: producers are concurrent but exactly
  // one thread may dequeue.  This lock protects only dequeue/dispatch, not
  // the public KV operation path or row/index concurrency.
  std::recursive_mutex transport_poll_mutex_;
  std::mutex response_mutex_;
  std::unordered_map<uint64_t, KvMessage> responses_;
  struct PendingScan {
    StatusCode status = StatusCode::kOk;
    bool done = false;
    std::vector<ScanItem> items;
  };
  std::mutex pending_scan_mutex_;
  std::unordered_map<uint64_t, PendingScan> pending_scans_;
  // Nested PollTransport defers request messages (Put/Get/Scan/...) so OLC
  // tree walks are not mutated mid-scan; drained when serve depth returns to 0.
  // Concurrent threads may still serve requests in parallel.
  std::mutex deferred_request_mutex_;
  std::deque<KvMessage> deferred_transport_requests_;
  // Soft cap on concurrent remote Scan send+await slots (ring backpressure).
  // Kept well above worker count so Scans stay parallel after nested-poll fix.
  static constexpr uint32_t kMaxInflightScanRpcs = 8;
  std::mutex scan_rpc_mutex_;
  uint32_t inflight_scan_rpcs_ = 0;
  // OLC cannot sustain many concurrent walkers on the same trees; bound owned
  // walks (local page + ServeScanRequest).  Scan() coordinators stay parallel
  // and a transport poller keeps serving while they await responses.
  static constexpr uint32_t kMaxConcurrentOwnedScans = 2;
  std::atomic<uint32_t> concurrent_owned_scans_{0};
  std::thread transport_poller_;
  std::atomic<bool> transport_poller_stop_{false};
  uint32_t transport_poller_worker_id_ = 0;
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
