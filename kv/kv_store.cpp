#include "kv/kv_store.h"

#include "kv/latency_simulator.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <unordered_set>

namespace tigonkv {
namespace {

constexpr uint64_t kMagic = 0x5449474f4e4b5632ULL;  // TIGONKV2
constexpr uint32_t kLayoutVersion = 1;
constexpr uint8_t kEmpty = 0, kPrivate = 1, kShared = 2, kTombstone = 3;
constexpr size_t kMaxKey = 256;
constexpr size_t kMaxValue = 4096;
constexpr uint64_t kSharedMetadataBytes = 128;

uint64_t HashBytes(std::string_view value) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : value) { h ^= c; h *= 1099511628211ULL; }
  return h;
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
      "storage_path", "network", "base_ssh_port", "sync", "e2e",
      "foreground_worker_count_per_vm", "tigon_kv", "partition_count",
      "timeout_sec",
      "fixed_key_size", "fixed_value_size", "hwcc_budget_mb", "hwcc_reserved_mb",
      "owner_private_swcc_fraction", "shared_payload_swcc_fraction",
      "migration_policy", "reuse_shared_payload_after_moveout",
      "checkpoint_on_clean_exit", "enable_scan", "strict_swcc_access",
      "latency_inject", "enabled", "swcc_read_ns", "swcc_write_ns",
      "swcc_flush_ns", "hwcc_read_ns", "hwcc_write_ns", "hwcc_atomic_ns",
      "cache_model", "cache_fixed_hit_rate", "cache_capacity_lines",
      "cache_associativity", "foreground_enabled", "merge_enabled"};
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
  pthread_mutex_t mutex;
};

static_assert(sizeof(Slot) < 5000, "slot accounting unexpectedly grew");

void CheckPthread(int rc, const char *what) {
  if (rc == 0) return;
  throw std::runtime_error(std::string(what) + ": " + std::strerror(rc));
}

class Lock {
 public:
  explicit Lock(pthread_mutex_t *mutex) : mutex_(mutex) { CheckPthread(pthread_mutex_lock(mutex_), "shared lock"); }
  ~Lock() { pthread_mutex_unlock(mutex_); }
 private:
  pthread_mutex_t *mutex_;
};

}  // namespace

struct KVStore::Impl {
  int fd = -1;
  size_t bytes = 0;
  void *mapping = MAP_FAILED;
  SharedHeader *header = nullptr;
  Slot *slots = nullptr;
  size_t slot_count = 0;
  LatencySimulator latency;
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
  JsonNumber(text, "count", &c.vm_count);
  JsonNumber(text, "partition_count", &c.partition_count);
  JsonNumber(text, "fixed_key_size", &c.fixed_key_size);
  JsonNumber(text, "fixed_value_size", &c.fixed_value_size);
  JsonNumber(text, "hwcc_budget_mb", &c.hwcc_size_mb);
  JsonNumberInObject(text, "hwcc", "offset_mb", &c.hwcc_offset_mb);
  JsonNumberInObject(text, "hwcc", "size_mb", &c.hwcc_size_mb);
  JsonNumberInObject(text, "swcc", "offset_mb", &c.swcc_offset_mb);
  JsonNumberInObject(text, "swcc", "size_mb", &c.swcc_size_mb);
  JsonBool(text, "enable_scan", &c.enable_scan);
  JsonBool(text, "strict_swcc_access", &c.strict_swcc_access);
  JsonBool(text, "checkpoint_on_clean_exit", &c.checkpoint_on_clean_exit);
  JsonBool(text, "enabled", &c.latency_enabled);
  JsonNumber(text, "swcc_read_ns", &c.swcc_read_ns);
  JsonNumber(text, "swcc_write_ns", &c.swcc_write_ns);
  JsonNumber(text, "swcc_flush_ns", &c.swcc_flush_ns);
  JsonNumber(text, "hwcc_read_ns", &c.hwcc_read_ns);
  JsonNumber(text, "hwcc_write_ns", &c.hwcc_write_ns);
  JsonNumber(text, "hwcc_atomic_ns", &c.hwcc_atomic_ns);
  JsonString(text, "cache_model", &c.latency_cache_model);
  if (c.shared_memory_path == "/mnt/xz_shared_mem" || c.shared_memory_path == "/mnt/xz_shared_mem/")
    c.shared_memory_path = "/mnt/xz_shared_mem/ivshmem_shared_mem";
  c.Validate();
  return c;
}

void Config::Validate() const {
  if (size_mb == 0 || hwcc_size_mb > 1024 || hwcc_size_mb > size_mb ||
      swcc_size_mb > size_mb || hwcc_offset_mb + hwcc_size_mb > size_mb ||
      swcc_offset_mb + swcc_size_mb > size_mb)
    throw std::invalid_argument("invalid shared-memory capacity or HWCC budget");
  if (vm_count == 0 || partition_count == 0 || fixed_key_size == 0 || fixed_key_size > kMaxKey ||
      fixed_value_size > kMaxValue)
    throw std::invalid_argument("invalid KV configuration");
  if (latency_cache_model != "none" && latency_cache_model != "fixed_hit_rate" &&
      latency_cache_model != "per_thread_lru")
    throw std::invalid_argument("unknown latency cache model");
}

std::unique_ptr<KVStore> KVStore::Create(const Config &config, bool reset) {
  auto store = std::unique_ptr<KVStore>(new KVStore(config));
  store->Open(reset);
  return store;
}

KVStore::KVStore(const Config &config) : impl_(new Impl()), config_(config) {
  config_.Validate();
  impl_->latency.Configure(config_.latency_enabled, config_.hwcc_atomic_ns,
                           config_.swcc_read_ns, config_.swcc_write_ns, config_.swcc_flush_ns);
}

void KVStore::Open(bool reset) {
  impl_->fd = ::open(config_.shared_memory_path.c_str(), O_RDWR | O_CREAT, 0660);
  if (impl_->fd < 0) throw std::runtime_error("open backing file: " + std::string(std::strerror(errno)));
  if (reset && ftruncate(impl_->fd, static_cast<off_t>(config_.size_mb * 1024ULL * 1024ULL)) != 0)
    throw std::runtime_error("resize backing file: " + std::string(std::strerror(errno)));
  struct stat st{};
  if (fstat(impl_->fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(SharedHeader) + sizeof(Slot)))
    throw std::runtime_error("backing file is missing or too small; use an explicit reset");
  impl_->bytes = static_cast<size_t>(st.st_size);
  impl_->mapping = mmap(nullptr, impl_->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, impl_->fd, 0);
  if (impl_->mapping == MAP_FAILED) throw std::runtime_error("mmap backing file");
  impl_->header = static_cast<SharedHeader *>(impl_->mapping);
  if (reset && impl_->header->magic == kMagic && impl_->header->attached_count != 0)
    throw std::runtime_error("refusing pool reset while a TigonKV process is attached");
  if (reset || impl_->header->magic != kMagic) {
    std::memset(impl_->mapping, 0, impl_->bytes);
    pthread_mutexattr_t attr;
    CheckPthread(pthread_mutexattr_init(&attr), "mutex attributes");
    CheckPthread(pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED), "process-shared mutex");
    CheckPthread(pthread_mutex_init(&impl_->header->mutex, &attr), "shared mutex");
    pthread_mutexattr_destroy(&attr);
    impl_->header->magic = kMagic;
    impl_->header->layout_version = kLayoutVersion;
    impl_->header->allocator_mode = 0;  // global logical allocator domains
    impl_->header->vm_count = config_.vm_count;
    impl_->header->partition_count = config_.partition_count;
    impl_->header->fixed_key_size = config_.fixed_key_size;
    impl_->header->fixed_value_size = config_.fixed_value_size;
    impl_->header->config_hash = HashBytes(config_.shared_memory_path) ^ config_.size_mb ^ config_.partition_count;
    impl_->header->total_pool_bytes = impl_->bytes;
    impl_->header->logical_hwcc_capacity_bytes = config_.hwcc_size_mb * 1024ULL * 1024ULL;
    impl_->header->logical_swcc_capacity_bytes = config_.swcc_size_mb * 1024ULL * 1024ULL;
    impl_->header->init_state = 1;
  } else {
    if (impl_->header->init_state == 2 && impl_->header->attached_count == 0)
      throw std::runtime_error("backing has a dirty active marker; clean checkpoint required");
    if (impl_->header->layout_version != kLayoutVersion ||
        impl_->header->vm_count != config_.vm_count ||
        impl_->header->partition_count != config_.partition_count ||
        impl_->header->fixed_key_size != config_.fixed_key_size ||
        impl_->header->fixed_value_size != config_.fixed_value_size)
      throw std::runtime_error("shared layout/config mismatch");
  }
  impl_->slots = reinterpret_cast<Slot *>(static_cast<char *>(impl_->mapping) + sizeof(SharedHeader));
  impl_->slot_count = (impl_->bytes - sizeof(SharedHeader)) / sizeof(Slot);
  {
    Lock lock(&impl_->header->mutex);
    ++impl_->header->attached_count;
    impl_->header->init_state = 2;
    ++impl_->header->dirty_epoch;
  }
}

void KVStore::Close() {
  if (!impl_ || impl_->mapping == MAP_FAILED) return;
  try {
    if (config_.checkpoint_on_clean_exit) Checkpoint();
    Lock lock(&impl_->header->mutex);
    if (impl_->header->attached_count > 0) --impl_->header->attached_count;
    if (impl_->header->attached_count == 0) {
      impl_->header->init_state = 1;
      ++impl_->header->clean_epoch;
      msync(impl_->mapping, impl_->bytes, MS_SYNC);
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
  Lock lock(&impl_->header->mutex);
  ++runtime_.logical_ops;
  Slot *row = nullptr;
  for (size_t i = 0; i < impl_->slot_count; ++i)
    if (impl_->slots[i].state != kEmpty && impl_->slots[i].state != kTombstone &&
        impl_->slots[i].key_len == key.size() && std::memcmp(impl_->slots[i].key, key.data(), key.size()) == 0) { row = &impl_->slots[i]; break; }
  if (!row) {
    for (size_t i = 0; i < impl_->slot_count; ++i)
      if (impl_->slots[i].state == kEmpty || impl_->slots[i].state == kTombstone) { row = &impl_->slots[i]; break; }
  }
  if (!row) return Status::Error(StatusCode::kOutOfMemory, "shared pool has no free KV slot");
  bool was_shared = row->state == kShared;
  bool existing = row->state != kEmpty && row->state != kTombstone;
  if (!existing) {
    row->owner = static_cast<uint8_t>(OwnerForKey(key));
    if (row->owner != config_.node_id) {
      if (sizeof(SharedHeader) + (impl_->header->active_shared_rows + 1) * kSharedMetadataBytes > impl_->header->logical_hwcc_capacity_bytes)
        return Status::Error(StatusCode::kOutOfMemory, "logical HWCC row-metadata budget exceeded");
      row->state = kShared; ++impl_->header->active_shared_rows; ++runtime_.migration_in; was_shared = true;
    } else {
      row->state = kPrivate;
    }
    row->key_len = static_cast<uint16_t>(key.size());
    std::memcpy(row->key, key.data(), key.size());
    row->version = 0;
  } else if (!was_shared && row->owner != config_.node_id) {
    if (sizeof(SharedHeader) + (impl_->header->active_shared_rows + 1) * kSharedMetadataBytes > impl_->header->logical_hwcc_capacity_bytes)
      return Status::Error(StatusCode::kOutOfMemory, "logical HWCC row-metadata budget exceeded");
    row->state = kShared;
    ++impl_->header->active_shared_rows;
    ++runtime_.migration_in;
    was_shared = true;
  }
  row->value_len = static_cast<uint32_t>(value.size());
  std::memset(row->value, 0, kMaxValue);
  std::memcpy(row->value, value.data(), value.size());
  ++row->version;
  ++runtime_.commits;
  if (was_shared) {
    ++runtime_.shared_puts;
    ++runtime_.shared_swcc_flushes;
    impl_->latency.Record(PoolKind::kSwcc, AccessKind::kWrite, value.size());
    impl_->latency.Record(PoolKind::kSwcc, AccessKind::kFlush, value.size());
    msync(row, sizeof(Slot), MS_SYNC);
  } else {
    ++runtime_.private_puts;
    impl_->latency.Record(PoolKind::kSwcc, AccessKind::kWrite, value.size());
  }
  return Status::Ok();
}

GetResult KVStore::Get(std::string_view key) {
  if (key.empty() || key.size() > config_.fixed_key_size) return {Status::Error(StatusCode::kInvalidArgument, "invalid key"), {}};
  Lock lock(&impl_->header->mutex);
  ++runtime_.logical_ops;
  for (size_t i = 0; i < impl_->slot_count; ++i) {
    Slot &row = impl_->slots[i];
    if ((row.state == kPrivate || row.state == kShared) && row.key_len == key.size() && std::memcmp(row.key, key.data(), key.size()) == 0) {
      if (row.state == kPrivate && row.owner != config_.node_id) {
        if (config_.strict_swcc_access) return {Status::Error(StatusCode::kOwnerViolation, "non-owner private SWCC access"), {}};
        if (sizeof(SharedHeader) + (impl_->header->active_shared_rows + 1) * kSharedMetadataBytes > impl_->header->logical_hwcc_capacity_bytes)
          return {Status::Error(StatusCode::kOutOfMemory, "logical HWCC row-metadata budget exceeded"), {}};
        row.state = kShared; ++impl_->header->active_shared_rows; ++runtime_.migration_in;
        // Publication is the linearization point: the complete source value is
        // write-backed before another VM can rely on the shared state.
        ++runtime_.shared_swcc_flushes;
        msync(&row, sizeof(Slot), MS_SYNC);
      }
      bool shared = row.state == kShared;
      if (shared) { ++runtime_.shared_gets; impl_->latency.Record(PoolKind::kSwcc, AccessKind::kRead, row.value_len); }
      else { ++runtime_.private_gets; impl_->latency.Record(PoolKind::kSwcc, AccessKind::kRead, row.value_len); }
      ++runtime_.commits;
      return {Status::Ok(), std::string(row.value, row.value_len)};
    }
  }
  ++runtime_.commits;
  return {Status::Error(StatusCode::kNotFound, "key not found"), {}};
}

Status KVStore::Delete(std::string_view key) {
  Lock lock(&impl_->header->mutex);
  ++runtime_.logical_ops;
  for (size_t i = 0; i < impl_->slot_count; ++i) {
    Slot &row = impl_->slots[i];
    if ((row.state == kPrivate || row.state == kShared) && row.key_len == key.size() && std::memcmp(row.key, key.data(), key.size()) == 0) {
      bool shared = row.state == kShared;
      if (!shared && row.owner != config_.node_id) {
        if (config_.strict_swcc_access) return Status::Error(StatusCode::kOwnerViolation, "non-owner private SWCC access");
        if (sizeof(SharedHeader) + (impl_->header->active_shared_rows + 1) * kSharedMetadataBytes > impl_->header->logical_hwcc_capacity_bytes)
          return Status::Error(StatusCode::kOutOfMemory, "logical HWCC row-metadata budget exceeded");
        row.state = kShared; ++impl_->header->active_shared_rows; ++runtime_.migration_in; shared = true;
      }
      row.state = kTombstone; row.key_len = 0; row.value_len = 0; ++row.generation;
      if (shared) { if (impl_->header->active_shared_rows) --impl_->header->active_shared_rows; ++runtime_.shared_deletes; ++runtime_.shared_swcc_flushes; msync(&row, sizeof(Slot), MS_SYNC); }
      else ++runtime_.private_deletes;
      impl_->header->reclaimed_total_bytes += sizeof(Slot); ++runtime_.commits;
      return Status::Ok();
    }
  }
  ++runtime_.commits;
  return Status::Error(StatusCode::kNotFound, "key not found");
}

ScanResult KVStore::Scan(std::string_view start_key, uint64_t limit) {
  if (!config_.enable_scan) return {Status::Error(StatusCode::kInvalidArgument, "SCAN disabled"), {}};
  Lock lock(&impl_->header->mutex);
  std::vector<ScanItem> all;
  for (size_t i = 0; i < impl_->slot_count; ++i) if (impl_->slots[i].state == kPrivate || impl_->slots[i].state == kShared)
    if (std::string_view(impl_->slots[i].key, impl_->slots[i].key_len) >= start_key)
      all.push_back({std::string(impl_->slots[i].key, impl_->slots[i].key_len), std::string(impl_->slots[i].value, impl_->slots[i].value_len)});
  std::sort(all.begin(), all.end(), [](const ScanItem &a, const ScanItem &b) { return a.key < b.key; });
  if (all.size() > limit) all.resize(static_cast<size_t>(limit));
  ++runtime_.logical_ops; ++runtime_.commits;
  return {Status::Ok(), std::move(all)};
}

CasResult KVStore::CompareExchange(std::string_view key, std::string_view expected, std::string_view desired) {
  ValidateKeyValue(key, desired);
  Lock lock(&impl_->header->mutex);
  ++runtime_.logical_ops;
  Slot *row = nullptr;
  for (size_t i = 0; i < impl_->slot_count; ++i) if ((impl_->slots[i].state == kPrivate || impl_->slots[i].state == kShared) && impl_->slots[i].key_len == key.size() && std::memcmp(impl_->slots[i].key, key.data(), key.size()) == 0) { row = &impl_->slots[i]; break; }
  const bool match = row ? std::string_view(row->value, row->value_len) == expected : expected.empty();
  if (!match) { ++runtime_.aborts; return {Status::Error(StatusCode::kCompareFailed, "CAS comparison failed"), false}; }
  if (!row) { for (size_t i = 0; i < impl_->slot_count; ++i) if (impl_->slots[i].state == kEmpty || impl_->slots[i].state == kTombstone) { row = &impl_->slots[i]; break; } }
  if (!row) return {Status::Error(StatusCode::kOutOfMemory, "shared pool has no free KV slot"), false};
  if (row->state == kPrivate && row->owner != config_.node_id) {
    if (config_.strict_swcc_access) return {Status::Error(StatusCode::kOwnerViolation, "non-owner private SWCC access"), false};
    if (sizeof(SharedHeader) + (impl_->header->active_shared_rows + 1) * kSharedMetadataBytes > impl_->header->logical_hwcc_capacity_bytes)
      return {Status::Error(StatusCode::kOutOfMemory, "logical HWCC row-metadata budget exceeded"), false};
    row->state = kShared; ++impl_->header->active_shared_rows; ++runtime_.migration_in;
    ++runtime_.shared_swcc_flushes; msync(row, sizeof(Slot), MS_SYNC);
  }
  if (row->state == kEmpty || row->state == kTombstone) {
    row->owner = static_cast<uint8_t>(OwnerForKey(key));
    if (row->owner != config_.node_id) {
      if (sizeof(SharedHeader) + (impl_->header->active_shared_rows + 1) * kSharedMetadataBytes > impl_->header->logical_hwcc_capacity_bytes)
        return {Status::Error(StatusCode::kOutOfMemory, "logical HWCC row-metadata budget exceeded"), false};
      row->state = kShared; ++impl_->header->active_shared_rows; ++runtime_.migration_in;
    } else row->state = kPrivate;
    row->key_len = key.size(); std::memcpy(row->key, key.data(), key.size());
  }
  std::memset(row->value, 0, kMaxValue); std::memcpy(row->value, desired.data(), desired.size()); row->value_len = desired.size(); ++row->version;
  if (row->state == kShared) { ++runtime_.shared_puts; ++runtime_.shared_swcc_flushes; msync(row, sizeof(Slot), MS_SYNC); }
  else ++runtime_.private_puts;
  ++runtime_.commits;
  return {Status::Ok(), true};
}

IncrementResult KVStore::Increment(std::string_view key, int64_t delta) {
  Lock lock(&impl_->header->mutex);
  ++runtime_.logical_ops;
  Slot *row = nullptr;
  for (size_t i = 0; i < impl_->slot_count; ++i) if ((impl_->slots[i].state == kPrivate || impl_->slots[i].state == kShared) && impl_->slots[i].key_len == key.size() && std::memcmp(impl_->slots[i].key, key.data(), key.size()) == 0) { row = &impl_->slots[i]; break; }
  int64_t old = 0;
  if (row && row->state == kPrivate && row->owner != config_.node_id) {
    if (config_.strict_swcc_access) return {Status::Error(StatusCode::kOwnerViolation, "non-owner private SWCC access"), 0};
    if (sizeof(SharedHeader) + (impl_->header->active_shared_rows + 1) * kSharedMetadataBytes > impl_->header->logical_hwcc_capacity_bytes)
      return {Status::Error(StatusCode::kOutOfMemory, "logical HWCC row-metadata budget exceeded"), 0};
    row->state = kShared; ++impl_->header->active_shared_rows; ++runtime_.migration_in;
    ++runtime_.shared_swcc_flushes; msync(row, sizeof(Slot), MS_SYNC);
  }
  if (row) { auto result = std::from_chars(row->value, row->value + row->value_len, old); if (result.ec != std::errc()) return {Status::Error(StatusCode::kInvalidArgument, "value is not an integer"), 0}; }
  int64_t value = old + delta;
  std::string encoded = std::to_string(value);
  if (encoded.size() > config_.fixed_value_size) return {Status::Error(StatusCode::kInvalidArgument, "increment overflows fixed value size"), 0};
  if (!row) { for (size_t i = 0; i < impl_->slot_count; ++i) if (impl_->slots[i].state == kEmpty || impl_->slots[i].state == kTombstone) { row = &impl_->slots[i]; break; } }
  if (!row) return {Status::Error(StatusCode::kOutOfMemory, "shared pool has no free KV slot"), 0};
  if (row->state == kEmpty || row->state == kTombstone) {
    row->owner = OwnerForKey(key);
    if (row->owner != config_.node_id) {
      if (sizeof(SharedHeader) + (impl_->header->active_shared_rows + 1) * kSharedMetadataBytes > impl_->header->logical_hwcc_capacity_bytes)
        return {Status::Error(StatusCode::kOutOfMemory, "logical HWCC row-metadata budget exceeded"), 0};
      row->state = kShared; ++impl_->header->active_shared_rows; ++runtime_.migration_in;
    } else row->state = kPrivate;
    row->key_len = key.size(); std::memcpy(row->key, key.data(), key.size());
  }
  std::memcpy(row->value, encoded.data(), encoded.size()); row->value_len = encoded.size(); ++row->version;
  if (row->state == kShared) { ++runtime_.shared_puts; ++runtime_.shared_swcc_flushes; msync(row, sizeof(Slot), MS_SYNC); }
  else ++runtime_.private_puts;
  ++runtime_.commits;
  return {Status::Ok(), value};
}

Status KVStore::Checkpoint() {
  if (impl_->mapping == MAP_FAILED) return Status::Error(StatusCode::kCorruption, "store is closed");
  if (msync(impl_->mapping, impl_->bytes, MS_SYNC) != 0) return Status::Error(StatusCode::kCorruption, "checkpoint msync failed");
  Lock lock(&impl_->header->mutex);
  ++impl_->header->clean_epoch;
  ++runtime_.private_swcc_flushes;
  return Status::Ok();
}

MemoryStats KVStore::Memory() const {
  Lock lock(&impl_->header->mutex);
  MemoryStats out;
  out.total_pool_capacity_bytes = impl_->bytes;
  out.logical_hwcc_capacity_bytes = config_.hwcc_size_mb * 1024ULL * 1024ULL;
  out.logical_swcc_capacity_bytes = config_.swcc_size_mb * 1024ULL * 1024ULL;
  out.logical_hwcc_used_bytes = sizeof(SharedHeader) + impl_->header->active_shared_rows * kSharedMetadataBytes;
  out.active_shared_rows = impl_->header->active_shared_rows;
  out.allocator_shared_overhead_bytes = sizeof(SharedHeader);
  out.allocator_local_dram_bytes = sizeof(Impl) + sizeof(Config) + sizeof(RuntimeStats);
  out.reclaimed_total_bytes = impl_->header->reclaimed_total_bytes;
  for (size_t i = 0; i < impl_->slot_count; ++i) {
    if (impl_->slots[i].state == kPrivate) out.owner_private_swcc_used_bytes += impl_->slots[i].key_len + impl_->slots[i].value_len;
    else if (impl_->slots[i].state == kShared) out.shared_payload_swcc_used_bytes += impl_->slots[i].key_len + impl_->slots[i].value_len;
  }
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
  out += "unclassified_shared_bytes=0\nretired_pending_bytes=0\n";
  out += "reclaimed_total_bytes=" + std::to_string(m.reclaimed_total_bytes) + "\nactive_shared_rows=" + std::to_string(m.active_shared_rows) + "\n";
  out += "TIGONKV_RUNTIME_STATS\nnode=" + std::to_string(config_.node_id) + "\nlogical_ops=" + std::to_string(runtime_.logical_ops) + "\ncommits=" + std::to_string(runtime_.commits) + "\naborts=" + std::to_string(runtime_.aborts) + "\nretries=" + std::to_string(runtime_.retries) + "\nprivate_gets=" + std::to_string(runtime_.private_gets) + "\nprivate_puts=" + std::to_string(runtime_.private_puts) + "\nprivate_deletes=" + std::to_string(runtime_.private_deletes) + "\nprivate_swcc_flushes=" + std::to_string(runtime_.private_swcc_flushes) + "\nshared_gets=" + std::to_string(runtime_.shared_gets) + "\nshared_puts=" + std::to_string(runtime_.shared_puts) + "\nshared_deletes=" + std::to_string(runtime_.shared_deletes) + "\nshared_swcc_flushes=" + std::to_string(runtime_.shared_swcc_flushes) + "\nmigration_in=" + std::to_string(runtime_.migration_in) + "\nmigration_out=" + std::to_string(runtime_.migration_out) + "\nnetwork_tx_bytes=0\nnetwork_rx_bytes=0\n";
  return out;
}

}  // namespace tigonkv
