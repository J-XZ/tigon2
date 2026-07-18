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
};

struct MemoryStats {
  std::string allocator_mode = "global";
  bool physical_region_split = false;
  uint64_t total_pool_capacity_bytes = 0;
  uint64_t logical_hwcc_capacity_bytes = 0;
  uint64_t logical_swcc_capacity_bytes = 0;
  uint64_t logical_hwcc_used_bytes = 0;
  uint64_t owner_private_swcc_used_bytes = 0;
  uint64_t shared_payload_swcc_used_bytes = 0;
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
  uint64_t hwcc_reserved_mb = 0;
  double owner_private_swcc_fraction = 0.0;
  double shared_payload_swcc_fraction = 0.0;
  std::string migration_policy = "Clock";
  bool reuse_shared_payload_after_moveout = false;
  bool enable_scan = true;
  bool strict_swcc_access = false;
  bool checkpoint_on_clean_exit = true;
  bool latency_enabled = false;
  uint64_t swcc_read_ns = 0;
  uint64_t swcc_write_ns = 0;
  uint64_t swcc_flush_ns = 0;
  uint64_t hwcc_read_ns = 0;
  uint64_t hwcc_write_ns = 0;
  uint64_t hwcc_atomic_ns = 0;
  std::string latency_cache_model = "none";
  double latency_cache_fixed_hit_rate = 0.0;
  uint64_t latency_cache_capacity_lines = 0;

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
  ScanResult Scan(std::string_view start_key, uint64_t limit);
  CasResult CompareExchange(std::string_view key, std::string_view expected,
                            std::string_view desired);
  IncrementResult Increment(std::string_view key, int64_t delta);

  Status Checkpoint();
  MemoryStats Memory() const;
  RuntimeStats Runtime() const { return runtime_; }
  uint32_t StablePartitionForKey(std::string_view key) const;
  uint32_t OwnerForKey(std::string_view key) const;
  std::string DumpStats() const;

 private:
  explicit KVStore(const Config &config);
  void Open(bool reset);
  void Close();
  void ValidateKeyValue(std::string_view key, std::string_view value) const;
  struct Impl;
  std::unique_ptr<Impl> impl_;
  Config config_;
  RuntimeStats runtime_;
};

}  // namespace tigonkv
