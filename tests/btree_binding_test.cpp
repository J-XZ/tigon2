#include "common/btree_olc_cxl/BTreeOLC_CXL.h"
#include "kv/engine/kv_types_layout.h"
#include "kv/engine/region_allocator.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <sys/mman.h>

namespace {
using tigonkv::engine::AllocationDomain;
using tigonkv::engine::DualRegionAllocator;
using tigonkv::engine::DualRegionConfig;
using tigonkv::engine::FixedKey;
using tigonkv::engine::FixedKeyComparator;

DualRegionConfig MakeConfig(size_t bytes) {
  DualRegionConfig config;
  config.total_pool_bytes = bytes;
  config.hwcc_size_bytes = 2 * 1024 * 1024;
  config.swcc_offset_bytes = config.hwcc_size_bytes;
  config.swcc_size_bytes = bytes - config.swcc_offset_bytes;
  config.config_hash = 0xbeef;
  config.vm_count = 2;
  config.partition_count = 8;
  config.fixed_key_size = 32;
  config.fixed_value_size = 128;
  return config;
}

FixedKey Key(uint32_t value) {
  char text[32];
  const int count = std::snprintf(text, sizeof(text), "%08u", value);
  return FixedKey::From(std::string_view(text, static_cast<size_t>(count)), 32);
}

}  // namespace

int main() {
  constexpr size_t kPoolBytes = 8 * 1024 * 1024;
  void *pool = mmap(nullptr, kPoolBytes, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(pool != MAP_FAILED);
  auto regions = DualRegionAllocator::Initialize(pool, MakeConfig(kPoolBytes));
  using Tree = btreeolc_cxl::BPlusTree<FixedKey, uint64_t, FixedKeyComparator,
                                       std::equal_to<uint64_t>>;
  btreeolc_cxl::TreeNodeAllocation private_binding{
      &regions, AllocationDomain::kOwnerPrivateSwcc, 0};
  btreeolc_cxl::TreeNodeAllocation shared_binding{
      &regions, AllocationDomain::kHwccIndex, 1};
  // The original tree intentionally has no safe destructor; this test keeps
  // the tree lifetime within the mapped pool and releases the entire test map.
  auto *private_tree = new Tree(private_binding);
  auto *shared_tree = new Tree(shared_binding);
  for (uint32_t i = 0; i < 400; ++i) {
    assert(private_tree->insert(Key(i), i));
    assert(shared_tree->insert(Key(i), i + 1000));
  }
  for (uint32_t i = 0; i < 400; ++i) {
    uint64_t value = 0;
    assert(private_tree->lookup(Key(i), value) && value == i);
    assert(shared_tree->lookup(Key(i), value) && value == i + 1000);
  }
  const auto &layout = regions.layout();
  assert(layout.domains[static_cast<size_t>(AllocationDomain::kOwnerPrivateSwcc)]
             .used_bytes.load() > 0);
  assert(layout.domains[static_cast<size_t>(AllocationDomain::kHwccIndex)]
             .used_bytes.load() > 0);
  munmap(pool, kPoolBytes);
  return 0;
}
