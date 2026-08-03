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
  config.hwcc_size_bytes = 32 * 1024 * 1024;
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
  std::string fixed(text, static_cast<size_t>(count));
  fixed.resize(32, ' ');
  return FixedKey::From(fixed, 32);
}

tigonkv::engine::OwnerPrivateArenaHeader *PrivateArena(
    DualRegionAllocator &regions, uint32_t partition_id, uint32_t owner_shard) {
  const auto header_bytes =
      (sizeof(tigonkv::engine::OwnerPrivateArenaHeader) +
       tigonkv::engine::RegionAllocator::kAlignment - 1) /
      tigonkv::engine::RegionAllocator::kAlignment *
      tigonkv::engine::RegionAllocator::kAlignment;
  auto *after_header = static_cast<std::byte *>(regions.ResolveOwnerPrivate(
      regions.OwnerPrivateArenaOffset(partition_id) + header_bytes, 1,
      partition_id, owner_shard));
  return reinterpret_cast<tigonkv::engine::OwnerPrivateArenaHeader *>(
      after_header - header_bytes);
}

}  // namespace

int main() {
  constexpr size_t kPoolBytes = 64 * 1024 * 1024;
  char path_template[] = "/tmp/tigonkv-btree-XXXXXX";
  const int temp_fd = mkstemp(path_template);
  assert(temp_fd >= 0);
  close(temp_fd);
  const std::string path(path_template);
  auto pool = tigonkv::engine::DualRegionMappedPool::Open(
      path, MakeConfig(kPoolBytes), true);
  auto &regions = pool.allocator();
  regions.FinalizeStaticHwccLayout();
  regions.PublishStaticHwccLayout();
  regions.InitializeOwnerPrivateArenas(0);
  regions.InitializeOwnerPrivateArenas(1);
  star::CXL_EBR ebr(2, 1, &regions);
  ebr.thread_init_ebr_meta(0, 0);
  using Tree = btreeolc_cxl::BPlusTree<FixedKey, uint64_t, FixedKeyComparator,
                                       std::equal_to<uint64_t>>;
  btreeolc_cxl::TreeNodeAllocation private_binding{
      &regions, AllocationDomain::kOwnerPrivateSwcc, 0, &ebr, 0};
  btreeolc_cxl::TreeNodeAllocation shared_binding{
      &regions, AllocationDomain::kHwccIndex, 1, &ebr};
  regions.BindOwnerPrivateAllocators(0);
  // The original tree intentionally has no safe destructor; this test keeps
  // the tree lifetime within the mapped pool and releases the entire test map.
  auto *private_tree = new Tree(private_binding);
  regions.BindOwnerPrivateAllocators(1);
  auto *shared_tree = new Tree(shared_binding);
  regions.BindOwnerPrivateAllocators(0);
  auto &private_root_slot = PrivateArena(regions, 0, 0)->private_root;
  private_tree->bind_published_root(&private_root_slot);
  const auto initial_private_root =
      private_root_slot.load(std::memory_order_acquire);
  assert(initial_private_root != tigonkv::engine::kNullOffset);
  auto &shared_root_slot = regions.layout().partitions[0].shared_root;
  shared_tree->bind_published_root(&shared_root_slot);
  assert(shared_root_slot.load() != tigonkv::engine::kNullOffset);
  for (uint32_t i = 0; i < 400; ++i) {
    regions.BindOwnerPrivateAllocators(0);
    assert(private_tree->insert(Key(i), i));
    regions.BindOwnerPrivateAllocators(1);
    assert(shared_tree->insert(Key(i), i + 1000));
  }
  const auto split_private_root =
      private_root_slot.load(std::memory_order_acquire);
  assert(split_private_root != tigonkv::engine::kNullOffset);
  assert(split_private_root != initial_private_root);
  assert(shared_root_slot.load() != tigonkv::engine::kNullOffset);
  regions.BindOwnerPrivateAllocators(0);
  for (uint32_t i = 0; i < 200; ++i)
    assert(private_tree->remove(Key(i)));
  regions.BindOwnerPrivateAllocators(1);
  for (uint32_t i = 0; i < 200; ++i)
    assert(shared_tree->remove(Key(i)));
  assert(shared_root_slot.load() != tigonkv::engine::kNullOffset);
  regions.BindOwnerPrivateAllocators(0);
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
  // Original adjacent callback must observe the immediate leaf-chain
  // neighbours while their leaf latches remain held.  The rightmost existing
  // tuple supplies the normal Tigon next-key sentinel contract for this test.
  bool private_adjacent = false;
  assert(private_tree->insert_and_process_adjacent_tuples(
      Key(100), 100,
      [&](const FixedKey *prev_key, uint64_t *prev_value,
          const FixedKey *next_key, uint64_t *next_value) {
        assert(prev_key == nullptr && prev_value == nullptr);
        assert(next_key != nullptr && next_value != nullptr);
        assert(next_key->Compare(Key(200)) == 0 && *next_value == 200);
        private_adjacent = true;
        return true;
      }));
  assert(private_adjacent);
  bool shared_adjacent = false;
  regions.BindOwnerPrivateAllocators(1);
  assert(shared_tree->insert_and_process_adjacent_tuples(
      Key(100), 1100,
      [&](const FixedKey *prev_key, uint64_t *prev_value,
          const FixedKey *next_key, uint64_t *next_value) {
        assert(prev_key == nullptr && prev_value == nullptr);
        assert(next_key != nullptr && next_value != nullptr);
        assert(next_key->Compare(Key(200)) == 0 && *next_value == 1200);
        shared_adjacent = true;
        return true;
      }));
  assert(shared_adjacent);
  bool private_removed = false;
  regions.BindOwnerPrivateAllocators(0);
  assert(private_tree->remove_and_process_adjacent_keys(
      Key(100),
      [&](const FixedKey *prev_key, uint64_t *prev_value,
          const FixedKey *cur_key, uint64_t *cur_value,
          const FixedKey *next_key, uint64_t *next_value) {
        assert(prev_key == nullptr && prev_value == nullptr);
        assert(cur_key != nullptr && cur_value != nullptr &&
               cur_key->Compare(Key(100)) == 0 && *cur_value == 100);
        assert(next_key != nullptr && next_value != nullptr &&
               next_key->Compare(Key(200)) == 0 && *next_value == 200);
        private_removed = true;
        return true;
      }));
  bool shared_removed = false;
  regions.BindOwnerPrivateAllocators(1);
  assert(shared_tree->remove_and_process_adjacent_keys(
      Key(100),
      [&](const FixedKey *prev_key, uint64_t *prev_value,
          const FixedKey *cur_key, uint64_t *cur_value,
          const FixedKey *next_key, uint64_t *next_value) {
        assert(prev_key == nullptr && prev_value == nullptr);
        assert(cur_key != nullptr && cur_value != nullptr &&
               cur_key->Compare(Key(100)) == 0 && *cur_value == 1100);
        assert(next_key != nullptr && next_value != nullptr &&
               next_key->Compare(Key(200)) == 0 && *next_value == 1200);
        shared_removed = true;
        return true;
      }));
  assert(private_removed && shared_removed);
  bool private_next_key_update = false;
  regions.BindOwnerPrivateAllocators(0);
  assert(private_tree->lookupForNextKeyUpdate(
      Key(200),
      [&](const FixedKey *prev_key, uint64_t *prev_value,
          const FixedKey *cur_key, uint64_t *cur_value,
          const FixedKey *next_key, uint64_t *next_value) {
        assert(prev_key == nullptr && prev_value == nullptr);
        assert(cur_key != nullptr && cur_value != nullptr &&
               cur_key->Compare(Key(200)) == 0 && *cur_value == 200);
        assert(next_key != nullptr && next_value != nullptr &&
               next_key->Compare(Key(201)) == 0 && *next_value == 201);
        private_next_key_update = true;
      }));
  bool shared_next_key_update = false;
  regions.BindOwnerPrivateAllocators(1);
  assert(shared_tree->lookupForNextKeyUpdate(
      Key(200),
      [&](const FixedKey *prev_key, uint64_t *prev_value,
          const FixedKey *cur_key, uint64_t *cur_value,
          const FixedKey *next_key, uint64_t *next_value) {
        assert(prev_key == nullptr && prev_value == nullptr);
        assert(cur_key != nullptr && cur_value != nullptr &&
               cur_key->Compare(Key(200)) == 0 && *cur_value == 1200);
        assert(next_key != nullptr && next_value != nullptr &&
               next_key->Compare(Key(201)) == 0 && *next_value == 1201);
        shared_next_key_update = true;
      }));
  assert(private_next_key_update && shared_next_key_update);
  std::vector<Tree::KeyValuePair> private_scan;
  std::vector<Tree::KeyValuePair> shared_scan;
  regions.BindOwnerPrivateAllocators(0);
  private_tree->scan(Key(250), Key(260), true, true, 0, private_scan);
  shared_tree->scan(Key(250), Key(260), true, true, 0, shared_scan);
  assert(private_scan.size() == 11 && shared_scan.size() == 11);
  for (uint32_t i = 0; i < private_scan.size(); ++i) {
    assert(private_scan[i].first.Compare(Key(250 + i)) == 0);
    assert(private_scan[i].second == 250 + i);
    assert(shared_scan[i].second == 1250 + i);
  }
  assert(regions.OwnerPrivateUsedBytes(0) > 0);
  regions.BindOwnerPrivateAllocators(1);
  assert(regions.DynamicHwccUsedBytes(1) > 0);
  latency_sim::Config latency;
  latency.fixed_latency.enabled = true;
  latency.fixed_latency.foreground_enabled = true;
  latency.fixed_latency.swcc_fixed_ns_per_line = 1;
  latency.fixed_latency.hwcc_fixed_ns_per_line = 1;
  auto &simulator = latency_sim::GlobalLatencySimulator();
  simulator.Configure(latency);
  uint64_t value = 0;
  regions.BindOwnerPrivateAllocators(0);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  assert(private_tree->lookup(Key(250), value) && value == 250);
  assert(simulator.PendingDelayNsForTest() > 0);
  simulator.EndScopeAndDelay();
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  assert(shared_tree->lookup(Key(250), value) && value == 1250);
  assert(simulator.PendingDelayNsForTest() > 0);
  simulator.EndScopeAndDelay();
  simulator.Configure(latency_sim::Config{});

  // Collapse the private root after a split.  The only persistent authority
  // must be the owner-private atomic slot, not the process-local wrapper.
  regions.BindOwnerPrivateAllocators(0);
  for (uint32_t i = 201; i < 400; ++i)
    assert(private_tree->remove(Key(i)));
  const auto merged_private_root =
      private_root_slot.load(std::memory_order_acquire);
  assert(merged_private_root != tigonkv::engine::kNullOffset);
  assert(merged_private_root != split_private_root);

  regions.PublishOwnerInitialized(0);
  regions.PublishOwnerInitialized(1);
  regions.PublishReady();
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    auto attached_pool = tigonkv::engine::DualRegionMappedPool::Open(
        path, MakeConfig(kPoolBytes), false);
    auto &attached_regions = attached_pool.allocator();
    attached_regions.BindOwnerPrivateAllocators(0);
    star::CXL_EBR attached_ebr(2, 1, &attached_regions);
    attached_ebr.thread_init_ebr_meta(0, 0);
    btreeolc_cxl::TreeNodeAllocation attached_binding{
        &attached_regions, AllocationDomain::kOwnerPrivateSwcc, 0, &attached_ebr, 0};
    auto &attached_private_root =
        PrivateArena(attached_regions, 0, 0)->private_root;
    if (attached_private_root.load(std::memory_order_acquire) ==
        tigonkv::engine::kNullOffset)
      _exit(1);
    Tree attached_tree(attached_binding,
                       attached_regions.ResolveOwnerPrivate(
                           attached_private_root.load(std::memory_order_acquire),
                           btreeolc_cxl::kPageSize, 0, 0));
    attached_tree.bind_published_root(&attached_private_root);
    uint64_t attached_private_value = 0;
    if (!attached_tree.lookup(Key(200), attached_private_value) ||
        attached_private_value != 200)
      _exit(2);
    // Peer adopts the HWCC published shared root from the layout slot.
    btreeolc_cxl::TreeNodeAllocation attached_shared_binding{
        &attached_regions, AllocationDomain::kHwccIndex, 1, &attached_ebr};
    auto &live_slot = attached_regions.layout().partitions[0].shared_root;
    const auto live = live_slot.load();
    if (live == tigonkv::engine::kNullOffset) _exit(3);
    Tree attached_shared(
        attached_shared_binding, attached_regions.ResolveDynamicHwcc(
                                     live, btreeolc_cxl::kPageSize, 1));
    attached_shared.bind_published_root(&live_slot);
    for (uint32_t i = 200; i < 400; ++i) {
      uint64_t value = 0;
      if (!attached_shared.lookup(Key(i), value) || value != i + 1000) _exit(4);
    }
    _exit(0);
  }
  int child_status = 0;
  assert(waitpid(child, &child_status, 0) == child);
  assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
  unlink(path.c_str());
  return 0;
}
