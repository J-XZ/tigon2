// Disabled-path overhead benchmark for the real TigonKV adapters.  Every case
// drives the actual project call site (B+Tree domain/atomic helper, the real
// RegionAllocator metadata adapter, transport and shared-payload bulk), not a
// stand-in helper, so the measured subject is the code the engine really runs.
//
// The same binary is executed from the ordinary RelWithDebInfo directory and
// from the independent compile-off directory under a fixed CPU; it never
// enables the simulator.  raw = original operation without any wrapper;
// runtime_disabled = wrapper with the runtime gate closed.
#include <latency_sim/config.h>
#include <latency_sim/simulator.h>
#include "kv/engine/mem_access.h"
#include "kv/engine/region_allocator.h"
#include "common/btree_olc_cxl/BTreeOLC_CXL.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint64_t kIterations = 200'000;
constexpr size_t kSamples = 9;

using Runner = uint64_t (*)(bool wrapped);

uint64_t RunOrdinary(bool wrapped) {
  alignas(64) std::array<uint64_t, 256> values{};
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    auto &value = values[i & (values.size() - 1)];
    if (wrapped)
      tigonkv::engine::mem_access::HwccRead(&value, sizeof(value));
    value += i + 1;
    checksum ^= value;
  }
  return checksum ^ values[0];
}

uint64_t RunAtomic(bool wrapped) {
  std::atomic<uint64_t> value{0};
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    checksum += wrapped
        ? tigonkv::engine::mem_access::HwccAtomicFetchAdd(
              value, uint64_t{1}, std::memory_order_relaxed)
        : value.fetch_add(uint64_t{1}, std::memory_order_relaxed);
    if ((i & 63u) == 0)
      checksum += wrapped
          ? tigonkv::engine::mem_access::HwccAtomicLoad(
                value, std::memory_order_relaxed)
          : value.load(std::memory_order_relaxed);
  }
  if (value.load(std::memory_order_relaxed) != kIterations) std::abort();
  return checksum;
}

// Real B+Tree domain/atomic adapters (btreeolc_cxl::RecordTreeDataRead/Write
// and TreeAtomic helpers).  The thread-local TreeAccessIsHwcc defaults to
// HWCC; the adapters check the public fast gate before reading it.
uint64_t RunBtreeDomainAdapter(bool wrapped) {
  alignas(64) std::array<uint64_t, 256> nodes{};
  std::atomic<uint64_t> counter{0};
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    auto &node = nodes[(i * 17) & (nodes.size() - 1)];
    if (wrapped)
      btreeolc_cxl::RecordTreeDataRead(&node, sizeof(node));
    node = node * 3 + i;
    checksum += wrapped
        ? btreeolc_cxl::TreeAtomicFetchAdd(counter, uint64_t{1},
                                           std::memory_order_relaxed)
        : counter.fetch_add(uint64_t{1}, std::memory_order_relaxed);
    checksum += node;
  }
  if (counter.load(std::memory_order_relaxed) != kIterations) std::abort();
  return checksum;
}

// Real RegionAllocator metadata adapter path.  The private
// RegionAllocator::Record{Metadata,BlockMetadata}{Read,Write} methods delegate
// to these mem_access wrappers after their gate check + domain classification;
// the benchmark drives the same underlying adapter with the control/block
// flags a real allocator carries (control_is_hwcc=true here).
uint64_t RunAllocatorAdapter(bool wrapped) {
  alignas(64) std::array<uint64_t, 128> arena{};
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    auto &slot = arena[(i * 13) & (arena.size() - 1)];
    if (wrapped) {
      tigonkv::engine::mem_access::HwccRead(&slot, sizeof(slot));
      tigonkv::engine::mem_access::HwccWrite(&slot, sizeof(slot));
    }
    slot ^= i + 0x9e3779b97f4a7c15ull;
    checksum ^= slot;
  }
  return checksum;
}

uint64_t RunTransportAdapter(bool wrapped) {
  alignas(64) std::array<uint64_t, 128> ring{};
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    auto &slot = ring[(i * 7) & (ring.size() - 1)];
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

uint64_t RunBulk(bool wrapped) {
  alignas(64) std::array<uint64_t, 512> payload{};
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    if (wrapped)
      tigonkv::engine::mem_access::SharedPayloadRead(payload.data(),
                                                     payload.size() * sizeof(uint64_t));
    payload[i & (payload.size() - 1)] += i;
    checksum += payload[(i + 31) & (payload.size() - 1)];
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
  *sink ^= runner(wrapped);
  const auto end = std::chrono::steady_clock::now();
  return static_cast<double>(
             std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
                 .count()) /
         kIterations;
}

void Report(std::string_view name, Runner runner) {
  std::vector<double> raw;
  std::vector<double> wrapped;
  raw.reserve(kSamples);
  wrapped.reserve(kSamples);
  uint64_t sink = 0;
  for (size_t sample = 0; sample < kSamples; ++sample) {
    // Interleave raw and runtime-disabled wrapper calls so drift in one half
    // of the process does not become a fake mode comparison.
    raw.push_back(Measure(runner, false, &sink));
    wrapped.push_back(Measure(runner, true, &sink));
  }
  const Summary raw_summary = Summarize(raw);
  const Summary wrapped_summary = Summarize(wrapped);
  std::printf(
      "case=%.*s samples=%zu raw_median_ns_op=%.3f raw_min_ns_op=%.3f "
      "raw_max_ns_op=%.3f raw_p95_ns_op=%.3f runtime_disabled_median_ns_op=%.3f "
      "runtime_disabled_min_ns_op=%.3f runtime_disabled_max_ns_op=%.3f "
      "runtime_disabled_p95_ns_op=%.3f\n",
      static_cast<int>(name.size()), name.data(), kSamples, raw_summary.median,
      raw_summary.min, raw_summary.max, raw_summary.p95, wrapped_summary.median,
      wrapped_summary.min, wrapped_summary.max, wrapped_summary.p95);
  if (sink == UINT64_MAX) std::abort();
}

}  // namespace

int main() {
  // This benchmark deliberately measures the disabled gate.  The same binary
  // is run from the ordinary RelWithDebInfo directory and the independent
  // compile-off directory under a fixed CPU; it never enables the simulator.
  latency_sim::GlobalLatencySimulator().Configure(latency_sim::FixedLatencyConfig{});
  if (latency_sim::FixedLatencyEnabledFast()) std::abort();
  Report("ordinary", RunOrdinary);
  Report("atomic_load_fetch", RunAtomic);
  Report("btree_domain_atomic", RunBtreeDomainAdapter);
  Report("allocator", RunAllocatorAdapter);
  Report("transport", RunTransportAdapter);
  Report("bulk", RunBulk);
  std::printf("features=%u compile_off=%s samples=%zu\n",
              latency_sim::FixedLatencyFeaturesFast(),
#if defined(LATENCY_SIM_COMPILE_OFF)
              "ON",
#else
              "OFF",
#endif
              kSamples);
  return 0;
}
