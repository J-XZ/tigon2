#pragma once

#include <latency_sim/access.h>
#include <latency_sim/atomic_access.h>
#include <latency_sim/config.h>
#include <latency_sim/domain.h>
#include <latency_sim/scope.h>
#include <latency_sim/simulator.h>

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
  // A cooperative transport response was not received within
  // Config::transport_response_timeout_ms.  Remote operations roll back their
  // intermediate row state and return this error.
  kTimeout,
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
  // Minimal Scan diagnostics (§13 / §14.18); TLS or once-per-op aggregates.
  uint64_t scan_ops = 0;
  uint64_t scan_partition_probes = 0;
  uint64_t scan_migrate_rpcs = 0;
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
  // Physical HWCC region size vs per-owner Clock dynamic limit (§11.10).
  uint64_t physical_hwcc_capacity_bytes = 0;
  uint64_t owner_migration_dynamic_budget_bytes = 0;
  uint64_t rss_kb = 0;
};

struct Config {
  struct PartitionRange {
    // Empty lower/upper are respectively -infinity/+infinity.  Non-empty
    // bounds are external string keys and describe [lower_key, upper_key).
    std::string lower_key;
    std::string upper_key;
  };
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
  // Bounded cooperative-wait deadline for a remote RPC response.  A missing
  // owner (crash/partition/disconnect) must fail with kTimeout instead of
  // busy-looping forever; remote operations roll back their intermediate
  // state on timeout.
  uint32_t transport_response_timeout_ms = 60000;
  uint32_t foreground_worker_count_per_vm = 1;
  uint32_t node_id = 0;
  uint32_t partition_count = 16;
  // The experiment interface has one logical table, range partitioned in the
  // same order as the original Tigon YCSB partitioner.  Config files must
  // supply exactly partition_count contiguous ranges; this vector is also
  // deliberately available to small in-process test configurations.
  std::vector<PartitionRange> partition_ranges;
  uint32_t fixed_key_size = 32;
  uint32_t fixed_value_size = 1000;
  uint64_t hw_cc_budget_mb = 1024;
  double owner_private_swcc_fraction = 0.35;
  std::string migration_policy = "Clock";
  std::string when_to_move_out = "OnDemand";
  std::string scc_mechanism = "WriteThrough";
  uint64_t transport_ring_total_mb = 16;
  bool verbose = false;
  bool extra_check = false;
  // Pin foreground workers and the inbound demuxer to distinct CPUs from the
  // process's allowed affinity mask. Performance configurations enable this.
  bool cpu_affinity = false;
  // Canonical fixed-latency-only configuration. JSONC is the only
  // configuration surface; removed statistics/cache-model fields are rejected.
  latency_sim::FixedLatencyConfig hardware_simulation{};

  static Config FromJsonc(const std::string &path);
  // Normalizes non-infinite range boundaries into their persisted FixedKey
  // representation, then validates the resulting contiguous range map.
  // Config is immutable after construction/open, so routing never depends on
  // a second parser or a process-local key cache.
  void Validate();
  uint32_t PartitionForKey(std::string_view key) const;
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
  // Ordered cursor scan, matching cxlkv's non-transactional Scan contract:
  // keys are unique and strictly increasing, and migration alone is invisible.
  // This is not a multi-key snapshot; concurrent insert/delete may appear or
  // not according to whether it crosses the advancing cursor.
  ScanResult Scan(std::string_view start_key, std::string_view end_key,
                  uint64_t limit);
  CasResult CompareExchange(std::string_view key, std::string_view expected,
                            std::string_view desired);
  IncrementResult Increment(std::string_view key, int64_t delta);
  // Serve inbound owner requests while this foreground worker is otherwise idle.
  Status PollTransport();
  // Bind this OS thread as EBR worker `worker_id` (0 .. foreground_worker_count-1).
  // Required once per foreground worker thread before KV ops.
  void BindWorker(uint32_t worker_id);
  void ReleaseWorker();

  MemoryStats Memory() const;
  RuntimeStats Runtime() const;
  // The engine owns the sole worker runtime slot.  KVStore only forwards its
  // facade counters to the currently bound foreground worker.
  RuntimeStats &CurrentWorkerRuntime();
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
  std::unique_ptr<Impl> impl_;
  Config config_;
};

}  // namespace tigonkv
