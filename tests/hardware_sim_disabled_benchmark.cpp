// Fixed-latency overhead benchmark for the real TigonKV adapters, run as two
// independent binaries selected by the build (no runtime mode argument):
//   compile-on + 0ns   (default build)   prints variant=compile_on_zero
//   compile-off        (LATENCY_SIM_COMPILE_OFF=ON)   prints variant=compile_off
//
// Every case drives the actual project call site (B+Tree domain/atomic
// helper, the real RegionAllocator metadata adapter, transport and
// shared-payload bulk), not a stand-in helper, so the measured subject is the
// code the engine really runs.  All measured addresses lie inside the two
// registered pool ranges (one HWCC + one SWCC) and the wrapped loops run
// inside an explicit mem_access::LatencyScope.  The primary comparison is the
// same wrapped code under a 0/0 fixed-latency model (compile-on) versus the
// raw compile-off build; a raw (unwrapped) baseline on the same addresses is
// also reported for the cases that have one.  In a compile-on build the
// simulator is always active once configured, so the wrapped path is the only
// runtime variant (compile-off wrappers compile to the raw operations).
#include <latency_sim/config.h>
#include <latency_sim/simulator.h>
#include "kv/engine/mem_access.h"
#include "kv/engine/region_allocator.h"
#include "common/btree_olc_cxl/BTreeOLC_CXL.h"
#include "tests/latency_test_support.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint64_t kIterations = 200'000;
constexpr size_t kSamples = 9;
constexpr size_t kPoolBytes = 8 * 1024 * 1024;
constexpr size_t kAllocRegionBytes = 4 * 1024 * 1024;

// The registered HWCC/SWCC pool buffers.  Every measured address comes from
// one of these two ranges.
std::byte *g_hwcc = nullptr;
std::byte *g_swcc = nullptr;

using Runner = uint64_t (*)(bool wrapped);

// Ordinary HwccRead/HwccWrite: one 8-byte slot touched on a covered HWCC line.
uint64_t RunOrdinary(bool wrapped) {
  auto *values = reinterpret_cast<uint64_t *>(g_hwcc);
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    auto &value = values[i & 255u];
    if (wrapped) {
      tigonkv::engine::mem_access::HwccRead(&value, sizeof(value));
      tigonkv::engine::mem_access::HwccWrite(&value, sizeof(value));
    }
    value += i + 1;
    checksum ^= value;
  }
  return checksum ^ values[0];
}

// Atomic fetch/load on a real std::atomic inside the HWCC pool.
uint64_t RunAtomic(bool wrapped) {
  auto *value = new (g_hwcc) std::atomic<uint64_t>{0};
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    checksum += wrapped
        ? tigonkv::engine::mem_access::HwccAtomicFetchAdd(
              *value, uint64_t{1}, std::memory_order_relaxed)
        : value->fetch_add(uint64_t{1}, std::memory_order_relaxed);
    if ((i & 63u) == 0)
      checksum += wrapped
          ? tigonkv::engine::mem_access::HwccAtomicLoad(
                *value, std::memory_order_relaxed)
          : value->load(std::memory_order_relaxed);
  }
  if (value->load(std::memory_order_relaxed) != kIterations) std::abort();
  return checksum;
}

// Real B+Tree domain/atomic adapters (btreeolc_cxl::RecordTreeDataRead and
// TreeAtomicFetchAdd).  TreeAccessIsHwcc defaults to HWCC, and both the node
// array and the counter live inside the registered HWCC pool.
uint64_t RunBtreeDomainAdapter(bool wrapped) {
  auto *nodes = reinterpret_cast<uint64_t *>(g_hwcc);
  auto *counter =
      new (g_hwcc + 4096) std::atomic<uint64_t>{0};
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    auto &node = nodes[(i * 17) & 255u];
    if (wrapped) btreeolc_cxl::RecordTreeDataRead(&node, sizeof(node));
    node = node * 3 + i;
    checksum += wrapped
        ? btreeolc_cxl::TreeAtomicFetchAdd(*counter, uint64_t{1},
                                           std::memory_order_relaxed)
        : counter->fetch_add(uint64_t{1}, std::memory_order_relaxed);
    checksum += node;
  }
  if (counter->load(std::memory_order_relaxed) != kIterations) std::abort();
  return checksum;
}

// The REAL RegionAllocator path: a live RegionAllocator constructed over the
// HWCC pool (control_is_hwcc=true) drives its private
// RecordMetadata{Read,Write} / RecordBlockMetadata{Read,Write} adapters and
// real Allocate/Free.  This path always wraps; there is no raw variant.
uint64_t RunAllocator(bool wrapped) {
  (void)wrapped;
  auto *region = g_hwcc;
  auto *counter = new (g_hwcc + kAllocRegionBytes)
      tigonkv::engine::DomainCounter;
  auto allocator = tigonkv::engine::RegionAllocator::Initialize(
      region, kAllocRegionBytes, 2, 0, /*control_is_hwcc=*/true,
      /*block_is_hwcc=*/true);
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    const uint32_t shard = static_cast<uint32_t>(i & 1u);
    void *block = allocator.Allocate(
        80, tigonkv::engine::AllocationDomain::kHwccMetadata, counter, shard);
    allocator.Free(block, 80,
                   tigonkv::engine::AllocationDomain::kHwccMetadata, counter,
                   shard, shard);
    checksum ^= reinterpret_cast<uintptr_t>(block);
  }
  return checksum;
}

// Transport adapter: TransportRead/Write route to HwccRead/HwccWrite.
uint64_t RunTransportAdapter(bool wrapped) {
  auto *ring = reinterpret_cast<uint64_t *>(g_hwcc);
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    auto &slot = ring[(i * 7) & 127u];
    if (wrapped)
      tigonkv::engine::mem_access::TransportRead(&slot, sizeof(slot));
    const uint64_t message = slot + i;
    if (wrapped)
      tigonkv::engine::mem_access::TransportWrite(&slot, sizeof(slot));
    slot = message;
    checksum += message;
  }
  return checksum;
}

// Shared-payload bulk: SharedPayloadRead covers a 4 KiB SWCC payload block.
uint64_t RunBulk(bool wrapped) {
  auto *payload = reinterpret_cast<uint64_t *>(g_swcc);
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    if (wrapped)
      tigonkv::engine::mem_access::SharedPayloadRead(
          payload, 512 * sizeof(uint64_t));
    payload[i & 511u] += i;
    checksum += payload[(i + 31) & 511u];
  }
  return checksum;
}

struct Summary {
  double median;
  double min;
  double max;
  double p95;
};

Summary Summarize(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const size_t p95_index = (values.size() * 95 + 99) / 100 - 1;
  return {values[values.size() / 2], values.front(), values.back(),
          values[p95_index]};
}

double Measure(Runner runner, bool wrapped, uint64_t *sink) {
  const auto begin = std::chrono::steady_clock::now();
  if (wrapped) {
    // The explicit scope is required by the always-active compile-on
    // simulator; in a compile-off build the scope is a no-op.
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kForeground);
    *sink ^= runner(true);
  } else {
    *sink ^= runner(false);
  }
  const auto end = std::chrono::steady_clock::now();
  return static_cast<double>(
             std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
                 .count()) /
         kIterations;
}

void Report(std::string_view name, Runner runner, bool has_raw) {
  std::vector<double> raw;
  std::vector<double> wrapped;
  raw.reserve(kSamples);
  wrapped.reserve(kSamples);
  uint64_t sink = 0;
  for (size_t sample = 0; sample < kSamples; ++sample) {
    // Interleave raw and wrapped baselines on the same addresses so drift in
    // one half of the process does not become a fake mode comparison.
    if (has_raw) raw.push_back(Measure(runner, false, &sink));
    wrapped.push_back(Measure(runner, true, &sink));
  }
  const Summary wrapped_summary = Summarize(wrapped);
  if (has_raw) {
    const Summary raw_summary = Summarize(raw);
    std::printf(
        "case=%.*s samples=%zu wrapped_median_ns_op=%.3f "
        "wrapped_min_ns_op=%.3f wrapped_max_ns_op=%.3f "
        "wrapped_p95_ns_op=%.3f raw_median_ns_op=%.3f raw_min_ns_op=%.3f "
        "raw_max_ns_op=%.3f raw_p95_ns_op=%.3f\n",
        static_cast<int>(name.size()), name.data(), kSamples,
        wrapped_summary.median, wrapped_summary.min, wrapped_summary.max,
        wrapped_summary.p95, raw_summary.median, raw_summary.min,
        raw_summary.max, raw_summary.p95);
  } else {
    std::printf(
        "case=%.*s samples=%zu wrapped_median_ns_op=%.3f "
        "wrapped_min_ns_op=%.3f wrapped_max_ns_op=%.3f "
        "wrapped_p95_ns_op=%.3f\n",
        static_cast<int>(name.size()), name.data(), kSamples,
        wrapped_summary.median, wrapped_summary.min, wrapped_summary.max,
        wrapped_summary.p95);
  }
  if (sink == UINT64_MAX) std::abort();
}

}  // namespace

int main(int argc, char **argv) {
  const int cpu = argc > 1 ? std::atoi(argv[1]) : -1;
  if (cpu >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(cpu), &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0)
      std::perror("sched_setaffinity");
  }

  void *hwcc_map = mmap(nullptr, kPoolBytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  void *swcc_map = mmap(nullptr, kPoolBytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(hwcc_map != MAP_FAILED && swcc_map != MAP_FAILED);
  g_hwcc = static_cast<std::byte *>(hwcc_map);
  g_swcc = static_cast<std::byte *>(swcc_map);
  std::memset(g_hwcc, 0, kPoolBytes);
  std::memset(g_swcc, 0, kPoolBytes);

#if !defined(LATENCY_SIM_COMPILE_OFF)
  // Compile-on: register exactly one HWCC + one SWCC range and apply the 0/0
  // fixed-latency model.  Once configured the simulator is always active, so
  // the wrapped loops below run inside a scope on these same ranges.  The
  // RAII pools stay registered for the whole measurement.
  latency_sim::FixedLatencyConfig zero;
  zero.cache_line_bytes = 64;
  zero.swcc_fixed_ns_per_line = 0.0;
  zero.hwcc_fixed_ns_per_line = 0.0;
  tigonkv::test::ScopedLatencyPools pools(g_swcc, kPoolBytes, g_hwcc,
                                          kPoolBytes, zero);
#endif

  std::printf(
      "variant=%s cpu=%d samples=%zu iterations=%llu\n",
#if defined(LATENCY_SIM_COMPILE_OFF)
      "compile_off",
#else
      "compile_on_zero",
#endif
      cpu, kSamples, static_cast<unsigned long long>(kIterations));

  Report("ordinary", RunOrdinary, true);
  Report("atomic_fetch_load", RunAtomic, true);
  Report("btree_domain_atomic", RunBtreeDomainAdapter, true);
  Report("allocator", RunAllocator, false);
  Report("transport", RunTransportAdapter, true);
  Report("bulk_shared_payload", RunBulk, true);

  munmap(g_hwcc, kPoolBytes);
  munmap(g_swcc, kPoolBytes);
  return 0;
}
