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
  auto &shared_root_slot = regions.layout().partitions[0].shared_root;
  shared_tree->bind_published_root(&shared_root_slot);
  assert(shared_root_slot.load() != tigonkv::engine::kNullOffset);
  for (uint32_t i = 0; i < 400; ++i) {
    assert(private_tree->insert(Key(i), i));
    assert(shared_tree->insert(Key(i), i + 1000));
  }
  assert(shared_root_slot.load() != tigonkv::engine::kNullOffset);
  for (uint32_t i = 0; i < 200; ++i) {
    assert(private_tree->remove(Key(i)));
    assert(shared_tree->remove(Key(i)));
  }
  assert(shared_root_slot.load() != tigonkv::engine::kNullOffset);
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
  std::vector<Tree::KeyValuePair> private_scan;
  std::vector<Tree::KeyValuePair> shared_scan;
  private_tree->scan(Key(250), Key(260), true, true, 0, private_scan);
  shared_tree->scan(Key(250), Key(260), true, true, 0, shared_scan);
  assert(private_scan.size() == 11 && shared_scan.size() == 11);
  for (uint32_t i = 0; i < private_scan.size(); ++i) {
    assert(private_scan[i].first.Compare(Key(250 + i)) == 0);
    assert(private_scan[i].second == 250 + i);
    assert(shared_scan[i].second == 1250 + i);
  }
  const auto &layout = regions.layout();
  assert(layout.domains[static_cast<size_t>(AllocationDomain::kOwnerPrivateSwcc)]
             .used_bytes.load() > 0);
  assert(layout.domains[static_cast<size_t>(AllocationDomain::kHwccIndex)]
             .used_bytes.load() > 0);
  const auto root_offset = regions.swcc().ToOffset(
      private_tree->root_for_persistence());
  assert(root_offset != tigonkv::engine::kNullOffset);

  latency_sim::Config latency;
  latency.enabled = true;
  latency.foreground_enabled = true;
  latency.stats_enabled = true;
  auto &simulator = latency_sim::GlobalLatencySimulator();
  simulator.Configure(latency);
  uint64_t value = 0;
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  assert(private_tree->lookup(Key(250), value) && value == 250);
  simulator.EndScopeAndDelay();
  const auto private_stats = simulator.TakeStatsAndReset();
  assert(private_stats.swcc_raw_line_accesses > 1);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  assert(shared_tree->lookup(Key(250), value) && value == 1250);
  simulator.EndScopeAndDelay();
  const auto shared_stats = simulator.TakeStatsAndReset();
  assert(shared_stats.hwcc_raw_line_accesses > 1);
  simulator.Configure(latency_sim::Config{});

  regions.PublishOwnerInitialized(0);
  regions.PublishOwnerInitialized(1);
  regions.PublishReady();
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
    // Peer adopts the HWCC published shared root from the layout slot.
    btreeolc_cxl::TreeNodeAllocation attached_shared_binding{
        &attached_regions, AllocationDomain::kHwccIndex, 1, &attached_ebr};
    auto &live_slot = attached_regions.layout().partitions[0].shared_root;
    const auto live = live_slot.load();
    if (live == tigonkv::engine::kNullOffset) _exit(2);
    Tree attached_shared(
        attached_shared_binding, attached_regions.hwcc().FromOffset(live));
    attached_shared.bind_published_root(&live_slot);
    for (uint32_t i = 200; i < 400; ++i) {
      uint64_t value = 0;
      if (!attached_shared.lookup(Key(i), value) || value != i + 1000) _exit(3);
    }
    _exit(0);
  }
  int child_status = 0;
  assert(waitpid(child, &child_status, 0) == child);
  assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
  unlink(path.c_str());
  return 0;
}
