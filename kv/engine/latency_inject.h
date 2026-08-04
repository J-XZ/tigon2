#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace latency_sim {

class LatencySimulator;
struct ThreadState;

// The fixed-latency simulator is the only hardware model retained by TigonKV.
// This bit is kept as a process-local fast-path value for the wrappers below;
// it is not a statistics or event-module registry.
enum Feature : uint32_t { kFixedLatency = 1u << 0 };

enum class MemoryDomain : uint8_t {
  kSwcc = 0,
  kHwcc = 1,
  kOwnerPrivateSwcc = 2,
};
using PoolKind = MemoryDomain;
using AtomicDomain = MemoryDomain;

enum class AccessKind : uint8_t {
  kRead = 0,
  kWrite = 1,
  kAtomicLoad = 2,
  kAtomicStore = 3,
  kAtomicRmw = 4,
  kFlush = 5,
  kInvalidate = 6,
};

enum class ScopeKind : uint8_t {
  kForeground = 0,
  kMerge = 1,
  kOther = 2,
};

struct FixedLatencyConfig {
  bool enabled = false;
  uint64_t cache_line_bytes = 64;
  double swcc_fixed_ns_per_line = 0.0;
  double hwcc_fixed_ns_per_line = 0.0;
  bool foreground_enabled = true;
  bool background_enabled = true;
};

struct Config {
  FixedLatencyConfig fixed_latency;
};

#if defined(TIGONKV_DISABLE_HARDWARE_SIMULATION)
inline constexpr uint32_t FixedLatencyFeaturesFast() noexcept { return 0; }
#else
// Immutable-after-startup feature mask.  Configure() must run at an explicit
// startup boundary (or after business threads joined) and never concurrently
// with wrapper use.  A plain aligned read lets the disabled fast gate be
// hoisted out of hot loops while keeping one predictable branch; no TLS,
// clock, address arithmetic, lock, or allocation is touched on that path.
inline uint32_t g_hardware_simulation_features = 0;
inline uint32_t FixedLatencyFeaturesFast() noexcept {
  return g_hardware_simulation_features;
}
#endif

inline bool FixedLatencyEnabledFast() noexcept {
  return FixedLatencyFeaturesFast() != 0;
}

// Test-only conversions.  RoundDelayPsToNsForTest is round-half-up and cannot
// overflow for any uint64 input; TicksForDelayNsForTest validates the tick
// conversion (finite, positive, <= UINT64_MAX) and hard fails on overflow
// instead of saturating.  Neither is a production statistics interface.
uint64_t RoundDelayPsToNsForTest(uint64_t pending_ps);
uint64_t TicksForDelayNsForTest(double ticks_per_ns, uint64_t ns);

LatencySimulator& GlobalLatencySimulator();

class LatencySimulator {
 public:
  explicit LatencySimulator(Config config = {});
  ~LatencySimulator();

  void Configure(Config config);
  const Config& config() const { return config_; }
  uint32_t feature_mask() const noexcept { return feature_mask_; }

  // Process-initialization registration of immutable HWCC/SWCC mapping
  // boundaries.  When fixed latency is enabled every charged access is
  // validated against the matching domain range and converted to a pool-local
  // offset before cache-line accounting; wrong-domain, out-of-range and
  // overflowing accesses hard fail.
  void RegisterPool(PoolKind pool, const void* base, uint64_t size);
  // Clear all registered pool ranges.  Legal only at an initialization
  // boundary: the gate must be disabled and every business thread joined.
  void ClearPoolRegistrations();

  void BeginScope(ScopeKind scope);
  void EndScopeAndDelay();
  bool HasActiveScopeForCurrentThread() const;
  bool HasActiveForegroundScopeForCurrentThread() const;
  // True only when the current thread's active scope is a top-level
  // foreground scope (depth == 1).  Suspending a nested foreground would
  // otherwise decrement a nesting level without a matching resume and could
  // settle the outer budget while guards are still alive.
  bool HasTopLevelForegroundScopeForCurrentThread() const;
  // Deactivate the current top-level scope without busy-waiting and defer its
  // pending delay to the next outermost EndScopeAndDelay.  Returns true when a
  // scope was actually suspended.  Used to switch scope classes at points
  // where EBR/other guards are still alive; the busy-wait happens only after
  // every guard has exited.
  bool SuspendScopeAndDelayLater();
  // Restore a top-level foreground scope after a suspension.
  void ResumeScope();

  // The caller performs the real access before calling this method.  Only the
  // covered HWCC/SWCC cache lines are added to the current scope's pending
  // delay; no access history, cache state, counters, or shared log is kept.
  void RecordRange(PoolKind pool, AccessKind kind, const void* address,
                   uint64_t bytes);
  void RecordAtomicAccess(AtomicDomain domain, AccessKind kind,
                          const void* address, uint64_t bytes);

  uint64_t PendingDelayNsForTest() const;
  uint64_t PendingDelayPsForTest() const;
  size_t ThreadStateCountForTest() const;

 private:
  ThreadState& GetThreadState();
  uint64_t LineDelayPs(PoolKind pool) const;

  Config config_;
  uint32_t feature_mask_ = 0;
  uint64_t generation_ = 0;
  struct PoolRange {
    uintptr_t base = 0;
    uint64_t size = 0;
  };
  std::array<std::vector<PoolRange>, 3> pool_ranges_;
  uint64_t swcc_delay_ps_per_line_ = 0;
  uint64_t hwcc_delay_ps_per_line_ = 0;
};

// Typed memory wrappers execute the real operation first and charge exactly
// that operation once.  Their names intentionally do not claim to count it.
template <typename T>
T FixedLatencyMemoryLoad(PoolKind pool, const T* address) {
  if (!FixedLatencyEnabledFast()) [[likely]] return *address;
  const T result = *address;
  GlobalLatencySimulator().RecordRange(pool, AccessKind::kRead, address,
                                       sizeof(T));
  return result;
}

template <typename T>
void FixedLatencyMemoryStore(PoolKind pool, T* address, T value) {
  if (!FixedLatencyEnabledFast()) [[likely]] {
    *address = value;
    return;
  }
  *address = value;
  GlobalLatencySimulator().RecordRange(pool, AccessKind::kWrite, address,
                                       sizeof(T));
}

inline bool FixedLatencyAtomicFlagTestAndSet(std::atomic_flag& value,
                                             std::memory_order order,
                                             AtomicDomain domain,
                                             uint32_t = 0) {
  if (!FixedLatencyEnabledFast()) [[likely]] return value.test_and_set(order);
  const bool old = value.test_and_set(order);
  GlobalLatencySimulator().RecordAtomicAccess(
      domain, AccessKind::kAtomicRmw, &value, sizeof(value));
  return old;
}

inline void FixedLatencyAtomicFlagClear(std::atomic_flag& value,
                                        std::memory_order order,
                                        AtomicDomain domain, uint32_t = 0) {
  if (!FixedLatencyEnabledFast()) [[likely]] {
    value.clear(order);
    return;
  }
  value.clear(order);
  GlobalLatencySimulator().RecordAtomicAccess(
      domain, AccessKind::kAtomicStore, &value, sizeof(value));
}

inline void FixedLatencyAtomicFence(std::memory_order order, AtomicDomain,
                                    uint32_t = 0) {
  // A fence has no memory address or covered cache line.  Preserve its real
  // ordering semantics without inventing an access charge.
  std::atomic_thread_fence(order);
}

template <typename T>
T FixedLatencyAtomicLoad(const std::atomic<T>& value, std::memory_order order,
                         AtomicDomain domain, uint32_t = 0) {
  if (!FixedLatencyEnabledFast()) [[likely]] return value.load(order);
  const T result = value.load(order);
  GlobalLatencySimulator().RecordAtomicAccess(
      domain, AccessKind::kAtomicLoad, &value, sizeof(T));
  return result;
}

template <typename T>
void FixedLatencyAtomicStore(std::atomic<T>& value, T desired,
                             std::memory_order order, AtomicDomain domain,
                             uint32_t = 0) {
  if (!FixedLatencyEnabledFast()) [[likely]] {
    value.store(desired, order);
    return;
  }
  value.store(desired, order);
  GlobalLatencySimulator().RecordAtomicAccess(
      domain, AccessKind::kAtomicStore, &value, sizeof(T));
}

template <typename T>
T FixedLatencyAtomicExchange(std::atomic<T>& value, T desired,
                             std::memory_order order, AtomicDomain domain,
                             uint32_t = 0) {
  if (!FixedLatencyEnabledFast()) [[likely]]
    return value.exchange(desired, order);
  const T result = value.exchange(desired, order);
  GlobalLatencySimulator().RecordAtomicAccess(
      domain, AccessKind::kAtomicRmw, &value, sizeof(T));
  return result;
}

template <typename T>
T FixedLatencyAtomicFetchAdd(std::atomic<T>& value, T operand,
                             std::memory_order order, AtomicDomain domain,
                             uint32_t = 0) {
  if (!FixedLatencyEnabledFast()) [[likely]]
    return value.fetch_add(operand, order);
  const T result = value.fetch_add(operand, order);
  GlobalLatencySimulator().RecordAtomicAccess(
      domain, AccessKind::kAtomicRmw, &value, sizeof(T));
  return result;
}

template <typename T>
T FixedLatencyAtomicFetchSub(std::atomic<T>& value, T operand,
                             std::memory_order order, AtomicDomain domain,
                             uint32_t = 0) {
  if (!FixedLatencyEnabledFast()) [[likely]]
    return value.fetch_sub(operand, order);
  const T result = value.fetch_sub(operand, order);
  GlobalLatencySimulator().RecordAtomicAccess(
      domain, AccessKind::kAtomicRmw, &value, sizeof(T));
  return result;
}

template <typename T>
T FixedLatencyAtomicFetchOr(std::atomic<T>& value, T operand,
                            std::memory_order order, AtomicDomain domain,
                            uint32_t = 0) {
  if (!FixedLatencyEnabledFast()) [[likely]]
    return value.fetch_or(operand, order);
  const T result = value.fetch_or(operand, order);
  GlobalLatencySimulator().RecordAtomicAccess(
      domain, AccessKind::kAtomicRmw, &value, sizeof(T));
  return result;
}

template <typename T>
T FixedLatencyAtomicFetchAnd(std::atomic<T>& value, T operand,
                             std::memory_order order, AtomicDomain domain,
                             uint32_t = 0) {
  if (!FixedLatencyEnabledFast()) [[likely]]
    return value.fetch_and(operand, order);
  const T result = value.fetch_and(operand, order);
  GlobalLatencySimulator().RecordAtomicAccess(
      domain, AccessKind::kAtomicRmw, &value, sizeof(T));
  return result;
}

template <typename T>
T FixedLatencyAtomicFetchXor(std::atomic<T>& value, T operand,
                             std::memory_order order, AtomicDomain domain,
                             uint32_t = 0) {
  if (!FixedLatencyEnabledFast()) [[likely]]
    return value.fetch_xor(operand, order);
  const T result = value.fetch_xor(operand, order);
  GlobalLatencySimulator().RecordAtomicAccess(
      domain, AccessKind::kAtomicRmw, &value, sizeof(T));
  return result;
}

template <typename T>
bool FixedLatencyAtomicCompareExchangeWeak(
    std::atomic<T>& value, T& expected, T desired, std::memory_order success,
    std::memory_order failure, AtomicDomain domain, uint32_t = 0) {
  if (!FixedLatencyEnabledFast()) [[likely]]
    return value.compare_exchange_weak(expected, desired, success, failure);
  const bool ok = value.compare_exchange_weak(expected, desired, success,
                                               failure);
  {
    GlobalLatencySimulator().RecordAtomicAccess(
        domain, AccessKind::kAtomicRmw, &value, sizeof(T));
  }
  return ok;
}

template <typename T>
bool FixedLatencyAtomicCompareExchangeStrong(
    std::atomic<T>& value, T& expected, T desired, std::memory_order success,
    std::memory_order failure, AtomicDomain domain, uint32_t = 0) {
  if (!FixedLatencyEnabledFast()) [[likely]]
    return value.compare_exchange_strong(expected, desired, success, failure);
  const bool ok = value.compare_exchange_strong(expected, desired, success,
                                                failure);
  {
    GlobalLatencySimulator().RecordAtomicAccess(
        domain, AccessKind::kAtomicRmw, &value, sizeof(T));
  }
  return ok;
}

}  // namespace latency_sim
