#include "kv/engine/kv_partition.h"
#include "kv/engine/kv_migration.h"
#include "kv/engine/latency_inject.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {

class PassthroughScc final : public star::SCCManager {
 public:
  void init_scc_metadata(void *meta, std::size_t host) override {
    auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(meta);
    smeta->clear_all_scc_bits(); smeta->set_scc_bit(host);
  }
  void do_read(void *, std::size_t, void *dst, const void *src, uint64_t bytes) override {
    std::memcpy(dst, src, bytes);
  }
  void do_write(void *, std::size_t, void *dst, const void *src, uint64_t bytes) override {
    std::memcpy(dst, src, bytes);
  }
  void prepare_read(void *, std::size_t, void *, uint64_t) override {}
  void finish_write(void *, std::size_t, void *, uint64_t) override {}
};

tigonkv::engine::DualRegionConfig Config() {
  tigonkv::engine::DualRegionConfig config;
  config.total_pool_bytes = 8 * 1024 * 1024;
  config.hwcc_size_bytes = 2 * 1024 * 1024;
  config.swcc_offset_bytes = config.hwcc_size_bytes;
  config.swcc_size_bytes = config.total_pool_bytes - config.swcc_offset_bytes;
  config.config_hash = 0x9911;
  config.vm_count = 2;
  config.partition_count = 8;
  config.fixed_key_size = 32;
  config.fixed_value_size = 128;
  return config;
}

}  // namespace

int main() {
  char path_template[] = "/tmp/tigonkv-partition-XXXXXX";
  const int fd = mkstemp(path_template);
  assert(fd >= 0);
  close(fd);
  const std::string path(path_template);
  auto pool = tigonkv::engine::DualRegionMappedPool::Open(path, Config(), true);
  auto &regions = pool.allocator();
  star::CXLMemory memory;
  star::CXLMemory::bind_dual_region_allocator(&regions, 1);
  void *index_object = memory.cxlalloc_malloc_wrapper(
      128, star::CXLMemory::INDEX_ALLOCATION);
  void *payload_object = memory.cxlalloc_malloc_wrapper(
      128, star::CXLMemory::DATA_ALLOCATION);
  assert(regions.IsHwccAddress(index_object));
  assert(regions.IsSwccAddress(payload_object));
  const uint64_t payload_offset = star::CXLMemory::pointer_to_pool_offset(payload_object);
  assert(payload_offset < (1ull << 37));
  assert(star::CXLMemory::pool_offset_to_pointer(payload_offset) == payload_object);
  star::CXLMemory::commit_shared_data_initialization(
      star::CXLMemory::cxl_global_epoch_root_index, index_object);
  void *recovered_root = nullptr;
  star::CXLMemory::wait_and_retrieve_cxl_shared_data(
      star::CXLMemory::cxl_global_epoch_root_index, &recovered_root);
  assert(recovered_root == index_object);
  star::CXL_EBR ebr(2, 1, &regions);
  ebr.thread_init_ebr_meta(0, 0);
  star::global_ebr_meta = &ebr;
  void *private_reuse = regions.AllocateOwnerPrivate(128, 5, 1);
  regions.FreeOwnerPrivate(private_reuse, 128, 5, 1);
  assert(regions.AllocateOwnerPrivate(128, 5, 1) == private_reuse);
  tigonkv::engine::KVPartition partition(regions, ebr, 5, 1, false);
  assert(regions.OwnerPrivateArenaOffset(5) ==
         regions.layout().partitions[5].private_arena);
  {
    std::vector<tigonkv::engine::KVPartition *> parts(8, nullptr);
    parts[5] = &partition;
    const uint64_t hw_budget = (1024ULL * 1024ULL * 1024ULL -
        star::CXL_EBR::max_ebr_retiring_memory) / 2;
    tigonkv::engine::KvMigrationRuntime::Instance().Install(
        parts, 32, 128, 0, 8, hw_budget);
  }
  bool wrong_owner_rejected = false;
  try {
    (void)regions.AllocateOwnerPrivate(64, 5, 0);
  } catch (const std::runtime_error &) {
    wrong_owner_rejected = true;
  }
  assert(wrong_owner_rejected);
  assert(partition.PutPrivate("alpha", "one"));
  assert(partition.PutPrivate("beta", "two"));
  assert(!partition.PutPrivate("alpha", "updated"));
  assert(regions.IsInOwnerPrivateArena(
      regions.swcc().FromOffset(regions.layout().partitions[5].private_root), 5));
  std::string value;
  assert(partition.GetPrivate("alpha", &value) && value == "updated");
  for (uint32_t i = 0; i < 256; ++i) {
    char key[32];
    std::snprintf(key, sizeof(key), "tree-split-%08u", i);
    assert(partition.PutPrivate(key, "tree-value"));
  }
  for (uint32_t i = 0; i < 256; ++i) {
    char key[32];
    std::snprintf(key, sizeof(key), "tree-split-%08u", i);
    assert(partition.GetPrivate(key, &value) && value == "tree-value");
    assert(partition.DeletePrivate(key));
  }
  PassthroughScc scc;
  star::scc_manager = &scc;
  auto &simulator = latency_sim::GlobalLatencySimulator();
  latency_sim::Config tree_write_latency;
  tree_write_latency.enabled = true;
  tree_write_latency.foreground_enabled = true;
  tree_write_latency.swcc_write_ns_per_line = 1;
  simulator.Configure(tree_write_latency);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  assert(!partition.GetPrivate("tree-lookup-miss", &value));
  assert(simulator.PendingDelayNsForTest() == 0);
  simulator.EndScopeAndDelay();
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  assert(partition.PutPrivate("tree-write", "value"));
  assert(simulator.PendingDelayNsForTest() > 0);
  simulator.EndScopeAndDelay();
  simulator.Configure(latency_sim::Config{});
  assert(partition.DeletePrivate("tree-write"));
  latency_sim::Config latency;
  latency.enabled = true;
  latency.foreground_enabled = true;
  latency.stats_enabled = true;
  simulator.Configure(latency);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  assert(partition.PutPrivate("latency-only", "payload"));
  assert(partition.GetPrivate("latency-only", &value) && value == "payload");
  std::vector<std::pair<std::string, std::string>> latency_scan;
  assert(partition.ScanOwned("latency-only", 1, &latency_scan));
  assert(latency_scan.size() == 1 && latency_scan[0].second == "payload");
  std::vector<std::string> latency_scan_keys;
  assert(partition.ScanOwnedKeys("latency-only", 1, &latency_scan_keys));
  assert(latency_scan_keys.size() == 1 &&
         latency_scan_keys[0] == "latency-only");
  simulator.EndScopeAndDelay();
  auto latency_stats = simulator.TakeStatsAndReset();
  assert(latency_stats.swcc_raw_line_accesses > 0);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  assert(partition.PromotePrivate("latency-only", 1));
  assert(partition.MoveOutPrivate("latency-only", 1));
  simulator.EndScopeAndDelay();
  latency_stats = simulator.TakeStatsAndReset();
  assert(latency_stats.swcc_raw_line_accesses > 0);
  assert(latency_stats.hwcc_raw_line_accesses > 0);
  simulator.Configure(latency_sim::Config{});
  assert(partition.DeletePrivate("latency-only"));
  const uint64_t migration_in_before_alpha =
      regions.layout().partitions[5].migration_in_seq.load();
  const uint64_t removal_before_alpha =
      regions.layout().partitions[5].shared_removal_state.load();
  assert(partition.PromotePrivate("alpha", 1));
  assert(partition.GetPrivate("alpha", &value) && value == "updated");
  // Once migrated, PUT must update the shared SCC authority rather than the
  // retained private locator row.
  assert(!partition.PutPrivate("alpha", "shared-update"));
  assert(partition.GetPrivate("alpha", &value) && value == "shared-update");
  assert(regions.layout().partitions[5].migration_in_seq.load() ==
         migration_in_before_alpha + 1);
  const auto hwcc_before_moveout = regions.layout().domains[
      static_cast<size_t>(tigonkv::engine::AllocationDomain::kHwccMetadata)].used_bytes.load();
  const auto swcc_before_moveout = regions.layout().domains[
      static_cast<size_t>(tigonkv::engine::AllocationDomain::kSharedPayloadSwcc)].used_bytes.load();
  assert(partition.MoveOutPrivate("alpha", 1));
  assert(partition.GetPrivate("alpha", &value) && value == "shared-update");
  assert(regions.layout().partitions[5].shared_removal_state.load() ==
         removal_before_alpha + (uint64_t{1} << 32));
  assert(ebr.drain_quiescent() > 0);
  assert(regions.layout().domains[
      static_cast<size_t>(tigonkv::engine::AllocationDomain::kHwccMetadata)].used_bytes.load() < hwcc_before_moveout);
  assert(regions.layout().domains[
      static_cast<size_t>(tigonkv::engine::AllocationDomain::kSharedPayloadSwcc)].used_bytes.load() < swcc_before_moveout);
  assert(partition.PutPrivate("delete-shared", "value"));
  assert(partition.PromotePrivate("delete-shared", 1));
  const uint64_t removal_before_delete =
      regions.layout().partitions[5].shared_removal_state.load();
  assert(partition.DeletePrivate("delete-shared"));
  assert(regions.layout().partitions[5].shared_removal_state.load() ==
         removal_before_delete + (uint64_t{1} << 32));
  star::scc_manager = nullptr;
  assert(partition.DeletePrivate("beta"));
  assert(!partition.GetPrivate("beta", &value));

  // Deleting a migrated key must remove both the shared authority and its
  // private locator, then retire all three objects through EBR.
  star::scc_manager = &scc;
  assert(partition.PutPrivate("gamma", "three"));
  assert(partition.PromotePrivate("gamma", 1));
  assert(partition.DeletePrivate("gamma"));
  assert(!partition.GetPrivate("gamma", &value));
  assert(ebr.drain_quiescent() > 0);
  assert(partition.PutPrivate("gamma", "replacement"));
  assert(partition.GetPrivate("gamma", &value) && value == "replacement");
  bool exchanged = false;
  assert(partition.CompareExchangePrivate("gamma", "replacement", "cas-private", &exchanged));
  assert(exchanged);
  assert(partition.CompareExchangePrivate("gamma", "wrong", "ignored", &exchanged));
  assert(!exchanged);
  assert(partition.PromotePrivate("gamma", 1));
  assert(partition.CompareExchangePrivate("gamma", "cas-private", "cas-shared", &exchanged));
  assert(exchanged);
  assert(partition.GetPrivate("gamma", &value) && value == "cas-shared");
  assert(partition.PutPrivate("counter", "1"));
  int64_t incremented = 0;
  assert(partition.IncrementPrivate("counter", 2, &incremented) && incremented == 3);
  assert(partition.GetPrivate("counter", &value) && value == "3");
  assert(partition.IncrementPrivate("new-counter", -2, &incremented) && incremented == -2);
  assert(partition.GetPrivate("new-counter", &value) && value == "-2");
  assert(partition.CompareExchangePrivate("new-cas", "", "created", &exchanged));
  assert(exchanged && partition.GetPrivate("new-cas", &value) && value == "created");

  // PolicyClock init sets second_chance=0, so a never-accessed migrated row is
  // eligible on the first over-budget move_row_out (original OnDemand gate).
  assert(partition.PutPrivate("clock", "victim"));
  assert(partition.PromotePrivate("clock", 1));
  star::cxl_memory.set_total_hw_cc_usage(
      (1024ULL * 1024ULL * 1024ULL - star::CXL_EBR::max_ebr_retiring_memory) / 2);
  assert(partition.MoveOutClockVictim(1));
  assert(partition.GetPrivate("clock", &value) && value == "victim");
  assert(partition.hwcc_used_bytes() > 0);
  assert(partition.shared_payload_used_bytes() < partition.shared_payload_capacity_bytes());

  // FAIL_ALREADY_IN_CXL must pin ref_cnt so move-out quiescence waits; unpin
  // restores the three-condition allow path used by MoveOutPrivate.
  assert(partition.PutPrivate("pinned", "hold"));
  assert(partition.PromotePrivate("pinned", 1));
  star::TwoPLPashaMetadataShared *pinned = nullptr;
  assert(!partition.PromotePrivate("pinned", 1, &pinned));
  assert(pinned != nullptr);
  assert(!partition.MoveOutPrivate("pinned", 1));
  star::TwoPLPashaHelper::kv_unpin_shared_ref(pinned);
  assert(partition.MoveOutPrivate("pinned", 1));
  assert(partition.GetPrivate("pinned", &value) && value == "hold");

  // Pin failure (write_locked) must leave *pinned_existing null so Serve
  // cannot mismatched-unpin and wrap uint8_t ref_cnt under NDEBUG.
  assert(partition.PutPrivate("pinfail", "x"));
  assert(partition.PromotePrivate("pinfail", 1));
  star::TwoPLPashaMetadataShared *held = nullptr;
  assert(!partition.PromotePrivate("pinfail", 1, &held));
  assert(held != nullptr);
  held->set_write_locked();
  star::TwoPLPashaMetadataShared *no_pin =
      reinterpret_cast<star::TwoPLPashaMetadataShared *>(0x1);
  assert(!partition.PromotePrivate("pinfail", 1, &no_pin));
  assert(no_pin == nullptr);
  held->clear_write_locked();
  star::TwoPLPashaHelper::kv_unpin_shared_ref(held);
  assert(partition.MoveOutPrivate("pinfail", 1));

  // DELETE contention is a retry, not NotFound: a range/point pin must delay
  // physical removal and the tombstone must not escape between attempts.
  assert(partition.PutPrivate("delete-pinned", "present"));
  assert(partition.PromotePrivate("delete-pinned", 1));
  star::TwoPLPashaMetadataShared *delete_pin = nullptr;
  assert(!partition.PromotePrivate("delete-pinned", 1, &delete_pin));
  assert(delete_pin != nullptr);
  std::thread release_delete_pin([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    star::TwoPLPashaHelper::kv_unpin_shared_ref(delete_pin);
  });
  assert(partition.DeletePrivate("delete-pinned"));
  release_delete_pin.join();
  assert(!partition.GetPrivate("delete-pinned", &value));

  // A partition scan merges the private and shared authorities in key order,
  // without resurrecting tombstones or duplicate migrated locator rows.
  std::vector<std::pair<std::string, std::string>> scan;
  assert(partition.ScanOwned("alpha", 0, &scan));
  assert(scan.size() == 8);
  assert(scan[0] == std::make_pair(std::string("alpha"), std::string("shared-update")));
  assert(scan[1] == std::make_pair(std::string("clock"), std::string("victim")));
  assert(scan[2] == std::make_pair(std::string("counter"), std::string("3")));
  assert(scan[3] == std::make_pair(std::string("gamma"), std::string("cas-shared")));
  assert(scan[4] == std::make_pair(std::string("new-cas"), std::string("created")));
  assert(scan[5] == std::make_pair(std::string("new-counter"), std::string("-2")));
  assert(scan[6] == std::make_pair(std::string("pinfail"), std::string("x")));
  assert(scan[7] == std::make_pair(std::string("pinned"), std::string("hold")));
  assert(partition.ScanOwned("alpha", 2, &scan));
  assert(scan.size() == 2);
  assert(scan[0].first == "alpha" && scan[1].first == "clock");

  // Original TwoPLPasha range migration first walks owner locators without
  // reading values, then move_row_in(..., inc_ref=false). The requester reads
  // values only from CXL.
  std::vector<std::string> scan_keys;
  assert(partition.ScanOwnedKeys("alpha", 2, &scan_keys));
  assert(scan_keys.size() == 2);
  for (const auto &key : scan_keys)
    assert(partition.EnsureInShared(key, 1) == tigonkv::StatusCode::kOk);
  std::vector<std::pair<std::string, std::string>> shared_scan;
  assert(partition.ScanShared("alpha", 2, &shared_scan));
  assert(shared_scan == scan);
  assert(partition.MoveOutPrivate("alpha", 1));

  // Bounded dual-tree merge: migrated shared authority + later private rows,
  // without collecting the full remaining keyspace before applying limit.
  assert(partition.PutPrivate("m1", "shared-m1"));
  assert(partition.PromotePrivate("m1", 1));
  assert(partition.PutPrivate("m2", "priv-m2"));
  assert(partition.PutPrivate("m3", "priv-m3"));
  for (int i = 0; i < 128; ++i) {
    char key[16];
    std::snprintf(key, sizeof(key), "m9%03d", i);
    assert(partition.PutPrivate(key, "tail"));
  }
  assert(partition.ScanOwned("m1", 3, &scan));
  assert(scan.size() == 3);
  assert(scan[0] == std::make_pair(std::string("m1"), std::string("shared-m1")));
  assert(scan[1] == std::make_pair(std::string("m2"), std::string("priv-m2")));
  assert(scan[2] == std::make_pair(std::string("m3"), std::string("priv-m3")));

  // Migration is not a logical mutation: repeated owner scans must retain
  // every stable key exactly once while rows move between private and shared.
  std::vector<std::string> moving_keys;
  for (uint32_t i = 0; i < 32; ++i) {
    char key[32];
    std::snprintf(key, sizeof(key), "scan-race-%02u", i);
    assert(partition.PutPrivate(key, "stable"));
    moving_keys.emplace_back(key);
  }
  std::atomic<bool> migration_done{false};
  std::thread migrator([&] {
    ebr.thread_init_ebr_meta(0, 0);
    for (uint32_t round = 0; round < 8; ++round) {
      for (const auto &key : moving_keys) {
        partition.PromotePrivate(key, 1);
        partition.MoveOutPrivate(key, 1);
      }
    }
    ebr.handoff_retired_objects();
    migration_done.store(true, std::memory_order_release);
  });
  uint32_t scan_rounds = 0;
  do {
    assert(partition.ScanOwned("scan-race-", 32, &scan));
    assert(scan.size() == moving_keys.size());
    for (size_t i = 0; i < scan.size(); ++i) {
      assert(scan[i].first == moving_keys[i]);
      assert(scan[i].second == "stable");
      if (i != 0) assert(scan[i - 1].first < scan[i].first);
    }
    ++scan_rounds;
  } while (!migration_done.load(std::memory_order_acquire) ||
           scan_rounds < 32);
  migrator.join();

  void *handed_off = regions.Allocate(
      64, tigonkv::engine::AllocationDomain::kSharedPayloadSwcc, 0);
  std::thread retiring_worker([&] {
    ebr.thread_init_ebr_meta(0, 0);
    ebr.add_retired_object(handed_off, 64, star::CXLMemory::DATA_FREE, 0);
    ebr.handoff_retired_objects();
  });
  retiring_worker.join();
  assert(ebr.drain_quiescent() >= 64);
  star::scc_manager = nullptr;

  regions.PublishReady();
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    auto attached_pool = tigonkv::engine::DualRegionMappedPool::Open(path, Config(), false);
    auto &attached_regions = attached_pool.allocator();
    star::CXL_EBR attached_ebr(2, 1, &attached_regions);
    attached_ebr.thread_init_ebr_meta(0, 0);
    PassthroughScc attached_scc;
    star::scc_manager = &attached_scc;
    tigonkv::engine::KVPartition attached(attached_regions, attached_ebr, 5, 1, true);
    std::string child_value;
    if (!attached.GetPrivate("alpha", &child_value) || child_value != "shared-update") _exit(1);
    if (attached.GetPrivate("beta", &child_value)) _exit(1);
    _exit(0);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  star::global_ebr_meta = nullptr;
  tigonkv::engine::KvMigrationRuntime::Instance().Reset();
  unlink(path.c_str());
  return 0;
}
