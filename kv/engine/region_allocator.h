#pragma once

#include "kv/engine/kv_types_layout.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

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

// The pool is mapped once, but allocations are physically constrained to one
// of the two configured intervals.  This is deliberately a concrete routing
// object, not a callback layer: hot callers select a domain and use an inline
// switch to reach the correct RegionAllocator.
struct DualRegionConfig {
  uint64_t total_pool_bytes = 0;
  uint64_t hwcc_offset_bytes = 0;
  uint64_t hwcc_size_bytes = 0;
  uint64_t swcc_offset_bytes = 0;
  uint64_t swcc_size_bytes = 0;
  uint64_t config_hash = 0;
  uint32_t vm_count = 0;
  uint32_t partition_count = 0;
  uint32_t fixed_key_size = 0;
  uint32_t fixed_value_size = 0;
};

struct alignas(64) DualRegionPersistentHeader {
  SharedLayoutHeader layout{};
  uint64_t hwcc_allocator_offset = 0;
  uint64_t hwcc_allocator_bytes = 0;
  uint64_t swcc_allocator_offset = 0;
  uint64_t swcc_allocator_bytes = 0;
};

class DualRegionAllocator {
 public:
  static DualRegionAllocator Initialize(void *pool, const DualRegionConfig &config);
  static DualRegionAllocator Attach(void *pool, const DualRegionConfig &config);

  void *Allocate(uint64_t bytes, AllocationDomain domain, uint32_t owner_shard);
  void Free(void *pointer, uint64_t bytes, AllocationDomain domain,
            uint32_t owner_shard, uint32_t current_shard);
  bool IsHwccAddress(const void *pointer) const;
  bool IsSwccAddress(const void *pointer) const;
  const SharedLayoutHeader &layout() const { return header_->layout; }
  SharedLayoutHeader &layout() { return header_->layout; }
  const RegionAllocator &hwcc() const { return hwcc_; }
  const RegionAllocator &swcc() const { return swcc_; }

 private:
  DualRegionAllocator(std::byte *pool, const DualRegionConfig &config,
                      DualRegionPersistentHeader *header, RegionAllocator hwcc,
                      RegionAllocator swcc)
      : pool_(pool), config_(config), header_(header), hwcc_(hwcc), swcc_(swcc) {}
  static bool IsHwccDomain(AllocationDomain domain);
  std::byte *pool_;
  DualRegionConfig config_;
  DualRegionPersistentHeader *header_;
  RegionAllocator hwcc_;
  RegionAllocator swcc_;
};

// Owns one MAP_SHARED backing-file mapping. Initialization is serialized with
// the backing file lock; normal attach never rewrites allocator metadata.
class DualRegionMappedPool {
 public:
  static DualRegionMappedPool Open(const std::string &path,
                                   const DualRegionConfig &config, bool reset);
  ~DualRegionMappedPool();
  DualRegionMappedPool(DualRegionMappedPool &&other) noexcept;
  DualRegionMappedPool &operator=(DualRegionMappedPool &&other) noexcept;
  DualRegionMappedPool(const DualRegionMappedPool &) = delete;
  DualRegionMappedPool &operator=(const DualRegionMappedPool &) = delete;

  DualRegionAllocator &allocator() { return *allocator_; }
  const DualRegionAllocator &allocator() const { return *allocator_; }
  void *base() const { return base_; }
  uint64_t bytes() const { return bytes_; }

 private:
  DualRegionMappedPool(int fd, void *base, uint64_t bytes,
                       std::unique_ptr<DualRegionAllocator> allocator)
      : fd_(fd), base_(base), bytes_(bytes), allocator_(std::move(allocator)) {}
  void Close() noexcept;
  int fd_ = -1;
  void *base_ = nullptr;
  uint64_t bytes_ = 0;
  std::unique_ptr<DualRegionAllocator> allocator_;
};

}  // namespace tigonkv::engine
