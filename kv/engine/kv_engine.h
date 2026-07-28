#pragma once

#include "kv/engine/region_allocator.h"
#include "kv/kv_store.h"

#include "common/LockfreeQueue.h"
#include "common/Message.h"

#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <string_view>
#include <vector>

namespace star {
class CXL_EBR;
class SCCManager;
}

namespace tigonkv::engine {

enum class RpcKind : uint8_t { kMigrate, kInsert, kDelete, kScanMigrate };

class KVPartition;
}

namespace star { class MPSCRingBuffer; }

namespace tigonkv::engine {

// Process-local assembly over persistent dual-region state. Only partitions
// owned by config.node_id are materialized locally; remote traffic reuses the
// original TwoPLPasha Message/MessagePiece framing.
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
  // drain only this worker's original SPSC Message queue. The dedicated
  // inbound demuxer is the sole MPSC consumer.
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
  Status Forward(RpcKind type, std::string_view key, std::string_view value,
                 uint32_t partition_id, uint32_t owner,
                 std::string_view scan_max = {}, uint64_t scan_limit = 0);
  // TwoPLPasha DATA_MIGRATION: ask owner to move_row_in, then requester CXL-accesses.
  Status RequestMigrate(std::string_view key);
  struct OperationContext {
    uint32_t expected_response_type = 0;
    uint32_t partition_id = 0;
    uint64_t operation_sequence = 0;
    bool done = false;
    Status result = Status::Error(StatusCode::kCorruption, "unset RPC result");
  };
  struct WorkerMailbox {
    star::LockfreeQueue<star::Message *> inbox;
    // Original Executor-shaped per-destination buffers. A foreground worker
    // owns its buffers; SendTransportMessage copies the one-piece frame before
    // it is cleared for reuse.
    std::vector<std::unique_ptr<star::Message>> outbound;
    OperationContext operation;
    uint64_t next_operation_sequence = 1;
  };
  WorkerMailbox &CurrentMailbox();
  Status AwaitResponse(WorkerMailbox &mailbox);
  void DispatchMessage(star::Message &message, WorkerMailbox &mailbox);
  void ServeTransportRequest(star::Message &message,
                             star::MessagePiece piece,
                             WorkerMailbox &mailbox);
  void ConsumeTransportResponse(star::MessagePiece piece,
                                WorkerMailbox &mailbox);
  star::Message &OutboundMessage(WorkerMailbox &mailbox, uint32_t destination,
                                 uint64_t operation_sequence);
  void SendTransportMessage(star::Message &message);
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
  std::vector<std::unique_ptr<WorkerMailbox>> worker_mailboxes_;
  star::MPSCRingBuffer *rings_ = nullptr;
  // Sole MPSC consumer — mirrors Tigon IncomingDispatcher.  Never serves
  // Put/Get/Scan and never SendTransportMessage (avoids full-ring circular wait).
  std::thread inbound_demuxer_;
  std::atomic<bool> inbound_demuxer_stop_{false};
  uint32_t inbound_demuxer_worker_id_ = 0;
  std::atomic<uint64_t> network_tx_bytes_{0};
  std::atomic<uint64_t> network_rx_bytes_{0};
  std::atomic<uint64_t> shared_gets_{0};
  std::atomic<uint64_t> shared_puts_{0};
  std::atomic<uint64_t> shared_deletes_{0};
  std::atomic<uint64_t> shared_swcc_flushes_{0};
  std::atomic<uint64_t> migration_in_{0};
  std::atomic<uint64_t> migration_out_{0};
  // §13 Scan diagnostics: flushed from TLS at Scan boundaries / once per RPC.
  std::atomic<uint64_t> scan_partition_probes_{0};
  std::atomic<uint64_t> scan_migrate_rpcs_{0};
  std::atomic<uint64_t> scan_owner_rows_movein_attempted_{0};
};

}  // namespace tigonkv::engine
