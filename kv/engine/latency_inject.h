#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace latency_sim {

class LatencySimulator;
struct ThreadState;

// These are the four independent runtime modules.  The mask is deliberately
// process-local: it is published once during startup and is never changed
// while business threads are running.
enum Feature : uint32_t {
  kFixedLatency = 1u << 0,
  kHwccAccessCount = 1u << 1,
  kAtomicCount = 1u << 2,
  kRemoteInvalidation = 1u << 3,
};

enum class MemoryDomain : uint8_t {
  kSwcc = 0,
  kHwcc = 1,
  kOwnerPrivateSwcc = 2,
  kLocalDram = 3,
};
// PoolKind is retained as the project-facing name used by existing CXLKV
// wrappers.  It is an alias, not a second classification.
using PoolKind = MemoryDomain;

enum class AccessKind : uint8_t {
  kRead = 0,
  kWrite = 1,
  kAtomicLoad = 2,
  kAtomicStore = 3,
  kAtomicRmw = 4,
  kFlush = 5,
  kInvalidate = 6,
};

// The operation family is carried by the executed wrapper.  Keeping this
// separate from AccessKind lets the configuration disable exchange, fetch
// arithmetic, or fetch bitwise operations independently without guessing from
// the resulting RMW access.
enum class AtomicOperation : uint8_t {
  kLoad = 0,
  kStore = 1,
  kCas = 2,
  kCasWeak = 3,
  kCasStrong = 4,
  kExchange = 5,
  kFetchAdd = 6,
  kFetchSub = 7,
  kFetchOr = 8,
  kFetchAnd = 9,
  kFetchXor = 10,
  kFence = 11,
  kWaitNotify = 12,
};

enum class AtomicDomain : uint8_t {
  kHwcc = static_cast<uint8_t>(MemoryDomain::kHwcc),
  kOwnerPrivateSwcc = static_cast<uint8_t>(MemoryDomain::kOwnerPrivateSwcc),
  kLocalDram = static_cast<uint8_t>(MemoryDomain::kLocalDram),
};

enum class ScopeKind : uint8_t {
  kForeground = 0,
  kMerge = 1,
  kOther = 2,
};

inline constexpr std::size_t kScopeCount = 3;
inline constexpr std::size_t kAtomicDomainCount = 3;
inline constexpr std::size_t kMemoryOrderBucketCount = 5;
inline constexpr std::size_t kMaxBreakdownTags = 64;

struct FixedLatencyConfig {
  bool enabled = false;
  uint64_t cache_line_bytes = 64;
  double swcc_fixed_ns_per_line = 0.0;
  double hwcc_fixed_ns_per_line = 0.0;
  bool foreground_enabled = true;
  bool background_enabled = true;
  bool delayed_time_stats_enabled = false;
};

struct HwccAccessCountConfig {
  bool enabled = false;
  uint64_t cache_line_bytes = 64;
  bool read_enabled = true;
  bool write_enabled = true;
  bool operation_count_enabled = true;
  bool line_count_enabled = true;
  bool byte_count_enabled = false;
  bool breakdown_by_scope_enabled = false;
  bool breakdown_by_tag_enabled = false;
  uint64_t max_tags = 32;
};

struct AtomicCountConfig {
  bool enabled = false;
  bool hwcc_enabled = true;
  bool owner_private_swcc_enabled = false;
  bool local_dram_enabled = true;
  bool load_enabled = true;
  bool store_enabled = true;
  bool cas_enabled = true;
  bool exchange_enabled = true;
  bool fetch_arithmetic_enabled = true;
  bool fetch_bitwise_enabled = true;
  bool result_breakdown_enabled = true;
  bool fence_enabled = false;
  bool wait_notify_enabled = false;
  bool memory_order_breakdown_enabled = false;
  bool scope_breakdown_enabled = false;
  bool tag_breakdown_enabled = false;
  uint64_t max_tags = 32;
};

struct RemoteInvalidationConfig {
  bool enabled = false;
  bool dirty_handoff_enabled = true;
  bool clean_copy_invalidation_enabled = true;
  bool dirty_eviction_writeback_enabled = true;
  bool swcc_explicit_visibility_handoff_enabled = true;
  uint64_t cache_line_bytes = 64;
  uint64_t node_count = 4;
  uint64_t cache_size_bytes_per_node = 40ull * 1024 * 1024;
  uint64_t total_cpu_cache_size_bytes = 0;
  std::vector<uint64_t> cache_size_bytes_by_node;
  uint64_t cache_instances_per_node = 1;
  uint64_t associativity = 16;
  std::string capacity_mode = "per_node";
  std::string replacement_policy = "lru";
  uint64_t lfu_counter_bits = 16;
  uint64_t lfu_aging_interval_accesses = 1ull << 20;
  std::string lfu_tie_breaker = "lru";
  bool scope_breakdown_enabled = false;
  bool tag_breakdown_enabled = false;
  uint64_t max_tags = 32;
  uint64_t shared_sequencer_offset = 192;
  // Each record is 64 bytes.  Four million records reserve 256 MiB from the
  // HWCC area, enough for the four-VM smoke while retaining hard overflow
  // detection instead of silently wrapping the global sequence.
  uint64_t event_log_capacity = 1ull << 22;
};

struct Config {
  FixedLatencyConfig fixed_latency;
  HwccAccessCountConfig hwcc_access_count;
  AtomicCountConfig atomic_count;
  RemoteInvalidationConfig remote_cache_invalidation;

  // Compatibility for low-level unit tests that construct a simulator
  // directly.  The policy parser never exposes this field.  If set, it is
  // normalized to fixed_latency.enabled during Configure().
  bool enabled = false;
};

struct MemoryIdentity {
  MemoryDomain domain = MemoryDomain::kLocalDram;
  uint64_t pool_id = 0;
  uint64_t pool_offset = 0;

  bool operator==(const MemoryIdentity& other) const {
    return domain == other.domain && pool_id == other.pool_id &&
           pool_offset == other.pool_offset;
  }
};

inline constexpr uint64_t kRemoteEventRecordBytes = 64;

// Layout in an attached HWCC instrumentation area.  These bytes are reserved
// from the business allocator and are never passed through business wrappers.
struct alignas(64) RemoteEventRecord {
  uint64_t sequence = 0;
  uint64_t pool_id = 0;
  uint64_t pool_offset = 0;
  uint64_t domain_and_kind = 0;
  uint64_t node = 0;
  uint64_t reserved[3] = {};
};
static_assert(sizeof(RemoteEventRecord) == kRemoteEventRecordBytes);

enum class RemoteEventKind : uint8_t {
  kRead = 0,
  kWrite = 1,
  kAtomicRmw = 2,
  kExplicitSwccHandoff = 3,
  kDirtyCapacityEviction = 4,
};

struct Stats {
  uint64_t swcc_read_ops = 0;
  uint64_t swcc_write_ops = 0;
  uint64_t swcc_read_lines = 0;
  uint64_t swcc_write_lines = 0;
  uint64_t swcc_read_bytes = 0;
  uint64_t swcc_write_bytes = 0;
  uint64_t hwcc_read_ops = 0;
  uint64_t hwcc_write_ops = 0;
  uint64_t hwcc_read_lines = 0;
  uint64_t hwcc_write_lines = 0;
  uint64_t hwcc_read_bytes = 0;
  uint64_t hwcc_write_bytes = 0;

  std::array<uint64_t, kScopeCount> hwcc_read_ops_by_scope{};
  std::array<uint64_t, kScopeCount> hwcc_write_ops_by_scope{};
  std::array<uint64_t, kScopeCount> hwcc_read_lines_by_scope{};
  std::array<uint64_t, kScopeCount> hwcc_write_lines_by_scope{};
  std::array<uint64_t, kScopeCount> hwcc_read_bytes_by_scope{};
  std::array<uint64_t, kScopeCount> hwcc_write_bytes_by_scope{};
  std::array<uint64_t, kMaxBreakdownTags> hwcc_read_ops_by_tag{};
  std::array<uint64_t, kMaxBreakdownTags> hwcc_write_ops_by_tag{};
  std::array<uint64_t, kMaxBreakdownTags> hwcc_read_lines_by_tag{};
  std::array<uint64_t, kMaxBreakdownTags> hwcc_write_lines_by_tag{};
  std::array<uint64_t, kMaxBreakdownTags> hwcc_read_bytes_by_tag{};
  std::array<uint64_t, kMaxBreakdownTags> hwcc_write_bytes_by_tag{};

  uint64_t hwcc_atomic_ops = 0;
  uint64_t hwcc_atomic_loads = 0;
  uint64_t hwcc_atomic_stores = 0;
  uint64_t hwcc_atomic_rmw = 0;
  uint64_t hwcc_exchange_ops = 0;
  uint64_t hwcc_fetch_add_ops = 0;
  uint64_t hwcc_fetch_sub_ops = 0;
  uint64_t hwcc_fetch_or_ops = 0;
  uint64_t hwcc_fetch_and_ops = 0;
  uint64_t hwcc_fetch_xor_ops = 0;
  uint64_t hwcc_cas_attempts = 0;
  uint64_t hwcc_cas_weak_attempts = 0;
  uint64_t hwcc_cas_strong_attempts = 0;
  uint64_t hwcc_cas_successes = 0;
  uint64_t hwcc_cas_failures = 0;
  uint64_t owner_private_swcc_atomic_ops = 0;
  uint64_t local_dram_atomic_ops = 0;
  uint64_t owner_private_swcc_atomic_loads = 0;
  uint64_t owner_private_swcc_atomic_stores = 0;
  uint64_t owner_private_swcc_atomic_rmw = 0;
  uint64_t local_dram_atomic_loads = 0;
  uint64_t local_dram_atomic_stores = 0;
  uint64_t local_dram_atomic_rmw = 0;
  uint64_t owner_private_swcc_exchange_ops = 0;
  uint64_t owner_private_swcc_fetch_add_ops = 0;
  uint64_t owner_private_swcc_fetch_sub_ops = 0;
  uint64_t owner_private_swcc_fetch_or_ops = 0;
  uint64_t owner_private_swcc_fetch_and_ops = 0;
  uint64_t owner_private_swcc_fetch_xor_ops = 0;
  uint64_t local_dram_exchange_ops = 0;
  uint64_t local_dram_fetch_add_ops = 0;
  uint64_t local_dram_fetch_sub_ops = 0;
  uint64_t local_dram_fetch_or_ops = 0;
  uint64_t local_dram_fetch_and_ops = 0;
  uint64_t local_dram_fetch_xor_ops = 0;
  uint64_t owner_private_swcc_cas_attempts = 0;
  uint64_t owner_private_swcc_cas_weak_attempts = 0;
  uint64_t owner_private_swcc_cas_strong_attempts = 0;
  uint64_t owner_private_swcc_cas_successes = 0;
  uint64_t owner_private_swcc_cas_failures = 0;
  uint64_t local_dram_cas_attempts = 0;
  uint64_t local_dram_cas_weak_attempts = 0;
  uint64_t local_dram_cas_strong_attempts = 0;
  uint64_t local_dram_cas_successes = 0;
  uint64_t local_dram_cas_failures = 0;
  uint64_t hwcc_fence_ops = 0;
  uint64_t owner_private_swcc_fence_ops = 0;
  uint64_t local_dram_fence_ops = 0;
  uint64_t hwcc_wait_notify_ops = 0;
  uint64_t owner_private_swcc_wait_notify_ops = 0;
  uint64_t local_dram_wait_notify_ops = 0;

  // Flattened indices are domain * kScopeCount + scope and domain *
  // kMemoryOrderBucketCount + order.  Keeping these fixed-size arrays means
  // enabled breakdowns never allocate or look up a tag in the hot path.
  std::array<uint64_t, kAtomicDomainCount * kScopeCount>
      atomic_ops_by_domain_scope{};
  std::array<uint64_t, kAtomicDomainCount * kMemoryOrderBucketCount>
      atomic_ops_by_domain_memory_order{};
  std::array<uint64_t, kAtomicDomainCount * kMaxBreakdownTags>
      atomic_ops_by_domain_tag{};

  uint64_t swcc_delayed_ns = 0;
  uint64_t hwcc_delayed_ns = 0;

  uint64_t remote_dirty_handoffs = 0;
  uint64_t remote_clean_copy_invalidations = 0;
  uint64_t remote_write_transactions_causing_invalidation = 0;
  uint64_t remote_dirty_capacity_evictions = 0;
  uint64_t remote_swcc_explicit_handoffs = 0;
  uint64_t remote_events = 0;
  std::array<uint64_t, kScopeCount> remote_dirty_handoffs_by_scope{};
  std::array<uint64_t, kScopeCount> remote_clean_copy_invalidations_by_scope{};
  std::array<uint64_t, kScopeCount>
      remote_write_transactions_causing_invalidation_by_scope{};
  std::array<uint64_t, kScopeCount> remote_dirty_capacity_evictions_by_scope{};
  std::array<uint64_t, kScopeCount> remote_swcc_explicit_handoffs_by_scope{};
  std::array<uint64_t, kScopeCount> remote_events_by_scope{};
  std::array<uint64_t, kMaxBreakdownTags> remote_dirty_handoffs_by_tag{};
  std::array<uint64_t, kMaxBreakdownTags>
      remote_clean_copy_invalidations_by_tag{};
  std::array<uint64_t, kMaxBreakdownTags>
      remote_write_transactions_causing_invalidation_by_tag{};
  std::array<uint64_t, kMaxBreakdownTags>
      remote_dirty_capacity_evictions_by_tag{};
  std::array<uint64_t, kMaxBreakdownTags>
      remote_swcc_explicit_handoffs_by_tag{};
  std::array<uint64_t, kMaxBreakdownTags> remote_events_by_tag{};

  uint64_t RawLineAccesses(PoolKind pool) const;
  uint64_t DelayedNs(PoolKind pool) const;
  uint64_t TotalDelayedNs() const { return swcc_delayed_ns + hwcc_delayed_ns; }
};

// One relaxed load is the only operation allowed before a disabled wrapper
// returns to the real memory operation.
#if defined(TIGONKV_DISABLE_HARDWARE_SIMULATION)
inline constexpr uint32_t HardwareSimulationFeaturesFast() noexcept {
  return 0;
}
#else
uint32_t HardwareSimulationFeaturesFast();
#endif
inline bool InstrumentationEnabledFast() {
  return HardwareSimulationFeaturesFast() != 0;
}

LatencySimulator& GlobalLatencySimulator();

class LatencySimulator {
 public:
  explicit LatencySimulator(Config config = {});
  ~LatencySimulator();

  void Configure(Config config);
  const Config& config() const { return config_; }
  uint32_t feature_mask() const { return feature_mask_; }

  void BeginScope(ScopeKind scope);
  void EndScopeAndDelay();
  bool HasActiveScopeForCurrentThread() const;

  // Range observations for direct pool operations. They contain no cache
  // hit/miss model; callers emit one observation immediately beside the real
  // operation, while typed loads/stores use the executed wrappers below.
  void RecordRange(PoolKind pool, AccessKind kind, const void* addr,
                   uint64_t bytes, uint32_t tag = 0);
  void RecordLine(PoolKind pool, AccessKind kind, const void* addr,
                  uint32_t tag = 0);

  void RecordAtomicCompleted(AtomicDomain domain, AccessKind kind,
                             AtomicOperation operation, const void* addr,
                             uint64_t bytes, bool cas_success,
                             std::memory_order order = std::memory_order_seq_cst,
                             uint32_t tag = 0);
  void RecordRemoteEvent(const MemoryIdentity& identity, uint32_t node,
                         RemoteEventKind event,
                         uint32_t source_node = UINT32_MAX,
                         ScopeKind scope = ScopeKind::kOther,
                         uint32_t tag = 0);
  void RecordRemoteRead(const MemoryIdentity& identity, uint32_t node,
                        ScopeKind scope = ScopeKind::kOther, uint32_t tag = 0);
  void RecordRemoteWrite(const MemoryIdentity& identity, uint32_t node,
                         bool atomic_rmw, ScopeKind scope = ScopeKind::kOther,
                         uint32_t tag = 0);
  void RecordSwccExplicitHandoff(const MemoryIdentity& identity,
                                 uint32_t from_node, uint32_t to_node,
                                 ScopeKind scope = ScopeKind::kOther,
                                 uint32_t tag = 0);
  void RecordSwccExplicitHandoff(PoolKind pool, const void* addr,
                                 uint64_t bytes, uint32_t from_node,
                                 uint32_t to_node,
                                 uint32_t tag = 0);
  void RecordDirtyCapacityEviction(const MemoryIdentity& identity,
                                   uint32_t node,
                                   ScopeKind scope = ScopeKind::kOther,
                                   uint32_t tag = 0);

  // The mapping is established by the pool attach layer.  Remote identity
  // resolution never uses a virtual address in the event key.
  void RegisterMemoryRange(const void* base, uint64_t bytes,
                           MemoryDomain domain, uint64_t pool_id,
                           uint64_t pool_offset);
  void UnregisterMemoryRange(const void* base, uint64_t bytes);
  void AttachSharedRemoteLog(void* sequence_word, void* event_log,
                             uint64_t event_capacity);
  void DetachSharedRemoteLog(void* sequence_word, void* event_log);
  void ValidateSharedRemoteLog() const;
  void SetNodeId(uint32_t node_id);
  uint32_t node_id() const;

  Stats SnapshotStats() const;
  Stats TakeStatsAndReset();
  uint64_t PendingDelayNsForTest() const;

 private:
  struct Impl;
  ThreadState& GetThreadState();
  MemoryIdentity ResolveIdentity(const void* address, MemoryDomain domain) const;
  Config config_;
  uint32_t feature_mask_ = 0;
  uint64_t generation_ = 0;
  std::unique_ptr<Impl> impl_;
};

// Executed typed memory wrappers.  The real access is performed before the
// observation is emitted, so a later refactor cannot leave a detached record.
template <typename T>
T CountedMemoryLoad(PoolKind pool, const T* address, uint32_t tag = 0) {
  const uint32_t features = HardwareSimulationFeaturesFast();
  if (features == 0 ||
      !(features & (kFixedLatency | kHwccAccessCount |
                    kRemoteInvalidation))) {
    return *address;
  }
  const T result = *address;
  GlobalLatencySimulator().RecordRange(pool, AccessKind::kRead, address,
                                       sizeof(T), tag);
  return result;
}

template <typename T>
void CountedMemoryStore(PoolKind pool, T* address, T value, uint32_t tag = 0) {
  const uint32_t features = HardwareSimulationFeaturesFast();
  if (features == 0 ||
      !(features & (kFixedLatency | kHwccAccessCount |
                    kRemoteInvalidation))) {
    *address = value;
    return;
  }
  *address = value;
  GlobalLatencySimulator().RecordRange(pool, AccessKind::kWrite, address,
                                       sizeof(T), tag);
}

inline bool CountedAtomicFlagTestAndSet(std::atomic_flag& value,
                                        std::memory_order order,
                                        AtomicDomain domain,
                                        uint32_t tag = 0) {
  const uint32_t features = HardwareSimulationFeaturesFast();
  if (features == 0 ||
      !(features & (kAtomicCount | kFixedLatency | kRemoteInvalidation))) {
    return value.test_and_set(order);
  }
  const bool old = value.test_and_set(order);
  GlobalLatencySimulator().RecordAtomicCompleted(
      domain, AccessKind::kAtomicRmw, AtomicOperation::kExchange, &value,
      sizeof(value), false, order, tag);
  return old;
}

inline void CountedAtomicFlagClear(std::atomic_flag& value,
                                   std::memory_order order,
                                   AtomicDomain domain, uint32_t tag = 0) {
  const uint32_t features = HardwareSimulationFeaturesFast();
  if (features == 0 ||
      !(features & (kAtomicCount | kFixedLatency | kRemoteInvalidation))) {
    value.clear(order);
    return;
  }
  value.clear(order);
  GlobalLatencySimulator().RecordAtomicCompleted(
      domain, AccessKind::kAtomicStore, AtomicOperation::kStore, &value,
      sizeof(value), false, order, tag);
}

inline void CountedAtomicFence(std::memory_order order, AtomicDomain domain,
                               uint32_t tag = 0) {
  const uint32_t features = HardwareSimulationFeaturesFast();
  std::atomic_thread_fence(order);
  if (features != 0 &&
      (features & (kAtomicCount | kFixedLatency | kRemoteInvalidation))) {
    GlobalLatencySimulator().RecordAtomicCompleted(
        domain, AccessKind::kAtomicRmw, AtomicOperation::kFence, nullptr, 0,
        false, order, tag);
  }
}

// Executed atomic wrappers.  They preserve the standard atomic operation and
// memory-order/CAS semantics; accounting is performed only after the real
// operation returns.  These templates intentionally use the feature mask
// before touching any TLS or address metadata.
template <typename T>
T CountedAtomicLoad(const std::atomic<T>& value, std::memory_order order,
                    AtomicDomain domain, uint32_t tag = 0) {
  const uint32_t features = HardwareSimulationFeaturesFast();
  if (features == 0 ||
      !(features & (kAtomicCount | kFixedLatency | kRemoteInvalidation))) {
    return value.load(order);
  }
  const T result = value.load(order);
  GlobalLatencySimulator().RecordAtomicCompleted(
      domain, AccessKind::kAtomicLoad, AtomicOperation::kLoad, &value,
      sizeof(T), false, order, tag);
  return result;
}

template <typename T>
void CountedAtomicStore(std::atomic<T>& value, T desired,
                        std::memory_order order, AtomicDomain domain,
                        uint32_t tag = 0) {
  const uint32_t features = HardwareSimulationFeaturesFast();
  if (features == 0 ||
      !(features & (kAtomicCount | kFixedLatency | kRemoteInvalidation))) {
    value.store(desired, order);
    return;
  }
  value.store(desired, order);
  GlobalLatencySimulator().RecordAtomicCompleted(
      domain, AccessKind::kAtomicStore, AtomicOperation::kStore, &value,
      sizeof(T), false, order, tag);
}

template <typename T>
T CountedAtomicExchange(std::atomic<T>& value, T desired,
                        std::memory_order order, AtomicDomain domain,
                        uint32_t tag = 0) {
  const uint32_t features = HardwareSimulationFeaturesFast();
  if (features == 0 ||
      !(features & (kAtomicCount | kFixedLatency | kRemoteInvalidation))) {
    return value.exchange(desired, order);
  }
  const T result = value.exchange(desired, order);
  GlobalLatencySimulator().RecordAtomicCompleted(
      domain, AccessKind::kAtomicRmw, AtomicOperation::kExchange, &value,
      sizeof(T), false, order, tag);
  return result;
}

template <typename T>
T CountedAtomicFetchAdd(std::atomic<T>& value, T operand,
                        std::memory_order order, AtomicDomain domain,
                        uint32_t tag = 0) {
  const uint32_t features = HardwareSimulationFeaturesFast();
  if (features == 0 ||
      !(features & (kAtomicCount | kFixedLatency | kRemoteInvalidation))) {
    return value.fetch_add(operand, order);
  }
  const T result = value.fetch_add(operand, order);
  GlobalLatencySimulator().RecordAtomicCompleted(
      domain, AccessKind::kAtomicRmw, AtomicOperation::kFetchAdd,
      &value, sizeof(T), false, order, tag);
  return result;
}

template <typename T>
T CountedAtomicFetchSub(std::atomic<T>& value, T operand,
                        std::memory_order order, AtomicDomain domain,
                        uint32_t tag = 0) {
  const uint32_t features = HardwareSimulationFeaturesFast();
  if (features == 0 ||
      !(features & (kAtomicCount | kFixedLatency | kRemoteInvalidation))) {
    return value.fetch_sub(operand, order);
  }
  const T result = value.fetch_sub(operand, order);
  GlobalLatencySimulator().RecordAtomicCompleted(
      domain, AccessKind::kAtomicRmw, AtomicOperation::kFetchSub,
      &value, sizeof(T), false, order, tag);
  return result;
}

template <typename T>
T CountedAtomicFetchOr(std::atomic<T>& value, T operand,
                       std::memory_order order, AtomicDomain domain,
                       uint32_t tag = 0) {
  const uint32_t features = HardwareSimulationFeaturesFast();
  if (features == 0 ||
      !(features & (kAtomicCount | kFixedLatency | kRemoteInvalidation))) {
    return value.fetch_or(operand, order);
  }
  const T result = value.fetch_or(operand, order);
  GlobalLatencySimulator().RecordAtomicCompleted(
      domain, AccessKind::kAtomicRmw, AtomicOperation::kFetchOr, &value,
      sizeof(T), false, order, tag);
  return result;
}

template <typename T>
bool CountedCompareExchangeWeak(std::atomic<T>& value, T& expected, T desired,
                                std::memory_order success,
                                std::memory_order failure, AtomicDomain domain,
                                uint32_t tag = 0) {
  const uint32_t features = HardwareSimulationFeaturesFast();
  if (features == 0 ||
      !(features & (kAtomicCount | kFixedLatency | kRemoteInvalidation))) {
    return value.compare_exchange_weak(expected, desired, success, failure);
  }
  const bool ok = value.compare_exchange_weak(expected, desired, success,
                                              failure);
  GlobalLatencySimulator().RecordAtomicCompleted(
      domain, AccessKind::kAtomicRmw, AtomicOperation::kCasWeak, &value,
      sizeof(T), ok, ok ? success : failure, tag);
  return ok;
}

template <typename T>
bool CountedCompareExchangeStrong(std::atomic<T>& value, T& expected,
                                  T desired, std::memory_order success,
                                  std::memory_order failure,
                                  AtomicDomain domain, uint32_t tag = 0) {
  const uint32_t features = HardwareSimulationFeaturesFast();
  if (features == 0 ||
      !(features & (kAtomicCount | kFixedLatency | kRemoteInvalidation))) {
    return value.compare_exchange_strong(expected, desired, success, failure);
  }
  const bool ok = value.compare_exchange_strong(expected, desired, success,
                                                failure);
  GlobalLatencySimulator().RecordAtomicCompleted(
      domain, AccessKind::kAtomicRmw, AtomicOperation::kCasStrong, &value,
      sizeof(T), ok, ok ? success : failure, tag);
  return ok;
}

template <typename T>
T CountedAtomicFetchAnd(std::atomic<T>& value, T operand,
                        std::memory_order order, AtomicDomain domain,
                        uint32_t tag = 0) {
  const uint32_t features = HardwareSimulationFeaturesFast();
  if (features == 0 ||
      !(features & (kAtomicCount | kFixedLatency | kRemoteInvalidation))) {
    return value.fetch_and(operand, order);
  }
  const T result = value.fetch_and(operand, order);
  GlobalLatencySimulator().RecordAtomicCompleted(
      domain, AccessKind::kAtomicRmw, AtomicOperation::kFetchAnd, &value,
      sizeof(T), false, order, tag);
  return result;
}

template <typename T>
T CountedAtomicFetchXor(std::atomic<T>& value, T operand,
                        std::memory_order order, AtomicDomain domain,
                        uint32_t tag = 0) {
  const uint32_t features = HardwareSimulationFeaturesFast();
  if (features == 0 ||
      !(features & (kAtomicCount | kFixedLatency | kRemoteInvalidation))) {
    return value.fetch_xor(operand, order);
  }
  const T result = value.fetch_xor(operand, order);
  GlobalLatencySimulator().RecordAtomicCompleted(
      domain, AccessKind::kAtomicRmw, AtomicOperation::kFetchXor, &value,
      sizeof(T), false, order, tag);
  return result;
}

}  // namespace latency_sim
