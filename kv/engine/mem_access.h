#pragma once

#include <latency_sim/access.h>
#include <latency_sim/atomic_access.h>
#include <latency_sim/bulk_access.h>
#include <latency_sim/config.h>
#include <latency_sim/domain.h>
#include <latency_sim/scope.h>
#include <latency_sim/simulator.h>

#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>

namespace tigonkv::engine::mem_access {

// Thin project-scoped alias over the library ScopeGuard.  The library settles
// the pending delay at the outermost safe scope exit; this wrapper only maps
// tigonkv::LatencyScope to latency_sim::ScopeGuard when simulation is compiled
// in.  The compile-off branch is deliberately an empty type: it must not
// retain project TLS or RAII bookkeeping at Debug/O0.
#if defined(LATENCY_SIM_COMPILE_OFF)

class LatencyScope {
 public:
  LATENCY_SIM_FORCE_INLINE explicit LatencyScope(
      latency_sim::ExecutionClass) {}
  LatencyScope(const LatencyScope&) = delete;
  LatencyScope& operator=(const LatencyScope&) = delete;
};

class ForegroundScopeSuspension {
 public:
  ForegroundScopeSuspension() = default;
  ForegroundScopeSuspension(const ForegroundScopeSuspension&) = delete;
  ForegroundScopeSuspension& operator=(const ForegroundScopeSuspension&) =
      delete;
};

#else

class LatencyScope {
 public:
  explicit LatencyScope(latency_sim::ExecutionClass scope)
      : previous_scope_(current_scope_), guard_(scope) {
    current_scope_ = this;
  }
  ~LatencyScope() { current_scope_ = previous_scope_; }
  LatencyScope(const LatencyScope&) = delete;
  LatencyScope& operator=(const LatencyScope&) = delete;

  latency_sim::ScopeGuard& Inner() { return guard_; }
  latency_sim::ScopeSuspension SuspendWithoutWaiting() {
    return guard_.SuspendWithoutWaiting();
  }
  static LatencyScope* Current() { return current_scope_; }

 private:
  inline static thread_local LatencyScope* current_scope_ = nullptr;
  LatencyScope* previous_scope_ = nullptr;
  latency_sim::ScopeGuard guard_;
};

// Suspends an active top-level foreground scope without busy-waiting and
// restores it on destruction.  The busy-wait for the suspended segment happens
// later at the outermost scope exit, after EBR and other protocol guards have
// gone.  RAII guarantees that exceptions and early returns neither lose nor
// duplicate the foreground scope restoration.  Nested scopes are never
// touched (the library suspension returns false for them), so a suspension
// inside an already nested scope never swallows a nesting level.
class ForegroundScopeSuspension {
 public:
  ForegroundScopeSuspension() {
    // The project never inspects simulator TLS.  The current LatencyScope
    // delegates suspension to the public move-only RAII token; a nested scope
    // returns an empty token and is left untouched.
    if (auto* scope = LatencyScope::Current()) {
      auto token = scope->SuspendWithoutWaiting();
      if (token) suspension_.emplace(std::move(token));
    }
  }
  ~ForegroundScopeSuspension() = default;
  ForegroundScopeSuspension(const ForegroundScopeSuspension&) = delete;
  ForegroundScopeSuspension& operator=(const ForegroundScopeSuspension&) =
      delete;

 private:
  std::optional<latency_sim::ScopeSuspension> suspension_;
};

#endif  // LATENCY_SIM_COMPILE_OFF

inline void Record(latency_sim::MemoryDomain pool, latency_sim::AccessKind kind,
                   const void* address, size_t bytes) {
  latency_sim::FixedLatencyChargeRange(pool, kind, address, bytes);
}

inline void PrivateRead(const void* address, size_t bytes) {
  Record(latency_sim::MemoryDomain::kOwnerPrivateSwcc,
         latency_sim::AccessKind::kRead, address, bytes);
}
inline void PrivateWrite(const void* address, size_t bytes) {
  Record(latency_sim::MemoryDomain::kOwnerPrivateSwcc,
         latency_sim::AccessKind::kWrite, address, bytes);
}

template <typename T>
inline T PrivateAtomicLoad(const std::atomic<T>& value,
                           std::memory_order order = std::memory_order_seq_cst) {
  return latency_sim::FixedLatencyAtomicLoad(
      value, order, latency_sim::MemoryDomain::kOwnerPrivateSwcc);
}
template <typename T>
inline void PrivateAtomicStore(std::atomic<T>& value, T desired,
                               std::memory_order order = std::memory_order_seq_cst) {
  latency_sim::FixedLatencyAtomicStore(
      value, desired, order, latency_sim::MemoryDomain::kOwnerPrivateSwcc);
}
template <typename T>
inline T PrivateAtomicExchange(std::atomic<T>& value, T desired,
                               std::memory_order order = std::memory_order_seq_cst) {
  return latency_sim::FixedLatencyAtomicExchange(
      value, desired, order, latency_sim::MemoryDomain::kOwnerPrivateSwcc);
}
template <typename T>
inline T PrivateAtomicFetchAdd(std::atomic<T>& value, T operand,
                               std::memory_order order = std::memory_order_seq_cst) {
  return latency_sim::FixedLatencyAtomicFetchAdd(
      value, operand, order, latency_sim::MemoryDomain::kOwnerPrivateSwcc);
}
template <typename T>
inline T PrivateAtomicFetchSub(std::atomic<T>& value, T operand,
                               std::memory_order order = std::memory_order_seq_cst) {
  return latency_sim::FixedLatencyAtomicFetchSub(
      value, operand, order, latency_sim::MemoryDomain::kOwnerPrivateSwcc);
}
template <typename T>
inline T PrivateAtomicFetchOr(std::atomic<T>& value, T operand,
                              std::memory_order order = std::memory_order_seq_cst) {
  return latency_sim::FixedLatencyAtomicFetchOr(
      value, operand, order, latency_sim::MemoryDomain::kOwnerPrivateSwcc);
}
template <typename T>
inline T PrivateAtomicFetchAnd(std::atomic<T>& value, T operand,
                               std::memory_order order = std::memory_order_seq_cst) {
  return latency_sim::FixedLatencyAtomicFetchAnd(
      value, operand, order, latency_sim::MemoryDomain::kOwnerPrivateSwcc);
}
template <typename T>
inline T PrivateAtomicFetchXor(std::atomic<T>& value, T operand,
                               std::memory_order order = std::memory_order_seq_cst) {
  return latency_sim::FixedLatencyAtomicFetchXor(
      value, operand, order, latency_sim::MemoryDomain::kOwnerPrivateSwcc);
}
template <typename T>
inline bool PrivateAtomicCompareExchangeWeak(
    std::atomic<T>& value, T& expected, T desired, std::memory_order success,
    std::memory_order failure) {
  return latency_sim::FixedLatencyAtomicCompareExchangeWeak(
      value, expected, desired, success, failure,
      latency_sim::MemoryDomain::kOwnerPrivateSwcc);
}
template <typename T>
inline bool PrivateAtomicCompareExchangeStrong(
    std::atomic<T>& value, T& expected, T desired, std::memory_order success,
    std::memory_order failure) {
  return latency_sim::FixedLatencyAtomicCompareExchangeStrong(
      value, expected, desired, success, failure,
      latency_sim::MemoryDomain::kOwnerPrivateSwcc);
}

inline void HwccRead(const void* address, size_t bytes) {
  Record(latency_sim::MemoryDomain::kHwcc, latency_sim::AccessKind::kRead, address,
         bytes);
}
inline void HwccWrite(const void* address, size_t bytes) {
  Record(latency_sim::MemoryDomain::kHwcc, latency_sim::AccessKind::kWrite, address,
         bytes);
}
template <typename T>
inline T HwccLoad(const T* address) {
  return latency_sim::FixedLatencyMemoryLoad(
      latency_sim::MemoryDomain::kHwcc, address);
}
template <typename T>
inline void HwccStore(T* address, T value) {
  latency_sim::FixedLatencyMemoryStore(
      latency_sim::MemoryDomain::kHwcc, address, value);
}
inline void* HwccCopyLocalToShared(void* dst, const void* src, size_t bytes) {
  return latency_sim::FixedLatencyCopyLocalToShared(
      latency_sim::MemoryDomain::kHwcc, dst, src, bytes);
}
inline void* HwccCopySharedToLocal(void* dst, const void* src, size_t bytes) {
  return latency_sim::FixedLatencyCopySharedToLocal(
      latency_sim::MemoryDomain::kHwcc, dst, src, bytes);
}
inline void* HwccMemsetShared(void* dst, int value, size_t bytes) {
  return latency_sim::FixedLatencyMemsetShared(
      latency_sim::MemoryDomain::kHwcc, dst, value, bytes);
}
template <typename T>
inline T HwccAtomicLoad(const std::atomic<T>& value,
                        std::memory_order order = std::memory_order_seq_cst) {
  return latency_sim::FixedLatencyAtomicLoad(
      value, order, latency_sim::MemoryDomain::kHwcc);
}
template <typename T>
inline void HwccAtomicStore(std::atomic<T>& value, T desired,
                            std::memory_order order = std::memory_order_seq_cst) {
  latency_sim::FixedLatencyAtomicStore(value, desired, order,
                                       latency_sim::MemoryDomain::kHwcc);
}
template <typename T>
inline T HwccAtomicExchange(std::atomic<T>& value, T desired,
                            std::memory_order order = std::memory_order_seq_cst) {
  return latency_sim::FixedLatencyAtomicExchange(
      value, desired, order, latency_sim::MemoryDomain::kHwcc);
}
template <typename T>
inline T HwccAtomicFetchAdd(std::atomic<T>& value, T operand,
                            std::memory_order order = std::memory_order_seq_cst) {
  return latency_sim::FixedLatencyAtomicFetchAdd(
      value, operand, order, latency_sim::MemoryDomain::kHwcc);
}
template <typename T>
inline T HwccAtomicFetchSub(std::atomic<T>& value, T operand,
                            std::memory_order order = std::memory_order_seq_cst) {
  return latency_sim::FixedLatencyAtomicFetchSub(
      value, operand, order, latency_sim::MemoryDomain::kHwcc);
}
template <typename T>
inline T HwccAtomicFetchOr(std::atomic<T>& value, T operand,
                           std::memory_order order = std::memory_order_seq_cst) {
  return latency_sim::FixedLatencyAtomicFetchOr(
      value, operand, order, latency_sim::MemoryDomain::kHwcc);
}
template <typename T>
inline T HwccAtomicFetchAnd(std::atomic<T>& value, T operand,
                            std::memory_order order = std::memory_order_seq_cst) {
  return latency_sim::FixedLatencyAtomicFetchAnd(
      value, operand, order, latency_sim::MemoryDomain::kHwcc);
}
template <typename T>
inline T HwccAtomicFetchXor(std::atomic<T>& value, T operand,
                            std::memory_order order = std::memory_order_seq_cst) {
  return latency_sim::FixedLatencyAtomicFetchXor(
      value, operand, order, latency_sim::MemoryDomain::kHwcc);
}
template <typename T>
inline bool HwccAtomicCompareExchangeWeak(
    std::atomic<T>& value, T& expected, T desired, std::memory_order success,
    std::memory_order failure) {
  return latency_sim::FixedLatencyAtomicCompareExchangeWeak(
      value, expected, desired, success, failure,
      latency_sim::MemoryDomain::kHwcc);
}
template <typename T>
inline bool HwccAtomicCompareExchangeStrong(
    std::atomic<T>& value, T& expected, T desired, std::memory_order success,
    std::memory_order failure) {
  return latency_sim::FixedLatencyAtomicCompareExchangeStrong(
      value, expected, desired, success, failure,
      latency_sim::MemoryDomain::kHwcc);
}

inline void TransportRead(const void* address, size_t bytes) {
  HwccRead(address, bytes);
}
inline void TransportWrite(const void* address, size_t bytes) {
  HwccWrite(address, bytes);
}
inline void SharedPayloadRead(const void* address, size_t bytes) {
  Record(latency_sim::MemoryDomain::kSwcc, latency_sim::AccessKind::kRead, address,
         bytes);
}
inline void SharedPayloadWrite(const void* address, size_t bytes) {
  Record(latency_sim::MemoryDomain::kSwcc, latency_sim::AccessKind::kWrite, address,
         bytes);
}

inline void* PrivateCopyLocalToShared(void* dst, const void* src, size_t bytes) {
  return latency_sim::FixedLatencyCopyLocalToShared(
      latency_sim::MemoryDomain::kOwnerPrivateSwcc, dst, src, bytes);
}

inline void* PrivateCopySharedToLocal(void* dst, const void* src, size_t bytes) {
  return latency_sim::FixedLatencyCopySharedToLocal(
      latency_sim::MemoryDomain::kOwnerPrivateSwcc, dst, src, bytes);
}

inline void* PrivateMemsetShared(void* dst, int value, size_t bytes) {
  return latency_sim::FixedLatencyMemsetShared(
      latency_sim::MemoryDomain::kOwnerPrivateSwcc, dst, value, bytes);
}

inline void* SharedMemmove(latency_sim::MemoryDomain pool, void* dst,
                           const void* src, size_t bytes) {
  return latency_sim::FixedLatencyMemmoveSharedToShared(
      pool, dst, pool, src, bytes);
}

template <typename T>
inline T SharedPayloadAtomicLoad(
    const std::atomic<T>& value,
    std::memory_order order = std::memory_order_seq_cst) {
  return latency_sim::FixedLatencyAtomicLoad(
      value, order, latency_sim::MemoryDomain::kSwcc);
}
template <typename T>
inline void SharedPayloadAtomicStore(
    std::atomic<T>& value, T desired,
    std::memory_order order = std::memory_order_seq_cst) {
  latency_sim::FixedLatencyAtomicStore(value, desired, order,
                                       latency_sim::MemoryDomain::kSwcc);
}
template <typename T>
inline T SharedPayloadAtomicExchange(
    std::atomic<T>& value, T desired,
    std::memory_order order = std::memory_order_seq_cst) {
  return latency_sim::FixedLatencyAtomicExchange(
      value, desired, order, latency_sim::MemoryDomain::kSwcc);
}
template <typename T>
inline bool SharedPayloadAtomicCompareExchangeWeak(
    std::atomic<T>& value, T& expected, T desired,
    std::memory_order success, std::memory_order failure) {
  return latency_sim::FixedLatencyAtomicCompareExchangeWeak(
      value, expected, desired, success, failure,
      latency_sim::MemoryDomain::kSwcc);
}
template <typename T>
inline bool SharedPayloadAtomicCompareExchangeStrong(
    std::atomic<T>& value, T& expected, T desired,
    std::memory_order success, std::memory_order failure) {
  return latency_sim::FixedLatencyAtomicCompareExchangeStrong(
      value, expected, desired, success, failure,
      latency_sim::MemoryDomain::kSwcc);
}

}  // namespace tigonkv::engine::mem_access
