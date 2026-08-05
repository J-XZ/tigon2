#pragma once

#include <latency_sim/access.h>
#include <latency_sim/atomic_access.h>
#include <latency_sim/config.h>
#include <latency_sim/domain.h>
#include <latency_sim/scope.h>
#include <latency_sim/simulator.h>

#include <atomic>
#include <cstddef>

namespace tigonkv::engine::mem_access {

// Thin project-scoped alias over the library ScopeGuard.  The library settles
// the pending delay at the outermost safe scope exit; this wrapper only maps
// tigonkv::LatencyScope to latency_sim::ScopeGuard.
class LatencyScope {
 public:
  explicit LatencyScope(latency_sim::ExecutionClass scope) : guard_(scope) {}
  LatencyScope(const LatencyScope&) = delete;
  LatencyScope& operator=(const LatencyScope&) = delete;

  latency_sim::ScopeGuard& Inner() { return guard_; }

 private:
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
    // No runtime gate: in a compile-on build the simulator is always active;
    // in a compile-off build the simulator does not exist and this is inert.
#if !defined(LATENCY_SIM_COMPILE_OFF)
    if (latency_sim::GlobalLatencySimulator()
            .HasTopLevelScopeForCurrentThread(
                latency_sim::ExecutionClass::kForeground)) {
      suspended_ = latency_sim::GlobalLatencySimulator()
                       .SuspendScopeAndDelayLater();
    }
#endif
  }
  ~ForegroundScopeSuspension() {
#if !defined(LATENCY_SIM_COMPILE_OFF)
    if (suspended_)
      latency_sim::GlobalLatencySimulator().ResumeScope(
          latency_sim::ExecutionClass::kForeground);
#endif
  }
  ForegroundScopeSuspension(const ForegroundScopeSuspension&) = delete;
  ForegroundScopeSuspension& operator=(const ForegroundScopeSuspension&) =
      delete;

 private:
  bool suspended_ = false;
};

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
inline void SwccWriteback(const void* address, size_t bytes) {
  latency_sim::FixedLatencyFlushAuditTag(address, bytes);
}
inline void SwccInvalidate(const void* address, size_t bytes) {
  latency_sim::FixedLatencyInvalidateAuditTag(address, bytes);
}
inline void SharedPayloadRead(const void* address, size_t bytes) {
  Record(latency_sim::MemoryDomain::kSwcc, latency_sim::AccessKind::kRead, address,
         bytes);
}
inline void SharedPayloadWrite(const void* address, size_t bytes) {
  Record(latency_sim::MemoryDomain::kSwcc, latency_sim::AccessKind::kWrite, address,
         bytes);
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
