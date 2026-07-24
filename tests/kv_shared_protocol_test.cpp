#include "common/CXLMemory.h"
#include "protocol/TwoPLPasha/TwoPLPashaHelper.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
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
  void finish_write(void *, std::size_t, void *, uint64_t) override { ++finishes; }
  uint64_t reads = 0, writes = 0, prepares = 0, finishes = 0;
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
  auto *payload = new (regions.Allocate(sizeof(star::TwoPLPashaSharedDataSCC) + 16,
      tigonkv::engine::AllocationDomain::kSharedPayloadSwcc, 0)) star::TwoPLPashaSharedDataSCC;
  auto *meta = new (regions.Allocate(sizeof(star::TwoPLPashaMetadataShared),
      tigonkv::engine::AllocationDomain::kHwccMetadata, 0)) star::TwoPLPashaMetadataShared(payload);
  RecordingScc fake;
  star::scc_manager = &fake;
  fake.init_scc_metadata(meta, 0);
  assert(star::TwoPLPashaHelper::kv_shared_write(meta, 0, "shared-value", 12));
  char out[13] = {};
  assert(star::TwoPLPashaHelper::kv_shared_read(meta, 1, out, 12));
  assert(std::string(out, 12) == "shared-value");
  assert(fake.writes == 1 && fake.finishes == 1 && fake.prepares == 1 && fake.reads == 1);
  assert(meta->get_reader_count() == 0 && !meta->is_write_locked());
  star::scc_manager = nullptr;
  munmap(pool, bytes);
  return 0;
}
