// Deterministic fixed-latency accounting test for the real MPSC transport
// ring (star::MPSCRingBuffer): construction, enqueue and dequeue must charge
// the actual covered HWCC lines exactly once, adjacent to the real access
// that writes/reads them (ready atomic, offset/length metadata and payload
// each per covered line; no whole-entry envelope approximation), and every
// outermost scope must settle exactly once.  A recording delay backend makes
// the assertions deterministic.  The ring protocol round trip (payload bytes
// and head/tail/count/ready transitions, with the original wrapper memory
// orders) is asserted unchanged.
//
// Line model (ring object 64-byte aligned, entry_struct_size=64, entry_num=8):
//   ctor header: 3 header writes + 1 entries-offset read        = 4 lines
//   per entry  : ready store + metadata write + payload memset = 3 lines
//   enqueue    : 2 header reads + count/tail fetch-add + payload
//                write + metadata write + ready store           = 7 lines
//   dequeue    : 2 header reads + size() (1 read + 2 loads) +
//                head load + ready spin + metadata read + data
//                read + metadata write + ready store + head
//                store + count fetch-sub                       = 13 lines
#include "common/CXLMemory.h"
#include "common/MPSCRingBuffer.h"
#include "kv/engine/mem_access.h"
#include "kv/engine/region_allocator.h"
#include "tests/latency_test_support.h"

#include <latency_sim/simulator.h>
#include <latency_sim/testing.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

tigonkv::engine::DualRegionConfig MakeConfig(uint64_t bytes) {
  tigonkv::engine::DualRegionConfig config;
  config.total_pool_bytes = bytes;
  config.hwcc_size_bytes = 32 * 1024 * 1024;
  config.swcc_offset_bytes = config.hwcc_size_bytes;
  config.swcc_size_bytes = bytes - config.swcc_offset_bytes;
  config.config_hash = 0xfeed;
  config.vm_count = 1;
  config.partition_count = 4;
  config.fixed_key_size = 32;
  config.fixed_value_size = 128;
  return config;
}

latency_sim::FixedLatencyConfig OneNanosecondPerLine() {
  latency_sim::FixedLatencyConfig config;
  config.cache_line_bytes = 64;
  config.swcc_fixed_ns_per_line = 1.0;
  config.hwcc_fixed_ns_per_line = 1.0;
  return config;
}

}  // namespace

int main() {
#if defined(LATENCY_SIM_COMPILE_OFF)
  // Compile-off: no simulator, nothing to account.
  return 0;
#else
  // Deterministic backend: records every scope settlement instead of
  // busy-waiting.
  static std::vector<std::uint64_t> settlements;
  static std::mutex settlements_mutex;
  latency_sim::detail::SetDelaySpinBackendForTest(
      [](std::uint64_t ns) {
        std::lock_guard<std::mutex> lock(settlements_mutex);
        settlements.push_back(ns);
      });
  settlements.clear();

  char path_template[] = "/tmp/tigonkv-ring-latency-XXXXXX";
  const int fd = mkstemp(path_template);
  assert(fd >= 0);
  close(fd);
  const std::string path(path_template);
  constexpr uint64_t kPoolBytes = 64ull * 1024ull * 1024ull;

  auto pool = tigonkv::engine::DualRegionMappedPool::Open(
      path, MakeConfig(kPoolBytes), true);
  star::CXLMemory::bind_dual_region_allocator(&pool.allocator(), 0);

  // The pool init scope settled a zero-nanosecond budget; the backend is only
  // invoked for non-zero delays, so nothing was recorded for it.
  {
    std::lock_guard<std::mutex> lock(settlements_mutex);
    assert(settlements.empty());
    settlements.clear();
  }

  // Raise the per-line delays through a quiescent reopen of the same ranges.
  const auto config = MakeConfig(kPoolBytes);
  auto &sim = latency_sim::GlobalLatencySimulator();
  sim.ClearPoolRegistrations();
  sim.RegisterPool(
      latency_sim::MemoryDomain::kHwcc,
      static_cast<const std::byte *>(pool.base()) + config.hwcc_offset_bytes,
      config.hwcc_size_bytes);
  sim.RegisterPool(
      latency_sim::MemoryDomain::kSwcc,
      static_cast<const std::byte *>(pool.base()) + config.swcc_offset_bytes,
      config.swcc_size_bytes);
  sim.Configure(OneNanosecondPerLine());

  constexpr uint64_t kEntryStructSize = 64;
  constexpr uint64_t kEntryNum = 8;
  constexpr uint64_t kCtorHeaderLines = 4;
  constexpr uint64_t kCtorEntryLines = 3;
  constexpr uint64_t kEnqueueLines = 7;
  constexpr uint64_t kDequeueLines = 13;

  star::MPSCRingBuffer *ring = nullptr;
  {
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
    // The ring constructor itself allocates its storage through the real
    // allocator, so the allocator charge is isolated by a second identical
    // allocation: ctor_charge = (alloc2 + ring ctor) - (alloc1).
    const uint64_t before_alloc = sim.PendingDelayNsForTest();
    void *probe_storage = star::cxl_memory.cxlalloc_malloc_wrapper(
        sizeof(star::MPSCRingBuffer), star::CXLMemory::TRANSPORT_ALLOCATION);
    assert(probe_storage != nullptr);
    const uint64_t alloc_charge = sim.PendingDelayNsForTest() - before_alloc;
    void *storage = star::cxl_memory.cxlalloc_malloc_wrapper(
        sizeof(star::MPSCRingBuffer), star::CXLMemory::TRANSPORT_ALLOCATION);
    assert(storage != nullptr);
    assert((reinterpret_cast<std::uintptr_t>(storage) & 63u) == 0);
    const uint64_t before_ctor = sim.PendingDelayNsForTest();
    ring = new (storage) star::MPSCRingBuffer(kEntryStructSize, kEntryNum);
    const uint64_t ctor_delta =
        sim.PendingDelayNsForTest() - before_ctor - alloc_charge;
    std::fprintf(stderr, "ctor_delta=%llu expected=%llu\n",
                 static_cast<unsigned long long>(ctor_delta),
                 static_cast<unsigned long long>(
                     kCtorHeaderLines + kCtorEntryLines * kEntryNum));
    assert(ctor_delta == kCtorHeaderLines + kCtorEntryLines * kEntryNum);
  }
  {
    std::lock_guard<std::mutex> lock(settlements_mutex);
    assert(settlements.size() == 1);  // exactly one settlement per scope
    assert(settlements[0] > 0);       // the whole scope budget (incl. allocs)
    settlements.clear();
  }

  char payload[] = "ring";
  {
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
    const uint64_t before_enqueue = sim.PendingDelayNsForTest();
    assert(ring->enqueue(payload, sizeof(payload)));
    const uint64_t enqueue_delta = sim.PendingDelayNsForTest() - before_enqueue;
    assert(enqueue_delta == kEnqueueLines);
  }
  {
    std::lock_guard<std::mutex> lock(settlements_mutex);
    assert(settlements.size() == 1);
    assert(settlements[0] == kEnqueueLines);
    settlements.clear();
  }

  char output[256]{};
  {
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
    const uint64_t before_dequeue = sim.PendingDelayNsForTest();
    assert(ring->dequeue(output, sizeof(output)) == sizeof(payload));
    const uint64_t dequeue_delta = sim.PendingDelayNsForTest() - before_dequeue;
    assert(dequeue_delta == kDequeueLines);
  }
  {
    std::lock_guard<std::mutex> lock(settlements_mutex);
    assert(settlements.size() == 1);
    assert(settlements[0] == kDequeueLines);
    settlements.clear();
  }
  assert(std::memcmp(output, payload, sizeof(payload)) == 0);
  {
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
    assert(ring->size() == 0);  // head/tail/count transitions intact
  }

  latency_sim::detail::SetDelaySpinBackendForTest(nullptr);
  sim.ClearPoolRegistrations();
  star::CXLMemory::clear_dual_region_allocator();
  unlink(path_template);
  std::printf("ring_latency_test ok\n");
  return 0;
#endif
}
