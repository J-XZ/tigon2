#include "kv/engine/region_allocator.h"

#include <algorithm>
#include <cstring>

namespace tigonkv::engine {

RegionAllocator RegionAllocator::Initialize(void *region, uint64_t region_bytes,
                                            uint32_t shard_count) {
  if (region == nullptr || region_bytes < Align(sizeof(RegionAllocatorHeader)) || shard_count == 0)
    throw std::invalid_argument("invalid allocator region");
  std::memset(region, 0, sizeof(RegionAllocatorHeader));
  auto *header = new (region) RegionAllocatorHeader;
  header->shard_count = shard_count;
  header->region_bytes = region_bytes;
  header->bump.store(Align(sizeof(RegionAllocatorHeader)), std::memory_order_release);
  return RegionAllocator(region, region_bytes, header);
}

RegionAllocator RegionAllocator::Attach(void *region, uint64_t region_bytes) {
  if (region == nullptr || region_bytes < sizeof(RegionAllocatorHeader))
    throw std::invalid_argument("invalid allocator attachment");
  auto *header = static_cast<RegionAllocatorHeader *>(region);
  if (header->magic != 0x5449474f4e414c4cULL || header->version != 1 ||
      header->region_bytes != region_bytes || header->shard_count == 0)
    throw std::runtime_error("allocator attachment validation failed");
  return RegionAllocator(region, region_bytes, header);
}

void *RegionAllocator::Allocate(uint64_t bytes, AllocationDomain, DomainCounter *counter) {
  if (bytes == 0 || counter == nullptr) throw std::invalid_argument("invalid allocation");
  const uint64_t aligned = Align(bytes);
  uint64_t begin = header_->bump.load(std::memory_order_relaxed);
  for (;;) {
    if (begin > bytes_ || aligned > bytes_ - begin) throw std::bad_alloc();
    if (header_->bump.compare_exchange_weak(begin, begin + aligned,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) break;
  }
  header_->allocated_bytes.fetch_add(aligned, std::memory_order_relaxed);
  const uint64_t used = counter->used_bytes.fetch_add(aligned, std::memory_order_relaxed) + aligned;
  uint64_t peak = counter->peak_bytes.load(std::memory_order_relaxed);
  while (peak < used && !counter->peak_bytes.compare_exchange_weak(
                            peak, used, std::memory_order_relaxed)) {}
  return base_ + begin;
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

}  // namespace tigonkv::engine
