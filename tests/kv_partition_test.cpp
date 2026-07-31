#include "kv/engine/kv_migration.h"
#include "kv/engine/kv_partition.h"
#include "kv/engine/kv_worker_context.h"
#include "protocol/TwoPLPasha/TwoPLPashaMessage.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

class PassthroughScc final : public star::SCCManager {
 public:
  void init_scc_metadata(void *meta, std::size_t host) override {
    auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(meta);
    smeta->clear_all_scc_bits();
    smeta->set_scc_bit(host);
  }
  void do_read(void *, std::size_t, void *dst, const void *src,
               uint64_t bytes) override {
    std::memcpy(dst, src, bytes);
  }
  void do_write(void *, std::size_t, void *dst, const void *src,
                uint64_t bytes) override {
    std::memcpy(dst, src, bytes);
  }
};

tigonkv::engine::DualRegionConfig Config() {
  tigonkv::engine::DualRegionConfig config;
  config.total_pool_bytes = 64 * 1024 * 1024;
  config.hwcc_size_bytes = 32 * 1024 * 1024;
  config.swcc_offset_bytes = config.hwcc_size_bytes;
  config.swcc_size_bytes = config.total_pool_bytes - config.swcc_offset_bytes;
  config.config_hash = 0x9911;
  config.vm_count = 2;
  config.partition_count = 8;
  config.fixed_key_size = 32;
  config.fixed_value_size = 32;
  config.owner_private_swcc_fraction = 0.8;
  return config;
}

std::string Fixed32(std::string_view text) {
  assert(text.size() <= 32);
  std::string value(32, '\0');
  std::memcpy(value.data(), text.data(), text.size());
  return value;
}

}  // namespace

int main(int argc, char **argv) {
  const bool focused_e = argc == 2 && std::string_view(argv[1]) == "--focused-stage=E";
  const bool focused_f = argc == 2 && std::string_view(argv[1]) == "--focused-stage=F";
  const bool focused_h = argc == 2 && std::string_view(argv[1]) == "--focused-stage=H";
  const bool focused_i = argc == 2 && std::string_view(argv[1]) == "--focused-stage=I";
  const bool focused_j = argc == 2 && std::string_view(argv[1]) == "--focused-stage=J";
  const bool focused_k = argc == 2 && std::string_view(argv[1]) == "--focused-stage=K";
  if (argc != 1) {
    if (argc != 2 ||
        (std::string_view(argv[1]) != "--focused-stage=E" &&
         std::string_view(argv[1]) != "--focused-stage=F" &&
         std::string_view(argv[1]) != "--focused-stage=H" &&
         std::string_view(argv[1]) != "--focused-stage=I" &&
         std::string_view(argv[1]) != "--focused-stage=J" &&
         std::string_view(argv[1]) != "--focused-stage=K")) {
      std::fprintf(stderr,
                   "usage: kv_partition_test "
                   "[--focused-stage=E|F|H|I|J|K]\n");
      return 2;
    }
    if (!focused_e && !focused_f && !focused_h && !focused_i && !focused_j &&
        !focused_k) {
      std::fprintf(stderr, "focused stage is not implemented yet: %s\n", argv[1]);
      return 2;
    }
  }
  uint64_t worker_max_tid = 0;
  tigonkv::engine::ScopedKvWorkerTidBinding tid_binding(&worker_max_tid);
  char path_template[] = "/tmp/tigonkv-partition-XXXXXX";
  const int fd = mkstemp(path_template);
  assert(fd >= 0);
  close(fd);
  const std::string path(path_template);

  auto pool = tigonkv::engine::DualRegionMappedPool::Open(path, Config(), true);
  auto &regions = pool.allocator();
  regions.FinalizeStaticHwccLayout();
  regions.PublishStaticHwccLayout();
  regions.InitializeOwnerPrivateArenas(0);
  regions.InitializeOwnerPrivateArenas(1);
  star::CXLMemory memory;
  star::CXLMemory::bind_dual_region_allocator(&regions, 1);
  star::CXL_EBR ebr(2, 1, &regions);
  ebr.thread_init_ebr_meta(0, 0);
  star::global_ebr_meta = &ebr;
  PassthroughScc scc;
  star::scc_manager = &scc;

  tigonkv::engine::KVPartition partition(regions, ebr, 5, 1, false, true);
  std::vector<tigonkv::engine::KVPartition *> partitions(8, nullptr);
  partitions[5] = &partition;
  tigonkv::engine::KvMigrationRuntime::Instance().Install(
      partitions, 32, 32, 0, 8, 8 * 1024 * 1024);

  // This focused fixture covers only the owner-private master TableBTreeOLC
  // binding and its fixed-32B callback contract. Public KV semantics are
  // validated by the isolated 4VM facade E2E workflow.
  const std::string key = Fixed32("alpha");
  const std::string value = Fixed32("one");
  assert(partition.PutPrivate(key, value) == star::RowOutcome::kDone);
  auto *table = tigonkv::engine::KvMigrationRuntime::Instance().TableFor(5);
  assert(table != nullptr && table->key_size() == 32 && table->value_size() == 32);

  if (focused_e) {
    // Leave one 64-byte allocation in the free list, then exhaust the bump
    // region. AllocateAndConstruct must consume that slot for lmeta, fail on
    // the following ValueStruct allocation, and return the unpublished lmeta
    // directly to the same allocator.
    void *metadata_slot = regions.AllocateOwnerPrivate(
        sizeof(tigonkv::engine::PrivateMetadataLocal), 5, 1);
    std::vector<std::pair<void *, uint64_t>> held;
    const auto exhaust = [&](uint64_t bytes) {
      for (;;) {
        try {
          held.emplace_back(regions.AllocateOwnerPrivate(bytes, 5, 1), bytes);
        } catch (const std::bad_alloc &) {
          return;
        }
      }
    };
    exhaust(1024);
    exhaust(64);
    regions.FreeOwnerPrivate(
        metadata_slot, sizeof(tigonkv::engine::PrivateMetadataLocal), 5, 1);
    const uint64_t used_before_failure = regions.OwnerPrivateUsedBytes(1);
    const auto oom_key = tigonkv::engine::FixedKey::From(Fixed32("e2-oom"), 32);
    bool second_allocation_failed = false;
    try {
      (void)table->insert(&oom_key, Fixed32("oom").data(), false);
    } catch (const std::bad_alloc &) {
      second_allocation_failed = true;
    }
    assert(second_allocation_failed);
    assert(regions.OwnerPrivateUsedBytes(1) == used_before_failure);
    auto [oom_meta, oom_data] = table->search(&oom_key);
    assert(oom_meta == nullptr && oom_data == nullptr);
    for (auto it = held.rbegin(); it != held.rend(); ++it)
      regions.FreeOwnerPrivate(it->first, it->second, 5, 1);
    assert(table->insert(&oom_key, Fixed32("oom").data(), false));
  }

  const auto fixed_key = tigonkv::engine::FixedKey::From(key, 32);
  auto [meta, data] = table->search(&fixed_key);
  assert(meta != nullptr && data != nullptr);
  assert(std::string(static_cast<char *>(data), 32) == value);

  if (focused_h) {
    // The final owner table has exactly the original all-0xff terminal
    // tuple. It is physical tree state, never a public KV result.
    const auto sentinel = tigonkv::engine::FixedKey::InternalMax(32);
    auto [sentinel_meta, sentinel_data] = table->search(&sentinel);
    assert(sentinel_meta != nullptr && sentinel_data != nullptr);

    bool moved_in = false;
    assert(partition.EnsureInShared(key, 0, &moved_in) ==
           tigonkv::StatusCode::kOk);
    assert(moved_in);
    std::string shared;
    assert(partition.GetShared(key, 0, &shared) ==
           tigonkv::engine::SharedAccessState::kDone);
    assert(shared == value);
    std::string missing;
    assert(partition.GetShared(Fixed32("stable-miss"), 0, &missing) ==
           tigonkv::engine::SharedAccessState::kMissing);
  }

  if (focused_i) {
    const std::string cas_key = Fixed32("cas-key");
    assert(partition.PutPrivate(cas_key, value) == star::RowOutcome::kDone);
    bool exchanged = false;
    assert(partition.CompareExchangePrivate(
               cas_key, value, Fixed32("cas"), &exchanged) ==
           star::RowOutcome::kDone);
    assert(exchanged);
    assert(partition.CompareExchangePrivate(
               cas_key, value, Fixed32("wrong"), &exchanged) ==
           star::RowOutcome::kDone);
    assert(!exchanged);

    const std::string counter = Fixed32("counter");
    assert(partition.PutPrivate(counter, Fixed32("1")) ==
           star::RowOutcome::kDone);
    bool moved_counter = false;
    assert(partition.EnsureInShared(counter, 0, &moved_counter) ==
           tigonkv::StatusCode::kOk);
    assert(moved_counter);
    int64_t incremented = 0;
    assert(partition.IncrementPrivate(counter, 2, &incremented) ==
           star::RowOutcome::kDone);
    assert(incremented == 3);

    // The original remote-insert move-in reaches PolicyClock::track with the
    // actual ITable row. A null synthetic row would publish first, then throw
    // while building the tracker and incorrectly attempt rollback of that
    // published placeholder.
    const std::string remote_insert_key = Fixed32("remote-insert");
    assert(partition.InsertRemotePlaceholder(remote_insert_key, Fixed32("remote"),
                                             0) == tigonkv::StatusCode::kOk);
    assert(partition.PublishRemotePlaceholder(remote_insert_key, 0));
    std::string remote_insert_value;
    assert(partition.GetPrivate(remote_insert_key, &remote_insert_value) ==
           star::RowOutcome::kDone);
    assert(remote_insert_value == Fixed32("remote"));

    // Concurrent DATA_MIGRATION may move the invalid placeholder first. Create
    // the placeholder half, EnsureInShared, then finish PromotePrivate+publish
    // the same way InsertRemotePlaceholder does when it sees ALREADY_IN_CXL.
    const std::string race_key = Fixed32("race-insert");
    {
      auto *table = partition.private_table();
      assert(table != nullptr);
      const auto race_fixed = tigonkv::engine::FixedKey::From(race_key, 32);
      const std::string race_value = Fixed32("race");
      // Third argument true = placeholder (is_valid=false), matching remote insert.
      assert(table->insert(&race_fixed, race_value.data(), true));
      bool moved = false;
      assert(partition.EnsureInShared(race_key, 0, &moved) ==
             tigonkv::StatusCode::kOk);
      assert(moved);
      star::TwoPLPashaMetadataShared *pinned = nullptr;
      const auto already =
          partition.PromotePrivate(race_key, /*host_id=*/0, &pinned);
      assert(already == star::migration_result::FAIL_ALREADY_IN_CXL);
      assert(pinned != nullptr);
      assert(partition.PublishRemotePlaceholder(race_key, 0));
      std::string got;
      assert(partition.GetPrivate(race_key, &got) == star::RowOutcome::kDone);
      assert(got == race_value);
    }
  }

  if (focused_f) {
    bool moved_in = false;
    assert(partition.EnsureInShared(key, 0, &moved_in) ==
           tigonkv::StatusCode::kOk);
    assert(moved_in);
    assert(partition.DeletePrivate(key) == star::RowOutcome::kDone);
    std::string deleted_value;
    assert(partition.GetPrivate(key, &deleted_value) ==
           star::RowOutcome::kMissing);
    assert(partition.PutPrivate(key, value) == star::RowOutcome::kDone);
  }

  if (focused_j) {
    const std::string delete_key = Fixed32("j-delete");
    assert(partition.PutPrivate(delete_key, value) == star::RowOutcome::kDone);
    bool moved_in = false;
    assert(partition.EnsureInShared(delete_key, 0, &moved_in) ==
           tigonkv::StatusCode::kOk);
    assert(moved_in);
    assert(partition.DeletePrivate(delete_key) == star::RowOutcome::kDone);
    std::string deleted_value;
    assert(partition.GetPrivate(delete_key, &deleted_value) ==
           star::RowOutcome::kMissing);
    assert(partition.PutPrivate(delete_key, value) == star::RowOutcome::kDone);
  }

  if (focused_k) {
    const std::string scan_a = Fixed32("k-a");
    const std::string scan_b = Fixed32("k-b");
    const std::string scan_c = Fixed32("k-c");
    assert(partition.PutPrivate(scan_a, Fixed32("value-a")) ==
           star::RowOutcome::kDone);
    assert(partition.PutPrivate(scan_b, Fixed32("value-b")) ==
           star::RowOutcome::kDone);
    assert(partition.PutPrivate(scan_c, Fixed32("value-c")) ==
           star::RowOutcome::kDone);
    std::vector<std::pair<std::string, std::string>> local_scan;
    assert(partition.ScanLocalPartition(scan_a, 2, &local_scan,
                                        Fixed32("k-z")));
    assert(local_scan.size() == 2);
    assert(local_scan[0].first == scan_a && local_scan[0].second == Fixed32("value-a"));
    assert(local_scan[1].first == scan_b && local_scan[1].second == Fixed32("value-b"));

    bool moved_b = false;
    bool moved_c = false;
    assert(partition.EnsureInShared(scan_b, 0, &moved_b) ==
           tigonkv::StatusCode::kOk);
    assert(moved_b);

    // The first lower-bound row has neither migrated neighbour before the
    // owner has supplied its right boundary, so it must request move-in.
    const std::string missing_between = Fixed32(std::string("k-a\x7f", 4));
    auto missing_successor = partition.ScanSharedPartition(
        0, missing_between, 1, Fixed32("k-z"));
    assert(missing_successor.migration_required);

    assert(partition.EnsureInShared(scan_c, 0, &moved_c) ==
           tigonkv::StatusCode::kOk);
    assert(moved_c);
    auto shared_scan = partition.ScanSharedPartition(
        0, scan_b, 1, Fixed32("k-z"));
    assert(shared_scan.status.ok() && shared_scan.scan_success);
    assert(shared_scan.items.size() == 1);
    assert(shared_scan.items[0].first == scan_b &&
           shared_scan.items[0].second == Fixed32("value-b"));

    // Reproduce the post-range-move-in lower-bound shape: the first shared
    // row has no migrated predecessor, but it has its migrated successor.
    // The request begins strictly between a and b, so b must be accepted as
    // the fragment's lower-bound left boundary without a resolved-min state.
    // K1 applies only on this post-move-in probe shape (production re-probe).
    auto lower_bound_scan = partition.ScanSharedPartition(
        0, missing_between, 1, Fixed32("k-z"),
        /*allow_lower_bound_left_boundary=*/true);
    assert(lower_bound_scan.status.ok() && lower_bound_scan.scan_success);
    assert(lower_bound_scan.items.size() == 1);
    assert(lower_bound_scan.items[0].first == scan_b &&
           lower_bound_scan.items[0].second == Fixed32("value-b"));

    // First probe stays on master's exact-min rule: the same lower-bound
    // shape without K1 must still request move-in (prev_real missing).
    auto first_probe_strict = partition.ScanSharedPartition(
        0, missing_between, 1, Fixed32("k-z"),
        /*allow_lower_bound_left_boundary=*/false);
    assert(first_probe_strict.migration_required);

    // The physical internal-max tuple is the original right boundary.  It
    // remains subject to the unchanged adjacency predicate, so its move-in
    // must publish the terminal next_real bit rather than causing the same
    // unbounded range to be requested again.
    const auto sentinel = tigonkv::engine::FixedKey::InternalMax(32);
    const std::string sentinel_key(sentinel.bytes, 32);
    bool moved_sentinel = false;
    assert(partition.EnsureInShared(sentinel_key, 0, &moved_sentinel) ==
           tigonkv::StatusCode::kOk);
    assert(moved_sentinel);
    auto terminal_scan = partition.ScanSharedPartition(
        0, scan_b, 0, sentinel_key);
    assert(terminal_scan.status.ok() && terminal_scan.scan_success);
    assert(terminal_scan.items.size() == 2);
    assert(terminal_scan.items[0].first == scan_b);
    assert(terminal_scan.items[1].first == scan_c);

    // Production path: move_in_scan_range then re-probe. A short first migration
    // leaves an island edge (next_real=false); the next larger range move-in
    // must repair it so the second probe succeeds — matching the 4VM Busy shape
    // GDB observed (middle row prev=1 next=0).
    {
      std::vector<std::string> island_keys;
      for (int i = 0; i < 12; ++i) {
        std::string k = Fixed32("k-i" + std::to_string(i));
        assert(partition.PutPrivate(k, Fixed32("v")) == star::RowOutcome::kDone);
        island_keys.push_back(std::move(k));
      }
      std::sort(island_keys.begin(), island_keys.end());
      const std::string island_max = Fixed32("k-z");
      const auto min_fk =
          tigonkv::engine::FixedKey::From(island_keys.front(), 32);
      const auto max_fk = tigonkv::engine::FixedKey::From(island_max, 32);
      star::TwoPLPashaMessageHandler::move_in_scan_range(
          *table, min_fk.bytes, max_fk.bytes, /*limit=*/2);
      auto edge = partition.ScanSharedPartition(0, island_keys.front(), 5, island_max);
      assert(edge.migration_required);
      star::TwoPLPashaMessageHandler::move_in_scan_range(
          *table, min_fk.bytes, max_fk.bytes, /*limit=*/5);
      auto repaired = partition.ScanSharedPartition(0, island_keys.front(), 5, island_max);
      assert(repaired.status.ok() && repaired.scan_success);
      assert(repaired.items.size() == 5);

      // Cold long range: one move_in_scan_range(limit=56) must leave a probeable
      // contiguous shared fragment — the 4VM failure is middle-row next_real=0
      // after move-in, so this is the sequential shape of that repair.
      std::vector<std::string> long_keys;
      for (int i = 0; i < 80; ++i) {
        std::string k = Fixed32("k-l" + std::to_string(i));
        assert(partition.PutPrivate(k, Fixed32("v")) == star::RowOutcome::kDone);
        long_keys.push_back(std::move(k));
      }
      std::sort(long_keys.begin(), long_keys.end());
      const auto long_min = tigonkv::engine::FixedKey::From(long_keys.front(), 32);
      const auto long_max = tigonkv::engine::FixedKey::From(island_max, 32);
      star::TwoPLPashaMessageHandler::move_in_scan_range(
          *table, long_min.bytes, long_max.bytes, /*limit=*/56);
      auto long_scan = partition.ScanSharedPartition(
          0, long_keys.front(), 56, island_max);
      assert(long_scan.status.ok() && long_scan.scan_success);
      assert(long_scan.items.size() == 56);

      // Gap-before-island livelock shape (4VM Busy root cause): a migrated
      // island sits far past scan min. K1 on the first probe would accept the
      // island, fail at its trailing next_real=0, then move_in(min, limit)
      // only fills the private gap and never reaches the hole — permanent
      // Busy. Production keeps the first probe master-strict, move-in fills
      // from min, and only the re-probe uses K1.
      {
        std::vector<std::string> gap_keys;
        for (int i = 0; i < 40; ++i) {
          char buf[16];
          std::snprintf(buf, sizeof(buf), "k-g%02d", i);
          std::string k = Fixed32(buf);
          assert(partition.PutPrivate(k, Fixed32("v")) ==
                 star::RowOutcome::kDone);
          gap_keys.push_back(std::move(k));
        }
        std::sort(gap_keys.begin(), gap_keys.end());
        const std::string gap_max = Fixed32("k-z");
        // Migrate only a far island (keys 20..34), leaving [0,20) private.
        for (int i = 20; i < 35; ++i) {
          bool moved = false;
          assert(partition.EnsureInShared(gap_keys[i], 0, &moved) ==
                 tigonkv::StatusCode::kOk);
          assert(moved);
        }
        const std::string gap_start = gap_keys.front();
        // limit past the island length so K1-always reaches the trailing
        // next_real=0 edge (the 4VM Busy shape).
        auto k1_first = partition.ScanSharedPartition(
            0, gap_start, 16, gap_max,
            /*allow_lower_bound_left_boundary=*/true);
        assert(k1_first.migration_required);

        auto strict_first = partition.ScanSharedPartition(
            0, gap_start, 10, gap_max,
            /*allow_lower_bound_left_boundary=*/false);
        assert(strict_first.migration_required);

        const auto gap_min_fk =
            tigonkv::engine::FixedKey::From(gap_start, 32);
        const auto gap_max_fk =
            tigonkv::engine::FixedKey::From(gap_max, 32);
        star::TwoPLPashaMessageHandler::move_in_scan_range(
            *table, gap_min_fk.bytes, gap_max_fk.bytes, /*limit=*/10);
        // Same move-in cannot repair the distant island edge: K1 re-probe
        // with the large limit would still see next_real=0 there.
        auto k1_after_short_move = partition.ScanSharedPartition(
            0, gap_start, 16, gap_max,
            /*allow_lower_bound_left_boundary=*/true);
        assert(k1_after_short_move.migration_required);

        // Production pair: strict first → move_in(min, limit) → K1 re-probe
        // with the same limit reads the newly contiguous prefix from min.
        auto after_move = partition.ScanSharedPartition(
            0, gap_start, 10, gap_max,
            /*allow_lower_bound_left_boundary=*/true);
        assert(after_move.status.ok() && after_move.scan_success);
        assert(after_move.items.size() == 10);
        for (int i = 0; i < 10; ++i)
          assert(after_move.items[i].first == gap_keys[i]);
      }
    }

    std::vector<std::pair<std::string, std::string>> migrated_local;
    assert(partition.ScanLocalPartition(scan_b, 1, &migrated_local,
                                        Fixed32("k-z")));
    assert(migrated_local.size() == 1 && migrated_local[0].second == Fixed32("value-b"));
  }

  // A duplicate must release its unlinked offset row through the policy and
  // must not replace the original tree value.
  assert(!table->insert(&fixed_key, value.data(), false));

  bool adjacent_callback = false;
  assert(table->search_and_update_next_key_info(
      &fixed_key, [&](const void *, void *, void *, const void *current_key,
                      void *current_meta, void *current_data, const void *,
                      void *, void *) {
        assert(current_key != nullptr && current_meta != nullptr &&
               current_data != nullptr);
        assert(std::memcmp(current_data, value.data(), value.size()) == 0);
        adjacent_callback = true;
      }));
  assert(adjacent_callback);

  const std::string trailing_zero_key =
      Fixed32(std::string("tail", 4) + '\0');
  assert(partition.PutPrivate(trailing_zero_key, Fixed32("tail")) ==
         star::RowOutcome::kDone);
  std::vector<std::pair<std::string, std::string>> scan;
  assert(partition.ScanLocalPartition(trailing_zero_key, 1, &scan));
  assert(scan.size() == 1 && scan.front().first == Fixed32(trailing_zero_key));

  star::scc_manager = nullptr;
  star::global_ebr_meta = nullptr;
  tigonkv::engine::KvMigrationRuntime::Instance().Reset();
  unlink(path.c_str());
  return 0;
}
