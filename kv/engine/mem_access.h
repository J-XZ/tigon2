#pragma once

#include "kv/engine/latency_inject.h"

#include <cstddef>

namespace tigonkv::engine::mem_access {

class LatencyScope {
 public:
  explicit LatencyScope(latency_sim::ScopeKind scope) {
    active_ = latency_sim::InstrumentationEnabledFast();
    if (active_) latency_sim::GlobalLatencySimulator().BeginScope(scope);
  }
  ~LatencyScope() {
    if (active_) latency_sim::GlobalLatencySimulator().EndScopeAndDelay();
  }
  LatencyScope(const LatencyScope &) = delete;
  LatencyScope &operator=(const LatencyScope &) = delete;

 private:
  bool active_ = false;
};

inline bool HasActiveScope() {
  return latency_sim::InstrumentationEnabledFast() &&
         latency_sim::GlobalLatencySimulator().HasActiveScopeForCurrentThread();
}

inline void EndActiveScopeAndDelay() {
  if (latency_sim::InstrumentationEnabledFast())
    latency_sim::GlobalLatencySimulator().EndScopeAndDelay();
}

inline void BeginForegroundScope() {
  if (latency_sim::InstrumentationEnabledFast())
    latency_sim::GlobalLatencySimulator().BeginScope(
        latency_sim::ScopeKind::kForeground);
}

inline void Record(latency_sim::PoolKind pool, latency_sim::AccessKind kind,
                   const void *address, size_t bytes) {
  if (latency_sim::InstrumentationEnabledFast())
    latency_sim::GlobalLatencySimulator().RecordRange(pool, kind, address, bytes);
}

inline void PrivateRead(const void *address, size_t bytes) {
  Record(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead, address, bytes);
}
inline void PrivateWrite(const void *address, size_t bytes) {
  Record(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kWrite, address, bytes);
}
inline void PrivateAtomicLoad(const void *address) {
  Record(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kAtomicLoad,
         address, 1);
}
inline void PrivateAtomicStore(const void *address) {
  Record(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kAtomicStore,
         address, 1);
}
inline void PrivateAtomicRmw(const void *address) {
  Record(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kAtomicRmw,
         address, 1);
}
inline void HwccRead(const void *address, size_t bytes) {
  Record(latency_sim::PoolKind::kHwcc, latency_sim::AccessKind::kRead, address, bytes);
}
inline void HwccWrite(const void *address, size_t bytes) {
  Record(latency_sim::PoolKind::kHwcc, latency_sim::AccessKind::kWrite, address, bytes);
}
inline void HwccAtomicLoad(const void *address) {
  Record(latency_sim::PoolKind::kHwcc, latency_sim::AccessKind::kAtomicLoad,
         address, 1);
}
inline void HwccAtomicStore(const void *address) {
  Record(latency_sim::PoolKind::kHwcc, latency_sim::AccessKind::kAtomicStore,
         address, 1);
}
inline void HwccAtomicRmw(const void *address) {
  Record(latency_sim::PoolKind::kHwcc, latency_sim::AccessKind::kAtomicRmw,
         address, 1);
}
inline void TransportRead(const void *address, size_t bytes) {
  HwccRead(address, bytes);
}
inline void TransportWrite(const void *address, size_t bytes) {
  HwccWrite(address, bytes);
}
inline void SwccWriteback(const void *address, size_t bytes) {
  Record(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kWriteback,
         address, bytes);
}
inline void SwccInvalidate(const void *address, size_t bytes) {
  Record(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kInvalidate,
         address, bytes);
}
inline void SharedPayloadRead(const void *address, size_t bytes) {
  Record(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead, address, bytes);
}
inline void SharedPayloadWrite(const void *address, size_t bytes) {
  Record(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kWrite, address, bytes);
}
// Shared-payload freelist headers live in shared SWCC but are only touched by
// the owner allocator before publish / after EBR; pool is still SWCC.
inline void SharedPayloadAtomicLoad(const void *address) {
  Record(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kAtomicLoad,
         address, 1);
}
inline void SharedPayloadAtomicStore(const void *address) {
  Record(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kAtomicStore,
         address, 1);
}
inline void SharedPayloadAtomicRmw(const void *address) {
  Record(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kAtomicRmw,
         address, 1);
}

}  // namespace tigonkv::engine::mem_access
