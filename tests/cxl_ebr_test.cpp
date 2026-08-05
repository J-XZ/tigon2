#include "common/CXL_EBR.h"
#include "kv/engine/region_allocator.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

using tigonkv::engine::DualRegionConfig;
using tigonkv::engine::DualRegionMappedPool;

DualRegionConfig Config() {
  DualRegionConfig config;
  config.total_pool_bytes = 64 * 1024 * 1024;
  config.hwcc_size_bytes = 32 * 1024 * 1024;
  config.swcc_offset_bytes = config.hwcc_size_bytes;
  config.swcc_size_bytes = config.total_pool_bytes - config.swcc_offset_bytes;
  config.config_hash = 0x45524231;
  config.vm_count = 2;
  config.partition_count = 2;
  config.fixed_key_size = 32;
  config.fixed_value_size = 128;
  return config;
}

void WaitFor(const std::atomic<uint32_t> &phase, uint32_t target) {
  while (phase.load(std::memory_order_acquire) < target)
    std::this_thread::yield();
}

void RetireEpoch(star::CXL_EBR *ebr,
                 tigonkv::engine::DualRegionAllocator *regions,
                 uint32_t owner, uint32_t partition) {
  for (uint64_t i = 0; i < star::CXL_EBR::epoch_advance_threshold; ++i) {
    void *object = regions->AllocateOwnerPrivate(64, partition, owner);
    ebr->add_retired_object(object, 64, star::CXLMemory::DATA_FREE,
                            owner, partition, partition);
  }
}

}  // namespace

int main() {
  char path_template[] = "/tmp/tigonkv-cxl-ebr-XXXXXX";
  const int fd = mkstemp(path_template);
  assert(fd >= 0);
  close(fd);
  const std::string path(path_template);

  auto pool = DualRegionMappedPool::Open(path, Config(), true);
  auto &regions = pool.allocator();
  // The CXL_EBR objects must live inside the registered HWCC range: their
  // members are charged as HWCC.  Allocate the storage from the static HWCC
  // region before finalizing.  Pool Open already registered both ranges and
  // scoped its own init; the wrapped accesses below need explicit scopes.
  tigonkv::engine::mem_access::LatencyScope main_scope(
      latency_sim::ExecutionClass::kForeground);
  void *ebr_storage = regions.Allocate(
      sizeof(star::CXL_EBR), tigonkv::engine::AllocationDomain::kHwccEbr, 0);
  void *handoff_storage = regions.Allocate(
      sizeof(star::CXL_EBR), tigonkv::engine::AllocationDomain::kHwccEbr, 0);
  star::CXL_EBR *ebr = new (ebr_storage) star::CXL_EBR(1, 2, &regions);
  regions.FinalizeStaticHwccLayout();
  regions.PublishStaticHwccLayout();
  regions.InitializeOwnerPrivateArenas(0);
  regions.InitializeOwnerPrivateArenas(1);
  regions.BindOwnerPrivateAllocators(0);

  std::atomic<uint32_t> phase{0};

  std::thread lagging_worker([&] {
    tigonkv::engine::mem_access::LatencyScope worker_scope(
        latency_sim::ExecutionClass::kForeground);
    ebr->thread_init_ebr_meta(0, 1);
    phase.store(1, std::memory_order_release);

    // Worker 1 starts at epoch 0 after worker 0 advanced the global epoch.
    WaitFor(phase, 2);
    ebr->enter_critical_section();
    phase.store(3, std::memory_order_release);

    // Retire in epoch 1, then advance to epoch 2 after both workers caught up.
    WaitFor(phase, 4);
    RetireEpoch(ebr, &regions, 0, 0);
    assert(regions.RetireCount(0, 1, 1) ==
           star::CXL_EBR::epoch_advance_threshold);
    phase.store(5, std::memory_order_release);
    WaitFor(phase, 6);
    ebr->enter_critical_section();
    phase.store(7, std::memory_order_release);

    // Retire in epoch 2 to advance to 3. That grace period reclaims the
    // epoch-1 records and exercises the original consecutive-epoch check.
    WaitFor(phase, 8);
    RetireEpoch(ebr, &regions, 0, 0);
    phase.store(9, std::memory_order_release);
    WaitFor(phase, 10);
    ebr->enter_critical_section();
    assert(regions.RetireCount(0, 1, 1) == 0);
    phase.store(11, std::memory_order_release);

    // Empty queue is a valid next critical section, not a special path.
    WaitFor(phase, 12);
    ebr->enter_critical_section();
  });

  ebr->thread_init_ebr_meta(0, 0);
  WaitFor(phase, 1);
  RetireEpoch(ebr, &regions, 0, 0);
  assert(regions.RetireCount(0, 0, 0) ==
         star::CXL_EBR::epoch_advance_threshold);
  ebr->enter_critical_section();  // global 0 -> 1

  phase.store(2, std::memory_order_release);
  WaitFor(phase, 3);
  phase.store(4, std::memory_order_release);
  WaitFor(phase, 5);
  phase.store(6, std::memory_order_release);
  WaitFor(phase, 7);

  ebr->enter_critical_section();  // worker 0 catches up to epoch 2
  phase.store(8, std::memory_order_release);
  WaitFor(phase, 9);
  phase.store(10, std::memory_order_release);
  WaitFor(phase, 11);
  assert(regions.RetireCount(0, 0, 1) == 0);

  phase.store(12, std::memory_order_release);
  lagging_worker.join();

  // A worker id may move to another OS thread only after Release.  The
  // externally owned meta is the same object, so the reclaim cursor cannot
  // reset merely because the TLS handle changed threads.
  star::CXL_EBR *handoff = new (handoff_storage) star::CXL_EBR(1, 1, &regions);
  star::CXL_EBR::EBRMetaLocal handoff_meta{};
  handoff->initialize_ebr_meta(handoff_meta, 0, 0);
  handoff_meta.last_freed_epoch = 2;
  std::thread first([&] {
    handoff->bind_external_ebr_meta(&handoff_meta);
    handoff->unbind_external_ebr_meta();
  });
  first.join();
  std::thread second([&] {
    handoff->bind_external_ebr_meta(&handoff_meta);
    assert(handoff_meta.last_freed_epoch == 2);
    handoff->unbind_external_ebr_meta();
  });
  second.join();
  unlink(path.c_str());
  return 0;
}
