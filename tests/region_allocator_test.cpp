#include "kv/engine/region_allocator.h"
#include <latency_sim/access.h>
#include <latency_sim/atomic_access.h>
#include <latency_sim/config.h>
#include <latency_sim/domain.h>
#include <latency_sim/scope.h>
#include <latency_sim/simulator.h>

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
  explicit Mapping(bool shared, size_t bytes = kBytes) : bytes(bytes) {
    char path[] = "/tmp/tigonkv-region-XXXXXX";
    fd = mkstemp(path);
    assert(fd >= 0);
    name = path;
    assert(ftruncate(fd, bytes) == 0);
    base = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                shared ? MAP_SHARED : MAP_PRIVATE, fd, 0);
    assert(base != MAP_FAILED);
    std::memset(base, 0, bytes);
  }
  ~Mapping() {
    if (base != MAP_FAILED) munmap(base, bytes);
    if (fd >= 0) close(fd);
    if (!name.empty()) unlink(name.c_str());
  }
  int fd = -1;
  void *base = MAP_FAILED;
  size_t bytes = 0;
  std::string name;
};

void TestAttachReuseAndAccounting() {
  Mapping mapping(true);
  auto allocator = RegionAllocator::Initialize(mapping.base, kBytes, 2, 0, true, true);
  DomainCounter index;
  DomainCounter payload;
  void *first = allocator.Allocate(1, AllocationDomain::kHwccIndex, &index, 0);
  void *second = allocator.Allocate(80, AllocationDomain::kSharedPayloadSwcc, &payload, 1);
  assert(reinterpret_cast<uintptr_t>(first) % RegionAllocator::kAlignment == 0);
  assert(reinterpret_cast<uintptr_t>(second) % RegionAllocator::kAlignment == 0);
  assert(index.used_bytes.load() == 128);
  assert(payload.used_bytes.load() == 192);
  const RegionOffset offset = allocator.ToOffset(second);
  auto attached = RegionAllocator::Attach(mapping.base, kBytes, true, true);
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
  auto allocator = RegionAllocator::Initialize(mapping.base, kBytes, 2, 0, true, true);
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
      auto child_allocator = RegionAllocator::Attach(child_base, kBytes, true, true);
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

DualRegionConfig TestDualConfig();

void TestBusinessHwccUsesFullAllocator() {
  Mapping mapping(true, 64 * 1024 * 1024);
  DualRegionConfig config = TestDualConfig();
  auto dual = DualRegionAllocator::Initialize(mapping.base, config);
  const uint64_t dual_header_bytes =
      (sizeof(DualRegionPersistentHeader) + RegionAllocator::kAlignment - 1) &
      ~(RegionAllocator::kAlignment - 1);
  assert(dual.ReadStaticDomainUsedBytes(AllocationDomain::kHwccLayout) ==
         dual_header_bytes);
  void *business = dual.Allocate(64, AllocationDomain::kTransport, 0);
  assert(dual.hwcc().Contains(business));
  assert(dual.hwcc().ToOffset(business) >= dual.hwcc().metadata_bytes());
  dual.Free(business, 64, AllocationDomain::kTransport, 0, 0);
  dual.FinalizeStaticHwccLayout();
  dual.PublishStaticHwccLayout();
  auto attached = DualRegionAllocator::Attach(mapping.base, config);
  assert(attached.ReadStaticDomainUsedBytes(AllocationDomain::kHwccLayout) ==
         dual_header_bytes);
}

void TestDualPhysicalRegions() {
  Mapping mapping(true, 64 * 1024 * 1024);
  DualRegionConfig config;
  config.total_pool_bytes = 64 * 1024 * 1024;
  config.hwcc_offset_bytes = 0;
  config.hwcc_size_bytes = 32 * 1024 * 1024;
  config.swcc_offset_bytes = 32 * 1024 * 1024;
  config.swcc_size_bytes = config.total_pool_bytes - config.swcc_offset_bytes;
  config.config_hash = 0x1234;
  config.vm_count = 2;
  config.partition_count = 8;
  config.fixed_key_size = 32;
  config.fixed_value_size = 128;
  auto dual = DualRegionAllocator::Initialize(mapping.base, config);
  dual.FinalizeStaticHwccLayout();
  dual.PublishStaticHwccLayout();
  const auto &dynamic0 = dual.layout().owner_dynamic_arenas[0];
  const auto &dynamic1 = dual.layout().owner_dynamic_arenas[1];
  assert(dynamic0.hwcc_offset + dynamic0.hwcc_bytes <= dynamic1.hwcc_offset);
  assert(dynamic0.shared_swcc_offset + dynamic0.shared_swcc_bytes <=
         dynamic1.shared_swcc_offset);
  dual.InitializeOwnerPrivateArenas(0);
  // Phase two is owner-only: VM0 may publish immutable HWCC geometry, but it
  // must not construct VM1's private allocator control/header.
  dual.InitializeOwnerPrivateArenas(1);
  assert(dual.layout().state.load(std::memory_order_acquire) ==
         static_cast<uint32_t>(LayoutState::kInitializing));
  dual.BindOwnerPrivateAllocators(0);
  void *index = dual.Allocate(100, AllocationDomain::kHwccIndex, 0);
  void *owner = dual.AllocateOwnerPrivate(80, 0, 0);
  dual.BindOwnerPrivateAllocators(1);
  void *metadata = dual.Allocate(64, AllocationDomain::kHwccMetadata, 1);
  void *payload = dual.Allocate(100, AllocationDomain::kSharedPayloadSwcc, 1);
  dual.BindOwnerPrivateAllocators(0);
  void *remote_payload =
      dual.Allocate(100, AllocationDomain::kSharedPayloadSwcc, 0);
  assert(dual.IsHwccAddress(index) && dual.IsHwccAddress(metadata));
  assert(dual.IsSwccAddress(owner) && dual.IsSwccAddress(payload));
  assert(dual.ResolveDynamicHwcc(dual.hwcc().ToOffset(index), 64, 0) == index);
  bool wrong_dynamic_owner = false;
  try {
    (void)dual.ResolveDynamicHwcc(dual.hwcc().ToOffset(index), 64, 1);
  } catch (const std::runtime_error &) {
    wrong_dynamic_owner = true;
  }
  assert(wrong_dynamic_owner);
  assert(dual.ResolveOwnerPrivate(dual.ToOwnerPrivateOffset(owner, 0), 80, 0, 0) == owner);
  bool wrong_private_partition = false;
  try {
    (void)dual.ResolveOwnerPrivate(dual.ToOwnerPrivateOffset(owner, 0), 80, 1, 1);
  } catch (const std::runtime_error &) {
    wrong_private_partition = true;
  }
  assert(wrong_private_partition);
  const auto payload_pool_offset = dual.EncodeSharedPayloadOffset(payload, 1);
  assert(dual.ResolveSharedPayload(payload_pool_offset, 100) == payload);
  const auto remote_payload_pool_offset =
      dual.EncodeSharedPayloadOffset(remote_payload, 0);
  assert(dual.ResolveSharedPayload(remote_payload_pool_offset, 100) ==
         remote_payload);
  bool wrong_shared_payload_domain = false;
  try {
    (void)dual.ResolveSharedPayload(dual.hwcc().ToOffset(index), 64);
  } catch (const std::runtime_error &) {
    wrong_shared_payload_domain = true;
  }
  assert(wrong_shared_payload_domain);
  assert(!dual.IsHwccAddress(owner) && !dual.IsSwccAddress(index));
  assert(dual.DynamicHwccUsedBytes(0) > 0);
  dual.BindOwnerPrivateAllocators(1);
  assert(dual.DynamicHwccUsedBytes(1) > 0);
  bool nonowner_private_resolve_rejected = false;
  try {
    (void)dual.ResolveOwnerPrivate(dual.ToOwnerPrivateOffset(owner, 0), 80,
                                   0, 0);
  } catch (const std::runtime_error &) {
    nonowner_private_resolve_rejected = true;
  }
  assert(nonowner_private_resolve_rejected);
  assert(dual.layout().domains[static_cast<size_t>(AllocationDomain::kHwccIndex)]
             .used_bytes.load() == 0);
  assert(dual.layout().domains[static_cast<size_t>(AllocationDomain::kHwccLayout)]
             .used_bytes.load() > 0);
  const uint64_t hwcc_allocator_metadata =
      dual.layout().domains[static_cast<size_t>(
          AllocationDomain::kHwccAllocatorMetadata)].used_bytes.load();
  const uint64_t swcc_allocator_metadata =
      dual.layout().domains[static_cast<size_t>(
          AllocationDomain::kSwccAllocatorMetadata)].used_bytes.load();
  assert(hwcc_allocator_metadata == dual.hwcc().metadata_bytes());
  const uint64_t swcc_metadata_bytes =
      (sizeof(RegionAllocatorHeader) + RegionAllocator::kAlignment - 1) /
      RegionAllocator::kAlignment * RegionAllocator::kAlignment;
  assert(swcc_allocator_metadata == swcc_metadata_bytes);
  dual.PublishOwnerInitialized(0);
  dual.PublishOwnerInitialized(1);
  dual.PublishReady();
  assert(dual.layout().state.load(std::memory_order_acquire) ==
         static_cast<uint32_t>(LayoutState::kReady));
  auto attached = DualRegionAllocator::Attach(mapping.base, config);
  attached.BindOwnerPrivateAllocators(1);
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
  dual.BindOwnerPrivateAllocators(0);
  dual.Free(index, 100, AllocationDomain::kHwccIndex, 0, 0);
  // RegionOffset zero is null.  The dynamic arena therefore reserves its
  // first cache line and can immediately reuse the first freed block.
  void *reused_index = dual.Allocate(100, AllocationDomain::kHwccIndex, 0);
  assert(reused_index == index);
  dual.Free(reused_index, 100, AllocationDomain::kHwccIndex, 0, 0);
  dual.BindOwnerPrivateAllocators(1);
  dual.Free(metadata, 64, AllocationDomain::kHwccMetadata, 1, 1);
  dual.Free(payload, 100, AllocationDomain::kSharedPayloadSwcc, 1, 1);
  dual.BindOwnerPrivateAllocators(0);
  dual.FreeOwnerPrivate(owner, 80, 0, 0);
  dual.Free(remote_payload, 100, AllocationDomain::kSharedPayloadSwcc, 0, 0);
  assert(dual.DynamicHwccUsedBytes(0) == 0);
  dual.BindOwnerPrivateAllocators(1);
  assert(dual.DynamicHwccUsedBytes(1) == 0);
  latency_sim::FixedLatencyConfig checkpoint_latency;
  checkpoint_latency.enabled = true;
  checkpoint_latency.foreground_enabled = true;
  checkpoint_latency.background_enabled = true;
  checkpoint_latency.swcc_fixed_ns_per_line = 1;
  checkpoint_latency.hwcc_fixed_ns_per_line = 1;
  auto &checkpoint_simulator = latency_sim::GlobalLatencySimulator();
  checkpoint_simulator.RegisterPool(
      latency_sim::MemoryDomain::kHwcc,
      static_cast<const std::byte *>(mapping.base) + config.hwcc_offset_bytes,
      config.hwcc_size_bytes);
  checkpoint_simulator.RegisterPool(
      latency_sim::MemoryDomain::kSwcc,
      static_cast<const std::byte *>(mapping.base) + config.swcc_offset_bytes,
      config.swcc_size_bytes);
  checkpoint_simulator.Configure(checkpoint_latency);
#if !defined(LATENCY_SIM_COMPILE_OFF)
  checkpoint_simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  dual.FlushOwnedRanges(0);
  attached.FlushOwnedRanges(1);
  assert(checkpoint_simulator.PendingDelayNsForTest() > 0);
  checkpoint_simulator.EndScopeAndDelay();
#endif
  checkpoint_simulator.Configure(latency_sim::FixedLatencyConfig{});
  assert(dual.layout().state.load(std::memory_order_acquire) ==
         static_cast<uint32_t>(LayoutState::kReady));
}

DualRegionConfig TestDualConfig() {
  DualRegionConfig config;
  config.total_pool_bytes = 64 * 1024 * 1024;
  config.hwcc_offset_bytes = 0;
  config.hwcc_size_bytes = 32 * 1024 * 1024;
  config.swcc_offset_bytes = 32 * 1024 * 1024;
  config.swcc_size_bytes = config.total_pool_bytes - config.swcc_offset_bytes;
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
  parent.allocator().FinalizeStaticHwccLayout();
  parent.allocator().PublishStaticHwccLayout();
  parent.allocator().InitializeOwnerPrivateArenas(0);
  parent.allocator().InitializeOwnerPrivateArenas(1);
  parent.allocator().BindOwnerPrivateAllocators(0);
  auto *payload = static_cast<char *>(parent.allocator().Allocate(
      64, AllocationDomain::kSharedPayloadSwcc, 0));
  std::memcpy(payload, "mapped-payload", 15);
  const uint64_t payload_offset =
      parent.allocator().EncodeSharedPayloadOffset(payload, 0);
  parent.allocator().PublishOwnerInitialized(0);
  parent.allocator().PublishOwnerInitialized(1);
  parent.allocator().PublishReady();
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    try {
      auto attached = DualRegionMappedPool::Open(path, config, false);
      attached.allocator().BindOwnerPrivateAllocators(1);
      auto *child_payload = static_cast<char *>(
          attached.allocator().ResolveSharedPayload(payload_offset, 64));
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

#if !defined(LATENCY_SIM_COMPILE_OFF)
void TestAllocatorLatencyAccounting() {
  Mapping hwcc_mapping(true);
  Mapping swcc_mapping(true);
  Mapping dual_mapping(true, 64 * 1024 * 1024);
  Mapping hwcc_counter_mapping(true, kBytes);
  Mapping swcc_counter_mapping(true, kBytes);
  auto hwcc_allocator =
      RegionAllocator::Initialize(hwcc_mapping.base, kBytes, 2, 0, true, true);
  auto swcc_allocator =
      RegionAllocator::Initialize(swcc_mapping.base, kBytes, 2, 0, false, false);
  const DualRegionConfig config = TestDualConfig();
  auto dual = DualRegionAllocator::Initialize(dual_mapping.base, config);
  auto *hwcc_counter = new (hwcc_counter_mapping.base) DomainCounter;
  auto *swcc_counter = new (swcc_counter_mapping.base) DomainCounter;
  dual.FinalizeStaticHwccLayout();
  dual.PublishStaticHwccLayout();
  dual.InitializeOwnerPrivateArenas(0);
  dual.InitializeOwnerPrivateArenas(1);
  dual.BindOwnerPrivateAllocators(0);

  latency_sim::FixedLatencyConfig latency;
  latency.enabled = true;
  latency.foreground_enabled = true;
  latency.background_enabled = true;
  latency.swcc_fixed_ns_per_line = 1;
  latency.hwcc_fixed_ns_per_line = 1;
  auto &simulator = latency_sim::GlobalLatencySimulator();
  simulator.Configure(latency_sim::FixedLatencyConfig{});
  simulator.ClearPoolRegistrations();
  // The domain counters stand in for the shared HWCC layout / owner-private
  // SWCC control counters.  latency_sim requires every registered range to
  // belong to exactly one domain (HWCC and SWCC ranges must not overlap), so
  // the two counters use two separate pages.
  simulator.RegisterPool(latency_sim::MemoryDomain::kHwcc, hwcc_mapping.base,
                         kBytes);
  simulator.RegisterPool(latency_sim::MemoryDomain::kSwcc, swcc_mapping.base,
                         kBytes);
  simulator.RegisterPool(latency_sim::MemoryDomain::kHwcc,
                         static_cast<const std::byte *>(dual_mapping.base) +
                             config.hwcc_offset_bytes,
                         config.hwcc_size_bytes);
  simulator.RegisterPool(latency_sim::MemoryDomain::kSwcc,
                         static_cast<const std::byte *>(dual_mapping.base) +
                             config.swcc_offset_bytes,
                         config.swcc_size_bytes);
  simulator.RegisterPool(latency_sim::MemoryDomain::kHwcc,
                         hwcc_counter_mapping.base, kBytes);
  simulator.RegisterPool(latency_sim::MemoryDomain::kSwcc,
                         swcc_counter_mapping.base, kBytes);
  simulator.Configure(latency);

  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  void *hwcc =
      hwcc_allocator.Allocate(80, AllocationDomain::kHwccMetadata,
                              hwcc_counter, 0);
  hwcc_allocator.Free(hwcc, 80, AllocationDomain::kHwccMetadata,
                      hwcc_counter, 0, 0);
  assert(simulator.PendingDelayNsForTest() > 0);
  simulator.EndScopeAndDelay();

  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  void *swcc =
      swcc_allocator.Allocate(80, AllocationDomain::kSharedPayloadSwcc,
                              swcc_counter, 0);
  swcc_allocator.Free(swcc, 80, AllocationDomain::kSharedPayloadSwcc,
                      swcc_counter, 0, 0);
  assert(simulator.PendingDelayNsForTest() > 0);
  simulator.EndScopeAndDelay();

  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  void *owner = dual.AllocateOwnerPrivate(80, 0, 0);
  dual.FreeOwnerPrivate(owner, 80, 0, 0);
  assert(dual.SharedPayloadCapacityBytes(0) > 0);
  assert(simulator.PendingDelayNsForTest() > 0);
  simulator.EndScopeAndDelay();

  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  void *dynamic = dual.Allocate(80, AllocationDomain::kHwccIndex, 0);
  dual.Free(dynamic, 80, AllocationDomain::kHwccIndex, 0, 0);
  assert(simulator.PendingDelayNsForTest() > 0);
  simulator.EndScopeAndDelay();

  simulator.Configure(latency_sim::FixedLatencyConfig{});
}


#endif  // !defined(LATENCY_SIM_COMPILE_OFF)

void TestOwnerPrivateRetireQueue() {
  Mapping mapping(true, 64 * 1024 * 1024);
  DualRegionConfig config = TestDualConfig();
  auto dual = DualRegionAllocator::Initialize(mapping.base, config);
  dual.FinalizeStaticHwccLayout();
  dual.PublishStaticHwccLayout();
  dual.InitializeOwnerPrivateArenas(0);
  dual.InitializeOwnerPrivateArenas(1);
  dual.BindOwnerPrivateAllocators(0);
  void *object = dual.Allocate(96, AllocationDomain::kHwccIndex, 0);
  dual.Retire(0, 0, 0, 0, object, 96, AllocationDomain::kHwccIndex,
              UINT32_MAX);
  assert(dual.RetireCount(0, 0, 0) == 1);
  const auto retired = dual.TakeRetired(0, 0, 0);
  assert(retired.size() == 1);
  assert(retired[0].object_offset == dual.hwcc().ToOffset(object));
  assert(dual.RetireCount(0, 0, 0) == 0);
  dual.Free(object, 96, AllocationDomain::kHwccIndex, 0, 0);
}

// Directed test for the allocator free-head classification.  shard.free_heads
// is allocator control and must be charged at the control domain rate, while
// block->next stays at the block domain rate.  With rates swapped between two
// runs, control-HWCC/block-SWCC under rate A must equal control-SWCC/block-HWCC
// under rate B (and vice versa); a free_heads misclassification breaks the
// invariant.
#if !defined(LATENCY_SIM_COMPILE_OFF)
void TestFreeHeadControlDomainClassification() {
  // Re-initialize one region with different control/block classifications so
  // both variants charge identical addresses and cache-line boundaries.
  Mapping region(true);
  Mapping counters(true, kBytes);
  auto *counter = new (counters.base) DomainCounter;
  auto &simulator = latency_sim::GlobalLatencySimulator();

  // latency_sim requires every registered range to belong to exactly one
  // domain, so the allocator region is split into its control prefix and
  // block payload and each sub-range is registered under the domain of the
  // run's classification (re-registered per run since the classification
  // swaps between runs).  The control prefix length is read from the
  // allocator header after the first initialization.
  uint64_t control_bytes = 0;
  const auto control_begin = [&] { return region.base; };
  const auto block_begin = [&] {
    return static_cast<std::byte *>(region.base) + control_bytes;
  };
  const auto block_bytes = [&] { return kBytes - control_bytes; };

  latency_sim::FixedLatencyConfig rate_a;
  rate_a.enabled = true;
  rate_a.cache_line_bytes = 64;
  rate_a.swcc_fixed_ns_per_line = 1;
  rate_a.hwcc_fixed_ns_per_line = 4;
  latency_sim::FixedLatencyConfig rate_b = rate_a;
  rate_b.swcc_fixed_ns_per_line = 4;
  rate_b.hwcc_fixed_ns_per_line = 1;

  const auto measure = [&](bool control_hwcc, bool block_hwcc,
                           const latency_sim::FixedLatencyConfig &cfg) {
    auto allocator = RegionAllocator::Initialize(
        region.base, kBytes, 1, 0, control_hwcc, block_hwcc);
    control_bytes =
        static_cast<const RegionAllocatorHeader *>(region.base)->metadata_bytes;
    simulator.Configure(latency_sim::FixedLatencyConfig{});
    simulator.ClearPoolRegistrations();
    simulator.RegisterPool(control_hwcc ? latency_sim::MemoryDomain::kHwcc
                                        : latency_sim::MemoryDomain::kSwcc,
                           control_begin(), control_bytes);
    simulator.RegisterPool(block_hwcc ? latency_sim::MemoryDomain::kHwcc
                                      : latency_sim::MemoryDomain::kSwcc,
                           block_begin(), block_bytes());
    simulator.RegisterPool(latency_sim::MemoryDomain::kHwcc, counters.base,
                           kBytes);
    // Reset the accounting counter so the peak CAS charges identically on
    // every run (the peak only rises above the previous peak once otherwise).
    std::memset(counters.base, 0, kBytes);
    simulator.Configure(cfg);
    simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
    void *block = allocator.Allocate(80, AllocationDomain::kHwccMetadata,
                                     counter, 0);
    allocator.Free(block, 80, AllocationDomain::kHwccMetadata, counter, 0, 0);
    const uint64_t pending = simulator.PendingDelayPsForTest();
    simulator.EndScopeAndDelay();
    return pending;
  };

  // Same allocator, same addresses, three rate configurations.  With control
  // at HWCC rate and block at SWCC rate, the pending delay must fit the
  // linear model P = control_lines*rate_control + block_lines*rate_block, so
  // the differences isolate integer per-domain line counts.  A free_heads
  // misclassification (charged at the block rate instead of the control
  // rate) shifts those counts and breaks the model.
  const uint64_t p_hwcc_high = measure(true, false, rate_a);
  const uint64_t p_swcc_high = measure(true, false, rate_b);
  latency_sim::FixedLatencyConfig rate_eq = rate_a;
  rate_eq.hwcc_fixed_ns_per_line = 1;
  rate_eq.swcc_fixed_ns_per_line = 1;
  const uint64_t p_eq = measure(true, false, rate_eq);
  assert((p_hwcc_high - p_eq) % 3000 == 0);
  assert((p_swcc_high - p_eq) % 3000 == 0);
  const uint64_t control_lines = (p_hwcc_high - p_eq) / 3000;
  const uint64_t block_lines = (p_swcc_high - p_eq) / 3000;
  // Control (lock, free_heads, bump, accounting) dominates the block fields
  // (next/size-class) in an allocate+free cycle.
  assert(control_lines > 0 && block_lines > 0 && control_lines > block_lines);
  simulator.Configure(latency_sim::FixedLatencyConfig{});
}

#endif  // !defined(LATENCY_SIM_COMPILE_OFF)
}  // namespace

int main() {
  TestAttachReuseAndAccounting();
  TestCrossProcessFreeRejected();
  TestConcurrencyAndBounds();
  TestInvalidAttachment();
  TestBusinessHwccUsesFullAllocator();
  TestDualPhysicalRegions();
  TestMappedPoolAttach();
#if !defined(LATENCY_SIM_COMPILE_OFF)
  TestAllocatorLatencyAccounting();
  TestFreeHeadControlDomainClassification();
#endif
  TestOwnerPrivateRetireQueue();
  return 0;
}
