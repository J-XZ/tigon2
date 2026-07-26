#include "common/CXLMemory.h"
#include "protocol/TwoPLPasha/TwoPLPashaHelper.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <charconv>
#include <cstring>
#include <thread>
#include <vector>
#include <sys/mman.h>

namespace {

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
  void prepare_read(void *, std::size_t, void *, uint64_t) override { ++prepares; }
  void finish_write_bits(void *meta, std::size_t host) override {
    ++finish_bits;
    // Mirror WriteThrough: keep writer SCC bit under the caller's latch.
    auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(meta);
    smeta->clear_all_scc_bits();
    smeta->set_scc_bit(host);
  }
  void finish_write(void *, std::size_t, void *, uint64_t) override { ++finishes; }
  std::atomic<uint64_t> reads{0}, writes{0}, prepares{0}, finishes{0}, finish_bits{0};
};

tigonkv::engine::DualRegionConfig Config(size_t bytes) {
  tigonkv::engine::DualRegionConfig c;
  c.total_pool_bytes = bytes; c.hwcc_size_bytes = 2 * 1024 * 1024;
  c.swcc_offset_bytes = c.hwcc_size_bytes; c.swcc_size_bytes = bytes - c.swcc_offset_bytes;
  c.config_hash = 0x8811; c.vm_count = 2; c.partition_count = 8;
  c.fixed_key_size = 32; c.fixed_value_size = 128; return c;
}

}  // namespace

int main() {
  constexpr size_t bytes = 8 * 1024 * 1024;
  void *pool = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(pool != MAP_FAILED);
  auto regions = tigonkv::engine::DualRegionAllocator::Initialize(pool, Config(bytes));
  star::CXLMemory::bind_dual_region_allocator(&regions, 0);
  latency_sim::Config latency;
  latency.enabled = true;
  latency.foreground_enabled = true;
  latency.stats_enabled = true;
  auto &simulator = latency_sim::GlobalLatencySimulator();
  simulator.Configure(latency);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  auto *payload = new (regions.Allocate(16,
      tigonkv::engine::AllocationDomain::kSharedPayloadSwcc, 0)) star::TwoPLPashaSharedDataSCC;
  auto *meta = new (regions.Allocate(sizeof(star::TwoPLPashaMetadataShared),
      tigonkv::engine::AllocationDomain::kHwccMetadata, 0)) star::TwoPLPashaMetadataShared(payload);
  RecordingScc fake;
  star::scc_manager = &fake;
  fake.init_scc_metadata(meta, 0);
  assert(star::TwoPLPashaHelper::kv_shared_write(meta, 0, "shared-value", 12));
  assert(meta->ref_cnt == 0);
  assert(meta->value_len == 12);
  assert(meta->get_flag(star::TwoPLPashaMetadataShared::valid_flag_index));
  char out[13] = {};
  assert(star::TwoPLPashaHelper::kv_shared_read(meta, 1, out, 12));
  assert(std::string(out, 12) == "shared-value");
  assert(meta->ref_cnt == 0);
  assert(fake.writes == 1 && fake.finish_bits == 1 && fake.finishes == 0 &&
         fake.prepares == 0 && fake.reads == 1);
  assert(meta->get_reader_count() == 0 && !meta->is_write_locked());
  assert(star::TwoPLPashaHelper::kv_pin_shared_ref(meta));
  assert(meta->ref_cnt == 1);
  assert(star::TwoPLPashaHelper::kv_pin_shared_ref(meta));
  assert(meta->ref_cnt == 2);
  star::TwoPLPashaHelper::kv_unpin_shared_ref(meta);
  star::TwoPLPashaHelper::kv_unpin_shared_ref(meta);
  assert(meta->ref_cnt == 0);

  assert(star::TwoPLPashaHelper::kv_shared_write(meta, 0, "0", 1));
  std::vector<std::thread> incrementers;
  for (std::size_t host = 0; host < 4; ++host) {
    incrementers.emplace_back([&, host] {
      for (int iteration = 0; iteration < 250; ++iteration) {
        bool changed = false;
        assert(star::TwoPLPashaHelper::kv_shared_update(
            meta, host % 2, 16,
            [](const std::string &current, std::string *replacement) {
              int value = 0;
              const auto parsed =
                  std::from_chars(current.data(), current.data() + current.size(), value);
              assert(parsed.ec == std::errc{} &&
                     parsed.ptr == current.data() + current.size());
              *replacement = std::to_string(value + 1);
              return true;
            },
            &changed));
        assert(changed);
      }
    });
  }
  for (auto &thread : incrementers) thread.join();
  char incremented[16] = {};
  uint32_t incremented_size = 0;
  assert(star::TwoPLPashaHelper::kv_shared_read_value(
      meta, 0, incremented, sizeof(incremented), &incremented_size));
  assert(std::string(incremented, incremented_size) == "1000");
  assert(meta->ref_cnt == 0 && meta->get_reader_count() == 0 &&
         !meta->is_write_locked());

  simulator.EndScopeAndDelay();
  const auto latency_stats = simulator.TakeStatsAndReset();
  assert(latency_stats.hwcc_raw_line_accesses > 0);
  assert(latency_stats.hwcc_cache_misses ==
         latency_stats.hwcc_raw_line_accesses);
  star::scc_manager = nullptr;
  munmap(pool, bytes);
  return 0;
}
