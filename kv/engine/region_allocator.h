#pragma once

#include "kv/engine/kv_types_layout.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace tigonkv::engine {

// All allocator metadata is part of the mapped region.  The fixed upper bound
// keeps the on-region format inspectable and avoids a process-local directory.
constexpr uint32_t kMaxAllocatorShards = 64;
constexpr uint32_t kAllocatorSizeClasses = 32;

struct alignas(64) RegionFreeBlock {
  std::atomic<RegionOffset> next{kNullOffset};
  uint32_t size_class = 0;
  uint32_t owner_shard = 0;
};

struct alignas(64) RegionAllocatorShard {
  std::atomic<uint32_t> lock{0};
  std::atomic<RegionOffset> remote_free_head{kNullOffset};
  uint64_t begin = 0;
  uint64_t end = 0;
  uint64_t bump = 0;  // only protected by lock
  std::atomic<RegionOffset> free_heads[kAllocatorSizeClasses]{};
};

// Persistent per-region allocator metadata. It is safe to attach from a
// different mapping address because all links are offsets from base_.
struct alignas(64) RegionAllocatorHeader {
  uint64_t magic = 0x5449474f4e414c4cULL;  // TIGONALL
  uint32_t version = 2;
  uint32_t shard_count = 0;
  uint64_t region_bytes = 0;
  uint64_t metadata_bytes = 0;
  std::atomic<uint64_t> allocated_bytes{0};
  std::atomic<uint64_t> allocation_count{0};
  std::atomic<uint64_t> free_count{0};
  RegionAllocatorShard shards[kMaxAllocatorShards]{};
};

class RegionAllocator {
 public:
  static constexpr uint64_t kAlignment = 64;

  static RegionAllocator Initialize(void *region, uint64_t region_bytes,
                                    uint32_t shard_count);
  static RegionAllocator Attach(void *region, uint64_t region_bytes);

  // owner_shard identifies the VM which owns this allocation. current_shard
  // identifies the freeing VM; a different value is placed on the persistent
  // remote-free stack and reclaimed by the owner on its next allocation.
  void *Allocate(uint64_t bytes, AllocationDomain domain, DomainCounter *counter,
                 uint32_t owner_shard = 0);
  void Free(void *pointer, uint64_t bytes, AllocationDomain domain,
            DomainCounter *counter, uint32_t owner_shard,
            uint32_t current_shard);
  void ReapRemote(uint32_t owner_shard, DomainCounter *counter);

  RegionOffset ToOffset(const void *pointer) const;
  void *FromOffset(RegionOffset offset) const;
  bool Contains(const void *pointer) const;
  uint64_t capacity() const { return bytes_; }
  uint64_t allocated() const {
    return header_->allocated_bytes.load(std::memory_order_acquire);
  }
  uint32_t shard_count() const { return header_->shard_count; }

 private:
  RegionAllocator(void *base, uint64_t bytes, RegionAllocatorHeader *header)
      : base_(static_cast<std::byte *>(base)), bytes_(bytes), header_(header) {}
  static uint64_t Align(uint64_t bytes) {
    if (bytes > UINT64_MAX - (kAlignment - 1)) throw std::bad_alloc();
    return (bytes + kAlignment - 1) & ~(kAlignment - 1);
  }
  static uint32_t SizeClass(uint64_t bytes);
  static uint64_t ClassBytes(uint32_t size_class);
  static uint64_t MetadataBytes();
  void Lock(RegionAllocatorShard &shard) const;
  void Unlock(RegionAllocatorShard &shard) const;
  void *AllocateFromShard(uint64_t bytes, uint32_t size_class,
                          uint32_t owner_shard);
  void FreeLocal(RegionOffset offset, uint32_t size_class, uint32_t owner_shard);
  void AccountAllocate(uint64_t bytes, DomainCounter *counter);
  void AccountFree(uint64_t bytes, DomainCounter *counter);

  std::byte *base_;
  uint64_t bytes_;
  RegionAllocatorHeader *header_;
};

}  // namespace tigonkv::engine
