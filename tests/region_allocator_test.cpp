#include "kv/engine/region_allocator.h"

#include <cassert>
#include <cstddef>
#include <array>

int main() {
  using namespace tigonkv::engine;
  alignas(64) std::array<std::byte, 4096> region{};
  auto allocator = RegionAllocator::Initialize(region.data(), region.size(), 2);
  DomainCounter counter;
  void *first = allocator.Allocate(1, AllocationDomain::kHwccIndex, &counter);
  void *second = allocator.Allocate(80, AllocationDomain::kHwccMetadata, &counter);
  assert(reinterpret_cast<uintptr_t>(first) % RegionAllocator::kAlignment == 0);
  assert(reinterpret_cast<uintptr_t>(second) % RegionAllocator::kAlignment == 0);
  const RegionOffset offset = allocator.ToOffset(second);
  assert(offset != kNullOffset && allocator.FromOffset(offset) == second);
  assert(allocator.FromOffset(kNullOffset) == nullptr);
  assert(counter.used_bytes.load() == 192);
  assert(counter.peak_bytes.load() == 192);
  auto attached = RegionAllocator::Attach(region.data(), region.size());
  assert(attached.FromOffset(offset) == second);
  return 0;
}
