#include "common/CXLMemory.h"
#include "kv/engine/fixed_value.h"
#include "protocol/TwoPLPasha/TwoPLPashaHelper.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <charconv>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>
#include <sys/mman.h>

namespace {

uint64_t TestWorkerMaxTid = 0;

class RecordingScc final : public star::SCCManager {
 public:
  void init_scc_metadata(void *meta, std::size_t host) override {
    static_cast<star::TwoPLPashaMetadataShared *>(meta)->clear_all_scc_bits();
    static_cast<star::TwoPLPashaMetadataShared *>(meta)->set_scc_bit(host);
  }
  void do_read(void *, std::size_t, void *dst, const void *src, uint64_t bytes) override {
    ++reads; std::memcpy(dst, src, bytes);
  }
  void do_write(void *, std::size_t, void *dst, const void *src, uint64_t bytes) override {
    ++writes; std::memcpy(dst, src, bytes);
  }
  void prepare_read(void *, std::size_t, void *, uint64_t bytes) override {
    last_prepare_bytes.store(bytes, std::memory_order_relaxed);
    ++prepares;
  }
  void finish_write(void *meta, std::size_t host, void *, uint64_t bytes) override {
    last_finish_bytes.store(bytes, std::memory_order_relaxed);
    ++finishes;
    // Mirror WriteThrough: keep writer SCC bit under the caller's latch.
    auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(meta);
    if ((smeta->load_atomic_word(std::memory_order_relaxed) &
         (star::TwoPLPashaMetadataShared::LATCH_BIT_MASK
          << star::TwoPLPashaMetadataShared::LATCH_BIT_OFFSET)) == 0)
      finish_without_latch.store(true, std::memory_order_relaxed);
    smeta->clear_all_scc_bits();
    smeta->set_scc_bit(host);
  }
  std::atomic<uint64_t> reads{0}, writes{0}, prepares{0}, finishes{0};
  std::atomic<uint64_t> last_prepare_bytes{0}, last_finish_bytes{0};
  std::atomic<bool> finish_without_latch{false};
};

tigonkv::engine::DualRegionConfig Config(size_t bytes) {
  tigonkv::engine::DualRegionConfig c;
  c.total_pool_bytes = bytes; c.hwcc_size_bytes = 32 * 1024 * 1024;
  c.swcc_offset_bytes = c.hwcc_size_bytes; c.swcc_size_bytes = bytes - c.swcc_offset_bytes;
  c.config_hash = 0x8811; c.vm_count = 2; c.partition_count = 8;
  c.fixed_key_size = 32; c.fixed_value_size = 128; return c;
}

// Test-only spelling of the original single-row sequence.  These helpers are
// deliberately not part of TwoPLPashaHelper: production must invoke the
// original primitives at its own lock-lifetime boundary.
bool WriteViaOriginalPrimitives(star::TwoPLPashaMetadataShared *smeta,
                                std::size_t host, const void *src,
                                std::size_t bytes) {
  bool locked = false;
  const uint64_t observed =
      star::TwoPLPashaHelper::remote_take_write_lock_and_read(
          smeta, host, nullptr, bytes, /*inc_ref_cnt=*/true, locked,
          nullptr, /*allow_invalid=*/true);
  if (!locked) return false;
  if (star::TwoPLPashaHelper::remote_write_lock_update_and_release(
          smeta, host, src, bytes,
          star::TwoPLPashaHelper::kv_next_commit_tid(observed, TestWorkerMaxTid),
          /*dec_ref_cnt=*/true, /*allow_invalid=*/true))
    return true;
  star::TwoPLPashaHelper::remote_write_lock_abort(
      smeta, /*dec_ref_cnt=*/true);
  return false;
}

bool ReadViaOriginalPrimitives(
    star::TwoPLPashaMetadataShared *smeta, std::size_t host, void *dest,
    std::size_t bytes,
    star::RowOutcome *result = nullptr) {
  bool locked = false;
  (void)star::TwoPLPashaHelper::remote_take_read_lock_and_read(
      smeta, host, dest, bytes, /*inc_ref_cnt=*/true, locked, result);
  if (!locked) return false;
  star::TwoPLPashaHelper::remote_read_lock_release(
      smeta, /*dec_ref_cnt=*/true);
  return true;
}

bool IncrementViaOriginalPrimitives(star::TwoPLPashaMetadataShared *smeta,
                                    std::size_t host) {
  std::string current(16, '\0');
  bool locked = false;
  const uint64_t observed =
      star::TwoPLPashaHelper::remote_take_write_lock_and_read(
          smeta, host, current.data(), current.size(), /*inc_ref_cnt=*/true,
          locked);
  if (!locked) return false;
  int64_t value = 0;
  assert(tigonkv::engine::DecodeCanonicalFixedDecimal(current, &value));
  std::string replacement;
  assert(tigonkv::engine::EncodeCanonicalFixedDecimal(
      value + 1, current.size(), &replacement));
  if (star::TwoPLPashaHelper::remote_write_lock_update_and_release(
          smeta, host, replacement.data(), replacement.size(),
          star::TwoPLPashaHelper::kv_next_commit_tid(observed, TestWorkerMaxTid),
          /*dec_ref_cnt=*/true))
    return true;
  star::TwoPLPashaHelper::remote_write_lock_abort(
      smeta, /*dec_ref_cnt=*/true);
  return false;
}

}  // namespace

int main() {
  static_assert(offsetof(star::TwoPLPashaMetadataLocal, latch) ==
                offsetof(star::TwoPLPashaMetadataLocalOffset, latch));
  static_assert(sizeof(star::TwoPLPashaMetadataLocal) ==
                sizeof(star::TwoPLPashaMetadataLocalOffset));
  star::TwoPLPashaMetadataLocal legacy_local;
  star::TwoPLPashaMetadataLocalOffset offset_local;
  legacy_local.tid = 7;
  legacy_local.is_valid = true;
  legacy_local.is_migrated = true;
  legacy_local.is_data_modified_since_moved_out = false;
  legacy_local.lock();
  legacy_local.unlock();
  offset_local.tid = legacy_local.tid;
  offset_local.is_valid = legacy_local.is_valid;
  offset_local.is_migrated = legacy_local.is_migrated;
  offset_local.is_data_modified_since_moved_out =
      legacy_local.is_data_modified_since_moved_out;
  offset_local.lock();
  offset_local.unlock();
  assert(offset_local.tid == 7 && offset_local.is_valid &&
         offset_local.is_migrated &&
         !offset_local.is_data_modified_since_moved_out);
  static_assert(offsetof(star::TwoPLPashaSharedDataSCC, data) == 34);
  static_assert(sizeof(star::TwoPLPashaSharedDataSCC) == 40);
  static_assert(offsetof(star::TwoPLPashaMetadataShared, ref_cnt) == 8);
  static_assert(sizeof(star::TwoPLPashaMetadataShared) == 16);
  constexpr size_t bytes = 64 * 1024 * 1024;
  void *pool = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(pool != MAP_FAILED);
  auto regions = tigonkv::engine::DualRegionAllocator::Initialize(pool, Config(bytes));
  regions.FinalizeStaticHwccLayout();
  regions.PublishStaticHwccLayout();
  regions.InitializeOwnerPrivateArenas(0);
  star::CXLMemory::bind_dual_region_allocator(&regions, 0);
  latency_sim::Config latency;
  latency.fixed_latency.enabled = true;
  latency.fixed_latency.foreground_enabled = true;
  latency.fixed_latency.swcc_fixed_ns_per_line = 10;
  latency.fixed_latency.hwcc_fixed_ns_per_line = 10;
  auto &simulator = latency_sim::GlobalLatencySimulator();
  const auto region_config = Config(bytes);
  simulator.RegisterPool(
      latency_sim::PoolKind::kHwcc,
      static_cast<const std::byte *>(pool) + region_config.hwcc_offset_bytes,
      region_config.hwcc_size_bytes);
  simulator.RegisterPool(
      latency_sim::PoolKind::kSwcc,
      static_cast<const std::byte *>(pool) + region_config.swcc_offset_bytes,
      region_config.swcc_size_bytes);
  simulator.Configure(latency);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  auto *payload = new (regions.Allocate(
      sizeof(star::TwoPLPashaSharedDataSCC) + 16,
      tigonkv::engine::AllocationDomain::kSharedPayloadSwcc, 0)) star::TwoPLPashaSharedDataSCC;
  auto *meta = new (regions.Allocate(sizeof(star::TwoPLPashaMetadataShared),
      tigonkv::engine::AllocationDomain::kHwccMetadata, 0)) star::TwoPLPashaMetadataShared(payload);
  RecordingScc fake;
  star::scc_manager = &fake;
  fake.init_scc_metadata(meta, 0);
  // §4.6.2 / §4.4: scan_row_adjacency_ok matches TwoPLPashaExecutor:294-310.
  {
    using H = star::TwoPLPashaHelper;
    // key == min: only next_real matters
    assert(H::scan_row_adjacency_ok(true, false, false, true));
    assert(!H::scan_row_adjacency_ok(true, false, true, false));
    assert(H::scan_row_adjacency_ok(true, true, false, true));  // min wins over limit
    // limit boundary: only prev_real
    assert(H::scan_row_adjacency_ok(false, true, true, false));
    assert(!H::scan_row_adjacency_ok(false, true, false, true));
    // intermediate: both
    assert(H::scan_row_adjacency_ok(false, false, true, true));
    assert(!H::scan_row_adjacency_ok(false, false, true, false));
    assert(!H::scan_row_adjacency_ok(false, false, false, true));
    assert(!H::scan_row_adjacency_ok(false, false, false, false));
    // Exhaustive oracle vs inlined original branches.
    for (int bits = 0; bits < 16; ++bits) {
      const bool key_eq = (bits & 1) != 0;
      const bool limit_b = (bits & 2) != 0;
      const bool prev = (bits & 4) != 0;
      const bool next = (bits & 8) != 0;
      bool expected = true;
      if (key_eq) expected = next;
      else if (limit_b) expected = prev;
      else expected = prev && next;
      assert(H::scan_row_adjacency_ok(key_eq, limit_b, prev, next) == expected);
    }
  }

  const auto fixed = [](std::string_view text) {
    std::string value(16, '\0');
    std::memcpy(value.data(), text.data(), text.size());
    return value;
  };
  const std::string shared_value = fixed("shared-value");
  assert(WriteViaOriginalPrimitives(
      meta, 0, shared_value.data(), shared_value.size()));
  assert(meta->ref_cnt == 0);
  assert(meta->get_flag(star::TwoPLPashaMetadataShared::valid_flag_index));
  char out[16] = {};
  assert(ReadViaOriginalPrimitives(meta, 1, out, sizeof(out)));
  assert(std::string(out, sizeof(out)) == shared_value);
  assert(meta->ref_cnt == 0);
  // The write acquires its lock and then refreshes the SCC row image again
  // in the single update-and-publish primitive; no payload access occurs
  // between those latch acquisitions without prepare_read.
  assert(fake.writes == 1 && fake.finishes == 1 &&
         fake.prepares == 3 && fake.reads == 1);
  assert(fake.last_prepare_bytes ==
         sizeof(star::TwoPLPashaSharedDataSCC) + sizeof(out));
  assert(fake.last_finish_bytes ==
         sizeof(star::TwoPLPashaSharedDataSCC) + shared_value.size());
  assert(meta->get_reader_count() == 0 && !meta->is_write_locked());
  assert(!fake.finish_without_latch.load(std::memory_order_relaxed));
  assert(star::TwoPLPashaHelper::get_migrated_row(meta, 0, shared_value.size()));
  assert(meta->ref_cnt == 1);
  assert(fake.last_prepare_bytes ==
         sizeof(star::TwoPLPashaSharedDataSCC) + shared_value.size());
  assert(star::TwoPLPashaHelper::get_migrated_row(meta, 0, shared_value.size()));
  assert(meta->ref_cnt == 2);
  star::TwoPLPashaHelper::release_migrated_row(meta);
  star::TwoPLPashaHelper::release_migrated_row(meta);
  assert(meta->ref_cnt == 0);

  const std::string zero = fixed("0");
  assert(WriteViaOriginalPrimitives(meta, 0, zero.data(), zero.size()));
  std::vector<std::thread> incrementers;
  for (std::size_t host = 0; host < 4; ++host) {
    incrementers.emplace_back([&, host] {
      simulator.BeginScope(latency_sim::ScopeKind::kForeground);
      for (int iteration = 0; iteration < 250; ++iteration) {
        for (;;) {
          if (IncrementViaOriginalPrimitives(meta, host % 2)) break;
        }
      }
      simulator.EndScopeAndDelay();
    });
  }
  for (auto &thread : incrementers) thread.join();
  char incremented[16] = {};
  assert(ReadViaOriginalPrimitives(meta, 0, incremented, sizeof(incremented)));
  assert(std::string(incremented, sizeof(incremented)) == fixed("1000"));
  assert(meta->ref_cnt == 0 && meta->get_reader_count() == 0 &&
         !meta->is_write_locked());

  // A contended shared writer is a single failed attempt; it must not leave
  // any writer-preference state that blocks later readers.
  {
    meta->lock();
    meta->set_write_locked();
    meta->unlock();
    const std::string x = fixed("x");
    assert(!WriteViaOriginalPrimitives(meta, 0, x.data(), x.size()));
    meta->lock();
    meta->clear_write_locked();
    meta->unlock();

    meta->lock();
    meta->increase_reader_count();
    meta->unlock();
    std::atomic<bool> writer_started{false};
    std::thread stalled_writer([&] {
      simulator.BeginScope(latency_sim::ScopeKind::kForeground);
      writer_started.store(true, std::memory_order_release);
      const std::string y = fixed("y");
      assert(!WriteViaOriginalPrimitives(meta, 0, y.data(), y.size()));
      simulator.EndScopeAndDelay();
    });
    while (!writer_started.load(std::memory_order_acquire))
      std::this_thread::yield();
    stalled_writer.join();
    meta->lock();
    meta->decrease_reader_count();
    meta->unlock();
    char readable[16] = {};
    assert(ReadViaOriginalPrimitives(meta, 0, readable, sizeof(readable)));
    assert(std::string(readable, sizeof(readable)) == fixed("1000"));

    meta->lock();
    auto *scc_data = meta->get_scc_data();
    fake.prepare_read(meta, 0, scc_data, sizeof(star::TwoPLPashaSharedDataSCC));
    scc_data->clear_flag(star::TwoPLPashaSharedDataSCC::valid_flag_index);
    fake.finish_write(meta, 0, scc_data, sizeof(star::TwoPLPashaSharedDataSCC));
    meta->unlock();
    star::RowOutcome missing_result = star::RowOutcome::kDone;
    assert(!ReadViaOriginalPrimitives(meta, 0, readable, sizeof(readable),
                                      &missing_result));
    assert(missing_result == star::RowOutcome::kMissing);
    // Match REMOTE_INSERT: an owner move-in holds one ref for the requester,
    // which publishes valid through SCC and consumes that ref.
    meta->lock();
    meta->increment_ref_cnt();
    meta->unlock();
    assert(star::TwoPLPashaHelper::remote_modify_tuple_valid_bit(
        meta, 1, sizeof(star::TwoPLPashaSharedDataSCC),
        /*is_valid=*/true, /*is_insert=*/true,
        /*consume_ref=*/true, /*mark_shared_dirty=*/true));
    assert(meta->get_ref_cnt() == 0);
    assert(meta->get_scc_data()->get_flag(
        star::TwoPLPashaSharedDataSCC::valid_flag_index));
    assert(fake.last_finish_bytes == sizeof(star::TwoPLPashaSharedDataSCC));
    assert(!fake.finish_without_latch.load(std::memory_order_relaxed));

    meta->lock();
    meta->set_write_locked();
    meta->unlock();
    assert(!IncrementViaOriginalPrimitives(meta, 0));
    meta->lock();
    meta->clear_write_locked();
    meta->unlock();
  }

  simulator.EndScopeAndDelay();
  pthread_spin_destroy(&legacy_local.latch);
  pthread_spin_destroy(&offset_local.latch);
  star::scc_manager = nullptr;
  munmap(pool, bytes);
  return 0;
}
