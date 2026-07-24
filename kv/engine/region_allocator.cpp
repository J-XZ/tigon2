#include "kv/engine/region_allocator.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <new>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tigonkv::engine {
namespace {

void FlushForRemoteVisibility(const void *address, size_t bytes) {
#if defined(__x86_64__) || defined(__i386__)
  const auto begin = reinterpret_cast<uintptr_t>(address) & ~(RegionAllocator::kAlignment - 1);
  const auto end = reinterpret_cast<uintptr_t>(address) + bytes;
  for (auto p = begin; p < end; p += RegionAllocator::kAlignment)
    _mm_clflush(reinterpret_cast<const void *>(p));
  _mm_sfence();
#else
  (void)address;
  (void)bytes;
  std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}

}  // namespace

uint64_t RegionAllocator::MetadataBytes() { return Align(sizeof(RegionAllocatorHeader)); }

uint64_t RegionAllocator::ClassBytes(uint32_t size_class) {
  if (size_class >= kAllocatorSizeClasses) return 0;
  uint64_t size = kAlignment;
  for (uint32_t i = 0; i < size_class; ++i) size = Align((size * 5 + 3) / 4);
  return size;
}

uint32_t RegionAllocator::SizeClass(uint64_t bytes) {
  for (uint32_t i = 0; i < kAllocatorSizeClasses; ++i)
    if (bytes <= ClassBytes(i)) return i;
  return kAllocatorSizeClasses;  // large allocation: aligned bump chunk
}

RegionAllocator RegionAllocator::Initialize(void *region, uint64_t region_bytes,
                                            uint32_t shard_count,
                                            uint64_t reserved_prefix_bytes) {
  if (region == nullptr || shard_count == 0 || shard_count > kMaxAllocatorShards ||
      region_bytes <= MetadataBytes() ||
      reserved_prefix_bytes > region_bytes - MetadataBytes())
    throw std::invalid_argument("invalid allocator region");
  if (reinterpret_cast<uintptr_t>(region) % kAlignment != 0)
    throw std::invalid_argument("allocator region is not cacheline aligned");
  std::memset(region, 0, MetadataBytes());
  auto *header = new (region) RegionAllocatorHeader;
  header->shard_count = shard_count;
  header->region_bytes = region_bytes;
  header->metadata_bytes = MetadataBytes();
  header->reserved_prefix_bytes = reserved_prefix_bytes;
  const uint64_t payload_begin = header->metadata_bytes + reserved_prefix_bytes;
  const uint64_t payload = region_bytes - payload_begin;
  for (uint32_t shard = 0; shard < shard_count; ++shard) {
    auto &entry = header->shards[shard];
    entry.begin = payload_begin + (payload * shard) / shard_count;
    entry.end = payload_begin + (payload * (shard + 1)) / shard_count;
    entry.bump = entry.begin;
  }
  FlushForRemoteVisibility(header, MetadataBytes());
  return RegionAllocator(region, region_bytes, header);
}

RegionAllocator RegionAllocator::Attach(void *region, uint64_t region_bytes) {
  if (region == nullptr || region_bytes <= MetadataBytes())
    throw std::invalid_argument("invalid allocator attachment");
  if (reinterpret_cast<uintptr_t>(region) % kAlignment != 0)
    throw std::invalid_argument("allocator attachment is not cacheline aligned");
  auto *header = static_cast<RegionAllocatorHeader *>(region);
  if (header->magic != 0x5449474f4e414c4cULL || header->version != 3 ||
      header->region_bytes != region_bytes || header->metadata_bytes != MetadataBytes() ||
      header->reserved_prefix_bytes > region_bytes - MetadataBytes() ||
      header->shard_count == 0 || header->shard_count > kMaxAllocatorShards)
    throw std::runtime_error("allocator attachment validation failed");
  return RegionAllocator(region, region_bytes, header);
}

void RegionAllocator::Lock(RegionAllocatorShard &shard) const {
  uint32_t expected = 0;
  while (!shard.lock.compare_exchange_weak(expected, 1, std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
    expected = 0;
    _mm_pause();
  }
}

void RegionAllocator::Unlock(RegionAllocatorShard &shard) const {
  shard.lock.store(0, std::memory_order_release);
}

void RegionAllocator::AccountAllocate(uint64_t bytes, DomainCounter *counter) {
  header_->allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);
  header_->allocation_count.fetch_add(1, std::memory_order_relaxed);
  const uint64_t used = counter->used_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
  uint64_t peak = counter->peak_bytes.load(std::memory_order_relaxed);
  while (peak < used && !counter->peak_bytes.compare_exchange_weak(
                             peak, used, std::memory_order_relaxed)) {}
}

void RegionAllocator::AccountFree(uint64_t bytes, DomainCounter *counter) {
  const uint64_t before = counter->used_bytes.fetch_sub(bytes, std::memory_order_relaxed);
  if (before < bytes) throw std::runtime_error("allocator domain accounting underflow");
  const uint64_t total_before = header_->allocated_bytes.fetch_sub(bytes, std::memory_order_relaxed);
  if (total_before < bytes) throw std::runtime_error("allocator total accounting underflow");
  header_->free_count.fetch_add(1, std::memory_order_relaxed);
}

void RegionAllocator::ReapRemote(uint32_t owner_shard, DomainCounter *counter) {
  if (owner_shard >= header_->shard_count || counter == nullptr)
    throw std::invalid_argument("invalid remote-free reap");
  auto &shard = header_->shards[owner_shard];
  RegionOffset head = shard.remote_free_head.exchange(kNullOffset, std::memory_order_acq_rel);
  while (head != kNullOffset) {
    auto *block = static_cast<RegionFreeBlock *>(FromOffset(head));
    const RegionOffset next = block->next.load(std::memory_order_acquire);
    if (block->owner_shard != owner_shard || block->size_class >= kAllocatorSizeClasses)
      throw std::runtime_error("invalid remote-free block");
    FreeLocal(head, block->size_class, owner_shard);
    // Remote frees were accounted on their issuing process; reaping must not
    // change counters a second time.
    head = next;
  }
}

void *RegionAllocator::AllocateFromShard(uint64_t bytes, uint32_t size_class,
                                         uint32_t owner_shard) {
  auto &shard = header_->shards[owner_shard];
  Lock(shard);
  if (size_class < kAllocatorSizeClasses) {
    const RegionOffset head = shard.free_heads[size_class].load(std::memory_order_relaxed);
    if (head != kNullOffset) {
      auto *block = static_cast<RegionFreeBlock *>(FromOffset(head));
      shard.free_heads[size_class].store(block->next.load(std::memory_order_relaxed),
                                         std::memory_order_relaxed);
      Unlock(shard);
      return block;
    }
  }
  const uint64_t allocation_bytes = size_class < kAllocatorSizeClasses ? ClassBytes(size_class) : bytes;
  const uint64_t begin = Align(shard.bump);
  if (begin > shard.end || allocation_bytes > shard.end - begin) {
    Unlock(shard);
    throw std::bad_alloc();
  }
  shard.bump = begin + allocation_bytes;
  Unlock(shard);
  return base_ + begin;
}

void *RegionAllocator::Allocate(uint64_t bytes, AllocationDomain, DomainCounter *counter,
                                uint32_t owner_shard) {
  if (bytes == 0 || counter == nullptr || owner_shard >= header_->shard_count)
    throw std::invalid_argument("invalid allocation");
  ReapRemote(owner_shard, counter);
  const uint64_t requested = Align(bytes + Align(sizeof(RegionFreeBlock)));
  const uint32_t size_class = SizeClass(requested);
  void *result = AllocateFromShard(requested, size_class, owner_shard);
  if (size_class < kAllocatorSizeClasses) {
    auto *block = new (result) RegionFreeBlock;
    block->size_class = size_class;
    block->owner_shard = owner_shard;
  }
  AccountAllocate(size_class < kAllocatorSizeClasses ? ClassBytes(size_class) : requested, counter);
  return static_cast<std::byte *>(result) + Align(sizeof(RegionFreeBlock));
}

void RegionAllocator::FreeLocal(RegionOffset offset, uint32_t size_class, uint32_t owner_shard) {
  auto &shard = header_->shards[owner_shard];
  Lock(shard);
  auto *block = static_cast<RegionFreeBlock *>(FromOffset(offset));
  block->next.store(shard.free_heads[size_class].load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
  shard.free_heads[size_class].store(offset, std::memory_order_release);
  Unlock(shard);
}

void RegionAllocator::Free(void *pointer, uint64_t bytes, AllocationDomain,
                           DomainCounter *counter, uint32_t owner_shard,
                           uint32_t current_shard) {
  if (!Contains(pointer) || bytes == 0 || counter == nullptr ||
      owner_shard >= header_->shard_count || current_shard >= header_->shard_count)
    throw std::invalid_argument("invalid free");
  const uint64_t requested = Align(bytes + Align(sizeof(RegionFreeBlock)));
  const uint32_t size_class = SizeClass(requested);
  if (size_class == kAllocatorSizeClasses)
    throw std::invalid_argument("large allocations are not individually reusable");
  auto *block = reinterpret_cast<RegionFreeBlock *>(
      static_cast<std::byte *>(pointer) - Align(sizeof(RegionFreeBlock)));
  if (block->owner_shard != owner_shard || block->size_class != size_class)
    throw std::runtime_error("allocator owner or size-class mismatch");
  const RegionOffset offset = ToOffset(block);
  if (owner_shard == current_shard) {
    FreeLocal(offset, size_class, owner_shard);
  } else {
    auto &shard = header_->shards[owner_shard];
    RegionOffset head = shard.remote_free_head.load(std::memory_order_relaxed);
    do {
      block->next.store(head, std::memory_order_relaxed);
      FlushForRemoteVisibility(block, sizeof(*block));
    } while (!shard.remote_free_head.compare_exchange_weak(
        head, offset, std::memory_order_release, std::memory_order_relaxed));
  }
  AccountFree(ClassBytes(size_class), counter);
}

RegionOffset RegionAllocator::ToOffset(const void *pointer) const {
  const auto *p = static_cast<const std::byte *>(pointer);
  if (p == nullptr) return kNullOffset;
  if (p < base_ || p >= base_ + bytes_) throw std::invalid_argument("pointer outside region");
  return static_cast<RegionOffset>(p - base_);
}

void *RegionAllocator::FromOffset(RegionOffset offset) const {
  if (offset == kNullOffset) return nullptr;
  if (offset >= bytes_) throw std::invalid_argument("offset outside region");
  return base_ + offset;
}

bool RegionAllocator::Contains(const void *pointer) const {
  const auto *p = static_cast<const std::byte *>(pointer);
  return p != nullptr && p >= base_ + header_->metadata_bytes +
           header_->reserved_prefix_bytes + Align(sizeof(RegionFreeBlock)) &&
         p < base_ + bytes_;
}

bool DualRegionAllocator::IsHwccDomain(AllocationDomain domain) {
  switch (domain) {
    case AllocationDomain::kHwccIndex:
    case AllocationDomain::kHwccMetadata:
    case AllocationDomain::kHwccEbr:
    case AllocationDomain::kHwccLayout:
    case AllocationDomain::kTransport:
      return true;
    case AllocationDomain::kOwnerPrivateSwcc:
    case AllocationDomain::kSharedPayloadSwcc:
    case AllocationDomain::kAllocatorMetadata:
    case AllocationDomain::kCount:
      return false;
  }
  throw std::invalid_argument("invalid allocation domain");
}

DualRegionAllocator DualRegionAllocator::Initialize(void *pool,
                                                    const DualRegionConfig &config) {
  if (pool == nullptr || config.total_pool_bytes == 0 || config.vm_count == 0 ||
      config.partition_count == 0 || config.partition_count % config.vm_count != 0 ||
      config.hwcc_size_bytes <= sizeof(DualRegionPersistentHeader) ||
      config.swcc_size_bytes <= sizeof(RegionAllocatorHeader) ||
      config.hwcc_offset_bytes + config.hwcc_size_bytes > config.total_pool_bytes ||
      config.swcc_offset_bytes + config.swcc_size_bytes > config.total_pool_bytes ||
      (config.hwcc_offset_bytes < config.swcc_offset_bytes + config.swcc_size_bytes &&
       config.swcc_offset_bytes < config.hwcc_offset_bytes + config.hwcc_size_bytes))
    throw std::invalid_argument("invalid dual-region configuration");
  if (!(config.owner_private_swcc_fraction > 0.0 &&
        config.owner_private_swcc_fraction < 1.0))
    throw std::invalid_argument("invalid owner-private SWCC fraction");
  auto *base = static_cast<std::byte *>(pool);
  if (reinterpret_cast<uintptr_t>(base) % RegionAllocator::kAlignment != 0)
    throw std::invalid_argument("dual-region pool is not cacheline aligned");
  auto *header = new (base + config.hwcc_offset_bytes) DualRegionPersistentHeader;
  header->layout.config_hash = config.config_hash;
  header->layout.total_pool_bytes = config.total_pool_bytes;
  header->layout.hwcc_offset_bytes = config.hwcc_offset_bytes;
  header->layout.hwcc_size_bytes = config.hwcc_size_bytes;
  header->layout.swcc_offset_bytes = config.swcc_offset_bytes;
  header->layout.swcc_size_bytes = config.swcc_size_bytes;
  header->layout.vm_count = config.vm_count;
  header->layout.partition_count = config.partition_count;
  header->layout.fixed_key_size = config.fixed_key_size;
  header->layout.fixed_value_size = config.fixed_value_size;
  const uint64_t dual_header_bytes =
      (sizeof(*header) + RegionAllocator::kAlignment - 1) &
      ~(RegionAllocator::kAlignment - 1);
  header->hwcc_allocator_offset = config.hwcc_offset_bytes + dual_header_bytes;
  header->hwcc_allocator_bytes = config.hwcc_size_bytes -
                                  (header->hwcc_allocator_offset - config.hwcc_offset_bytes);
  header->swcc_allocator_offset = config.swcc_offset_bytes;
  header->swcc_allocator_bytes = config.swcc_size_bytes;
  const uint64_t swcc_metadata_bytes =
      (sizeof(RegionAllocatorHeader) + RegionAllocator::kAlignment - 1) &
      ~(RegionAllocator::kAlignment - 1);
  const uint64_t swcc_payload_bytes = config.swcc_size_bytes - swcc_metadata_bytes;
  header->owner_private_arenas_bytes =
      (static_cast<uint64_t>(swcc_payload_bytes * config.owner_private_swcc_fraction) /
       RegionAllocator::kAlignment) * RegionAllocator::kAlignment;
  header->owner_private_arenas_offset = swcc_metadata_bytes;
  header->owner_private_arena_stride =
      (header->owner_private_arenas_bytes / config.partition_count /
       RegionAllocator::kAlignment) * RegionAllocator::kAlignment;
  if (header->owner_private_arena_stride <= sizeof(OwnerPrivateArenaHeader))
    throw std::invalid_argument("owner-private arena is too small");
  header->owner_private_arenas_bytes =
      header->owner_private_arena_stride * config.partition_count;
  auto hwcc = RegionAllocator::Initialize(base + header->hwcc_allocator_offset,
                                          header->hwcc_allocator_bytes, config.vm_count);
  auto swcc = RegionAllocator::Initialize(base + header->swcc_allocator_offset,
                                          header->swcc_allocator_bytes, config.vm_count,
                                          header->owner_private_arenas_bytes);
  auto *swcc_base = base + header->swcc_allocator_offset;
  for (uint32_t partition = 0; partition < config.partition_count; ++partition) {
    auto *arena = new (swcc_base + header->owner_private_arenas_offset +
                       partition * header->owner_private_arena_stride)
        OwnerPrivateArenaHeader;
    arena->partition_id = partition;
    // The persistent layout follows the public routing contract: partitions
    // are striped across owners, rather than stored in contiguous owner runs.
    arena->owner_shard = partition % config.vm_count;
    arena->begin = header->owner_private_arenas_offset +
                   partition * header->owner_private_arena_stride +
                   ((sizeof(OwnerPrivateArenaHeader) + RegionAllocator::kAlignment - 1) &
                    ~(RegionAllocator::kAlignment - 1));
    arena->end = header->owner_private_arenas_offset +
                 (partition + 1) * header->owner_private_arena_stride;
    arena->bump = arena->begin;
    header->layout.partitions[partition].private_arena = swcc.ToOffset(arena);
  }
  header->layout.state.store(static_cast<uint32_t>(LayoutState::kClean),
                             std::memory_order_release);
  FlushForRemoteVisibility(header, sizeof(*header));
  return DualRegionAllocator(base, config, header, hwcc, swcc);
}

DualRegionAllocator DualRegionAllocator::Attach(void *pool,
                                                const DualRegionConfig &config) {
  if (pool == nullptr) throw std::invalid_argument("null dual-region pool");
  auto *base = static_cast<std::byte *>(pool);
  auto *header = reinterpret_cast<DualRegionPersistentHeader *>(base + config.hwcc_offset_bytes);
  if (!header->layout.IsCompatible(config.config_hash, config.total_pool_bytes,
                                   config.vm_count, config.partition_count) ||
      header->layout.hwcc_offset_bytes != config.hwcc_offset_bytes ||
      header->layout.hwcc_size_bytes != config.hwcc_size_bytes ||
      header->layout.swcc_offset_bytes != config.swcc_offset_bytes ||
      header->layout.swcc_size_bytes != config.swcc_size_bytes ||
      header->layout.fixed_key_size != config.fixed_key_size ||
      header->layout.fixed_value_size != config.fixed_value_size ||
      header->owner_private_arena_stride == 0)
    throw std::runtime_error("dual-region layout attachment validation failed");
  auto hwcc = RegionAllocator::Attach(base + header->hwcc_allocator_offset,
                                      header->hwcc_allocator_bytes);
  auto swcc = RegionAllocator::Attach(base + header->swcc_allocator_offset,
                                      header->swcc_allocator_bytes);
  return DualRegionAllocator(base, config, header, hwcc, swcc);
}

void *DualRegionAllocator::Allocate(uint64_t bytes, AllocationDomain domain,
                                    uint32_t owner_shard) {
  DomainCounter &counter = header_->layout.domains[static_cast<size_t>(domain)];
  return IsHwccDomain(domain)
             ? hwcc_.Allocate(bytes, domain, &counter, owner_shard)
             : swcc_.Allocate(bytes, domain, &counter, owner_shard);
}

OwnerPrivateArenaHeader *DualRegionAllocator::Arena(uint32_t partition_id) const {
  if (partition_id >= header_->layout.partition_count)
    throw std::invalid_argument("owner-private arena partition outside layout");
  auto *arena = static_cast<OwnerPrivateArenaHeader *>(swcc_.FromOffset(
      header_->owner_private_arenas_offset +
      partition_id * header_->owner_private_arena_stride));
  if (arena->magic != 0x5449474f4e41524eULL || arena->version != 1 ||
      arena->partition_id != partition_id)
    throw std::runtime_error("owner-private arena attachment validation failed");
  return arena;
}

void *DualRegionAllocator::AllocateOwnerPrivate(uint64_t bytes, uint32_t partition_id,
                                                uint32_t owner_shard) {
  if (bytes == 0) throw std::invalid_argument("zero-sized private allocation");
  auto *arena = Arena(partition_id);
  if (arena->owner_shard != owner_shard)
    throw std::runtime_error("private arena allocation from non-owner shard");
  uint32_t expected = 0;
  while (!arena->lock.compare_exchange_weak(expected, 1, std::memory_order_acquire,
                                             std::memory_order_relaxed)) {
    expected = 0;
    _mm_pause();
  }
  const uint64_t aligned = (bytes + RegionAllocator::kAlignment - 1) &
                           ~(RegionAllocator::kAlignment - 1);
  const uint64_t requested = aligned + RegionAllocator::kAlignment;
  if (requested > UINT32_MAX) {
    arena->lock.store(0, std::memory_order_release);
    throw std::bad_alloc();
  }
  RegionOffset previous = kNullOffset;
  RegionOffset current = arena->free_head.load(std::memory_order_relaxed);
  while (current != kNullOffset) {
    auto *block = static_cast<RegionFreeBlock *>(swcc_.FromOffset(current));
    if (block->size_class >= requested) {
      const RegionOffset next = block->next.load(std::memory_order_relaxed);
      if (previous == kNullOffset)
        arena->free_head.store(next, std::memory_order_relaxed);
      else
        static_cast<RegionFreeBlock *>(swcc_.FromOffset(previous))->next.store(
            next, std::memory_order_relaxed);
      block->owner_shard = owner_shard;
      arena->lock.store(0, std::memory_order_release);
      arena->allocated_bytes.fetch_add(block->size_class, std::memory_order_relaxed);
      auto &counter = header_->layout.domains[
          static_cast<size_t>(AllocationDomain::kOwnerPrivateSwcc)];
      const uint64_t used = counter.used_bytes.fetch_add(block->size_class, std::memory_order_relaxed) +
                            block->size_class;
      uint64_t peak = counter.peak_bytes.load(std::memory_order_relaxed);
      while (peak < used && !counter.peak_bytes.compare_exchange_weak(
                                 peak, used, std::memory_order_relaxed)) {}
      return static_cast<std::byte *>(static_cast<void *>(block)) + RegionAllocator::kAlignment;
    }
    previous = current;
    current = block->next.load(std::memory_order_relaxed);
  }
  const uint64_t begin = (arena->bump + RegionAllocator::kAlignment - 1) &
                         ~(RegionAllocator::kAlignment - 1);
  if (begin > arena->end || requested > arena->end - begin) {
    arena->lock.store(0, std::memory_order_release);
    throw std::bad_alloc();
  }
  arena->bump = begin + requested;
  auto *block = new (swcc_.FromOffset(begin)) RegionFreeBlock;
  block->size_class = static_cast<uint32_t>(requested);
  block->owner_shard = owner_shard;
  arena->allocated_bytes.fetch_add(requested, std::memory_order_relaxed);
  arena->lock.store(0, std::memory_order_release);
  auto &counter = header_->layout.domains[
      static_cast<size_t>(AllocationDomain::kOwnerPrivateSwcc)];
  const uint64_t used = counter.used_bytes.fetch_add(requested, std::memory_order_relaxed) + requested;
  uint64_t peak = counter.peak_bytes.load(std::memory_order_relaxed);
  while (peak < used && !counter.peak_bytes.compare_exchange_weak(
                             peak, used, std::memory_order_relaxed)) {}
  return static_cast<std::byte *>(swcc_.FromOffset(begin)) + RegionAllocator::kAlignment;
}

void DualRegionAllocator::FreeOwnerPrivate(void *pointer, uint64_t bytes,
                                           uint32_t partition_id,
                                           uint32_t owner_shard) {
  if (pointer == nullptr || bytes == 0) throw std::invalid_argument("invalid private free");
  auto *arena = Arena(partition_id);
  if (arena->owner_shard != owner_shard || !IsInOwnerPrivateArena(pointer, partition_id))
    throw std::runtime_error("private arena free from non-owner or wrong arena");
  auto *block = reinterpret_cast<RegionFreeBlock *>(
      static_cast<std::byte *>(pointer) - RegionAllocator::kAlignment);
  const uint64_t requested = ((bytes + RegionAllocator::kAlignment - 1) &
                              ~(RegionAllocator::kAlignment - 1)) + RegionAllocator::kAlignment;
  if (block->owner_shard != owner_shard || block->size_class < requested)
    throw std::runtime_error("private arena free size or owner mismatch");
  uint32_t expected = 0;
  while (!arena->lock.compare_exchange_weak(expected, 1, std::memory_order_acquire,
                                             std::memory_order_relaxed)) expected = 0;
  block->next.store(arena->free_head.load(std::memory_order_relaxed), std::memory_order_relaxed);
  arena->free_head.store(swcc_.ToOffset(block), std::memory_order_release);
  arena->lock.store(0, std::memory_order_release);
  arena->allocated_bytes.fetch_sub(block->size_class, std::memory_order_relaxed);
  auto &counter = header_->layout.domains[
      static_cast<size_t>(AllocationDomain::kOwnerPrivateSwcc)];
  const uint64_t before = counter.used_bytes.fetch_sub(block->size_class, std::memory_order_relaxed);
  if (before < block->size_class) throw std::runtime_error("private arena accounting underflow");
}

void DualRegionAllocator::Free(void *pointer, uint64_t bytes, AllocationDomain domain,
                               uint32_t owner_shard, uint32_t current_shard) {
  DomainCounter &counter = header_->layout.domains[static_cast<size_t>(domain)];
  if (IsHwccDomain(domain)) {
    if (!hwcc_.Contains(pointer)) throw std::runtime_error("HWCC free outside HWCC region");
    hwcc_.Free(pointer, bytes, domain, &counter, owner_shard, current_shard);
  } else {
    if (!swcc_.Contains(pointer)) throw std::runtime_error("SWCC free outside SWCC region");
    swcc_.Free(pointer, bytes, domain, &counter, owner_shard, current_shard);
  }
}

bool DualRegionAllocator::IsHwccAddress(const void *pointer) const {
  return hwcc_.Contains(pointer);
}

bool DualRegionAllocator::IsSwccAddress(const void *pointer) const {
  if (swcc_.Contains(pointer)) return true;
  const auto *p = static_cast<const std::byte *>(pointer);
  const auto *base = static_cast<const std::byte *>(swcc_.FromOffset(1)) - 1;
  return p >= base + header_->owner_private_arenas_offset &&
         p < base + header_->owner_private_arenas_offset + header_->owner_private_arenas_bytes;
}

uint64_t DualRegionAllocator::ToPoolOffset(const void *pointer) const {
  const auto *p = static_cast<const std::byte *>(pointer);
  if (p == nullptr || p < pool_ || p >= pool_ + config_.total_pool_bytes)
    throw std::invalid_argument("pointer outside dual-region pool");
  return static_cast<uint64_t>(p - pool_);
}

void *DualRegionAllocator::FromPoolOffset(uint64_t offset) const {
  if (offset >= config_.total_pool_bytes)
    throw std::invalid_argument("offset outside dual-region pool");
  return pool_ + offset;
}

bool DualRegionAllocator::IsInOwnerPrivateArena(const void *pointer,
                                                uint32_t partition_id) const {
  const auto *arena = Arena(partition_id);
  const auto *p = static_cast<const std::byte *>(pointer);
  const auto *base = static_cast<const std::byte *>(swcc_.FromOffset(1)) - 1;
  return p >= base + arena->begin && p < base + arena->end;
}

RegionOffset DualRegionAllocator::OwnerPrivateArenaOffset(uint32_t partition_id) const {
  return swcc_.ToOffset(Arena(partition_id));
}

DualRegionMappedPool DualRegionMappedPool::Open(const std::string &path,
                                                const DualRegionConfig &config,
                                                bool reset) {
  if (path.empty() || config.total_pool_bytes == 0)
    throw std::invalid_argument("invalid dual-region backing-file request");
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0660);
  if (fd < 0) throw std::runtime_error("open dual-region backing file failed");
  try {
    if (flock(fd, LOCK_EX) != 0) throw std::runtime_error("lock dual-region backing file failed");
    struct stat st {};
    if (fstat(fd, &st) != 0) throw std::runtime_error("stat dual-region backing file failed");
    if (reset) {
      if (ftruncate(fd, static_cast<off_t>(config.total_pool_bytes)) != 0)
        throw std::runtime_error("resize dual-region backing file failed");
    } else if (st.st_size != static_cast<off_t>(config.total_pool_bytes)) {
      throw std::runtime_error("dual-region backing file size mismatch");
    }
    void *base = mmap(nullptr, config.total_pool_bytes, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) throw std::runtime_error("map dual-region backing file failed");
    std::unique_ptr<DualRegionAllocator> allocator;
    try {
      if (reset) {
        std::memset(base, 0, config.total_pool_bytes);
        allocator = std::make_unique<DualRegionAllocator>(
            DualRegionAllocator::Initialize(base, config));
      } else {
        allocator = std::make_unique<DualRegionAllocator>(
            DualRegionAllocator::Attach(base, config));
      }
    } catch (...) {
      munmap(base, config.total_pool_bytes);
      throw;
    }
    flock(fd, LOCK_UN);
    return DualRegionMappedPool(fd, base, config.total_pool_bytes, std::move(allocator));
  } catch (...) {
    flock(fd, LOCK_UN);
    ::close(fd);
    throw;
  }
}

DualRegionMappedPool::~DualRegionMappedPool() { Close(); }

DualRegionMappedPool::DualRegionMappedPool(DualRegionMappedPool &&other) noexcept
    : fd_(other.fd_), base_(other.base_), bytes_(other.bytes_),
      allocator_(std::move(other.allocator_)) {
  other.fd_ = -1;
  other.base_ = nullptr;
  other.bytes_ = 0;
}

DualRegionMappedPool &DualRegionMappedPool::operator=(DualRegionMappedPool &&other) noexcept {
  if (this == &other) return *this;
  Close();
  fd_ = other.fd_;
  base_ = other.base_;
  bytes_ = other.bytes_;
  allocator_ = std::move(other.allocator_);
  other.fd_ = -1;
  other.base_ = nullptr;
  other.bytes_ = 0;
  return *this;
}

void DualRegionMappedPool::Close() noexcept {
  allocator_.reset();
  if (base_ != nullptr) munmap(base_, bytes_);
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  base_ = nullptr;
  bytes_ = 0;
}

}  // namespace tigonkv::engine
