#pragma once

#include "kv/engine/latency_inject.h"

#include <atomic>
#include <cstddef>

namespace tigonkv::engine::mem_access {

class LatencyScope {
 public:
  explicit LatencyScope(latency_sim::ScopeKind scope) {
    active_ = latency_sim::FixedLatencyEnabledFast();
    if (active_) latency_sim::GlobalLatencySimulator().BeginScope(scope);
  }
  ~LatencyScope() {
    if (active_) latency_sim::GlobalLatencySimulator().EndScopeAndDelay();
  }
  LatencyScope(const LatencyScope&) = delete;
  LatencyScope& operator=(const LatencyScope&) = delete;

 private:
  bool active_ = false;
};

inline bool HasActiveScope() {
  return latency_sim::FixedLatencyEnabledFast() &&
         latency_sim::GlobalLatencySimulator().HasActiveScopeForCurrentThread();
}

inline void EndActiveScopeAndDelay() {
  if (latency_sim::FixedLatencyEnabledFast())
    latency_sim::GlobalLatencySimulator().EndScopeAndDelay();
}

inline void BeginForegroundScope() {
  if (latency_sim::FixedLatencyEnabledFast())
    latency_sim::GlobalLatencySimulator().BeginScope(
        latency_sim::ScopeKind::kForeground);
}

inline void Record(latency_sim::PoolKind pool, latency_sim::AccessKind kind,
                   const void* address, size_t bytes) {
  if (latency_sim::FixedLatencyEnabledFast())
    latency_sim::GlobalLatencySimulator().RecordRange(pool, kind, address,
                                                      bytes);
}

inline void PrivateRead(const void* address, size_t bytes) {
  Record(latency_sim::PoolKind::kOwnerPrivateSwcc,
         latency_sim::AccessKind::kRead, address, bytes);
}
inline void PrivateWrite(const void* address, size_t bytes) {
  Record(latency_sim::PoolKind::kOwnerPrivateSwcc,
         latency_sim::AccessKind::kWrite, address, bytes);
}

template <typename T>
inline T PrivateAtomicLoad(const std::atomic<T>& value,
                           std::memory_order order = std::memory_order_seq_cst,
                           uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicLoad(
      value, order, latency_sim::AtomicDomain::kOwnerPrivateSwcc, tag);
}
template <typename T>
inline void PrivateAtomicStore(std::atomic<T>& value, T desired,
                               std::memory_order order = std::memory_order_seq_cst,
                               uint32_t tag = 0) {
  latency_sim::FixedLatencyAtomicStore(
      value, desired, order, latency_sim::AtomicDomain::kOwnerPrivateSwcc,
      tag);
}
template <typename T>
inline T PrivateAtomicExchange(std::atomic<T>& value, T desired,
                               std::memory_order order = std::memory_order_seq_cst,
                               uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicExchange(
      value, desired, order, latency_sim::AtomicDomain::kOwnerPrivateSwcc,
      tag);
}
template <typename T>
inline T PrivateAtomicFetchAdd(std::atomic<T>& value, T operand,
                               std::memory_order order = std::memory_order_seq_cst,
                               uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicFetchAdd(
      value, operand, order, latency_sim::AtomicDomain::kOwnerPrivateSwcc,
      tag);
}
template <typename T>
inline T PrivateAtomicFetchSub(std::atomic<T>& value, T operand,
                               std::memory_order order = std::memory_order_seq_cst,
                               uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicFetchSub(
      value, operand, order, latency_sim::AtomicDomain::kOwnerPrivateSwcc,
      tag);
}
template <typename T>
inline T PrivateAtomicFetchOr(std::atomic<T>& value, T operand,
                              std::memory_order order = std::memory_order_seq_cst,
                              uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicFetchOr(
      value, operand, order, latency_sim::AtomicDomain::kOwnerPrivateSwcc,
      tag);
}
template <typename T>
inline T PrivateAtomicFetchAnd(std::atomic<T>& value, T operand,
                               std::memory_order order = std::memory_order_seq_cst,
                               uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicFetchAnd(
      value, operand, order, latency_sim::AtomicDomain::kOwnerPrivateSwcc,
      tag);
}
template <typename T>
inline T PrivateAtomicFetchXor(std::atomic<T>& value, T operand,
                               std::memory_order order = std::memory_order_seq_cst,
                               uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicFetchXor(
      value, operand, order, latency_sim::AtomicDomain::kOwnerPrivateSwcc,
      tag);
}
template <typename T>
inline bool PrivateAtomicCompareExchangeWeak(
    std::atomic<T>& value, T& expected, T desired, std::memory_order success,
    std::memory_order failure, uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicCompareExchangeWeak(
      value, expected, desired, success, failure,
      latency_sim::AtomicDomain::kOwnerPrivateSwcc, tag);
}
template <typename T>
inline bool PrivateAtomicCompareExchangeStrong(
    std::atomic<T>& value, T& expected, T desired, std::memory_order success,
    std::memory_order failure, uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicCompareExchangeStrong(
      value, expected, desired, success, failure,
      latency_sim::AtomicDomain::kOwnerPrivateSwcc, tag);
}

inline void HwccRead(const void* address, size_t bytes) {
  Record(latency_sim::PoolKind::kHwcc, latency_sim::AccessKind::kRead, address,
         bytes);
}
inline void HwccWrite(const void* address, size_t bytes) {
  Record(latency_sim::PoolKind::kHwcc, latency_sim::AccessKind::kWrite, address,
         bytes);
}
template <typename T>
inline T HwccAtomicLoad(const std::atomic<T>& value,
                        std::memory_order order = std::memory_order_seq_cst,
                        uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicLoad(
      value, order, latency_sim::AtomicDomain::kHwcc, tag);
}
template <typename T>
inline void HwccAtomicStore(std::atomic<T>& value, T desired,
                            std::memory_order order = std::memory_order_seq_cst,
                            uint32_t tag = 0) {
  latency_sim::FixedLatencyAtomicStore(value, desired, order,
                                       latency_sim::AtomicDomain::kHwcc, tag);
}
template <typename T>
inline T HwccAtomicExchange(std::atomic<T>& value, T desired,
                            std::memory_order order = std::memory_order_seq_cst,
                            uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicExchange(
      value, desired, order, latency_sim::AtomicDomain::kHwcc, tag);
}
template <typename T>
inline T HwccAtomicFetchAdd(std::atomic<T>& value, T operand,
                            std::memory_order order = std::memory_order_seq_cst,
                            uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicFetchAdd(
      value, operand, order, latency_sim::AtomicDomain::kHwcc, tag);
}
template <typename T>
inline T HwccAtomicFetchSub(std::atomic<T>& value, T operand,
                            std::memory_order order = std::memory_order_seq_cst,
                            uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicFetchSub(
      value, operand, order, latency_sim::AtomicDomain::kHwcc, tag);
}
template <typename T>
inline T HwccAtomicFetchOr(std::atomic<T>& value, T operand,
                           std::memory_order order = std::memory_order_seq_cst,
                           uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicFetchOr(
      value, operand, order, latency_sim::AtomicDomain::kHwcc, tag);
}
template <typename T>
inline T HwccAtomicFetchAnd(std::atomic<T>& value, T operand,
                            std::memory_order order = std::memory_order_seq_cst,
                            uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicFetchAnd(
      value, operand, order, latency_sim::AtomicDomain::kHwcc, tag);
}
template <typename T>
inline T HwccAtomicFetchXor(std::atomic<T>& value, T operand,
                            std::memory_order order = std::memory_order_seq_cst,
                            uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicFetchXor(
      value, operand, order, latency_sim::AtomicDomain::kHwcc, tag);
}
template <typename T>
inline bool HwccAtomicCompareExchangeWeak(
    std::atomic<T>& value, T& expected, T desired, std::memory_order success,
    std::memory_order failure, uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicCompareExchangeWeak(
      value, expected, desired, success, failure,
      latency_sim::AtomicDomain::kHwcc, tag);
}
template <typename T>
inline bool HwccAtomicCompareExchangeStrong(
    std::atomic<T>& value, T& expected, T desired, std::memory_order success,
    std::memory_order failure, uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicCompareExchangeStrong(
      value, expected, desired, success, failure,
      latency_sim::AtomicDomain::kHwcc, tag);
}

inline void TransportRead(const void* address, size_t bytes) {
  HwccRead(address, bytes);
}
inline void TransportWrite(const void* address, size_t bytes) {
  HwccWrite(address, bytes);
}
inline void SwccWriteback(const void* address, size_t bytes) {
  Record(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kFlush, address,
         bytes);
}
inline void SwccInvalidate(const void* address, size_t bytes) {
  Record(latency_sim::PoolKind::kSwcc,
         latency_sim::AccessKind::kInvalidate, address, bytes);
}
inline void SharedPayloadRead(const void* address, size_t bytes) {
  Record(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead, address,
         bytes);
}
inline void SharedPayloadWrite(const void* address, size_t bytes) {
  Record(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kWrite, address,
         bytes);
}

template <typename T>
inline T SharedPayloadAtomicLoad(
    const std::atomic<T>& value,
    std::memory_order order = std::memory_order_seq_cst, uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicLoad(
      value, order, latency_sim::AtomicDomain::kSwcc, tag);
}
template <typename T>
inline void SharedPayloadAtomicStore(
    std::atomic<T>& value, T desired,
    std::memory_order order = std::memory_order_seq_cst, uint32_t tag = 0) {
  latency_sim::FixedLatencyAtomicStore(value, desired, order,
                                       latency_sim::AtomicDomain::kSwcc, tag);
}
template <typename T>
inline T SharedPayloadAtomicExchange(
    std::atomic<T>& value, T desired,
    std::memory_order order = std::memory_order_seq_cst, uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicExchange(
      value, desired, order, latency_sim::AtomicDomain::kSwcc, tag);
}
template <typename T>
inline bool SharedPayloadAtomicCompareExchangeWeak(
    std::atomic<T>& value, T& expected, T desired,
    std::memory_order success, std::memory_order failure, uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicCompareExchangeWeak(
      value, expected, desired, success, failure,
      latency_sim::AtomicDomain::kSwcc, tag);
}
template <typename T>
inline bool SharedPayloadAtomicCompareExchangeStrong(
    std::atomic<T>& value, T& expected, T desired,
    std::memory_order success, std::memory_order failure, uint32_t tag = 0) {
  return latency_sim::FixedLatencyAtomicCompareExchangeStrong(
      value, expected, desired, success, failure,
      latency_sim::AtomicDomain::kSwcc, tag);
}

}  // namespace tigonkv::engine::mem_access
