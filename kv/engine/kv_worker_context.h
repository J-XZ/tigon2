#pragma once

#include <cstdint>

namespace tigonkv::engine {

// The pointer is only a process-local binding handle.  The authoritative TID
// high-water mark remains in the bound foreground worker's DRAM slot.
uint64_t &CurrentKvWorkerMaxTid();
void BindKvWorkerMaxTid(uint64_t *max_tid);
void ReleaseKvWorkerMaxTid();

class ScopedKvWorkerTidBinding {
 public:
  explicit ScopedKvWorkerTidBinding(uint64_t *max_tid) {
    BindKvWorkerMaxTid(max_tid);
  }
  ~ScopedKvWorkerTidBinding() { ReleaseKvWorkerMaxTid(); }
  ScopedKvWorkerTidBinding(const ScopedKvWorkerTidBinding &) = delete;
  ScopedKvWorkerTidBinding &operator=(const ScopedKvWorkerTidBinding &) = delete;
};

}  // namespace tigonkv::engine
