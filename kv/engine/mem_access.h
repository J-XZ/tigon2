#pragma once

#include "kv/engine/latency_inject.h"

#include <cstddef>

namespace tigonkv::engine::mem_access {

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
inline void TransportRead(const void *address, size_t bytes) {
  Record(latency_sim::PoolKind::kHwcc, latency_sim::AccessKind::kRead, address, bytes);
}
inline void TransportWrite(const void *address, size_t bytes) {
  Record(latency_sim::PoolKind::kHwcc, latency_sim::AccessKind::kWrite, address, bytes);
}
inline void SwccFlush(const void *address, size_t bytes) {
  Record(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kFlush, address, bytes);
}
inline void SharedPayloadRead(const void *address, size_t bytes) {
  Record(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead, address, bytes);
}
inline void SharedPayloadWrite(const void *address, size_t bytes) {
  Record(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kWrite, address, bytes);
}

}  // namespace tigonkv::engine::mem_access
