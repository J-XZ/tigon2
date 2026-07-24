#pragma once

#include "kv/engine/kv_types_layout.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace tigonkv::engine {

// Persistent per-region allocator metadata.  This is placed inside the region
// and is therefore valid after a process maps the same backing object again.
struct alignas(64) RegionAllocatorHeader {
  uint64_t magic = 0x5449474f4e414c4cULL;  // TIGONALL
  uint32_t version = 1;
  uint32_t shard_count = 0;
  uint64_t region_bytes = 0;
  std::atomic<uint64_t> bump{0};
  std::atomic<uint64_t> allocated_bytes{0};
};

class RegionAllocator {
 public:
  static constexpr uint64_t kAlignment = 64;

  // Initialize is only valid for a newly zeroed region; Attach validates an
  // existing persistent header and never resets allocation state.
  static RegionAllocator Initialize(void *region, uint64_t region_bytes,
                                    uint32_t shard_count);
  static RegionAllocator Attach(void *region, uint64_t region_bytes);

  void *Allocate(uint64_t bytes, AllocationDomain domain, DomainCounter *counter);
  RegionOffset ToOffset(const void *pointer) const;
  void *FromOffset(RegionOffset offset) const;
  uint64_t capacity() const { return bytes_; }
  uint64_t allocated() const { return header_->allocated_bytes.load(std::memory_order_acquire); }

 private:
  RegionAllocator(void *base, uint64_t bytes, RegionAllocatorHeader *header)
      : base_(static_cast<std::byte *>(base)), bytes_(bytes), header_(header) {}
  static uint64_t Align(uint64_t bytes) { return (bytes + kAlignment - 1) & ~(kAlignment - 1); }
  std::byte *base_;
  uint64_t bytes_;
  RegionAllocatorHeader *header_;
};

}  // namespace tigonkv::engine
