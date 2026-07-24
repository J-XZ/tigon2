#include "common/btree_olc_cxl/BTreeOLC_CXL.h"
#include "kv/engine/kv_types_layout.h"
#include "kv/engine/region_allocator.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

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
  char path_template[] = "/tmp/tigonkv-btree-XXXXXX";
  const int temp_fd = mkstemp(path_template);
  assert(temp_fd >= 0);
  close(temp_fd);
  const std::string path(path_template);
  auto pool = tigonkv::engine::DualRegionMappedPool::Open(
      path, MakeConfig(kPoolBytes), true);
  auto &regions = pool.allocator();
  star::CXL_EBR ebr(2, 1, &regions);
  ebr.thread_init_ebr_meta(0, 0);
  using Tree = btreeolc_cxl::BPlusTree<FixedKey, uint64_t, FixedKeyComparator,
                                       std::equal_to<uint64_t>>;
  btreeolc_cxl::TreeNodeAllocation private_binding{
      &regions, AllocationDomain::kOwnerPrivateSwcc, 0, &ebr};
  btreeolc_cxl::TreeNodeAllocation shared_binding{
      &regions, AllocationDomain::kHwccIndex, 1, &ebr};
  // The original tree intentionally has no safe destructor; this test keeps
  // the tree lifetime within the mapped pool and releases the entire test map.
  auto *private_tree = new Tree(private_binding);
  auto *shared_tree = new Tree(shared_binding);
  for (uint32_t i = 0; i < 400; ++i) {
    assert(private_tree->insert(Key(i), i));
    assert(shared_tree->insert(Key(i), i + 1000));
  }
  for (uint32_t i = 0; i < 200; ++i) {
    assert(private_tree->remove(Key(i)));
    assert(shared_tree->remove(Key(i)));
  }
  for (uint32_t i = 0; i < 200; ++i) {
    uint64_t value = 0;
    assert(!private_tree->lookup(Key(i), value));
    assert(!shared_tree->lookup(Key(i), value));
  }
  for (uint32_t i = 200; i < 400; ++i) {
    uint64_t value = 0;
    assert(private_tree->lookup(Key(i), value) && value == i);
    assert(shared_tree->lookup(Key(i), value) && value == i + 1000);
  }
  const auto &layout = regions.layout();
  assert(layout.domains[static_cast<size_t>(AllocationDomain::kOwnerPrivateSwcc)]
             .used_bytes.load() > 0);
  assert(layout.domains[static_cast<size_t>(AllocationDomain::kHwccIndex)]
             .used_bytes.load() > 0);
  const auto root_offset = regions.swcc().ToOffset(
      private_tree->root_for_persistence());
  assert(root_offset != tigonkv::engine::kNullOffset);

  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    auto attached_pool = tigonkv::engine::DualRegionMappedPool::Open(
        path, MakeConfig(kPoolBytes), false);
    auto &attached_regions = attached_pool.allocator();
    star::CXL_EBR attached_ebr(2, 1, &attached_regions);
    attached_ebr.thread_init_ebr_meta(0, 0);
    btreeolc_cxl::TreeNodeAllocation attached_binding{
        &attached_regions, AllocationDomain::kOwnerPrivateSwcc, 0, &attached_ebr};
    Tree attached_tree(attached_binding,
                       attached_regions.swcc().FromOffset(root_offset));
    for (uint32_t i = 200; i < 400; ++i) {
      uint64_t value = 0;
      if (!attached_tree.lookup(Key(i), value) || value != i) _exit(1);
    }
    _exit(0);
  }
  int child_status = 0;
  assert(waitpid(child, &child_status, 0) == child);
  assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
  unlink(path.c_str());
  return 0;
}
