#pragma once

#include "kv/engine/latency_inject.h"

#include <cstddef>

namespace tigonkv::engine::mem_access {

inline void PrivateRead(const void *address, size_t bytes) {
  if (InstrumentationEnabledFast())
    GlobalLatencySimulator().Record(PoolKind::kSwcc, AccessKind::kRead, bytes,
                                    reinterpret_cast<uintptr_t>(address) / 64);
}
inline void PrivateWrite(const void *address, size_t bytes) {
  if (InstrumentationEnabledFast())
    GlobalLatencySimulator().Record(PoolKind::kSwcc, AccessKind::kWrite, bytes,
                                    reinterpret_cast<uintptr_t>(address) / 64);
}
inline void TransportRead(const void *address, size_t bytes) {
  if (InstrumentationEnabledFast())
    GlobalLatencySimulator().Record(PoolKind::kHwcc, AccessKind::kRead, bytes,
                                    reinterpret_cast<uintptr_t>(address) / 64);
}
inline void TransportWrite(const void *address, size_t bytes) {
  if (InstrumentationEnabledFast())
    GlobalLatencySimulator().Record(PoolKind::kHwcc, AccessKind::kWrite, bytes,
                                    reinterpret_cast<uintptr_t>(address) / 64);
}
inline void SwccFlush(const void *address, size_t bytes) {
  if (InstrumentationEnabledFast())
    GlobalLatencySimulator().Record(PoolKind::kSwcc, AccessKind::kFlush, bytes,
                                    reinterpret_cast<uintptr_t>(address) / 64);
}
inline void SharedPayloadRead(const void *address, size_t bytes) {
  if (InstrumentationEnabledFast())
    GlobalLatencySimulator().Record(PoolKind::kSwcc, AccessKind::kRead, bytes,
                                    reinterpret_cast<uintptr_t>(address) / 64);
}
inline void SharedPayloadWrite(const void *address, size_t bytes) {
  if (InstrumentationEnabledFast())
    GlobalLatencySimulator().Record(PoolKind::kSwcc, AccessKind::kWrite, bytes,
                                    reinterpret_cast<uintptr_t>(address) / 64);
}

}  // namespace tigonkv::engine::mem_access
