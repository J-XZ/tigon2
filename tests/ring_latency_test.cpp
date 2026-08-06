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
// Line model, derived from the actual per-access implementation (ring object
// 64-byte aligned, entry_struct_size=64, entry_num=8, one cache line per
// wrapped access):
//   ctor header: 7 wrapped stores (3 size fields + 3 atomics +
//                entries offset)                              = 7 lines
//   per entry  : ready store + metadata memset + payload
//                memset                                       = 3 lines
//   enqueue    : 4 header loads + count/tail fetch-add +
//                payload copy + 2 metadata stores + ready
//                store                                        = 10 lines
//   dequeue    : 4 header loads + head/tail loads + ready
//                load + 2 metadata loads + payload copy +
//                2 metadata stores + ready store + head
//                store + count fetch-sub                      = 15 lines
// A 2-line payload (70 bytes) charges one extra line in both enqueue and
// dequeue, which catches both whole-entry-envelope overcharging (fixed line
// count regardless of payload) and per-entry undercharging (payload always
// charged once).
#include "common/CXLMemory.h"
#include "common/BufferedReader.h"
#include "common/MPSCRingBuffer.h"
#include "protocol/TwoPLPasha/TwoPLPashaMessage.h"
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
  constexpr uint64_t kCtorHeaderLines = 7;
  constexpr uint64_t kCtorEntryLines = 3;
  constexpr uint64_t kEnqueueLines = 10;
  constexpr uint64_t kDequeueLines = 15;
  constexpr uint64_t kSizeLines = 2;

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
  {
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
    // size() charges exactly the two real head/tail wrapped loads; a fake
    // header read would push the delta past kSizeLines.
    const uint64_t before_size = sim.PendingDelayNsForTest();
    assert(ring->size() == 0);
    const uint64_t size_delta = sim.PendingDelayNsForTest() - before_size;
    assert(size_delta == kSizeLines);
  }
  {
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
    // Header accessors return one real wrapped load each.
    const uint64_t before = sim.PendingDelayNsForTest();
    assert(ring->get_entry_num() == kEntryNum);
    assert(sim.PendingDelayNsForTest() - before == 1);
    const uint64_t before_size = sim.PendingDelayNsForTest();
    assert(ring->get_entry_size() == kEntryStructSize - 9);
    assert(sim.PendingDelayNsForTest() - before_size == 1);
  }

  // Multi-line payload on a 128-byte-entry ring: 70 bytes covers two cache
  // lines, so enqueue/dequeue must charge one additional line each over the
  // single-line payload (catches envelope approximation and per-entry
  // under-charging).  The deltas are asserted relative to the same ring's
  // single-line baseline so the per-access model stays the source of truth.
  char wide[70];
  std::memset(wide, 0x5A, sizeof(wide));
  star::MPSCRingBuffer *wide_ring = nullptr;
  {
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
    const uint64_t before_alloc = sim.PendingDelayNsForTest();
    void *wide_probe = star::cxl_memory.cxlalloc_malloc_wrapper(
        sizeof(star::MPSCRingBuffer),
        star::CXLMemory::TRANSPORT_ALLOCATION);
    const uint64_t alloc_charge = sim.PendingDelayNsForTest() - before_alloc;
    void *wide_storage = star::cxl_memory.cxlalloc_malloc_wrapper(
        sizeof(star::MPSCRingBuffer),
        star::CXLMemory::TRANSPORT_ALLOCATION);
    const uint64_t before_ctor = sim.PendingDelayNsForTest();
    wide_ring = new (wide_storage) star::MPSCRingBuffer(128, 4);
    const uint64_t ctor_delta =
        sim.PendingDelayNsForTest() - before_ctor - alloc_charge;
    // 128-byte entries: the 119-byte payload memset covers two lines, so each
    // entry charges ready(1) + metadata(1) + payload(2) = 4 lines.
    assert(ctor_delta == kCtorHeaderLines + 4 * (kCtorEntryLines + 1));
  }
  {
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
    const uint64_t before_small = sim.PendingDelayNsForTest();
    assert(wide_ring->enqueue(wide, 5));
    const uint64_t small_enqueue_delta =
        sim.PendingDelayNsForTest() - before_small;
    assert(small_enqueue_delta == kEnqueueLines);
    char small_out[128]{};
    const uint64_t before_small_deq = sim.PendingDelayNsForTest();
    assert(wide_ring->dequeue(small_out, sizeof(small_out)) == 5);
    const uint64_t small_dequeue_delta =
        sim.PendingDelayNsForTest() - before_small_deq;
    assert(small_dequeue_delta == kDequeueLines);
    assert(std::memcmp(small_out, wide, 5) == 0);

    const uint64_t before_wide = sim.PendingDelayNsForTest();
    assert(wide_ring->enqueue(wide, sizeof(wide)));
    const uint64_t wide_enqueue_delta =
        sim.PendingDelayNsForTest() - before_wide;
    assert(wide_enqueue_delta == small_enqueue_delta + 1);
    char wide_out[128]{};
    const uint64_t before_wide_deq = sim.PendingDelayNsForTest();
    assert(wide_ring->dequeue(wide_out, sizeof(wide_out)) == sizeof(wide));
    const uint64_t wide_dequeue_delta =
        sim.PendingDelayNsForTest() - before_wide_deq;
    assert(wide_dequeue_delta == small_dequeue_delta + 1);
    assert(std::memcmp(wide_out, wide, sizeof(wide)) == 0);
  }
  {
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
    // A second full cycle charges identically: no residual metadata, no
    // duplicated head/tail/count transitions.
    const uint64_t before_enqueue = sim.PendingDelayNsForTest();
    assert(ring->enqueue(payload, sizeof(payload)));
    const uint64_t enqueue_delta =
        sim.PendingDelayNsForTest() - before_enqueue;
    assert(enqueue_delta == kEnqueueLines);
    char second_out[256]{};
    const uint64_t before_dequeue = sim.PendingDelayNsForTest();
    assert(ring->dequeue(second_out, sizeof(second_out)) == sizeof(payload));
    const uint64_t dequeue_delta =
        sim.PendingDelayNsForTest() - before_dequeue;
    assert(dequeue_delta == kDequeueLines);
    assert(std::memcmp(second_out, payload, sizeof(payload)) == 0);
  }

  {
    // Real demux path: BufferedReader::next_message() pulls through recv(),
    // which is exactly the size()+dequeue() combination the demuxer uses.
    // The combined ledger must equal size-lines + dequeue-lines with no
    // missed or duplicated charge between the two.
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
    star::Message frame;
    const auto frame_size = star::TwoPLPashaMessageFactory::
        new_data_migration_message(
            frame, 0, 3, payload, sizeof(payload), /*transaction_id=*/1,
            /*key_offset=*/0);
    (void)frame_size;
    const uint64_t before_send = sim.PendingDelayNsForTest();
    assert(wide_ring->send(frame.get_raw_ptr(), frame.data.size()));
    const uint64_t send_delta = sim.PendingDelayNsForTest() - before_send;
    // The 81-byte frame covers two entry lines on the 128-byte-entry ring:
    // enqueue charges kEnqueueLines + 1.
    assert(send_delta == kEnqueueLines + 1);

    star::BufferedReader reader(*wide_ring);
    const uint64_t before_recv = sim.PendingDelayNsForTest();
    auto message = reader.next_message();
    const uint64_t recv_delta = sim.PendingDelayNsForTest() - before_recv;
    // recv() must equal size() + dequeue() exactly: the combination must not
    // double-charge size() nor skip dequeue metadata.  size() was measured
    // separately above (kSizeLines == 2); a standalone dequeue of the same
    // two-line-payload entry charges kDequeueLines + 1.  The demux
    // combination is therefore kSizeLines + kDequeueLines + 1 with no missed
    // or duplicated line.
    assert(recv_delta == kSizeLines + kDequeueLines + 1);
    assert(message != nullptr);
    assert(message->get_message_count() == 1);
    assert(reader.get_read_call_cnt() == 1);
  }

  latency_sim::detail::SetDelaySpinBackendForTest(nullptr);
  sim.ClearPoolRegistrations();
  star::CXLMemory::clear_dual_region_allocator();
  unlink(path_template);
  std::printf("ring_latency_test ok\n");
  return 0;
#endif
}
