#include "kv/kv_store.h"

#include "kv/engine/kv_engine.h"
#include "kv/latency_simulator.h"
#include "kv/memory_domains.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <sched.h>
#include <unordered_set>

namespace tigonkv {
namespace {

constexpr size_t kMaxKey = 256;
constexpr size_t kMaxValue = 4096;

std::string StripComments(std::string text) {
  text = std::regex_replace(text, std::regex(R"(//[^\n\r]*)"), "");
  text = std::regex_replace(text, std::regex(R"((/\*)(.|\n|\r)*?\*/)"), "");
  return text;
}

template <typename T>
bool JsonNumber(const std::string &s, const char *name, T *out) {
  std::regex r(std::string("\\\"") + name + R"(\"\s*:\s*(-?[0-9]+))");
  std::smatch m;
  if (!std::regex_search(s, m, r)) return false;
  long long value = std::stoll(m[1].str());
  if (value < 0) throw std::invalid_argument(std::string("negative config field: ") + name);
  *out = static_cast<T>(value);
  return true;
}

bool JsonBool(const std::string &s, const char *name, bool *out) {
  std::regex r(std::string("\\\"") + name + R"(\"\s*:\s*(true|false))");
  std::smatch m;
  if (!std::regex_search(s, m, r)) return false;
  *out = m[1].str() == "true";
  return true;
}

bool JsonString(const std::string &s, const char *name, std::string *out) {
  std::regex r(std::string("\\\"") + name + R"(\"\s*:\s*\"([^\"]*)\")");
  std::smatch m;
  if (!std::regex_search(s, m, r)) return false;
  *out = m[1].str();
  return true;
}

bool JsonDouble(const std::string &s, const char *name, double *out) {
  std::regex r(std::string("\\\"") + name + R"(\"\s*:\s*([0-9]+(?:\.[0-9]+)?))");
  std::smatch m;
  if (!std::regex_search(s, m, r)) return false;
  *out = std::stod(m[1].str());
  return true;
}

template <typename T>
bool JsonNumberArray(const std::string &s, const char *name, std::vector<T> *out) {
  std::regex r(std::string("\\\"") + name + R"(\"\s*:\s*\[([^\]]*)\])");
  std::smatch m;
  if (!std::regex_search(s, m, r)) return false;
  out->clear();
  std::regex number(R"(-?[0-9]+)");
  for (auto it = std::sregex_iterator(m[1].first, m[1].second, number);
       it != std::sregex_iterator(); ++it) {
    const long long value = std::stoll((*it)[0].str());
    if (value < 0) throw std::invalid_argument(std::string("negative config array field: ") + name);
    out->push_back(static_cast<T>(value));
  }
  return true;
}

template <typename T>
bool JsonNumberInObject(const std::string &s, const char *object, const char *name, T *out) {
  std::regex object_re(std::string("\\\"") + object + R"(\"\s*:\s*\{([^}]*)\})");
  std::smatch object_match;
  if (!std::regex_search(s, object_match, object_re)) return false;
  return JsonNumber(object_match[1].str(), name, out);
}

void ValidateKnownKeys(const std::string &s) {
  static const std::unordered_set<std::string> known = {
      "shared_memory", "size_mb", "path", "device_path", "numa_node",
      "hwcc", "offset_mb", "swcc", "host_cpu", "reserved_cores",
      "ivshmem_server_cores", "vm_cores", "vm", "count", "core_count_per_vm",
      "storage_path", "mem_size_mb_per_vm", "first_ip", "bridge_tap_ip",
      "copy_root_img", "use_ivshmem_doorbell", "local_ssh_pub_key",
      "network", "base_ssh_port", "sync", "e2e",
      "foreground_worker_count_per_vm", "tigon_kv", "partition_count",
      "timeout_sec",
      "fixed_key_size", "fixed_value_size", "hw_cc_budget_mb",
      "owner_private_swcc_fraction", "migration_policy", "when_to_move_out",
      "scc_mechanism", "transport_ring_total_mb",
      "latency_inject", "enabled", "foreground_enabled", "merge_enabled",
      "stats_enabled", "cache_line_bytes", "swcc_read_ns_per_line",
      "swcc_write_ns_per_line", "swcc_flush_ns_per_line", "hwcc_read_ns_per_line",
      "hwcc_write_ns_per_line", "hwcc_atomic_load_ns", "hwcc_atomic_store_ns",
      "hwcc_atomic_rmw_ns", "cache_model", "cache_hits_enabled",
      "cache_capacity_lines", "cache_associativity", "cache_fixed_hit_rate",
      "cache_hit_extra_ns"};
  std::regex key(R"KEY("([A-Za-z0-9_.-]+)"\s*:)KEY");
  for (auto it = std::sregex_iterator(s.begin(), s.end(), key); it != std::sregex_iterator(); ++it) {
    if (!known.count((*it)[1].str()))
      throw std::invalid_argument("unknown experiment_config field: " + (*it)[1].str());
  }
}

}  // namespace

struct KVStore::Impl {
  std::unique_ptr<engine::KVEngine> engine;
};
Config Config::FromJsonc(const std::string &path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open experiment config: " + path);
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  text = StripComments(std::move(text));
  ValidateKnownKeys(text);
  Config c;
  JsonString(text, "path", &c.shared_memory_path);
  JsonString(text, "device_path", &c.device_path);
  JsonNumber(text, "size_mb", &c.size_mb);
  JsonNumberInObject(text, "shared_memory", "numa_node", &c.shared_memory_numa_node);
  JsonNumberArray(text, "reserved_cores", &c.host_reserved_cores);
  JsonNumberArray(text, "ivshmem_server_cores", &c.ivshmem_server_cores);
  JsonNumberArray(text, "vm_cores", &c.vm_cores);
  JsonNumber(text, "count", &c.vm_count);
  JsonNumberInObject(text, "vm", "core_count_per_vm", &c.vm_core_count_per_vm);
  JsonString(text, "storage_path", &c.vm_storage_path);
  JsonNumberInObject(text, "vm", "numa_node", &c.vm_numa_node);
  JsonNumber(text, "base_ssh_port", &c.network_base_ssh_port);
  JsonNumber(text, "timeout_sec", &c.sync_timeout_sec);
  JsonNumber(text, "foreground_worker_count_per_vm", &c.foreground_worker_count_per_vm);
  JsonNumber(text, "partition_count", &c.partition_count);
  JsonNumber(text, "fixed_key_size", &c.fixed_key_size);
  JsonNumber(text, "fixed_value_size", &c.fixed_value_size);
  JsonNumber(text, "hw_cc_budget_mb", &c.hw_cc_budget_mb);
  JsonDouble(text, "owner_private_swcc_fraction", &c.owner_private_swcc_fraction);
  JsonString(text, "migration_policy", &c.migration_policy);
  JsonString(text, "when_to_move_out", &c.when_to_move_out);
  JsonString(text, "scc_mechanism", &c.scc_mechanism);
  JsonNumber(text, "transport_ring_total_mb", &c.transport_ring_total_mb);
  JsonNumberInObject(text, "hwcc", "offset_mb", &c.hwcc_offset_mb);
  JsonNumberInObject(text, "hwcc", "size_mb", &c.hwcc_size_mb);
  JsonNumberInObject(text, "swcc", "offset_mb", &c.swcc_offset_mb);
  JsonNumberInObject(text, "swcc", "size_mb", &c.swcc_size_mb);
  JsonBool(text, "enabled", &c.latency_enabled);
  JsonBool(text, "foreground_enabled", &c.latency_foreground_enabled);
  JsonBool(text, "merge_enabled", &c.latency_merge_enabled);
  JsonBool(text, "stats_enabled", &c.latency_stats_enabled);
  JsonNumber(text, "cache_line_bytes", &c.latency_cache_line_bytes);
  JsonNumber(text, "swcc_read_ns_per_line", &c.swcc_read_ns);
  JsonNumber(text, "swcc_write_ns_per_line", &c.swcc_write_ns);
  JsonNumber(text, "swcc_flush_ns_per_line", &c.swcc_flush_ns);
  JsonNumber(text, "hwcc_read_ns_per_line", &c.hwcc_read_ns);
  JsonNumber(text, "hwcc_write_ns_per_line", &c.hwcc_write_ns);
  JsonNumber(text, "hwcc_atomic_load_ns", &c.hwcc_atomic_load_ns);
  JsonNumber(text, "hwcc_atomic_store_ns", &c.hwcc_atomic_store_ns);
  JsonNumber(text, "hwcc_atomic_rmw_ns", &c.hwcc_atomic_rmw_ns);
  c.hwcc_atomic_ns = std::max({c.hwcc_atomic_load_ns, c.hwcc_atomic_store_ns,
                                c.hwcc_atomic_rmw_ns});
  JsonString(text, "cache_model", &c.latency_cache_model);
  JsonBool(text, "cache_hits_enabled", &c.latency_cache_hits_enabled);
  JsonDouble(text, "cache_fixed_hit_rate", &c.latency_cache_fixed_hit_rate);
  JsonNumber(text, "cache_capacity_lines", &c.latency_cache_capacity_lines);
  JsonNumber(text, "cache_associativity", &c.latency_cache_associativity);
  JsonNumber(text, "cache_hit_extra_ns", &c.latency_cache_hit_extra_ns);
  if (c.shared_memory_path == "/mnt/xz_shared_mem" || c.shared_memory_path == "/mnt/xz_shared_mem/")
    c.shared_memory_path = "/mnt/xz_shared_mem/ivshmem_shared_mem";
  c.Validate();
  return c;
}

void Config::Validate() const {
  if (size_mb == 0 || hwcc_size_mb > 1024 || hwcc_size_mb > size_mb ||
      swcc_size_mb > size_mb || hwcc_offset_mb + hwcc_size_mb > size_mb ||
      swcc_offset_mb + swcc_size_mb > size_mb || fixed_value_size == 0 ||
      (hwcc_offset_mb < swcc_offset_mb + swcc_size_mb &&
       swcc_offset_mb < hwcc_offset_mb + hwcc_size_mb))
    throw std::invalid_argument("invalid shared-memory capacity or HWCC budget");
  if (vm_count == 0 || partition_count == 0 || fixed_key_size == 0 || fixed_key_size > kMaxKey ||
      fixed_value_size > kMaxValue || shared_memory_numa_node < -1 || vm_numa_node < -1 ||
      network_base_ssh_port == 0 || sync_timeout_sec == 0 || foreground_worker_count_per_vm == 0 ||
      foreground_worker_count_per_vm > 64 || vm_count > 8)
    throw std::invalid_argument("invalid KV configuration");
  if (hw_cc_budget_mb == 0 || hw_cc_budget_mb > hwcc_size_mb ||
      owner_private_swcc_fraction <= 0.0 || owner_private_swcc_fraction >= 1.0 ||
      node_id >= vm_count ||
      transport_ring_total_mb == 0 || transport_ring_total_mb > hwcc_size_mb ||
      migration_policy != "Clock" || when_to_move_out != "OnDemand" ||
      scc_mechanism != "WriteThrough")
    throw std::invalid_argument("invalid allocator budget fractions");
  if (latency_cache_model != "none" && latency_cache_model != "fixed_hit_rate" &&
      latency_cache_model != "per_thread_lru")
    throw std::invalid_argument("unknown latency cache model");
  if (latency_cache_fixed_hit_rate < 0.0 || latency_cache_fixed_hit_rate > 1.0)
    throw std::invalid_argument("latency cache hit rate must be in [0,1]");
  if (latency_cache_line_bytes != 64 || latency_cache_associativity == 0)
    throw std::invalid_argument("invalid latency cache geometry");
}


namespace {
class ForegroundLatencyScope {
 public:
  ForegroundLatencyScope() { GlobalLatencySimulator().BeginScope(); }
  ~ForegroundLatencyScope() { GlobalLatencySimulator().EndScopeAndDelay(); }
};
}  // namespace

std::unique_ptr<KVStore> KVStore::Create(const Config &config, bool reset) {
  auto store = std::unique_ptr<KVStore>(new KVStore(config));
  store->Open(reset);
  return store;
}

KVStore::KVStore(const Config &config) : impl_(new Impl()), config_(config) {
  config_.Validate();
  GlobalLatencySimulator().ConfigureDetailed(
      config_.latency_enabled, config_.hwcc_read_ns, config_.hwcc_write_ns,
      config_.hwcc_atomic_ns, config_.swcc_read_ns, config_.swcc_write_ns,
      config_.swcc_flush_ns, config_.latency_cache_model.c_str(),
      config_.latency_cache_fixed_hit_rate, config_.latency_cache_capacity_lines);
}

KVStore::~KVStore() { Close(); }

void KVStore::Open(bool reset) {
  impl_->engine = engine::KVEngine::Open(config_, reset);
}

void KVStore::Close() {
  if (impl_ == nullptr || impl_->engine == nullptr) return;
  if (config_.checkpoint_on_clean_exit) (void)impl_->engine->Checkpoint();
  impl_->engine.reset();
}

uint32_t KVStore::StablePartitionForKey(std::string_view key) const {
  uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : key) { hash ^= c; hash *= 1099511628211ULL; }
  return static_cast<uint32_t>(hash % config_.partition_count);
}

uint32_t KVStore::OwnerForKey(std::string_view key) const {
  return StablePartitionForKey(key) % config_.vm_count;
}

void KVStore::ValidateKeyValue(std::string_view key, std::string_view value) const {
  if (key.empty() || key.size() > config_.fixed_key_size ||
      value.size() > config_.fixed_value_size)
    throw std::invalid_argument("key/value exceeds configured fixed size");
}

Status KVStore::Put(std::string_view key, std::string_view value) {
  ForegroundLatencyScope latency_scope;
  impl_->engine->PollTransport();
  try { ValidateKeyValue(key, value); }
  catch (const std::exception &e) { return Status::Error(StatusCode::kInvalidArgument, e.what()); }
  ++runtime_.logical_ops;
  Status status = impl_->engine->Put(key, value);
  if (status.ok()) { ++runtime_.commits; ++runtime_.private_puts; }
  else ++runtime_.aborts;
  return status;
}

GetResult KVStore::Get(std::string_view key) {
  ForegroundLatencyScope latency_scope;
  impl_->engine->PollTransport();
  if (key.empty() || key.size() > config_.fixed_key_size)
    return {Status::Error(StatusCode::kInvalidArgument, "invalid key"), {}};
  ++runtime_.logical_ops;
  GetResult result = impl_->engine->Get(key);
  if (result.status.ok()) { ++runtime_.commits; ++runtime_.private_gets; }
  else if (result.status.code != StatusCode::kNotFound) ++runtime_.aborts;
  return result;
}

Status KVStore::Delete(std::string_view key) {
  ForegroundLatencyScope latency_scope;
  impl_->engine->PollTransport();
  if (key.empty() || key.size() > config_.fixed_key_size)
    return Status::Error(StatusCode::kInvalidArgument, "invalid key");
  ++runtime_.logical_ops;
  Status status = impl_->engine->Delete(key);
  if (status.ok()) { ++runtime_.commits; ++runtime_.private_deletes; }
  else if (status.code != StatusCode::kNotFound) ++runtime_.aborts;
  return status;
}

Status KVStore::MoveOut(std::string_view key) {
  ForegroundLatencyScope latency_scope;
  impl_->engine->PollTransport();
  ++runtime_.logical_ops;
  Status status = impl_->engine->MoveOut(key);
  if (status.ok()) { ++runtime_.commits; ++runtime_.migration_out; }
  return status;
}

ScanResult KVStore::Scan(std::string_view start_key, uint64_t limit) {
  ForegroundLatencyScope latency_scope;
  impl_->engine->PollTransport();
  if (!config_.enable_scan)
    return {Status::Error(StatusCode::kInvalidArgument, "SCAN disabled"), {}};
  ++runtime_.logical_ops;
  ScanResult result = impl_->engine->Scan(start_key, limit);
  if (result.status.ok()) ++runtime_.commits;
  else ++runtime_.aborts;
  return result;
}

CasResult KVStore::CompareExchange(std::string_view key, std::string_view expected,
                                   std::string_view desired) {
  ForegroundLatencyScope latency_scope;
  impl_->engine->PollTransport();
  try { ValidateKeyValue(key, desired); }
  catch (const std::exception &e) { return {Status::Error(StatusCode::kInvalidArgument, e.what()), false}; }
  ++runtime_.logical_ops;
  CasResult result = impl_->engine->CompareExchange(key, expected, desired);
  if (result.status.ok()) { ++runtime_.commits; ++runtime_.private_puts; }
  else if (result.status.code == StatusCode::kCompareFailed) ++runtime_.aborts;
  return result;
}

IncrementResult KVStore::Increment(std::string_view key, int64_t delta) {
  ForegroundLatencyScope latency_scope;
  impl_->engine->PollTransport();
  if (key.empty() || key.size() > config_.fixed_key_size)
    return {Status::Error(StatusCode::kInvalidArgument, "invalid key"), 0};
  ++runtime_.logical_ops;
  IncrementResult result = impl_->engine->Increment(key, delta);
  if (result.status.ok()) { ++runtime_.commits; ++runtime_.private_puts; }
  else ++runtime_.aborts;
  return result;
}

Status KVStore::Checkpoint() {
  ForegroundLatencyScope latency_scope;
  Status status = impl_->engine->Checkpoint();
  if (status.ok()) ++runtime_.checkpoint_swcc_flushes;
  return status;
}

Status KVStore::RebuildSortedIndex() { return Status::Ok(); }

MemoryStats KVStore::Memory() const { return impl_->engine->Memory(); }

RuntimeStats KVStore::Runtime() const {
  RuntimeStats stats = runtime_;
  if (impl_ != nullptr && impl_->engine != nullptr) {
    const RuntimeStats engine = impl_->engine->EngineRuntime();
    stats.shared_gets += engine.shared_gets;
    stats.shared_puts += engine.shared_puts;
    stats.shared_deletes += engine.shared_deletes;
    stats.shared_swcc_flushes += engine.shared_swcc_flushes;
    stats.migration_in += engine.migration_in;
    stats.migration_out += engine.migration_out;
    stats.network_tx_bytes = engine.network_tx_bytes;
    stats.network_rx_bytes = engine.network_rx_bytes;
  }
  return stats;
}

std::string KVStore::DumpStats() const {
  const MemoryStats memory = Memory();
  const RuntimeStats runtime = Runtime();
  std::string out = "TIGONKV_MEMORY_STATS\n";
  out += "allocator_mode=" + memory.allocator_mode + "\n";
  out += "physical_region_split=" + std::to_string(memory.physical_region_split) + "\n";
  out += "total_pool_capacity_bytes=" + std::to_string(memory.total_pool_capacity_bytes) + "\n";
  out += "logical_hwcc_capacity_bytes=" + std::to_string(memory.logical_hwcc_capacity_bytes) + "\n";
  out += "logical_swcc_capacity_bytes=" + std::to_string(memory.logical_swcc_capacity_bytes) + "\n";
  out += "logical_hwcc_used_bytes=" + std::to_string(memory.logical_hwcc_used_bytes) + "\n";
  out += "owner_private_swcc_used_bytes=" + std::to_string(memory.owner_private_swcc_used_bytes) + "\n";
  out += "shared_payload_swcc_used_bytes=" + std::to_string(memory.shared_payload_swcc_used_bytes) + "\n";
  out += "allocator_shared_overhead_bytes=" + std::to_string(memory.allocator_shared_overhead_bytes) + "\n";
  out += "allocator_local_dram_bytes=" + std::to_string(memory.allocator_local_dram_bytes) + "\n";
  out += "unclassified_shared_bytes=" + std::to_string(memory.unclassified_shared_bytes) + "\n";
  out += "retired_pending_bytes=" + std::to_string(memory.retired_pending_bytes) + "\n";
  out += "reclaimed_total_bytes=" + std::to_string(memory.reclaimed_total_bytes) + "\n";
  out += "active_shared_rows=" + std::to_string(memory.active_shared_rows) + "\n";
  out += "rss_kb=" + std::to_string(memory.rss_kb) + "\n";
  out += "TIGONKV_RUNTIME_STATS\nnode=" + std::to_string(config_.node_id) + "\n";
  out += "logical_ops=" + std::to_string(runtime.logical_ops) + "\n";
  out += "commits=" + std::to_string(runtime.commits) + "\n";
  out += "aborts=" + std::to_string(runtime.aborts) + "\n";
  out += "retries=" + std::to_string(runtime.retries) + "\n";
  out += "private_gets=" + std::to_string(runtime.private_gets) + "\n";
  out += "private_puts=" + std::to_string(runtime.private_puts) + "\n";
  out += "private_deletes=" + std::to_string(runtime.private_deletes) + "\n";
  out += "checkpoint_swcc_flushes=" + std::to_string(runtime.checkpoint_swcc_flushes) + "\n";
  out += "private_swcc_flushes=" + std::to_string(runtime.private_swcc_flushes) + "\n";
  out += "shared_gets=" + std::to_string(runtime.shared_gets) + "\n";
  out += "shared_puts=" + std::to_string(runtime.shared_puts) + "\n";
  out += "shared_deletes=" + std::to_string(runtime.shared_deletes) + "\n";
  out += "shared_swcc_flushes=" + std::to_string(runtime.shared_swcc_flushes) + "\n";
  out += "migration_in=" + std::to_string(runtime.migration_in) + "\n";
  out += "migration_out=" + std::to_string(runtime.migration_out) + "\n";
  out += "network_tx_bytes=" + std::to_string(runtime.network_tx_bytes) + "\n";
  out += "network_rx_bytes=" + std::to_string(runtime.network_rx_bytes) + "\n";
  const LatencyStats latency = GlobalLatencySimulator().Snapshot();
  out += "TIGONKV_LATENCY_STATS\nhwcc_reads=" + std::to_string(latency.hwcc_reads) +
      "\nhwcc_writes=" + std::to_string(latency.hwcc_writes) +
      "\nhwcc_atomics=" + std::to_string(latency.hwcc_atomics) +
      "\nswcc_reads=" + std::to_string(latency.swcc_reads) +
      "\nswcc_writes=" + std::to_string(latency.swcc_writes) +
      "\nswcc_flushes=" + std::to_string(latency.swcc_flushes) +
      "\ncache_hits=" + std::to_string(latency.cache_hits) +
      "\ncache_misses=" + std::to_string(latency.cache_misses) +
      "\ndelayed_ns=" + std::to_string(latency.delayed_ns) + "\n";
  return out;
}

}  // namespace tigonkv
