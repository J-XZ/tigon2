#include "kv/engine/region_allocator.h"

#include <algorithm>
#include <cstring>
#include <immintrin.h>
#include <new>

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
                                            uint32_t shard_count) {
  if (region == nullptr || shard_count == 0 || shard_count > kMaxAllocatorShards ||
      region_bytes <= MetadataBytes())
    throw std::invalid_argument("invalid allocator region");
  if (reinterpret_cast<uintptr_t>(region) % kAlignment != 0)
    throw std::invalid_argument("allocator region is not cacheline aligned");
  std::memset(region, 0, MetadataBytes());
  auto *header = new (region) RegionAllocatorHeader;
  header->shard_count = shard_count;
  header->region_bytes = region_bytes;
  header->metadata_bytes = MetadataBytes();
  const uint64_t payload = region_bytes - header->metadata_bytes;
  for (uint32_t shard = 0; shard < shard_count; ++shard) {
    auto &entry = header->shards[shard];
    entry.begin = header->metadata_bytes + (payload * shard) / shard_count;
    entry.end = header->metadata_bytes + (payload * (shard + 1)) / shard_count;
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
  if (header->magic != 0x5449474f4e414c4cULL || header->version != 2 ||
      header->region_bytes != region_bytes || header->metadata_bytes != MetadataBytes() ||
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
  const uint64_t aligned = Align(std::max<uint64_t>(bytes, sizeof(RegionFreeBlock)));
  const uint32_t size_class = SizeClass(aligned);
  void *result = AllocateFromShard(aligned, size_class, owner_shard);
  if (size_class < kAllocatorSizeClasses) {
    auto *block = new (result) RegionFreeBlock;
    block->size_class = size_class;
    block->owner_shard = owner_shard;
  }
  AccountAllocate(size_class < kAllocatorSizeClasses ? ClassBytes(size_class) : aligned, counter);
  return result;
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
  const uint64_t aligned = Align(std::max<uint64_t>(bytes, sizeof(RegionFreeBlock)));
  const uint32_t size_class = SizeClass(aligned);
  if (size_class == kAllocatorSizeClasses)
    throw std::invalid_argument("large allocations are not individually reusable");
  auto *block = static_cast<RegionFreeBlock *>(pointer);
  if (block->owner_shard != owner_shard || block->size_class != size_class)
    throw std::runtime_error("allocator owner or size-class mismatch");
  const RegionOffset offset = ToOffset(pointer);
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
  return p != nullptr && p >= base_ + header_->metadata_bytes && p < base_ + bytes_;
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
  auto hwcc = RegionAllocator::Initialize(base + header->hwcc_allocator_offset,
                                          header->hwcc_allocator_bytes, config.vm_count);
  auto swcc = RegionAllocator::Initialize(base + header->swcc_allocator_offset,
                                          header->swcc_allocator_bytes, config.vm_count);
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
      header->layout.fixed_value_size != config.fixed_value_size)
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
  return swcc_.Contains(pointer);
}

}  // namespace tigonkv::engine
