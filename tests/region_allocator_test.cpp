#include "kv/engine/region_allocator.h"
#include "kv/engine/latency_inject.h"

#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace tigonkv::engine;

constexpr size_t kBytes = 4 * 1024 * 1024;

struct Mapping {
  explicit Mapping(bool shared) {
    char path[] = "/tmp/tigonkv-region-XXXXXX";
    fd = mkstemp(path);
    assert(fd >= 0);
    name = path;
    assert(ftruncate(fd, kBytes) == 0);
    base = mmap(nullptr, kBytes, PROT_READ | PROT_WRITE,
                shared ? MAP_SHARED : MAP_PRIVATE, fd, 0);
    assert(base != MAP_FAILED);
    std::memset(base, 0, kBytes);
  }
  ~Mapping() {
    if (base != MAP_FAILED) munmap(base, kBytes);
    if (fd >= 0) close(fd);
    if (!name.empty()) unlink(name.c_str());
  }
  int fd = -1;
  void *base = MAP_FAILED;
  std::string name;
};

void TestAttachReuseAndAccounting() {
  Mapping mapping(true);
  auto allocator = RegionAllocator::Initialize(mapping.base, kBytes, 2, 0, true);
  DomainCounter index;
  DomainCounter payload;
  void *first = allocator.Allocate(1, AllocationDomain::kHwccIndex, &index, 0);
  void *second = allocator.Allocate(80, AllocationDomain::kSharedPayloadSwcc, &payload, 1);
  assert(reinterpret_cast<uintptr_t>(first) % RegionAllocator::kAlignment == 0);
  assert(reinterpret_cast<uintptr_t>(second) % RegionAllocator::kAlignment == 0);
  assert(index.used_bytes.load() == 128);
  assert(payload.used_bytes.load() == 192);
  const RegionOffset offset = allocator.ToOffset(second);
  auto attached = RegionAllocator::Attach(mapping.base, kBytes, true);
  assert(attached.FromOffset(offset) == second);
  allocator.Free(first, 1, AllocationDomain::kHwccIndex, &index, 0, 0);
  assert(index.used_bytes.load() == 0);
  void *reused = allocator.Allocate(1, AllocationDomain::kHwccIndex, &index, 0);
  assert(reused == first);
  allocator.Free(reused, 1, AllocationDomain::kHwccIndex, &index, 0, 0);
  allocator.Free(second, 80, AllocationDomain::kSharedPayloadSwcc, &payload, 1, 1);
  assert(index.used_bytes.load() == 0 && payload.used_bytes.load() == 0);
  assert(index.peak_bytes.load() == 128 && payload.peak_bytes.load() == 192);
}

void TestCrossProcessFreeRejected() {
  Mapping mapping(true);
  auto allocator = RegionAllocator::Initialize(mapping.base, kBytes, 2, 0, true);
  void *counter_map = mmap(nullptr, sizeof(DomainCounter), PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  assert(counter_map != MAP_FAILED);
  auto *counter = new (counter_map) DomainCounter;
  void *block = allocator.Allocate(100, AllocationDomain::kHwccMetadata, counter, 0);
  const RegionOffset offset = allocator.ToOffset(block);
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    void *child_base = mmap(nullptr, kBytes, PROT_READ | PROT_WRITE, MAP_SHARED, mapping.fd, 0);
    if (child_base == MAP_FAILED) _exit(2);
    try {
      auto child_allocator = RegionAllocator::Attach(child_base, kBytes, true);
      bool rejected = false;
      try {
        child_allocator.Free(child_allocator.FromOffset(offset), 100,
                             AllocationDomain::kHwccMetadata, counter, 0, 1);
      } catch (const std::runtime_error &) {
        rejected = true;
      }
      munmap(child_base, kBytes);
      _exit(rejected ? 0 : 3);
    } catch (...) {
      _exit(3);
    }
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  allocator.Free(block, 100, AllocationDomain::kHwccMetadata, counter, 0, 0);
  assert(counter->used_bytes.load() == 0);
  munmap(counter_map, sizeof(DomainCounter));
}

void TestConcurrencyAndBounds() {
  Mapping mapping(true);
  auto allocator = RegionAllocator::Initialize(mapping.base, kBytes, 2);
  DomainCounter counter;
  std::vector<std::thread> threads;
  for (int worker = 0; worker < 8; ++worker) {
    threads.emplace_back([&] {
      for (int i = 0; i < 500; ++i) {
        const uint64_t bytes = 1 + (i % 700);
        void *block = allocator.Allocate(bytes, AllocationDomain::kOwnerPrivateSwcc,
                                         &counter, 1);
        allocator.Free(block, bytes, AllocationDomain::kOwnerPrivateSwcc,
                       &counter, 1, 1);
      }
    });
  }
  for (auto &thread : threads) thread.join();
  assert(counter.used_bytes.load() == 0);
  bool oom = false;
  try {
    for (;;) allocator.Allocate(65536, AllocationDomain::kSharedPayloadSwcc, &counter, 0);
  } catch (const std::bad_alloc &) {
    oom = true;
  }
  assert(oom);
}

void TestInvalidAttachment() {
  Mapping mapping(true);
  auto allocator = RegionAllocator::Initialize(mapping.base, kBytes, 2);
  (void)allocator;
  bool rejected = false;
  try {
    auto bad = RegionAllocator::Attach(mapping.base, kBytes / 2);
    (void)bad;
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  assert(rejected);
}

void TestDualPhysicalRegions() {
  Mapping mapping(true);
  DualRegionConfig config;
  config.total_pool_bytes = kBytes;
  config.hwcc_offset_bytes = 0;
  config.hwcc_size_bytes = 1024 * 1024;
  config.swcc_offset_bytes = 1024 * 1024;
  config.swcc_size_bytes = kBytes - config.swcc_offset_bytes;
  config.config_hash = 0x1234;
  config.vm_count = 2;
  config.partition_count = 8;
  config.fixed_key_size = 32;
  config.fixed_value_size = 128;
  auto dual = DualRegionAllocator::Initialize(mapping.base, config);
  assert(dual.layout().state.load(std::memory_order_acquire) ==
         static_cast<uint32_t>(LayoutState::kInitializing));
  void *index = dual.Allocate(100, AllocationDomain::kHwccIndex, 0);
  void *metadata = dual.Allocate(64, AllocationDomain::kHwccMetadata, 1);
  void *owner = dual.Allocate(80, AllocationDomain::kOwnerPrivateSwcc, 0);
  void *payload = dual.Allocate(100, AllocationDomain::kSharedPayloadSwcc, 1);
  void *remote_payload =
      dual.Allocate(100, AllocationDomain::kSharedPayloadSwcc, 0);
  assert(dual.IsHwccAddress(index) && dual.IsHwccAddress(metadata));
  assert(dual.IsSwccAddress(owner) && dual.IsSwccAddress(payload));
  assert(!dual.IsHwccAddress(owner) && !dual.IsSwccAddress(index));
  assert(dual.layout().domains[static_cast<size_t>(AllocationDomain::kHwccIndex)]
             .used_bytes.load() > 0);
  assert(dual.layout().owner_migration_hwcc[0].used_bytes.load() > 0);
  assert(dual.layout().owner_migration_hwcc[1].used_bytes.load() > 0);
  assert(dual.layout().domains[static_cast<size_t>(AllocationDomain::kHwccLayout)]
             .used_bytes.load() > 0);
  const uint64_t hwcc_allocator_metadata =
      dual.layout().domains[static_cast<size_t>(
          AllocationDomain::kHwccAllocatorMetadata)].used_bytes.load();
  const uint64_t swcc_allocator_metadata =
      dual.layout().domains[static_cast<size_t>(
          AllocationDomain::kSwccAllocatorMetadata)].used_bytes.load();
  assert(hwcc_allocator_metadata == dual.hwcc().metadata_bytes());
  const uint64_t arena_header_bytes =
      ((sizeof(OwnerPrivateArenaHeader) + RegionAllocator::kAlignment - 1) &
       ~(RegionAllocator::kAlignment - 1)) * config.partition_count;
  assert(swcc_allocator_metadata ==
         dual.swcc().metadata_bytes() + arena_header_bytes);
  dual.PublishOwnerInitialized(0);
  dual.PublishOwnerInitialized(1);
  dual.PublishReady();
  assert(dual.layout().state.load(std::memory_order_acquire) ==
         static_cast<uint32_t>(LayoutState::kReady));
  auto attached = DualRegionAllocator::Attach(mapping.base, config);
  assert(attached.IsHwccAddress(index) && attached.IsSwccAddress(payload));
  assert(attached.layout().domains[static_cast<size_t>(
             AllocationDomain::kHwccAllocatorMetadata)].used_bytes.load() ==
         hwcc_allocator_metadata);
  assert(attached.layout().domains[static_cast<size_t>(
             AllocationDomain::kSwccAllocatorMetadata)].used_bytes.load() ==
         swcc_allocator_metadata);
  bool fixed_domain_rejected = false;
  try {
    (void)attached.Allocate(
        64, AllocationDomain::kHwccAllocatorMetadata, 0);
  } catch (const std::invalid_argument &) {
    fixed_domain_rejected = true;
  }
  assert(fixed_domain_rejected);
  bool remote_free_rejected = false;
  try {
    attached.Free(remote_payload, 100, AllocationDomain::kSharedPayloadSwcc, 0, 1);
  } catch (const std::runtime_error &) {
    remote_free_rejected = true;
  }
  assert(remote_free_rejected);
  dual.Free(index, 100, AllocationDomain::kHwccIndex, 0, 0);
  dual.Free(metadata, 64, AllocationDomain::kHwccMetadata, 1, 1);
  dual.Free(owner, 80, AllocationDomain::kOwnerPrivateSwcc, 0, 0);
  dual.Free(payload, 100, AllocationDomain::kSharedPayloadSwcc, 1, 1);
  dual.Free(remote_payload, 100, AllocationDomain::kSharedPayloadSwcc, 0, 0);
  assert(dual.layout().domains[static_cast<size_t>(AllocationDomain::kHwccIndex)]
             .used_bytes.load() == 0);
  assert(dual.layout().owner_migration_hwcc[0].used_bytes.load() == 0);
  assert(dual.layout().owner_migration_hwcc[1].used_bytes.load() == 0);
  latency_sim::Config checkpoint_latency;
  checkpoint_latency.enabled = true;
  checkpoint_latency.foreground_enabled = true;
  checkpoint_latency.stats_enabled = true;
  checkpoint_latency.swcc_flush_ns_per_line = 1;
  checkpoint_latency.hwcc_read_ns_per_line = 1;
  checkpoint_latency.hwcc_atomic_load_ns = 1;
  checkpoint_latency.hwcc_atomic_store_ns = 1;
  auto &checkpoint_simulator = latency_sim::GlobalLatencySimulator();
  checkpoint_simulator.Configure(checkpoint_latency);
  checkpoint_simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  dual.FlushOwnedRanges(0);
  attached.FlushOwnedRanges(1);
  checkpoint_simulator.EndScopeAndDelay();
  const auto checkpoint_stats = checkpoint_simulator.TakeStatsAndReset();
  assert(checkpoint_stats.swcc_raw_line_accesses > 0);
  assert(checkpoint_stats.hwcc_raw_line_accesses > 0);
  checkpoint_simulator.Configure(latency_sim::Config{});
  assert(dual.layout().state.load(std::memory_order_acquire) ==
         static_cast<uint32_t>(LayoutState::kReady));
}

DualRegionConfig TestDualConfig() {
  DualRegionConfig config;
  config.total_pool_bytes = kBytes;
  config.hwcc_offset_bytes = 0;
  config.hwcc_size_bytes = 1024 * 1024;
  config.swcc_offset_bytes = 1024 * 1024;
  config.swcc_size_bytes = kBytes - config.swcc_offset_bytes;
  config.config_hash = 0x9898;
  config.vm_count = 2;
  config.partition_count = 8;
  config.fixed_key_size = 32;
  config.fixed_value_size = 128;
  return config;
}

void TestMappedPoolAttach() {
  char path[] = "/tmp/tigonkv-dual-pool-XXXXXX";
  const int seed_fd = mkstemp(path);
  assert(seed_fd >= 0);
  close(seed_fd);
  const DualRegionConfig config = TestDualConfig();
  auto parent = DualRegionMappedPool::Open(path, config, true);
  auto *payload = static_cast<char *>(parent.allocator().Allocate(
      64, AllocationDomain::kSharedPayloadSwcc, 0));
  std::memcpy(payload, "mapped-payload", 15);
  const RegionOffset payload_offset = parent.allocator().swcc().ToOffset(payload);
  parent.allocator().PublishOwnerInitialized(0);
  parent.allocator().PublishOwnerInitialized(1);
  parent.allocator().PublishReady();
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    try {
      auto attached = DualRegionMappedPool::Open(path, config, false);
      auto *child_payload = static_cast<char *>(
          attached.allocator().swcc().FromOffset(payload_offset));
      if (std::strcmp(child_payload, "mapped-payload") != 0) _exit(2);
      void *index = attached.allocator().Allocate(64, AllocationDomain::kHwccIndex, 1);
      if (!attached.allocator().IsHwccAddress(index)) _exit(3);
      _exit(0);
    } catch (...) {
      _exit(4);
    }
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  parent.allocator().Free(payload, 64, AllocationDomain::kSharedPayloadSwcc, 0, 0);
  unlink(path);
}

void TestAllocatorLatencyAccounting() {
  Mapping hwcc_mapping(true);
  Mapping swcc_mapping(true);
  Mapping dual_mapping(true);
  auto hwcc_allocator =
      RegionAllocator::Initialize(hwcc_mapping.base, kBytes, 2, 0, true);
  auto swcc_allocator =
      RegionAllocator::Initialize(swcc_mapping.base, kBytes, 2, 0, false);
  const DualRegionConfig config = TestDualConfig();
  auto dual = DualRegionAllocator::Initialize(dual_mapping.base, config);

  latency_sim::Config latency;
  latency.enabled = true;
  latency.foreground_enabled = true;
  latency.stats_enabled = true;
  auto &simulator = latency_sim::GlobalLatencySimulator();
  simulator.Configure(latency);

  DomainCounter hwcc_counter;
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  void *hwcc =
      hwcc_allocator.Allocate(80, AllocationDomain::kHwccMetadata,
                              &hwcc_counter, 0);
  hwcc_allocator.Free(hwcc, 80, AllocationDomain::kHwccMetadata,
                      &hwcc_counter, 0, 0);
  simulator.EndScopeAndDelay();
  auto stats = simulator.TakeStatsAndReset();
  assert(stats.hwcc_raw_line_accesses > 0);
  assert(stats.swcc_raw_line_accesses == 0);

  DomainCounter swcc_counter;
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  void *swcc =
      swcc_allocator.Allocate(80, AllocationDomain::kSharedPayloadSwcc,
                              &swcc_counter, 0);
  swcc_allocator.Free(swcc, 80, AllocationDomain::kSharedPayloadSwcc,
                      &swcc_counter, 0, 0);
  simulator.EndScopeAndDelay();
  stats = simulator.TakeStatsAndReset();
  // SWCC allocator metadata remains private, while the domain counter is
  // deliberately published in the HWCC layout.
  assert(stats.swcc_raw_line_accesses > 0);
  assert(stats.hwcc_raw_line_accesses > 0);

  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  void *owner = dual.AllocateOwnerPrivate(80, 0, 0);
  dual.FreeOwnerPrivate(owner, 80, 0, 0);
  assert(dual.SharedPayloadCapacityBytes() > 0);
  simulator.EndScopeAndDelay();
  stats = simulator.TakeStatsAndReset();
  // Owner arena metadata/data is SWCC; its persistent directory and aggregate
  // accounting live in HWCC.
  assert(stats.swcc_raw_line_accesses > 0);
  assert(stats.hwcc_raw_line_accesses > 0);

  simulator.Configure(latency_sim::Config{});
}

}  // namespace

int main() {
  TestAttachReuseAndAccounting();
  TestCrossProcessFreeRejected();
  TestConcurrencyAndBounds();
  TestInvalidAttachment();
  TestDualPhysicalRegions();
  TestMappedPoolAttach();
  TestAllocatorLatencyAccounting();
  return 0;
}
