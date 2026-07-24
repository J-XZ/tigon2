#include "kv/engine/kv_partition.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>
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
  void *private_reuse = regions.AllocateOwnerPrivate(128, 5, 1);
  regions.FreeOwnerPrivate(private_reuse, 128, 5, 1);
  assert(regions.AllocateOwnerPrivate(128, 5, 1) == private_reuse);
  tigonkv::engine::KVPartition partition(regions, ebr, 5, 1, false);
  assert(regions.OwnerPrivateArenaOffset(5) ==
         regions.layout().partitions[5].private_arena);
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
  PassthroughScc scc;
  star::scc_manager = &scc;
  assert(partition.PromotePrivate("alpha", 1));
  assert(partition.GetPrivate("alpha", &value) && value == "updated");
  // Once migrated, PUT must update the shared SCC authority rather than the
  // retained private locator row.
  assert(!partition.PutPrivate("alpha", "shared-update"));
  assert(partition.GetPrivate("alpha", &value) && value == "shared-update");
  assert(regions.layout().partitions[5].migration_in_seq.load() == 1);
  const auto hwcc_before_moveout = regions.layout().domains[
      static_cast<size_t>(tigonkv::engine::AllocationDomain::kHwccMetadata)].used_bytes.load();
  const auto swcc_before_moveout = regions.layout().domains[
      static_cast<size_t>(tigonkv::engine::AllocationDomain::kSharedPayloadSwcc)].used_bytes.load();
  assert(partition.MoveOutPrivate("alpha", 1));
  assert(partition.GetPrivate("alpha", &value) && value == "shared-update");
  assert(regions.layout().partitions[5].migration_out_seq.load() == 1);
  assert(ebr.drain_quiescent() > 0);
  assert(regions.layout().domains[
      static_cast<size_t>(tigonkv::engine::AllocationDomain::kHwccMetadata)].used_bytes.load() < hwcc_before_moveout);
  assert(regions.layout().domains[
      static_cast<size_t>(tigonkv::engine::AllocationDomain::kSharedPayloadSwcc)].used_bytes.load() < swcc_before_moveout);
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

  // PolicyClock's first pass consumes the shared metadata second-chance bit;
  // the next pass selects the same quiescent key and performs real move-out.
  assert(partition.PutPrivate("clock", "victim"));
  assert(partition.PromotePrivate("clock", 1));
  assert(!partition.MoveOutClockVictim(1));
  assert(partition.MoveOutClockVictim(1));
  assert(partition.GetPrivate("clock", &value) && value == "victim");
  assert(partition.hwcc_used_bytes() > 0);
  assert(partition.shared_payload_used_bytes() < partition.shared_payload_capacity_bytes());
  star::scc_manager = nullptr;

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
  unlink(path.c_str());
  return 0;
}
