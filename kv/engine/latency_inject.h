#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

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
  kLocalDram = 3,
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
uint32_t FixedLatencyFeaturesFast() noexcept;
#endif

inline bool FixedLatencyEnabledFast() noexcept {
  return FixedLatencyFeaturesFast() != 0;
}

LatencySimulator& GlobalLatencySimulator();

class LatencySimulator {
 public:
  explicit LatencySimulator(Config config = {});
  ~LatencySimulator();

  void Configure(Config config);
  const Config& config() const { return config_; }
  uint32_t feature_mask() const noexcept { return feature_mask_; }

  void BeginScope(ScopeKind scope);
  void EndScopeAndDelay();
  bool HasActiveScopeForCurrentThread() const;

  // The caller performs the real access before calling this method.  Only the
  // covered HWCC/SWCC cache lines are added to the current scope's pending
  // delay; no access history, cache state, counters, or shared log is kept.
  void RecordRange(PoolKind pool, AccessKind kind, const void* address,
                   uint64_t bytes);
  void RecordAtomicAccess(AtomicDomain domain, AccessKind kind,
                          const void* address, uint64_t bytes);

  uint64_t PendingDelayNsForTest() const;

 private:
  ThreadState& GetThreadState();

  Config config_;
  uint32_t feature_mask_ = 0;
  uint64_t generation_ = 0;
};

// Typed memory wrappers execute the real operation first and charge exactly
// that operation once.  Their names intentionally do not claim to count it.
template <typename T>
T FixedLatencyMemoryLoad(PoolKind pool, const T* address) {
  if (!FixedLatencyEnabledFast()) return *address;
  const T result = *address;
  GlobalLatencySimulator().RecordRange(pool, AccessKind::kRead, address,
                                       sizeof(T));
  return result;
}

template <typename T>
void FixedLatencyMemoryStore(PoolKind pool, T* address, T value) {
  if (!FixedLatencyEnabledFast()) {
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
  if (!FixedLatencyEnabledFast()) return value.test_and_set(order);
  const bool old = value.test_and_set(order);
  GlobalLatencySimulator().RecordAtomicAccess(
      domain, AccessKind::kAtomicRmw, &value, sizeof(value));
  return old;
}

inline void FixedLatencyAtomicFlagClear(std::atomic_flag& value,
                                        std::memory_order order,
                                        AtomicDomain domain, uint32_t = 0) {
  value.clear(order);
  if (FixedLatencyEnabledFast()) {
    GlobalLatencySimulator().RecordAtomicAccess(
        domain, AccessKind::kAtomicStore, &value, sizeof(value));
  }
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
  if (!FixedLatencyEnabledFast()) return value.load(order);
  const T result = value.load(order);
  GlobalLatencySimulator().RecordAtomicAccess(
      domain, AccessKind::kAtomicLoad, &value, sizeof(T));
  return result;
}

template <typename T>
void FixedLatencyAtomicStore(std::atomic<T>& value, T desired,
                             std::memory_order order, AtomicDomain domain,
                             uint32_t = 0) {
  value.store(desired, order);
  if (FixedLatencyEnabledFast()) {
    GlobalLatencySimulator().RecordAtomicAccess(
        domain, AccessKind::kAtomicStore, &value, sizeof(T));
  }
}

template <typename T>
T FixedLatencyAtomicExchange(std::atomic<T>& value, T desired,
                             std::memory_order order, AtomicDomain domain,
                             uint32_t = 0) {
  const T result = value.exchange(desired, order);
  if (FixedLatencyEnabledFast()) {
    GlobalLatencySimulator().RecordAtomicAccess(
        domain, AccessKind::kAtomicRmw, &value, sizeof(T));
  }
  return result;
}

template <typename T>
T FixedLatencyAtomicFetchAdd(std::atomic<T>& value, T operand,
                             std::memory_order order, AtomicDomain domain,
                             uint32_t = 0) {
  const T result = value.fetch_add(operand, order);
  if (FixedLatencyEnabledFast()) {
    GlobalLatencySimulator().RecordAtomicAccess(
        domain, AccessKind::kAtomicRmw, &value, sizeof(T));
  }
  return result;
}

template <typename T>
T FixedLatencyAtomicFetchSub(std::atomic<T>& value, T operand,
                             std::memory_order order, AtomicDomain domain,
                             uint32_t = 0) {
  const T result = value.fetch_sub(operand, order);
  if (FixedLatencyEnabledFast()) {
    GlobalLatencySimulator().RecordAtomicAccess(
        domain, AccessKind::kAtomicRmw, &value, sizeof(T));
  }
  return result;
}

template <typename T>
T FixedLatencyAtomicFetchOr(std::atomic<T>& value, T operand,
                            std::memory_order order, AtomicDomain domain,
                            uint32_t = 0) {
  const T result = value.fetch_or(operand, order);
  if (FixedLatencyEnabledFast()) {
    GlobalLatencySimulator().RecordAtomicAccess(
        domain, AccessKind::kAtomicRmw, &value, sizeof(T));
  }
  return result;
}

template <typename T>
T FixedLatencyAtomicFetchAnd(std::atomic<T>& value, T operand,
                             std::memory_order order, AtomicDomain domain,
                             uint32_t = 0) {
  const T result = value.fetch_and(operand, order);
  if (FixedLatencyEnabledFast()) {
    GlobalLatencySimulator().RecordAtomicAccess(
        domain, AccessKind::kAtomicRmw, &value, sizeof(T));
  }
  return result;
}

template <typename T>
T FixedLatencyAtomicFetchXor(std::atomic<T>& value, T operand,
                             std::memory_order order, AtomicDomain domain,
                             uint32_t = 0) {
  const T result = value.fetch_xor(operand, order);
  if (FixedLatencyEnabledFast()) {
    GlobalLatencySimulator().RecordAtomicAccess(
        domain, AccessKind::kAtomicRmw, &value, sizeof(T));
  }
  return result;
}

template <typename T>
bool FixedLatencyAtomicCompareExchangeWeak(
    std::atomic<T>& value, T& expected, T desired, std::memory_order success,
    std::memory_order failure, AtomicDomain domain, uint32_t = 0) {
  const bool ok = value.compare_exchange_weak(expected, desired, success,
                                               failure);
  if (FixedLatencyEnabledFast()) {
    GlobalLatencySimulator().RecordAtomicAccess(
        domain, AccessKind::kAtomicRmw, &value, sizeof(T));
  }
  return ok;
}

template <typename T>
bool FixedLatencyAtomicCompareExchangeStrong(
    std::atomic<T>& value, T& expected, T desired, std::memory_order success,
    std::memory_order failure, AtomicDomain domain, uint32_t = 0) {
  const bool ok = value.compare_exchange_strong(expected, desired, success,
                                                failure);
  if (FixedLatencyEnabledFast()) {
    GlobalLatencySimulator().RecordAtomicAccess(
        domain, AccessKind::kAtomicRmw, &value, sizeof(T));
  }
  return ok;
}

}  // namespace latency_sim
