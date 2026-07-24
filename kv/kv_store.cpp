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

constexpr uint64_t kMagic = 0x5449474f4e4b5632ULL;  // TIGONKV2
constexpr uint32_t kLayoutVersion = 4;
constexpr uint8_t kEmpty = 0, kPrivate = 1, kShared = 2, kTombstone = 3;
constexpr uint32_t kIndexTombstone = UINT32_MAX;
constexpr size_t kMaxKey = 256;
constexpr size_t kMaxValue = 4096;
constexpr uint64_t kSharedMetadataBytes = 128;

uint64_t HashBytes(std::string_view value) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : value) { h ^= c; h *= 1099511628211ULL; }
  return h;
}

uint64_t ConfigFingerprint(const Config &config) {
  uint64_t h = HashBytes(config.shared_memory_path) ^ (HashBytes(config.device_path) << 1);
  const uint64_t fields[] = {
      config.size_mb, config.hwcc_offset_mb, config.hwcc_size_mb,
      config.swcc_offset_mb, config.swcc_size_mb, config.vm_count,
      config.partition_count, config.fixed_key_size, config.fixed_value_size,
      static_cast<uint64_t>(config.shared_memory_numa_node + 1),
      config.vm_core_count_per_vm, static_cast<uint64_t>(config.vm_numa_node + 1),
      config.network_base_ssh_port, config.sync_timeout_sec,
      config.foreground_worker_count_per_vm, config.hw_cc_budget_mb,
      static_cast<uint64_t>(config.owner_private_swcc_fraction * 1000000000.0),
      config.transport_ring_total_mb};
  for (uint64_t field : fields) {
    h ^= field + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  }
  auto mix_string = [&h](const std::string &value) {
    h ^= HashBytes(value) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  };
  auto mix_core_list = [&h](const std::vector<uint32_t> &cores) {
    h ^= cores.size() + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    for (uint32_t core : cores)
      h ^= core + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  };
  mix_string(config.vm_storage_path);
  mix_core_list(config.host_reserved_cores);
  mix_core_list(config.ivshmem_server_cores);
  mix_core_list(config.vm_cores);
  h ^= HashBytes(config.migration_policy) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  h ^= HashBytes(config.when_to_move_out) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  h ^= HashBytes(config.scc_mechanism) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  return h;
}

uint64_t CurrentRssKb() {
  std::ifstream statm("/proc/self/statm");
  uint64_t total_pages = 0, resident_pages = 0;
  if (!(statm >> total_pages >> resident_pages)) return 0;
  const long page_size = ::sysconf(_SC_PAGESIZE);
  return page_size > 0 ? resident_pages * static_cast<uint64_t>(page_size) / 1024 : 0;
}

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

struct Slot {
  uint8_t state;
  uint32_t owner;
  uint16_t key_len;
  uint32_t value_len;
  uint64_t version;
  uint32_t ref_count;
  uint32_t generation;
  char key[kMaxKey];
  char value[kMaxValue];
};

struct SharedHeader {
  uint64_t magic;
  uint32_t layout_version;
  uint32_t init_state;  // 0 initializing, 1 clean, 2 active
  uint32_t allocator_mode;
  uint32_t vm_count;
  uint32_t partition_count;
  uint32_t fixed_key_size;
  uint32_t fixed_value_size;
  uint64_t config_hash;
  uint64_t total_pool_bytes;
  uint64_t logical_hwcc_capacity_bytes;
  uint64_t logical_swcc_capacity_bytes;
  uint64_t clean_epoch;
  uint64_t dirty_epoch;
  uint64_t attached_count;
  uint64_t active_shared_rows;
  uint64_t reclaimed_total_bytes;
  uint64_t retired_pending_bytes;
  uint64_t owner_private_payload_bytes;
  uint64_t shared_payload_bytes;
  uint64_t index_count;
  uint64_t sorted_index_capacity;
  uint64_t sorted_index_count;
  uint64_t next_slot_hint;
  // A guest futex cannot wait on a physical page owned by another guest.
  // Keep the protocol lock entirely in the shared BAR and acquire it with
  // inter-VM atomic operations instead of pthread_mutex/PTHREAD_PROCESS_SHARED.
  uint32_t lock_word;
};

static_assert(sizeof(Slot) < 5000, "slot accounting unexpectedly grew");

void SyncRangeOrThrow(const void *address, size_t length, const char *what,
                      bool device_backed = false) {
  if (length == 0) return;
  // A guest maps the ivshmem BAR through /dev/ivpci0 rather than a host file.
  // The PCI mapping is already the shared publication surface; msync is only
  // meaningful for the file-backed host-process fallback.
  if (device_backed) {
    __sync_synchronize();
    return;
  }
  const long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0) throw std::runtime_error("cannot determine page size");
  const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(address);
  const std::uintptr_t aligned = begin & ~static_cast<std::uintptr_t>(page_size - 1);
  const std::uintptr_t end = begin + length;
  const size_t span = static_cast<size_t>(end - aligned);
  if (::msync(reinterpret_cast<void *>(aligned), span, MS_SYNC) != 0)
    throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
  __sync_synchronize();
}

uint64_t CacheLineToken(const void *address) {
  return reinterpret_cast<std::uintptr_t>(address) / 64;
}

class Lock {
 public:
  explicit Lock(uint32_t *lock_word, LatencySimulator *latency = nullptr)
      : lock_word_(lock_word), latency_(latency) {
    for (;;) {
      uint32_t expected = 0;
      if (__atomic_compare_exchange_n(lock_word_, &expected, 1, false,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        break;
      for (unsigned spin = 0; spin < 256; ++spin)
        __asm__ __volatile__("pause" ::: "memory");
      sched_yield();
    }
  }
  ~Lock() {
    __atomic_store_n(lock_word_, 0, __ATOMIC_RELEASE);
    if (latency_ != nullptr) latency_->DrainPending();
  }
 private:
  uint32_t *lock_word_;
  LatencySimulator *latency_;
};

class FileLock {
 public:
  explicit FileLock(int fd) : fd_(fd) {
    if (flock(fd_, LOCK_EX) != 0)
      throw std::runtime_error(std::string("shared backing file lock: ") + std::strerror(errno));
  }
  ~FileLock() { if (fd_ >= 0) (void)flock(fd_, LOCK_UN); }
  FileLock(const FileLock &) = delete;
  FileLock &operator=(const FileLock &) = delete;
 private:
  int fd_;
};

}  // namespace

struct KVStore::Impl {
  // tigonkv: the only live storage implementation is KVEngine.  The legacy
  // slot fields remain temporarily below solely so the configuration parser
  // can be separated in the next cleanup checkpoint.
  std::unique_ptr<engine::KVEngine> engine;
  int fd = -1;
  size_t bytes = 0;
  void *mapping = MAP_FAILED;
  bool device_backed = false;
  bool attached = false;
  SharedHeader *header = nullptr;
  uint32_t *index = nullptr;
  uint32_t *sorted_index = nullptr;
  Slot *slots = nullptr;
  size_t slot_count = 0;
  mutable LatencySimulator latency;

  uint64_t LogicalHwccBytes(uint64_t active_rows) const {
    return sizeof(SharedHeader) + header->index_count * sizeof(uint32_t) +
           header->sorted_index_capacity * sizeof(uint32_t) +
           active_rows * kSharedMetadataBytes;
  }

  void ReclaimRetired() {
    // All public operations hold the process-shared mutex for their complete
    // row access.  A tombstoned row is therefore unreachable by the time the
    // next operation enters; this is the global-lock fast path of the EBR
    // grace period.  Keep the counters explicit so a future lock-free path
    // can replace this with per-reader epochs without changing accounting.
    if (header->retired_pending_bytes == 0) return;
    header->reclaimed_total_bytes += header->retired_pending_bytes;
    header->retired_pending_bytes = 0;
  }

  static uint64_t PayloadBytes(const Slot &row) {
    return static_cast<uint64_t>(row.key_len) + row.value_len;
  }

  bool HasPayloadCapacity(const Slot &row, uint64_t new_bytes) const {
    const uint64_t old_bytes = (row.state == kPrivate || row.state == kShared) ?
                               PayloadBytes(row) : 0;
    const uint64_t used = header->owner_private_payload_bytes + header->shared_payload_bytes;
    return used >= old_bytes && used - old_bytes + new_bytes <=
           header->logical_swcc_capacity_bytes;
  }

  void RemovePayload(const Slot &row) {
    const uint64_t bytes = PayloadBytes(row);
    if (row.state == kPrivate) {
      if (header->owner_private_payload_bytes < bytes)
        throw std::runtime_error("owner-private payload accounting underflow");
      header->owner_private_payload_bytes -= bytes;
    } else if (row.state == kShared) {
      if (header->shared_payload_bytes < bytes)
        throw std::runtime_error("shared payload accounting underflow");
      header->shared_payload_bytes -= bytes;
    }
  }

  void AddPayload(uint8_t state, uint64_t bytes) {
    if (state == kPrivate) header->owner_private_payload_bytes += bytes;
    else if (state == kShared) header->shared_payload_bytes += bytes;
  }

  void AcquireRef(Slot *row) {
    if (row->ref_count == UINT32_MAX)
      throw std::runtime_error("shared row reference count overflow");
    ++row->ref_count;
  }

  void ReleaseRef(Slot *row) {
    if (row->ref_count == 0)
      throw std::runtime_error("shared row reference count underflow");
    --row->ref_count;
  }

  void TransitionPayload(Slot *row, uint8_t new_state) {
    if (row->state == new_state) return;
    const uint64_t bytes = PayloadBytes(*row);
    RemovePayload(*row);
    row->state = new_state;
    AddPayload(new_state, bytes);
  }

  void SetPayload(Slot *row, uint32_t key_len, uint32_t value_len) {
    RemovePayload(*row);
    row->key_len = static_cast<uint16_t>(key_len);
    row->value_len = value_len;
    AddPayload(row->state, static_cast<uint64_t>(key_len) + value_len);
  }

  static std::string_view KeyOf(const Slot &row) {
    return std::string_view(row.key, row.key_len);
  }

  Slot *Find(std::string_view key) const {
    latency.Record(PoolKind::kHwcc, AccessKind::kRead, sizeof(uint32_t));
    if (header->index_count == 0) return nullptr;
    uint64_t bucket = HashBytes(key) & (header->index_count - 1);
    for (uint64_t probe = 0; probe < header->index_count; ++probe) {
      const uint32_t entry = index[(bucket + probe) & (header->index_count - 1)];
      if (entry == 0) return nullptr;
      if (entry == kIndexTombstone) continue;
      const size_t slot = static_cast<size_t>(entry - 1);
      if (slot >= slot_count) throw std::runtime_error("shared index points outside slot arena");
      Slot &row = slots[slot];
      if ((row.state == kPrivate || row.state == kShared) && row.key_len == key.size() &&
          std::memcmp(row.key, key.data(), key.size()) == 0) return &row;
    }
    return nullptr;
  }

  void PublishIndex(Slot *row) {
    latency.Record(PoolKind::kHwcc, AccessKind::kWrite, sizeof(uint32_t));
    const uint32_t slot = static_cast<uint32_t>(row - slots);
    uint64_t bucket = HashBytes(std::string_view(row->key, row->key_len)) & (header->index_count - 1);
    uint64_t tombstone = header->index_count;
    for (uint64_t probe = 0; probe < header->index_count; ++probe) {
      uint32_t &entry = index[(bucket + probe) & (header->index_count - 1)];
      if (entry == 0) { index[tombstone == header->index_count ? ((bucket + probe) & (header->index_count - 1)) : tombstone] = slot + 1; return; }
      if (entry == kIndexTombstone && tombstone == header->index_count) tombstone = (bucket + probe) & (header->index_count - 1);
      else if (entry == slot + 1) return;
    }
    if (tombstone != header->index_count) { index[tombstone] = slot + 1; return; }
    throw std::runtime_error("shared index is full");
  }

  void PublishSortedIndex(Slot *row) {
    if (std::getenv("TIGONKV_E2E_BATCH_SORTED_INDEX") != nullptr) return;
    latency.Record(PoolKind::kHwcc, AccessKind::kWrite, sizeof(uint32_t));
    if (header->sorted_index_count >= header->sorted_index_capacity)
      throw std::runtime_error("sorted shared index is full");
    const uint32_t slot = static_cast<uint32_t>(row - slots);
    size_t lo = 0;
    size_t hi = static_cast<size_t>(header->sorted_index_count);
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      const Slot &candidate = slots[sorted_index[mid] - 1];
      if (KeyOf(candidate) < KeyOf(*row)) lo = mid + 1;
      else hi = mid;
    }
    std::memmove(sorted_index + lo + 1, sorted_index + lo,
                 (header->sorted_index_count - lo) * sizeof(uint32_t));
    sorted_index[lo] = slot + 1;
    ++header->sorted_index_count;
  }

  void RetireIndex(Slot *row) {
    latency.Record(PoolKind::kHwcc, AccessKind::kWrite, sizeof(uint32_t));
    const uint32_t slot = static_cast<uint32_t>(row - slots);
    uint64_t bucket = HashBytes(std::string_view(row->key, row->key_len)) & (header->index_count - 1);
    for (uint64_t probe = 0; probe < header->index_count; ++probe) {
      uint32_t &entry = index[(bucket + probe) & (header->index_count - 1)];
      if (entry == 0) break;
      if (entry == slot + 1) { entry = kIndexTombstone; break; }
    }
    for (size_t i = 0; i < header->sorted_index_count; ++i) {
      if (sorted_index[i] != slot + 1) continue;
      std::memmove(sorted_index + i, sorted_index + i + 1,
                   (header->sorted_index_count - i - 1) * sizeof(uint32_t));
      --header->sorted_index_count;
      return;
    }
    throw std::runtime_error("shared sorted index missing row during retire");
  }

  Slot *AllocateSlot() {
    for (size_t probe = 0; probe < slot_count; ++probe) {
      const size_t pos = (static_cast<size_t>(header->next_slot_hint) + probe) % slot_count;
      if (slots[pos].state == kEmpty || slots[pos].state == kTombstone) {
        header->next_slot_hint = (pos + 1) % slot_count;
        return &slots[pos];
      }
    }
    return nullptr;
  }
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

#if 0  // tigonkv: retired slot/global-lock/msync engine; delete in M10 cleanup.
std::unique_ptr<KVStore> KVStore::Create(const Config &config, bool reset) {
  auto store = std::unique_ptr<KVStore>(new KVStore(config));
  try {
    store->Open(reset);
  } catch (...) {
    // Open can fail after mmap (for example on a dirty marker or config
    // mismatch).  The C++ destructor is not run for a failed construction, so
    // release every partially acquired descriptor/mapping here without
    // changing the persisted state.
    store->Close();
    throw;
  }
  return store;
}

KVStore::KVStore(const Config &config) : impl_(new Impl()), config_(config) {
  config_.Validate();
  impl_->latency.ConfigureDetailed(config_.latency_enabled, config_.hwcc_read_ns,
                                   config_.hwcc_write_ns, config_.hwcc_atomic_ns,
                                   config_.swcc_read_ns, config_.swcc_write_ns,
                                   config_.swcc_flush_ns, config_.latency_cache_model.c_str(),
                                   config_.latency_cache_fixed_hit_rate,
                                   config_.latency_cache_capacity_lines);
}

void KVStore::Open(bool reset) {
  // On a VM the ivshmem BAR is exposed as /dev/ivpci0.  On the host and in
  // unit tests that device is absent, so use the ordinary shared backing file.
  impl_->device_backed = ::access(config_.device_path.c_str(), F_OK) == 0;
  const std::string mapping_path = impl_->device_backed ? config_.device_path : config_.shared_memory_path;
  const int open_flags = O_RDWR | (impl_->device_backed ? 0 : O_CREAT);
  impl_->fd = ::open(mapping_path.c_str(), open_flags, 0660);
  if (impl_->fd < 0) throw std::runtime_error("open shared memory mapping: " + std::string(std::strerror(errno)));
  // The inter-VM atomic lock is not initialized until the first process has
  // built the layout. Serialize the file-size/layout/attach transition with an OS file
  // lock so concurrent workers cannot both observe an empty header and reset
  // the same backing.
  std::unique_ptr<FileLock> file_lock;
  if (!impl_->device_backed) file_lock = std::make_unique<FileLock>(impl_->fd);
  if (!impl_->device_backed && reset && ftruncate(impl_->fd, static_cast<off_t>(config_.size_mb * 1024ULL * 1024ULL)) != 0)
    throw std::runtime_error("resize backing file: " + std::string(std::strerror(errno)));
  const off_t expected_bytes = static_cast<off_t>(config_.size_mb * 1024ULL * 1024ULL);
  if (impl_->device_backed) {
    // Character devices do not expose the BAR length through fstat.  QEMU and
    // the guest driver establish the configured BAR size, so map exactly the
    // experiment's shared_memory.size_mb bytes.
    impl_->bytes = static_cast<size_t>(expected_bytes);
  } else {
    struct stat st{};
    if (fstat(impl_->fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(SharedHeader) + sizeof(Slot)))
      throw std::runtime_error("backing file is missing or too small; use an explicit reset");
    if (st.st_size != expected_bytes)
      throw std::runtime_error("backing file size does not match shared_memory.size_mb");
    impl_->bytes = static_cast<size_t>(st.st_size);
  }
  impl_->mapping = mmap(nullptr, impl_->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, impl_->fd, 0);
  if (impl_->mapping == MAP_FAILED) throw std::runtime_error("mmap backing file");
  impl_->header = static_cast<SharedHeader *>(impl_->mapping);
  const bool device_backing_was_zeroed =
      impl_->device_backed && reset &&
      std::getenv("TIGONKV_DEVICE_BACKING_ZEROED") != nullptr;
  if (reset && impl_->header->magic == kMagic && impl_->header->attached_count != 0)
    throw std::runtime_error("refusing pool reset while a TigonKV process is attached");
  if (reset || impl_->header->magic != kMagic) {
    // A fresh device BAR is normally zeroed by the host-side pool initializer.
    // Avoid touching every byte through a 32 GiB guest PCI mapping in that
    // explicitly declared case; regular files and untrusted device contents
    // retain the defensive full reset.
    if (device_backing_was_zeroed)
      std::memset(impl_->mapping, 0, sizeof(SharedHeader));
    else
      std::memset(impl_->mapping, 0, impl_->bytes);
    impl_->header->magic = kMagic;
    impl_->header->layout_version = kLayoutVersion;
    impl_->header->allocator_mode = 0;  // global logical allocator domains
    impl_->header->vm_count = config_.vm_count;
    impl_->header->partition_count = config_.partition_count;
    impl_->header->fixed_key_size = config_.fixed_key_size;
    impl_->header->fixed_value_size = config_.fixed_value_size;
    impl_->header->config_hash = ConfigFingerprint(config_);
    impl_->header->total_pool_bytes = impl_->bytes;
    impl_->header->logical_hwcc_capacity_bytes = config_.hwcc_size_mb * 1024ULL * 1024ULL;
    impl_->header->logical_swcc_capacity_bytes = config_.swcc_size_mb * 1024ULL * 1024ULL;
    uint64_t index_count = 1;
    for (;;) {
      const size_t available = impl_->bytes - sizeof(SharedHeader) -
                               index_count * sizeof(uint32_t);
      const size_t slot_capacity = available / (sizeof(Slot) + sizeof(uint32_t));
      if (slot_capacity == 0) throw std::invalid_argument("shared backing has no slot capacity");
      // Keep the open-addressed hash index at or below 50% load so that a
      // tombstone-heavy workload cannot make publication fail spuriously.
      if (index_count >= slot_capacity * 2) break;
      index_count <<= 1;
    }
    impl_->header->index_count = index_count;
    impl_->header->sorted_index_capacity =
        (impl_->bytes - sizeof(SharedHeader) - index_count * sizeof(uint32_t)) /
        (sizeof(Slot) + sizeof(uint32_t));
    impl_->header->sorted_index_count = 0;
    impl_->header->next_slot_hint = 0;
    if (impl_->header->index_count * sizeof(uint32_t) +
            impl_->header->sorted_index_capacity * sizeof(uint32_t) + sizeof(SharedHeader) >
        impl_->header->logical_hwcc_capacity_bytes)
      throw std::invalid_argument("shared indexes exceed logical HWCC budget");
    if (device_backing_was_zeroed) {
      char *index_begin = static_cast<char *>(impl_->mapping) + sizeof(SharedHeader);
      const size_t index_bytes = impl_->header->index_count * sizeof(uint32_t) +
                                 impl_->header->sorted_index_capacity * sizeof(uint32_t);
      std::memset(index_begin, 0, index_bytes);
      // ivshmem BAR writes are not guaranteed to update the server's original
      // memory-path file. Reset every slot's metadata in the guest mapping so
      // stale payload bytes can never be reached through an old index entry;
      // Put overwrites the value payload before publishing a new index entry.
      char *slot_begin = index_begin + index_bytes;
      const size_t slot_count =
          (impl_->bytes - sizeof(SharedHeader) - index_bytes) / sizeof(Slot);
      for (size_t slot = 0; slot < slot_count; ++slot)
        std::memset(slot_begin + slot * sizeof(Slot), 0, offsetof(Slot, key));
    }
    impl_->header->init_state = 1;
  } else {
    if (impl_->header->init_state == 2 && impl_->header->attached_count == 0)
      throw std::runtime_error("backing has a dirty active marker; clean checkpoint required");
    if (impl_->header->layout_version != kLayoutVersion ||
        impl_->header->vm_count != config_.vm_count ||
        impl_->header->partition_count != config_.partition_count ||
        impl_->header->fixed_key_size != config_.fixed_key_size ||
        impl_->header->fixed_value_size != config_.fixed_value_size ||
        impl_->header->config_hash != ConfigFingerprint(config_))
      throw std::runtime_error("shared layout/config mismatch");
  }
  impl_->index = reinterpret_cast<uint32_t *>(static_cast<char *>(impl_->mapping) + sizeof(SharedHeader));
  impl_->sorted_index = reinterpret_cast<uint32_t *>(static_cast<char *>(impl_->mapping) +
      sizeof(SharedHeader) + impl_->header->index_count * sizeof(uint32_t));
  impl_->slots = reinterpret_cast<Slot *>(static_cast<char *>(impl_->mapping) +
      sizeof(SharedHeader) + impl_->header->index_count * sizeof(uint32_t) +
      impl_->header->sorted_index_capacity * sizeof(uint32_t));
  impl_->slot_count = (impl_->bytes - sizeof(SharedHeader) -
      impl_->header->index_count * sizeof(uint32_t) -
      impl_->header->sorted_index_capacity * sizeof(uint32_t)) / sizeof(Slot);
  if (impl_->header->index_count == 0 ||
      (impl_->header->index_count & (impl_->header->index_count - 1)) != 0 ||
      impl_->header->sorted_index_capacity == 0 ||
      impl_->header->sorted_index_count > impl_->header->sorted_index_capacity ||
      impl_->header->sorted_index_count > impl_->slot_count || impl_->slot_count == 0)
    throw std::runtime_error("invalid shared index/layout capacity");
  if (impl_->header->owner_private_payload_bytes + impl_->header->shared_payload_bytes >
          impl_->header->logical_swcc_capacity_bytes ||
      impl_->LogicalHwccBytes(impl_->header->active_shared_rows) >
          impl_->header->logical_hwcc_capacity_bytes)
    throw std::runtime_error("persisted logical memory budget exceeded");
  {
    Lock lock(&impl_->header->lock_word, &impl_->latency);
    ++impl_->header->attached_count;
    impl_->header->init_state = 2;
    ++impl_->header->dirty_epoch;
    impl_->attached = true;
  }
}

void KVStore::Close() {
  if (!impl_) return;
  if (impl_->mapping == MAP_FAILED) {
    if (impl_->fd >= 0) ::close(impl_->fd);
    impl_->fd = -1;
    return;
  }
  if (!impl_->attached) {
    munmap(impl_->mapping, impl_->bytes);
    ::close(impl_->fd);
    impl_->mapping = MAP_FAILED;
    impl_->fd = -1;
    return;
  }

  bool checkpoint_ok = false;
  if (config_.checkpoint_on_clean_exit) {
    checkpoint_ok = Checkpoint().ok();
  }
  try {
    Lock lock(&impl_->header->lock_word, &impl_->latency);
    if (impl_->header->attached_count > 0) --impl_->header->attached_count;
    impl_->attached = false;
    if (impl_->header->attached_count == 0) {
      if (!checkpoint_ok) {
        // A failed or disabled checkpoint must remain dirty.  The next attach
        // must not mistake an unflushed process exit for clean state.
        impl_->header->init_state = 2;
        ++impl_->header->dirty_epoch;
      } else {
        impl_->header->init_state = 1;
        ++impl_->header->clean_epoch;
        try {
          SyncRangeOrThrow(impl_->mapping, impl_->bytes, "clean close write-back", impl_->device_backed);
        } catch (...) {
          // Destructors cannot report an I/O failure.  Preserve a hard dirty
          // marker so a later attach refuses to trust this backing.
          impl_->header->init_state = 2;
          ++impl_->header->dirty_epoch;
          throw;
        }
      }
    }
  } catch (...) {
    // Destructors cannot report errors. The dirty marker remains for the next attach.
  }
  munmap(impl_->mapping, impl_->bytes);
  ::close(impl_->fd);
  impl_->mapping = MAP_FAILED;
  impl_->fd = -1;
}

KVStore::~KVStore() { Close(); }

uint32_t KVStore::StablePartitionForKey(std::string_view key) const {
  return static_cast<uint32_t>(HashBytes(key) % config_.partition_count);
}
uint32_t KVStore::OwnerForKey(std::string_view key) const {
  return StablePartitionForKey(key) % config_.vm_count;
}

void KVStore::ValidateKeyValue(std::string_view key, std::string_view value) const {
  if (key.empty() || key.size() > config_.fixed_key_size || value.size() > config_.fixed_value_size)
    throw std::invalid_argument("key/value exceeds configured fixed size");
}

Status KVStore::Put(std::string_view key, std::string_view value) {
  ValidateKeyValue(key, value);
  Lock lock(&impl_->header->lock_word, &impl_->latency);
  impl_->ReclaimRetired();
  ++runtime_.logical_ops;
  Slot *row = impl_->Find(key);
  if (!row) row = impl_->AllocateSlot();
  if (!row) return Status::Error(StatusCode::kOutOfMemory, "shared pool has no free KV slot");
  if (!impl_->HasPayloadCapacity(*row, key.size() + value.size()))
    return Status::Error(StatusCode::kOutOfMemory, "logical SWCC payload budget exceeded");
  bool was_shared = row->state == kShared;
  bool existing = row->state != kEmpty && row->state != kTombstone;
  if (!existing) {
    row->owner = OwnerForKey(key);
    // A non-owner request is logically forwarded to the partition owner.
    // The owner creates the authoritative private row; no shared metadata is
    // needed until another VM subsequently reads or updates it.
    row->state = kPrivate;
    row->version = 0;
  } else if (!was_shared && row->owner != config_.node_id) {
    if (impl_->LogicalHwccBytes(impl_->header->active_shared_rows + 1) > impl_->header->logical_hwcc_capacity_bytes)
      return Status::Error(StatusCode::kOutOfMemory, "logical HWCC row-metadata budget exceeded");
    impl_->TransitionPayload(row, kShared);
    ++impl_->header->active_shared_rows;
    ++runtime_.migration_in;
    was_shared = true;
  }
  impl_->SetPayload(row, key.size(), value.size());
  std::memcpy(row->key, key.data(), key.size());
  std::memset(row->value, 0, kMaxValue);
  std::memcpy(row->value, value.data(), value.size());
  ++row->version;
  ++runtime_.commits;
  if (was_shared) {
    ++runtime_.shared_puts;
    ++runtime_.shared_swcc_flushes;
    impl_->latency.Record(PoolKind::kSwcc, AccessKind::kWrite, value.size(), CacheLineToken(row));
    impl_->latency.Record(PoolKind::kSwcc, AccessKind::kFlush, sizeof(Slot), CacheLineToken(row));
    SyncRangeOrThrow(row, sizeof(Slot), "shared promotion write-back", impl_->device_backed);
  } else {
    ++runtime_.private_puts;
    impl_->latency.Record(PoolKind::kSwcc, AccessKind::kWrite, value.size(), CacheLineToken(row));
  }
  if (!existing) { impl_->PublishIndex(row); impl_->PublishSortedIndex(row); }
  return Status::Ok();
}

GetResult KVStore::Get(std::string_view key) {
  if (key.empty() || key.size() > config_.fixed_key_size) return {Status::Error(StatusCode::kInvalidArgument, "invalid key"), {}};
  Lock lock(&impl_->header->lock_word, &impl_->latency);
  impl_->ReclaimRetired();
  ++runtime_.logical_ops;
  if (Slot *found = impl_->Find(key)) {
    Slot &row = *found;
      if (row.state == kPrivate && row.owner != config_.node_id) {
        if (config_.strict_swcc_access) return {Status::Error(StatusCode::kOwnerViolation, "non-owner private SWCC access"), {}};
        if (impl_->LogicalHwccBytes(impl_->header->active_shared_rows + 1) > impl_->header->logical_hwcc_capacity_bytes)
          return {Status::Error(StatusCode::kOutOfMemory, "logical HWCC row-metadata budget exceeded"), {}};
        impl_->TransitionPayload(&row, kShared); ++impl_->header->active_shared_rows; ++runtime_.migration_in;
        // Publication is the linearization point: the complete source value is
        // write-backed before another VM can rely on the shared state.
        ++runtime_.shared_swcc_flushes;
        impl_->latency.Record(PoolKind::kSwcc, AccessKind::kFlush, sizeof(Slot), CacheLineToken(&row));
        SyncRangeOrThrow(&row, sizeof(Slot), "shared promotion write-back", impl_->device_backed);
      }
      bool shared = row.state == kShared;
      impl_->AcquireRef(&row);
      const std::string value(row.value, row.value_len);
      impl_->ReleaseRef(&row);
      if (shared) { ++runtime_.shared_gets; impl_->latency.Record(PoolKind::kSwcc, AccessKind::kRead, value.size(), CacheLineToken(&row)); }
      else { ++runtime_.private_gets; impl_->latency.Record(PoolKind::kSwcc, AccessKind::kRead, value.size(), CacheLineToken(&row)); }
      ++runtime_.commits;
      return {Status::Ok(), value};
  }
  ++runtime_.commits;
  return {Status::Error(StatusCode::kNotFound, "key not found"), {}};
}

Status KVStore::Delete(std::string_view key) {
  ValidateKeyValue(key, {});
  Lock lock(&impl_->header->lock_word, &impl_->latency);
  impl_->ReclaimRetired();
  ++runtime_.logical_ops;
  if (Slot *found = impl_->Find(key)) {
    Slot &row = *found;
      bool shared = row.state == kShared;
      if (!shared && row.owner != config_.node_id) {
        if (config_.strict_swcc_access) return Status::Error(StatusCode::kOwnerViolation, "non-owner private SWCC access");
        if (impl_->LogicalHwccBytes(impl_->header->active_shared_rows + 1) > impl_->header->logical_hwcc_capacity_bytes)
          return Status::Error(StatusCode::kOutOfMemory, "logical HWCC row-metadata budget exceeded");
        impl_->TransitionPayload(&row, kShared); ++impl_->header->active_shared_rows; ++runtime_.migration_in; shared = true;
      }
      if (row.ref_count != 0)
        return Status::Error(StatusCode::kOwnerViolation, "cannot retire a referenced row");
      impl_->RetireIndex(&row);
      impl_->RemovePayload(row);
      row.state = kTombstone; row.key_len = 0; row.value_len = 0; ++row.generation;
      const size_t slot = static_cast<size_t>(&row - impl_->slots);
      if (slot < impl_->header->next_slot_hint) impl_->header->next_slot_hint = slot;
      if (shared) {
        if (impl_->header->active_shared_rows) --impl_->header->active_shared_rows;
        ++runtime_.shared_deletes;
        ++runtime_.shared_swcc_flushes;
        impl_->latency.Record(PoolKind::kSwcc, AccessKind::kFlush, sizeof(Slot), CacheLineToken(&row));
        SyncRangeOrThrow(&row, sizeof(Slot), "shared delete write-back", impl_->device_backed);
      }
      else ++runtime_.private_deletes;
      impl_->header->retired_pending_bytes += sizeof(Slot); ++runtime_.commits;
      return Status::Ok();
  }
  ++runtime_.commits;
  return Status::Error(StatusCode::kNotFound, "key not found");
}

Status KVStore::MoveOut(std::string_view key) {
  ValidateKeyValue(key, {});
  Lock lock(&impl_->header->lock_word, &impl_->latency);
  impl_->ReclaimRetired();
  ++runtime_.logical_ops;
  Slot *row = impl_->Find(key);
  if (!row) {
    ++runtime_.commits;
    return Status::Error(StatusCode::kNotFound, "key not found");
  }
  if (row->owner != config_.node_id)
    return Status::Error(StatusCode::kOwnerViolation, "only the partition owner may move out a row");
  if (row->state == kPrivate) {
    ++runtime_.commits;
    return Status::Ok();
  }
  if (row->state != kShared || row->ref_count != 0)
    return Status::Error(StatusCode::kOwnerViolation, "shared row still has active references");
  impl_->TransitionPayload(row, kPrivate);
  if (impl_->header->active_shared_rows == 0)
    throw std::runtime_error("shared row accounting underflow during move-out");
  --impl_->header->active_shared_rows;
  ++runtime_.migration_out;
  ++runtime_.commits;
  return Status::Ok();
}

ScanResult KVStore::Scan(std::string_view start_key, uint64_t limit) {
  if (!config_.enable_scan) return {Status::Error(StatusCode::kInvalidArgument, "SCAN disabled"), {}};
  Lock lock(&impl_->header->lock_word, &impl_->latency);
  impl_->ReclaimRetired();
  std::vector<ScanItem> all;
  size_t lo = 0;
  size_t hi = static_cast<size_t>(impl_->header->sorted_index_count);
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const Slot &row = impl_->slots[impl_->sorted_index[mid] - 1];
    if (std::string_view(row.key, row.key_len) < start_key) lo = mid + 1;
    else hi = mid;
  }
  for (size_t i = lo; i < impl_->header->sorted_index_count && all.size() < limit; ++i) {
    Slot &row = impl_->slots[impl_->sorted_index[i] - 1];
    if (row.state == kPrivate && row.owner != config_.node_id) {
      if (config_.strict_swcc_access)
        return {Status::Error(StatusCode::kOwnerViolation, "non-owner private SWCC access"), {}};
      if (impl_->LogicalHwccBytes(impl_->header->active_shared_rows + 1) >
          impl_->header->logical_hwcc_capacity_bytes)
        return {Status::Error(StatusCode::kOutOfMemory, "logical HWCC row-metadata budget exceeded"), {}};
      impl_->TransitionPayload(&row, kShared);
      ++impl_->header->active_shared_rows;
      ++runtime_.migration_in;
      ++runtime_.shared_swcc_flushes;
      impl_->latency.Record(PoolKind::kSwcc, AccessKind::kFlush, sizeof(Slot), CacheLineToken(&row));
      SyncRangeOrThrow(&row, sizeof(Slot), "shared scan promotion write-back", impl_->device_backed);
    }
    impl_->AcquireRef(&row);
    const std::string value(row.value, row.value_len);
    impl_->ReleaseRef(&row);
    if (row.state == kPrivate) ++runtime_.private_gets;
    else ++runtime_.shared_gets;
    impl_->latency.Record(PoolKind::kSwcc, AccessKind::kRead, value.size(),
                          CacheLineToken(&row));
    all.push_back({std::string(row.key, row.key_len), value});
  }
  ++runtime_.logical_ops; ++runtime_.commits;
  return {Status::Ok(), std::move(all)};
}

CasResult KVStore::CompareExchange(std::string_view key, std::string_view expected, std::string_view desired) {
  ValidateKeyValue(key, desired);
  Lock lock(&impl_->header->lock_word, &impl_->latency);
  impl_->ReclaimRetired();
  ++runtime_.logical_ops;
  Slot *row = nullptr;
  row = impl_->Find(key);
  const bool match = row ? std::string_view(row->value, row->value_len) == expected : expected.empty();
  if (!match) { ++runtime_.aborts; return {Status::Error(StatusCode::kCompareFailed, "CAS comparison failed"), false}; }
  if (!row) row = impl_->AllocateSlot();
  if (!row) return {Status::Error(StatusCode::kOutOfMemory, "shared pool has no free KV slot"), false};
  if (!impl_->HasPayloadCapacity(*row, key.size() + desired.size()))
    return {Status::Error(StatusCode::kOutOfMemory, "logical SWCC payload budget exceeded"), false};
  const bool new_row = row->state == kEmpty || row->state == kTombstone;
  if (row->state == kPrivate && row->owner != config_.node_id) {
    if (config_.strict_swcc_access) return {Status::Error(StatusCode::kOwnerViolation, "non-owner private SWCC access"), false};
    if (impl_->LogicalHwccBytes(impl_->header->active_shared_rows + 1) > impl_->header->logical_hwcc_capacity_bytes)
      return {Status::Error(StatusCode::kOutOfMemory, "logical HWCC row-metadata budget exceeded"), false};
    impl_->TransitionPayload(row, kShared); ++impl_->header->active_shared_rows; ++runtime_.migration_in;
    ++runtime_.shared_swcc_flushes;
    impl_->latency.Record(PoolKind::kSwcc, AccessKind::kFlush, sizeof(Slot), CacheLineToken(row));
    SyncRangeOrThrow(row, sizeof(Slot), "shared CAS promotion write-back", impl_->device_backed);
  }
  if (row->state == kEmpty || row->state == kTombstone) {
    row->owner = OwnerForKey(key);
    row->state = kPrivate;
    row->key_len = 0;
  }
  impl_->SetPayload(row, key.size(), desired.size());
  std::memcpy(row->key, key.data(), key.size());
  std::memset(row->value, 0, kMaxValue); std::memcpy(row->value, desired.data(), desired.size()); ++row->version;
  if (row->state == kShared) {
    ++runtime_.shared_puts;
    ++runtime_.shared_swcc_flushes;
    impl_->latency.Record(PoolKind::kSwcc, AccessKind::kFlush, sizeof(Slot), CacheLineToken(row));
    SyncRangeOrThrow(row, sizeof(Slot), "shared CAS write-back", impl_->device_backed);
  }
  else ++runtime_.private_puts;
  if (new_row) { impl_->PublishIndex(row); impl_->PublishSortedIndex(row); }
  ++runtime_.commits;
  return {Status::Ok(), true};
}

IncrementResult KVStore::Increment(std::string_view key, int64_t delta) {
  ValidateKeyValue(key, {});
  Lock lock(&impl_->header->lock_word, &impl_->latency);
  impl_->ReclaimRetired();
  ++runtime_.logical_ops;
  Slot *row = impl_->Find(key);
  int64_t old = 0;
  if (row && row->state == kPrivate && row->owner != config_.node_id) {
    if (config_.strict_swcc_access) return {Status::Error(StatusCode::kOwnerViolation, "non-owner private SWCC access"), 0};
    if (impl_->LogicalHwccBytes(impl_->header->active_shared_rows + 1) > impl_->header->logical_hwcc_capacity_bytes)
      return {Status::Error(StatusCode::kOutOfMemory, "logical HWCC row-metadata budget exceeded"), 0};
    impl_->TransitionPayload(row, kShared); ++impl_->header->active_shared_rows; ++runtime_.migration_in;
    ++runtime_.shared_swcc_flushes;
    impl_->latency.Record(PoolKind::kSwcc, AccessKind::kFlush, sizeof(Slot), CacheLineToken(row));
    SyncRangeOrThrow(row, sizeof(Slot), "shared increment promotion write-back", impl_->device_backed);
  }
  if (row) { auto result = std::from_chars(row->value, row->value + row->value_len, old); if (result.ec != std::errc()) return {Status::Error(StatusCode::kInvalidArgument, "value is not an integer"), 0}; }
  int64_t value = old + delta;
  std::string encoded = std::to_string(value);
  if (encoded.size() > config_.fixed_value_size) return {Status::Error(StatusCode::kInvalidArgument, "increment overflows fixed value size"), 0};
  if (!row) row = impl_->AllocateSlot();
  if (!row) return {Status::Error(StatusCode::kOutOfMemory, "shared pool has no free KV slot"), 0};
  if (!impl_->HasPayloadCapacity(*row, key.size() + encoded.size()))
    return {Status::Error(StatusCode::kOutOfMemory, "logical SWCC payload budget exceeded"), 0};
  const bool new_row = row->state == kEmpty || row->state == kTombstone;
  if (row->state == kEmpty || row->state == kTombstone) {
    row->owner = OwnerForKey(key);
    row->state = kPrivate;
    row->key_len = 0;
  }
  impl_->SetPayload(row, key.size(), encoded.size());
  std::memcpy(row->key, key.data(), key.size());
  std::memcpy(row->value, encoded.data(), encoded.size()); ++row->version;
  if (row->state == kShared) {
    ++runtime_.shared_puts;
    ++runtime_.shared_swcc_flushes;
    impl_->latency.Record(PoolKind::kSwcc, AccessKind::kFlush, sizeof(Slot), CacheLineToken(row));
    SyncRangeOrThrow(row, sizeof(Slot), "shared increment write-back", impl_->device_backed);
  }
  else ++runtime_.private_puts;
  if (new_row) { impl_->PublishIndex(row); impl_->PublishSortedIndex(row); }
  ++runtime_.commits;
  return {Status::Ok(), value};
}

Status KVStore::Checkpoint() {
  if (impl_->mapping == MAP_FAILED) return Status::Error(StatusCode::kCorruption, "store is closed");
  try {
    // Serialize the write-back with every KV operation.  Syncing before the
    // lock could race a writer and produce a checkpoint that was older than
    // the operation which had already acquired the protocol lock.
    Lock lock(&impl_->header->lock_word, &impl_->latency);
    impl_->ReclaimRetired();
    SyncRangeOrThrow(impl_->mapping, impl_->bytes, "checkpoint write-back", impl_->device_backed);
    impl_->latency.Record(PoolKind::kSwcc, AccessKind::kFlush,
                          config_.swcc_size_mb * 1024ULL * 1024ULL);
    ++impl_->header->clean_epoch;
    ++runtime_.checkpoint_swcc_flushes;
    return Status::Ok();
  } catch (const std::exception &error) {
    return Status::Error(StatusCode::kCorruption, error.what());
  }
}

Status KVStore::RebuildSortedIndex() {
  if (impl_->mapping == MAP_FAILED)
    return Status::Error(StatusCode::kCorruption, "store is closed");
  try {
    Lock lock(&impl_->header->lock_word, &impl_->latency);
    std::vector<uint32_t> entries;
    entries.reserve(static_cast<size_t>(impl_->header->sorted_index_count));
    for (uint64_t i = 0; i < impl_->header->index_count; ++i) {
      const uint32_t entry = impl_->index[i];
      if (entry == 0 || entry == kIndexTombstone) continue;
      const size_t slot = static_cast<size_t>(entry - 1);
      if (slot >= impl_->slot_count)
        return Status::Error(StatusCode::kCorruption, "shared index points outside slot arena");
      const Slot &row = impl_->slots[slot];
      if (row.state == kPrivate || row.state == kShared) entries.push_back(entry);
    }
    if (entries.size() > impl_->header->sorted_index_capacity)
      return Status::Error(StatusCode::kOutOfMemory, "sorted shared index is full");
    std::sort(entries.begin(), entries.end(), [&](uint32_t left, uint32_t right) {
      return std::string_view(impl_->slots[left - 1].key, impl_->slots[left - 1].key_len) <
             std::string_view(impl_->slots[right - 1].key, impl_->slots[right - 1].key_len);
    });
    if (!entries.empty())
      std::memcpy(impl_->sorted_index, entries.data(), entries.size() * sizeof(uint32_t));
    impl_->header->sorted_index_count = entries.size();
    return Status::Ok();
  } catch (const std::exception &error) {
    return Status::Error(StatusCode::kCorruption, error.what());
  }
}

MemoryStats KVStore::Memory() const {
  Lock lock(&impl_->header->lock_word);
  impl_->ReclaimRetired();
  MemoryStats out;
  out.total_pool_capacity_bytes = impl_->bytes;
  out.logical_hwcc_capacity_bytes = config_.hwcc_size_mb * 1024ULL * 1024ULL;
  out.logical_swcc_capacity_bytes = config_.swcc_size_mb * 1024ULL * 1024ULL;
  out.logical_hwcc_used_bytes = impl_->LogicalHwccBytes(impl_->header->active_shared_rows);
  out.active_shared_rows = impl_->header->active_shared_rows;
  out.allocator_shared_overhead_bytes = sizeof(SharedHeader) +
      (impl_->header->index_count + impl_->header->sorted_index_capacity) * sizeof(uint32_t);
  out.allocator_local_dram_bytes = sizeof(Impl) + sizeof(Config) + sizeof(RuntimeStats);
  out.reclaimed_total_bytes = impl_->header->reclaimed_total_bytes;
  out.retired_pending_bytes = impl_->header->retired_pending_bytes;
  out.rss_kb = CurrentRssKb();
  out.owner_private_swcc_used_bytes = impl_->header->owner_private_payload_bytes;
  out.shared_payload_swcc_used_bytes = impl_->header->shared_payload_bytes;
  return out;
}

std::string KVStore::DumpStats() const {
  MemoryStats m = Memory();
  std::string out = "TIGONKV_MEMORY_STATS\n";
  out += "allocator_mode=" + m.allocator_mode + "\nphysical_region_split=0\n";
  out += "total_pool_capacity_bytes=" + std::to_string(m.total_pool_capacity_bytes) + "\n";
  out += "logical_hwcc_capacity_bytes=" + std::to_string(m.logical_hwcc_capacity_bytes) + "\n";
  out += "logical_swcc_capacity_bytes=" + std::to_string(m.logical_swcc_capacity_bytes) + "\n";
  out += "logical_hwcc_used_bytes=" + std::to_string(m.logical_hwcc_used_bytes) + "\n";
  out += "owner_private_swcc_used_bytes=" + std::to_string(m.owner_private_swcc_used_bytes) + "\n";
  out += "shared_payload_swcc_used_bytes=" + std::to_string(m.shared_payload_swcc_used_bytes) + "\n";
  out += "allocator_shared_overhead_bytes=" + std::to_string(m.allocator_shared_overhead_bytes) + "\n";
  out += "allocator_local_dram_bytes=" + std::to_string(m.allocator_local_dram_bytes) + "\n";
  out += "unclassified_shared_bytes=0\nretired_pending_bytes=" + std::to_string(m.retired_pending_bytes) + "\n";
  out += "reclaimed_total_bytes=" + std::to_string(m.reclaimed_total_bytes) + "\nactive_shared_rows=" + std::to_string(m.active_shared_rows) + "\n";
  out += "rss_kb=" + std::to_string(m.rss_kb) + "\n";
  out += "TIGONKV_RUNTIME_STATS\nnode=" + std::to_string(config_.node_id) + "\nlogical_ops=" + std::to_string(runtime_.logical_ops) + "\ncommits=" + std::to_string(runtime_.commits) + "\naborts=" + std::to_string(runtime_.aborts) + "\nretries=" + std::to_string(runtime_.retries) + "\nprivate_gets=" + std::to_string(runtime_.private_gets) + "\nprivate_puts=" + std::to_string(runtime_.private_puts) + "\nprivate_deletes=" + std::to_string(runtime_.private_deletes) + "\nprivate_swcc_flushes=" + std::to_string(runtime_.private_swcc_flushes) + "\ncheckpoint_swcc_flushes=" + std::to_string(runtime_.checkpoint_swcc_flushes) + "\nshared_gets=" + std::to_string(runtime_.shared_gets) + "\nshared_puts=" + std::to_string(runtime_.shared_puts) + "\nshared_deletes=" + std::to_string(runtime_.shared_deletes) + "\nshared_swcc_flushes=" + std::to_string(runtime_.shared_swcc_flushes) + "\nmigration_in=" + std::to_string(runtime_.migration_in) + "\nmigration_out=" + std::to_string(runtime_.migration_out) + "\nnetwork_tx_bytes=0\nnetwork_rx_bytes=0\n";
  const LatencyStats latency = impl_->latency.Snapshot();
  out += "TIGONKV_LATENCY_STATS\nhwcc_reads=" + std::to_string(latency.hwcc_reads) + "\nhwcc_writes=" + std::to_string(latency.hwcc_writes) + "\nhwcc_atomics=" + std::to_string(latency.hwcc_atomics) + "\nswcc_reads=" + std::to_string(latency.swcc_reads) + "\nswcc_writes=" + std::to_string(latency.swcc_writes) + "\nswcc_flushes=" + std::to_string(latency.swcc_flushes) + "\ncache_hits=" + std::to_string(latency.cache_hits) + "\ncache_misses=" + std::to_string(latency.cache_misses) + "\ndelayed_ns=" + std::to_string(latency.delayed_ns) + "\n";
  return out;
}

#endif

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
    stats.network_tx_bytes = impl_->engine->NetworkTxBytes();
    stats.network_rx_bytes = impl_->engine->NetworkRxBytes();
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
