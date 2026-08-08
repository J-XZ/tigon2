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

namespace tigonkv::engine::mem_access {

#if !defined(LATENCY_SIM_COMPILE_OFF)
// Non-zero while the current thread is inside a remote-delete critical state
// (row write_locked with valid cleared): every transport poll must defer its
// settlement into the single deferred segment instead of busy-waiting, so
// nothing settles until the commit/rollback releases the row lock and SCC
// guards.
inline thread_local int TlsDeferTransportSettlementDepth = 0;
#endif

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
      generation_ = latency_sim::GlobalLatencySimulator()
                        .SuspendScopeAndDelayLater();
    }
#endif
  }
  ~ForegroundScopeSuspension() {
#if !defined(LATENCY_SIM_COMPILE_OFF)
    if (generation_ != 0)
      latency_sim::GlobalLatencySimulator().ResumeScope(
          latency_sim::ExecutionClass::kForeground, generation_);
#endif
  }
  ForegroundScopeSuspension(const ForegroundScopeSuspension&) = delete;
  ForegroundScopeSuspension& operator=(const ForegroundScopeSuspension&) =
      delete;

 private:
  // Lifecycle generation captured at suspension; the resume must present the
  // same generation so a token can never cross a lifecycle reopen.
  std::uint64_t generation_ = 0;
};

// RAII for the remote-delete critical state (deferred transport settlement
// mode).  Any return or exception decrements the defer depth exactly once.
class DeferTransportSettlement {
 public:
  DeferTransportSettlement() {
#if !defined(LATENCY_SIM_COMPILE_OFF)
    ++TlsDeferTransportSettlementDepth;
#endif
  }
  ~DeferTransportSettlement() {
#if !defined(LATENCY_SIM_COMPILE_OFF)
    --TlsDeferTransportSettlementDepth;
#endif
  }
  DeferTransportSettlement(const DeferTransportSettlement&) = delete;
  DeferTransportSettlement& operator=(const DeferTransportSettlement&) =
      delete;
};

// RAII for one transport poll executed inside the remote-delete critical
// state.  The poll must not settle (no busy-wait while the row is
// write_locked/invalid): the guard temporarily resumes the enclosing scope as
// a background-class scope so the poll charges into the same deferred
// segment, then suspends it again and restores the original thread scope
// state.  The destructor runs on every path, including a throwing poll body,
// so TLS depth/class/generation and the pending budget are never stranded.
class DeferredTransportPollScope {
 public:
  DeferredTransportPollScope() {
#if !defined(LATENCY_SIM_COMPILE_OFF)
    if (TlsDeferTransportSettlementDepth <= 0) return;
    auto &simulator = latency_sim::GlobalLatencySimulator();
    auto &state = latency_sim::detail::g_thread_state;
    if (state.scope_suspended) {
      // The enclosing scope is already suspended (cooperative foreground
      // wait): resume it with its original class so the V6 resume contract
      // (resume class must match the suspended scope class) holds; the poll
      // still charges into the deferred segment, and on exit suspend it
      // again.
      was_suspended_ = true;
      generation_ = state.generation;
      simulator.ResumeScope(state.suspended_scope_class, generation_);
      return;
    }
    if (state.scope_depth != 0) {
      // An active top-level scope of any class: suspend it, run the poll as a
      // background scope, then suspend again and restore the original class.
      outer_class_ = state.scope_class;
      generation_ = simulator.SuspendScopeAndDelayLater();
      if (generation_ != 0) {
        simulator.ResumeScope(latency_sim::ExecutionClass::kBackground,
                              generation_);
        return;
      }
    }
    // No suspendable enclosing scope: the poll runs in a plain nested
    // background scope (not reachable on the delete path, which always has an
    // enclosing scope).
    fallback_scope_.emplace(latency_sim::ExecutionClass::kBackground);
#endif
  }
  ~DeferredTransportPollScope() {
#if !defined(LATENCY_SIM_COMPILE_OFF)
    if (fallback_scope_.has_value()) return;
    if (generation_ == 0) return;
    auto &simulator = latency_sim::GlobalLatencySimulator();
    if (simulator.SuspendScopeAndDelayLater() == 0) {
      // The resumed background scope must always be suspendable.
      latency_sim::HardFail("deferred transport poll lost its resume scope");
    }
    if (!was_suspended_) {
      simulator.ResumeScope(outer_class_, generation_);
    }
#endif
  }
  DeferredTransportPollScope(const DeferredTransportPollScope&) = delete;
  DeferredTransportPollScope& operator=(const DeferredTransportPollScope&) =
      delete;

 private:
  bool was_suspended_ = false;
  std::uint64_t generation_ = 0;
  latency_sim::ExecutionClass outer_class_ =
      latency_sim::ExecutionClass::kBackground;
  std::optional<latency_sim::ScopeGuard> fallback_scope_;
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
