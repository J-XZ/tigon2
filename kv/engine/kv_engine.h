#pragma once

#include "kv/engine/region_allocator.h"
#include "kv/engine/kv_messages.h"
#include "kv/kv_store.h"

#include <memory>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
  static std::unique_ptr<KVEngine> Open(Config config, bool reset);
  ~KVEngine();

  Status Put(std::string_view key, std::string_view value);
  GetResult Get(std::string_view key);
  Status Delete(std::string_view key);
  Status MoveOut(std::string_view key);
  ScanResult Scan(std::string_view start_key, std::string_view end_key,
                  uint64_t limit);
  CasResult CompareExchange(std::string_view key, std::string_view expected,
                            std::string_view desired);
  IncrementResult Increment(std::string_view key, int64_t delta);
  MemoryStats Memory() const;
  // Foreground cooperative path (Tigon Worker::process_request analogue):
  // batch-pop deferred inbound requests from the shared Dispatcher FIFO.
  // The dedicated inbound demuxer is the sole MPSC consumer — FG never
  // contends for the recv lock.
  void PollTransport();
  // Bind the calling thread as foreground worker `worker_id` for CXL_EBR TLS
  // and, when configured, its distinct guest CPU.
  // Must be invoked once per worker thread before shared access (matches
  // core/Executor thread_init_ebr_meta).
  void BindWorker(uint32_t worker_id);
  void ReleaseWorker();
  uint32_t PartitionForKey(std::string_view key) const;
  uint32_t OwnerForKey(std::string_view key) const;
  uint64_t NetworkTxBytes() const { return network_tx_bytes_.load(std::memory_order_relaxed); }
  uint64_t NetworkRxBytes() const { return network_rx_bytes_.load(std::memory_order_relaxed); }
  RuntimeStats EngineRuntime() const;
  // Single-partition owner range move-in (§5.2). Does not return values.
  Status PreparePartitionSharedScan(uint32_t partition_id,
                                    std::string_view start_key,
                                    std::string_view inclusive_max,
                                    bool cursor_is_duplicate,
                                    uint64_t output_limit, uint32_t requester,
                                    bool *exhausted_out,
                                    bool *no_predecessor_out = nullptr);

 private:
  struct KeyRoute {
    uint32_t partition_id = 0;
    uint32_t owner = 0;
    KVPartition *partition = nullptr;
    bool owned_by_this_node = false;
  };
  KVEngine(const Config &config, std::unique_ptr<DualRegionMappedPool> pool,
           star::CXL_EBR *ebr, std::unique_ptr<star::SCCManager> scc);
  KeyRoute RouteForKey(std::string_view key) const;
  KVPartition *OwnedPartition(std::string_view key) const;
  KVPartition *VisiblePartition(std::string_view key) const;
  uint32_t OwnerForPartition(uint32_t partition) const;
  Status Forward(KvMessageType type, std::string_view key, std::string_view value,
                 std::string *response_value);
  Status Forward(KvMessageType type, std::string_view key, std::string_view value,
                 std::string *response_value, uint32_t owner,
                 bool *response_received = nullptr);
  // TwoPLPasha DATA_MIGRATION: ask owner to move_row_in, then requester CXL-accesses.
  Status RequestMigrate(std::string_view key);
  struct PendingResponse {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    KvMessage message{};
  };
  std::shared_ptr<PendingResponse> RegisterPendingResponse(uint64_t request_id);
  void RemovePendingResponse(uint64_t request_id);
  void AbandonPendingResponse(uint64_t request_id);
  // Caller must hold pending_response_mutex_.
  bool ConsumeAbandonedRequestLocked(uint64_t request_id);
  Status AwaitResponse(uint64_t request_id,
                       const std::shared_ptr<PendingResponse> &pending,
                       std::string *response_value,
                       bool *response_received = nullptr);
  // Demuxer path: apply responses / queue requests. Never sends.
  void DemuxTransportMessage(const KvMessage &message);
  void WakePendingForwarders();
  // Foreground path: serve a queued request (may Send).
  void ServeTransportRequest(const KvMessage &message);
  void ServeDeferredRequests();
  void SendTransportMessage(const KvMessage &message);
  void EnforceMigrationBudget(KVPartition &partition);
  void StartInboundDemuxer();
  void StopInboundDemuxer();
  void InboundDemuxerLoop();

  Config config_;
  // Per-owner Clock dynamic HWCC limit after Open clamps configured budget to
  // capacity remaining once static HWCC domains are accounted (§11.10).
  // Install / Memory / EnforceMigrationBudget all use this value — not the raw
  // (config.hw_cc_budget_mb − EBR) / vm_count formula alone.
  uint64_t owner_migration_dynamic_budget_bytes_ = 0;
  std::unique_ptr<DualRegionMappedPool> pool_;
  // CXL-resident shared EBR (HWCC); not owned / not deleted — pool lifetime.
  star::CXL_EBR *ebr_ = nullptr;
  std::unique_ptr<star::SCCManager> scc_;
  std::vector<std::unique_ptr<KVPartition>> partitions_;
  // Snapshot of the process affinity mask before any KV thread is pinned.
  // Empty means affinity is disabled.
  std::vector<int> affinity_cpus_;
  // Process-local ownership only; checked at Bind/Release, never on the op
  // hot path. Distinct OS threads must not share one EBR/statistics worker id.
  std::mutex worker_owner_mutex_;
  std::vector<std::thread::id> worker_owners_;
  star::MPSCRingBuffer *rings_ = nullptr;
  // Sole MPSC consumer — mirrors Tigon IncomingDispatcher.  Never serves
  // Put/Get/Scan and never SendTransportMessage (avoids full-ring circular wait).
  std::thread inbound_demuxer_;
  std::atomic<bool> inbound_demuxer_stop_{false};
  uint32_t inbound_demuxer_worker_id_ = 0;
  std::mutex pending_response_mutex_;
  std::unordered_map<uint64_t, std::shared_ptr<PendingResponse>>
      pending_responses_;
  // Bounded tombstones for Await timeouts (§10.11). Late responses matching a
  // tombstone are dropped; responses whose tombstone was FIFO-evicted are also
  // dropped and counted (never abort demuxer).
  std::unordered_set<uint64_t> abandoned_request_ids_;
  std::deque<uint64_t> abandoned_request_order_;
  // Demuxer enqueues requests here; FG PollTransport / Await drains them.
  // Nested serve (TlsRequestServeDepth != 0) must not pop/serve — OLC safety.
  // Single shared unbounded queue: demuxer never blocks on enqueue, and any
  // polling FG worker can serve any request (required for Forward liveness
  // under YCSB). Affinity is by cooperative steal-from-front FIFO.
  std::mutex deferred_request_mutex_;
  std::deque<KvMessage> deferred_transport_requests_;
  std::atomic<uint64_t> network_tx_bytes_{0};
  std::atomic<uint64_t> network_rx_bytes_{0};
  std::atomic<uint64_t> shared_gets_{0};
  std::atomic<uint64_t> shared_puts_{0};
  std::atomic<uint64_t> shared_deletes_{0};
  std::atomic<uint64_t> shared_swcc_flushes_{0};
  std::atomic<uint64_t> migration_in_{0};
  std::atomic<uint64_t> migration_out_{0};
  std::atomic<uint64_t> abandoned_responses_{0};
  // §13 Scan diagnostics: flushed from TLS at Scan boundaries / once per RPC.
  std::atomic<uint64_t> scan_partition_probes_{0};
  std::atomic<uint64_t> scan_migrate_rpcs_{0};
  std::atomic<uint64_t> scan_owner_rows_movein_attempted_{0};
  std::atomic<uint64_t> deferred_queue_peak_{0};
};

}  // namespace tigonkv::engine
