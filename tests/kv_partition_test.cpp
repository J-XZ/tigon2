#include "kv/engine/kv_partition.h"
#include "kv/engine/kv_migration.h"
#include "kv/engine/latency_inject.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <string>
#include <stdexcept>
#include <thread>
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
  config.total_pool_bytes = 16 * 1024 * 1024;
  config.hwcc_size_bytes = 4 * 1024 * 1024;
  config.swcc_offset_bytes = config.hwcc_size_bytes;
  config.swcc_size_bytes = config.total_pool_bytes - config.swcc_offset_bytes;
  config.config_hash = 0x9911;
  config.vm_count = 2;
  config.partition_count = 8;
  config.fixed_key_size = 32;
  config.fixed_value_size = 128;
  // The original ValueStruct/local-metadata split and owner-private Clock
  // tracker node use separate owner-private allocations.
  // This focused Clock test materializes more than 1024 owner rows while
  // retaining the original eight-partition layout.
  config.owner_private_swcc_fraction = 0.9;
  return config;
}

std::string FixedValue(std::string_view text) {
  std::string value(128, '\0');
  std::memcpy(value.data(), text.data(), text.size());
  return value;
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
  regions.FinalizeStaticHwccLayout();
  regions.InitializeOwnerPrivateArenas(0);
  regions.InitializeOwnerPrivateArenas(1);
  star::CXLMemory memory;
  star::CXLMemory::bind_dual_region_allocator(&regions, 1);
  // Fixture binds the process owner once for this VM before any partition
  // construct; KVPartition must not rebind (§11.3).
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
  tigonkv::engine::KVPartition partition(regions, ebr, 5, 1, false, true);
  assert(star::CXLMemory::bound_owner_shard() == 1);
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
  // The original max-key tuple is physically present but never leaks through
  // an empty logical scan.
  std::vector<std::pair<std::string, std::string>> empty_scan;
  assert(partition.ScanLocalPartition("", 0, &empty_scan));
  assert(empty_scan.empty());
  bool wrong_owner_rejected = false;
  try {
    (void)regions.AllocateOwnerPrivate(64, 5, 0);
  } catch (const std::runtime_error &) {
    wrong_owner_rejected = true;
  }
  assert(wrong_owner_rejected);
  assert(partition.PutPrivate("alpha", "one"));
  // The ITable view is a direct, non-owning view of the owner-private tree:
  // it must expose the original lookup/scan callback contract rather than
  // retaining KV-only throwing stubs.
  auto *table = tigonkv::engine::KvMigrationRuntime::Instance().TableFor(5);
  assert(table != nullptr);
  const auto alpha_key = tigonkv::engine::FixedKey::From("alpha", 32);
  auto [alpha_meta, alpha_data] = table->search(&alpha_key);
  assert(alpha_meta != nullptr && alpha_data != nullptr);
  assert(alpha_meta->load() != tigonkv::engine::kNullOffset);
  assert(std::string(static_cast<char *>(alpha_data), 128) == FixedValue("one"));
  bool table_scan_called = false;
  table->scan(&alpha_key,
              [&](const void *key, star::ITable::MetaDataType *meta, void *data,
                  bool) {
                assert(key != nullptr && meta != nullptr && data != nullptr);
                table_scan_called = true;
                return true;
              });
  assert(table_scan_called);
  bool table_adjacent_called = false;
  assert(table->search_and_update_next_key_info(
      &alpha_key, [&](const void *, void *, void *, const void *current_key,
                      void *current_meta, void *current_data, const void *,
                      void *, void *) {
        assert(current_key != nullptr && current_meta != nullptr &&
               current_data != nullptr);
        // TableBTreeOLC's adjacent callbacks expose the resolved local
        // metadata, not the ValueStruct::meta slot returned by search().
        assert(current_meta != alpha_meta);
        table_adjacent_called = true;
      }));
  assert(table_adjacent_called);
  const auto table_insert_key = tigonkv::engine::FixedKey::From("table-next", 32);
  const std::string table_insert_value = FixedValue("table-value");
  bool next_key_locked = false;
  assert(table->insert_lock_next_key(
      &table_insert_key, table_insert_value.data(),
      [&](const void *next_key, star::ITable::MetaDataType *next_meta,
          void *next_data) {
        assert(next_key != nullptr && next_meta != nullptr && next_data != nullptr);
        next_key_locked = true;
        return true;
      }, true));
  assert(next_key_locked);
  assert(table->remove(&table_insert_key));
  const uint64_t root_pubs_after_create = partition.PrivateRootPublishCount();
  assert(root_pubs_after_create >= 1);
  // §11.6: value updates that do not change private root must not republish.
  assert(!partition.PutPrivate("alpha", "two"));
  assert(!partition.PutPrivate("alpha", "three"));
  assert(partition.PrivateRootPublishCount() == root_pubs_after_create);
  const auto private_arena_offset = regions.OwnerPrivateArenaOffset(5);
  auto *private_arena = static_cast<tigonkv::engine::OwnerPrivateArenaHeader *>(
      regions.swcc().FromOffset(private_arena_offset));
  assert(private_arena->private_root != tigonkv::engine::kNullOffset);
  // §10.9: concurrent create races must free the unpublished loser and upsert.
  {
    std::atomic<uint32_t> ready{0};
    std::atomic<uint32_t> done{0};
    std::atomic<bool> start{false};
    const auto put_with_facade_retry = [&](std::string_view key,
                                           std::string_view value) {
      for (uint32_t attempt = 0; attempt < 1024; ++attempt) {
        try {
          (void)partition.PutPrivate(key, value);
          return;
        } catch (const std::runtime_error &error) {
          assert(std::string(error.what()).find("busy") != std::string::npos);
          std::this_thread::yield();
        }
      }
      assert(false && "KVPartition Busy did not clear at facade boundary");
    };
    std::thread t0([&] {
      ebr.thread_init_ebr_meta(0, 0);
      ready.fetch_add(1, std::memory_order_acq_rel);
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      put_with_facade_retry("race-key", "a");
      done.fetch_add(1, std::memory_order_acq_rel);
    });
    std::thread t1([&] {
      ebr.thread_init_ebr_meta(0, 0);
      ready.fetch_add(1, std::memory_order_acq_rel);
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      put_with_facade_retry("race-key", "b");
      done.fetch_add(1, std::memory_order_acq_rel);
    });
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    start.store(true, std::memory_order_release);
    t0.join();
    t1.join();
    assert(done.load(std::memory_order_acquire) == 2);
    std::string raced;
    assert(partition.GetPrivate("race-key", &raced));
    assert(raced == FixedValue("a") || raced == FixedValue("b"));
    bool cas_exchanged = false;
    bool cas_inserted = false;
    assert(partition.CompareExchangePrivate("race-cas", "", "created",
                                            &cas_exchanged, &cas_inserted));
    assert(cas_exchanged && cas_inserted);
    int64_t incr = 0;
    bool incr_inserted = false;
    assert(partition.IncrementPrivate("race-incr", 3, &incr, &incr_inserted));
    assert(incr_inserted && incr == 3);
    assert(partition.IncrementPrivate("race-incr", 2, &incr, &incr_inserted));
    assert(!incr_inserted && incr == 5);
    assert(partition.DeletePrivate("race-key"));
    assert(partition.DeletePrivate("race-cas"));
    assert(partition.DeletePrivate("race-incr"));
  }
  assert(partition.PutPrivate("beta", "two"));
  assert(!partition.PutPrivate("alpha", "updated"));
  assert(regions.IsInOwnerPrivateArena(
      regions.swcc().FromOffset(private_arena->private_root), 5));
  std::string value;
  assert(partition.GetPrivate("alpha", &value) && value == FixedValue("updated"));
  for (uint32_t i = 0; i < 256; ++i) {
    char key[32];
    std::snprintf(key, sizeof(key), "tree-split-%08u", i);
    assert(partition.PutPrivate(key, "tree-value"));
  }
  for (uint32_t i = 0; i < 256; ++i) {
    char key[32];
    std::snprintf(key, sizeof(key), "tree-split-%08u", i);
    assert(partition.GetPrivate(key, &value) && value == FixedValue("tree-value"));
    assert(partition.DeletePrivate(key));
  }
  PassthroughScc scc;
  star::scc_manager = &scc;
  auto &simulator = latency_sim::GlobalLatencySimulator();
  latency_sim::Config tree_write_latency;
  tree_write_latency.enabled = true;
  tree_write_latency.foreground_enabled = true;
  tree_write_latency.stats_enabled = true;
  tree_write_latency.swcc_write_ns_per_line = 1;
  simulator.Configure(tree_write_latency);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  assert(!partition.GetPrivate("tree-lookup-miss", &value));
  assert(simulator.PendingDelayNsForTest() == 0);
  simulator.EndScopeAndDelay();
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  assert(partition.PutPrivate("tree-write", "value"));
  // Certificate-era EndSharedMutation mid-Put settle is gone (§10.6); delay
  // remains pending until the outer scope ends.
  assert(simulator.PendingDelayNsForTest() > 0);
  simulator.EndScopeAndDelay();
  assert(simulator.TakeStatsAndReset().swcc_delayed_ns > 0);
  simulator.Configure(latency_sim::Config{});
  assert(partition.DeletePrivate("tree-write"));
  latency_sim::Config latency;
  latency.enabled = true;
  latency.foreground_enabled = true;
  latency.stats_enabled = true;
  simulator.Configure(latency);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  assert(partition.PutPrivate("latency-only", "payload"));
  assert(partition.GetPrivate("latency-only", &value) && value == FixedValue("payload"));
  std::vector<std::pair<std::string, std::string>> latency_scan;
  assert(partition.ScanLocalPartition("latency-only", 1, &latency_scan));
  assert(latency_scan.size() == 1 && latency_scan[0].second == FixedValue("payload"));
  simulator.EndScopeAndDelay();
  auto latency_stats = simulator.TakeStatsAndReset();
  assert(latency_stats.swcc_raw_line_accesses > 0);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  assert(partition.PromotePrivate("latency-only", 1));
  const uint64_t cached_payload_used = partition.shared_payload_used_bytes();
  assert(partition.MoveOutPrivate("latency-only", 1));
  // Original TwoPLPasha retains the SCC allocation across ordinary move-out;
  // the next move-in must reuse it rather than allocate a second payload.
  assert(partition.shared_payload_used_bytes() == cached_payload_used);
  assert(partition.PromotePrivate("latency-only", 1));
  assert(partition.shared_payload_used_bytes() == cached_payload_used);
  assert(partition.MoveOutPrivate("latency-only", 1));
  simulator.EndScopeAndDelay();
  latency_stats = simulator.TakeStatsAndReset();
  assert(latency_stats.swcc_raw_line_accesses > 0);
  assert(latency_stats.hwcc_raw_line_accesses > 0);
  simulator.Configure(latency_sim::Config{});
  assert(partition.DeletePrivate("latency-only"));
  const uint64_t migration_in_before_alpha =
      regions.layout().partitions[5].migration_in_seq.load();
  assert(partition.PromotePrivate("alpha", 1));
  assert(partition.GetPrivate("alpha", &value) && value == FixedValue("updated"));
  // Once migrated, PUT must update the shared SCC authority rather than the
  // retained private locator row.
  assert(!partition.PutPrivate("alpha", "shared-update"));
  assert(partition.GetPrivate("alpha", &value) && value == FixedValue("shared-update"));
  assert(regions.layout().partitions[5].migration_in_seq.load() ==
         migration_in_before_alpha + 1);
  assert(partition.MoveOutPrivate("alpha", 1));
  assert(partition.GetPrivate("alpha", &value) && value == FixedValue("shared-update"));
  // A shared write marks the cached payload dirty.  Move-in must copy that
  // authoritative value once, while still reusing the same allocation.
  const uint64_t alpha_payload_used = partition.shared_payload_used_bytes();
  assert(partition.PromotePrivate("alpha", 1));
  assert(partition.shared_payload_used_bytes() == alpha_payload_used);
  assert(partition.MoveOutPrivate("alpha", 1));
  assert(partition.GetPrivate("alpha", &value) && value == FixedValue("shared-update"));
  assert(partition.PutPrivate("delete-shared", "value"));
  assert(partition.PromotePrivate("delete-shared", 1));
  assert(partition.DeletePrivate("delete-shared"));
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
  assert(partition.PutPrivate("gamma", "replacement"));
  assert(partition.GetPrivate("gamma", &value) && value == FixedValue("replacement"));
  bool exchanged = false;
  assert(partition.CompareExchangePrivate("gamma", "replacement", "cas-private", &exchanged));
  assert(exchanged);
  assert(partition.CompareExchangePrivate("gamma", "wrong", "ignored", &exchanged));
  assert(!exchanged);
  assert(partition.PromotePrivate("gamma", 1));
  assert(partition.CompareExchangePrivate("gamma", "cas-private", "cas-shared", &exchanged));
  assert(exchanged);
  assert(partition.GetPrivate("gamma", &value) && value == FixedValue("cas-shared"));
  assert(partition.PutPrivate("counter", FixedValue("1")));
  int64_t incremented = 0;
  assert(partition.IncrementPrivate("counter", 2, &incremented) && incremented == 3);
  assert(partition.GetPrivate("counter", &value) && value == FixedValue("3"));
  assert(partition.IncrementPrivate("new-counter", -2, &incremented) && incremented == -2);
  assert(partition.GetPrivate("new-counter", &value) && value == FixedValue("-2"));
  assert(partition.CompareExchangePrivate("new-cas", "", "created", &exchanged));
  assert(exchanged && partition.GetPrivate("new-cas", &value) && value == FixedValue("created"));

  // Fresh move-in starts without a second chance; the first over-budget Clock
  // pass may therefore move it out immediately.  Only a real shared access
  // grants a chance.
  assert(partition.PutPrivate("clock", "victim"));
  assert(partition.PromotePrivate("clock", 1));
  star::cxl_memory.set_total_hw_cc_usage(
      (1024ULL * 1024ULL * 1024ULL - star::CXL_EBR::max_ebr_retiring_memory) / 2);
  assert(star::migration_manager != nullptr);
  assert(star::migration_manager->move_row_out(partition.partition_id()));
  assert(partition.GetPrivate("clock", &value) && value == FixedValue("victim"));
  latency_sim::Config budget_counter_latency;
  budget_counter_latency.enabled = true;
  budget_counter_latency.foreground_enabled = true;
  budget_counter_latency.stats_enabled = true;
  simulator.Configure(budget_counter_latency);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  assert(partition.hwcc_used_bytes() > 0);
  assert(partition.shared_payload_used_bytes() <
         partition.shared_payload_capacity_bytes());
  simulator.EndScopeAndDelay();
  latency_stats = simulator.TakeStatsAndReset();
  assert(latency_stats.swcc_raw_line_accesses > 0);
  simulator.Configure(latency_sim::Config{});

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
  assert(partition.GetPrivate("pinned", &value) && value == FixedValue("hold"));

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

  // DELETE under a range/point pin returns Busy (no 5s spin under Clock lock
  // §10.2b); callers retry until the pin is released.
  assert(partition.PutPrivate("delete-pinned", "present"));
  assert(partition.PromotePrivate("delete-pinned", 1));
  star::TwoPLPashaMetadataShared *delete_pin = nullptr;
  assert(!partition.PromotePrivate("delete-pinned", 1, &delete_pin));
  assert(delete_pin != nullptr);
  std::thread release_delete_pin([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    star::TwoPLPashaHelper::kv_unpin_shared_ref(delete_pin);
  });
  bool deleted = false;
  bool saw_busy = false;
  for (int attempt = 0; attempt < 10000; ++attempt) {
    try {
      deleted = partition.DeletePrivate("delete-pinned");
      break;
    } catch (const std::runtime_error &error) {
      assert(std::string(error.what()).find("busy") != std::string::npos);
      saw_busy = true;
      std::this_thread::yield();
    }
  }
  assert(saw_busy);
  assert(deleted);
  release_delete_pin.join();
  assert(!partition.GetPrivate("delete-pinned", &value));

  // A partition scan merges the private and shared authorities in key order,
  // without resurrecting tombstones or duplicate migrated locator rows.
  std::vector<std::pair<std::string, std::string>> scan;
  assert(partition.ScanLocalPartition("alpha", 0, &scan));
  assert(scan.size() == 8);
  assert(scan[0] == std::make_pair(std::string("alpha"), FixedValue("shared-update")));
  assert(scan[1] == std::make_pair(std::string("clock"), FixedValue("victim")));
  assert(scan[2] == std::make_pair(std::string("counter"), FixedValue("3")));
  assert(scan[3] == std::make_pair(std::string("gamma"), FixedValue("cas-shared")));
  assert(scan[4] == std::make_pair(std::string("new-cas"), FixedValue("created")));
  assert(scan[5] == std::make_pair(std::string("new-counter"), FixedValue("-2")));
  assert(scan[6] == std::make_pair(std::string("pinfail"), FixedValue("x")));
  assert(scan[7] == std::make_pair(std::string("pinned"), FixedValue("hold")));
  assert(partition.ScanLocalPartition("alpha", 2, &scan));
  assert(scan.size() == 2);
  assert(scan[0].first == "alpha" && scan[1].first == "clock");

  // Remote scan consumes only CXL rows.  Move the two result rows plus the
  // original right boundary, then hold all row locks/ref pins until the
  // fragment has copied its values.
  for (const auto &key : {"alpha", "clock", "counter"})
    assert(partition.EnsureInShared(key, 1) == tigonkv::StatusCode::kOk);
  const std::string scan_max(regions.layout().fixed_key_size,
                             static_cast<char>(0xff));
  const auto shared_scan = partition.ScanSharedPartition(
      /*host_id=*/1, "alpha", 2, scan_max);
  assert(shared_scan.status.ok() && shared_scan.scan_success);
  assert(shared_scan.items.size() == 2);
  assert(shared_scan.items[0] == scan[0]);
  assert(shared_scan.items[1] == scan[1]);
  // §4.3: ScanSharedForUpdate visits shared keys in order with is_last_tuple.
  {
    std::vector<tigonkv::engine::FixedKey> visited;
    size_t callbacks = 0;
    const auto start =
        tigonkv::engine::FixedKey::From("alpha", regions.layout().fixed_key_size);
    partition.ScanSharedForUpdate(
        start,
        [&](const tigonkv::engine::FixedKey &key, tigonkv::engine::RegionOffset off,
            bool /*is_last*/) {
          ++callbacks;
          assert(off != tigonkv::engine::kNullOffset);
          visited.push_back(key);
          return visited.size() >= 2;  // stop after two keys
        });
    assert(callbacks == 2);
    assert(visited.size() == 2);
    assert(visited[0].Compare(start) == 0);
    size_t full = 0;
    bool full_last = false;
    tigonkv::engine::FixedKey zero{};
    partition.ScanSharedForUpdate(
        zero,
        [&](const tigonkv::engine::FixedKey &, tigonkv::engine::RegionOffset,
            bool is_last) {
          ++full;
          if (is_last) full_last = true;
          return false;
        });
    assert(full >= 2);
    assert(full_last);
  }
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
  assert(partition.ScanLocalPartition("m1", 3, &scan));
  assert(scan.size() == 3);
  assert(scan[0] == std::make_pair(std::string("m1"), FixedValue("shared-m1")));
  assert(scan[1] == std::make_pair(std::string("m2"), FixedValue("priv-m2")));
  assert(scan[2] == std::make_pair(std::string("m3"), FixedValue("priv-m3")));

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
    migration_done.store(true, std::memory_order_release);
  });
  uint32_t scan_rounds = 0;
  do {
    // KVPartition is the one-attempt primitive; model the facade's logical
    // Busy retry here instead of restoring an internal Scan retry budget.
    bool scanned = false;
    for (uint32_t attempt = 0; attempt < 1024 && !scanned; ++attempt) {
      scanned = partition.ScanLocalPartition("scan-race-", 32, &scan);
      if (!scanned) std::this_thread::yield();
    }
    assert(scanned);
    assert(scan.size() == moving_keys.size());
    for (size_t i = 0; i < scan.size(); ++i) {
      assert(scan[i].first == moving_keys[i]);
      assert(scan[i].second == FixedValue("stable"));
      if (i != 0) assert(scan[i - 1].first < scan[i].first);
    }
    ++scan_rounds;
  } while (!migration_done.load(std::memory_order_acquire) ||
           scan_rounds < 32);
  migrator.join();

  // The owner create path keeps the original next-row write lock across
  // placeholder insertion and publication.  Make that successor migrated so
  // this covers the offset adapter's shared-row acquire/release branch.
  assert(partition.PutPrivate("owner-next-middle", "middle"));
  assert(partition.PromotePrivate("owner-next-middle", 1));
  assert(partition.PutPrivate("owner-next-before", "before"));
  assert(partition.GetPrivate("owner-next-before", &value) &&
         value == FixedValue("before"));
  assert(partition.GetPrivate("owner-next-middle", &value) &&
         value == FixedValue("middle"));

  star::scc_manager = nullptr;

  regions.PublishOwnerInitialized(0);
  regions.PublishOwnerInitialized(1);
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
    tigonkv::engine::KVPartition attached(attached_regions, attached_ebr, 5, 1, true, true);
    std::string child_value;
    if (!attached.GetPrivate("alpha", &child_value) ||
        child_value != FixedValue("shared-update")) _exit(1);
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
