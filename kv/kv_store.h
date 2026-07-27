#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tigonkv {

enum class StatusCode {
  kOk,
  kNotFound,
  kAlreadyExists,
  kCompareFailed,
  kInvalidArgument,
  kOutOfMemory,
  kCorruption,
  kOwnerViolation,
  // Transient concurrency conflict. Callers may retry without treating it as
  // protocol corruption or changing worker concurrency.
  kBusy,
};

struct Status {
  StatusCode code = StatusCode::kOk;
  std::string message;
  bool ok() const { return code == StatusCode::kOk; }
  static Status Ok() { return {}; }
  static Status Error(StatusCode c, std::string m) { return {c, std::move(m)}; }
};

struct GetResult {
  Status status;
  std::string value;
};

struct ScanItem {
  std::string key;
  std::string value;
};

struct ScanResult {
  Status status;
  std::vector<ScanItem> items;
};

struct CasResult {
  Status status;
  bool exchanged = false;
};

struct IncrementResult {
  Status status;
  int64_t value = 0;
};

struct RuntimeStats {
  uint64_t logical_ops = 0;
  uint64_t commits = 0;
  uint64_t aborts = 0;
  uint64_t retries = 0;
  uint64_t private_gets = 0;
  uint64_t private_puts = 0;
  uint64_t private_deletes = 0;
  uint64_t checkpoint_swcc_flushes = 0;
  uint64_t private_swcc_flushes = 0;
  uint64_t shared_gets = 0;
  uint64_t shared_puts = 0;
  uint64_t shared_deletes = 0;
  uint64_t shared_swcc_flushes = 0;
  uint64_t migration_in = 0;
  uint64_t migration_out = 0;
  uint64_t network_tx_bytes = 0;
  uint64_t network_rx_bytes = 0;
  // Sum of Scan result rows returned by successful Scan calls (§11.12).
  uint64_t scan_rows_returned = 0;
  // Late responses dropped after Await timeout tombstones (§10.11).
  uint64_t abandoned_responses = 0;
};

struct MemoryStats {
  std::string allocator_mode = "global";
  bool physical_region_split = false;
  uint64_t total_pool_capacity_bytes = 0;
  uint64_t logical_hwcc_capacity_bytes = 0;
  uint64_t logical_swcc_capacity_bytes = 0;
  uint64_t logical_hwcc_used_bytes = 0;
  uint64_t physical_hwcc_used_bytes = 0;
  uint64_t physical_swcc_used_bytes = 0;
  uint64_t owner_private_swcc_used_bytes = 0;
  uint64_t shared_payload_swcc_used_bytes = 0;
  uint64_t allocator_hwcc_metadata_bytes = 0;
  uint64_t allocator_swcc_metadata_bytes = 0;
  uint64_t allocator_shared_overhead_bytes = 0;
  uint64_t allocator_local_dram_bytes = 0;
  uint64_t unclassified_shared_bytes = 0;
  uint64_t retired_pending_bytes = 0;
  uint64_t reclaimed_total_bytes = 0;
  uint64_t active_shared_rows = 0;
  uint64_t rss_kb = 0;
};

struct Config {
  std::string shared_memory_path = "/mnt/xz_shared_mem/ivshmem_shared_mem";
  std::string device_path = "/dev/ivpci0";
  uint64_t size_mb = 4096;
  uint64_t hwcc_offset_mb = 0;
  uint64_t hwcc_size_mb = 1024;
  uint64_t swcc_offset_mb = 1024;
  uint64_t swcc_size_mb = 3072;
  int32_t shared_memory_numa_node = 1;
  std::vector<uint32_t> host_reserved_cores;
  std::vector<uint32_t> ivshmem_server_cores;
  std::vector<uint32_t> vm_cores;
  uint32_t vm_count = 2;
  uint32_t vm_core_count_per_vm = 0;
  std::string vm_storage_path = "/mnt/xz_vm_storage";
  int32_t vm_numa_node = 0;
  uint32_t network_base_ssh_port = 2200;
  uint32_t sync_timeout_sec = 60;
  uint32_t foreground_worker_count_per_vm = 1;
  uint32_t node_id = 0;
  uint32_t partition_count = 16;
  uint32_t fixed_key_size = 32;
  uint32_t fixed_value_size = 1000;
  uint64_t hw_cc_budget_mb = 1024;
  double owner_private_swcc_fraction = 0.35;
  std::string migration_policy = "Clock";
  std::string when_to_move_out = "OnDemand";
  std::string scc_mechanism = "WriteThrough";
  uint64_t transport_ring_total_mb = 16;
  bool enable_scan = true;
  bool strict_swcc_access = false;
  bool checkpoint_on_clean_exit = true;
  bool verbose = false;
  bool extra_check = false;
  // Pin foreground workers and the inbound demuxer to distinct CPUs from the
  // process's allowed affinity mask. Performance configurations enable this.
  bool cpu_affinity = false;
  bool latency_enabled = false;
  bool latency_foreground_enabled = false;
  bool latency_merge_enabled = false;
  bool latency_stats_enabled = false;
  uint64_t latency_cache_line_bytes = 64;
  double swcc_read_ns = 0;
  double swcc_write_ns = 0;
  double swcc_flush_ns = 0;
  double hwcc_read_ns = 0;
  double hwcc_write_ns = 0;
  double hwcc_atomic_ns = 0;
  double hwcc_atomic_load_ns = 0;
  double hwcc_atomic_store_ns = 0;
  double hwcc_atomic_rmw_ns = 0;
  std::string latency_cache_model = "none";
  bool latency_cache_hits_enabled = false;
  double latency_cache_fixed_hit_rate = 0.0;
  uint64_t latency_cache_capacity_lines = 4096;
  uint64_t latency_cache_associativity = 8;
  double latency_cache_hit_extra_ns = 0;

  static Config FromJsonc(const std::string &path);
  void Validate() const;
};

class KVStore {
 public:
  static std::unique_ptr<KVStore> Create(const Config &config, bool reset = false);
  ~KVStore();

  KVStore(const KVStore &) = delete;
  KVStore &operator=(const KVStore &) = delete;

  Status Put(std::string_view key, std::string_view value);
  GetResult Get(std::string_view key);
  Status Delete(std::string_view key);
  // Owner-only logical move-out: the shared row becomes private again after
  // the caller has established that no remote references remain.
  Status MoveOut(std::string_view key);
  // Ordered cursor scan, matching cxlkv's non-transactional Scan contract:
  // keys are unique and strictly increasing, and migration alone is invisible.
  // This is not a multi-key snapshot; concurrent insert/delete may appear or
  // not according to whether it crosses the advancing cursor.
  ScanResult Scan(std::string_view start_key, uint64_t limit);
  CasResult CompareExchange(std::string_view key, std::string_view expected,
                            std::string_view desired);
  IncrementResult Increment(std::string_view key, int64_t delta);
  // Serve inbound owner requests while this foreground worker is otherwise idle.
  Status PollTransport();
  // Bind this OS thread as EBR worker `worker_id` (0 .. foreground_worker_count-1).
  // Required once per foreground worker thread before KV ops.
  void BindWorker(uint32_t worker_id);
  void ReleaseWorker();

  Status Checkpoint();
  MemoryStats Memory() const;
  RuntimeStats Runtime() const;
  uint32_t StablePartitionForKey(std::string_view key) const;
  uint32_t OwnerForKey(std::string_view key) const;
  std::string DumpStats() const;

 private:
  explicit KVStore(const Config &config);
  void Open(bool reset);
  void Close();
  void ValidateKeyValue(std::string_view key, std::string_view value) const;
  RuntimeStats &ThreadRuntime();
  struct Impl;
  struct alignas(64) WorkerRuntime {
    RuntimeStats stats;
  };
  std::unique_ptr<Impl> impl_;
  Config config_;
  // PLAN §1.8: one cache-line-isolated counter set per foreground worker.
  // BindWorker selects the calling thread's slot, so hot operations never
  // update a shared counter or execute an atomic RMW.
  std::vector<WorkerRuntime> worker_runtime_;
  WorkerRuntime unbound_runtime_;
};

}  // namespace tigonkv
