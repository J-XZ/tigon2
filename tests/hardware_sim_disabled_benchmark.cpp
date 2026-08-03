#include "kv/engine/latency_inject.h"
#include "kv/engine/mem_access.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

double Run(bool wrapped) {
  constexpr uint64_t kIterations = 200'000;
  std::atomic<uint64_t> value{0};
  uint64_t checksum = 0;
  const auto begin = std::chrono::steady_clock::now();
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
  const auto end = std::chrono::steady_clock::now();
  if (value.load(std::memory_order_relaxed) != kIterations) std::abort();
  if (checksum == UINT64_MAX) std::abort();
  return static_cast<double>(
             std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
                 .count()) /
         kIterations;
}

double Median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

}  // namespace

int main() {
  latency_sim::GlobalLatencySimulator().Configure(latency_sim::Config{});
  constexpr size_t kSamples = 5;
  std::vector<double> direct;
  std::vector<double> wrapped;
  direct.reserve(kSamples);
  wrapped.reserve(kSamples);
  for (size_t i = 0; i < kSamples; ++i) {
    direct.push_back(Run(false));
    wrapped.push_back(Run(true));
  }
  const double direct_median = Median(direct);
  const double wrapped_median = Median(wrapped);
  std::printf(
      "hardware_sim_disabled_benchmark features=%u direct_median_ns_op=%.3f "
      "wrapped_median_ns_op=%.3f direct_min_ns_op=%.3f direct_max_ns_op=%.3f "
      "wrapped_min_ns_op=%.3f wrapped_max_ns_op=%.3f\n",
      latency_sim::FixedLatencyFeaturesFast(), direct_median, wrapped_median,
      *std::min_element(direct.begin(), direct.end()),
      *std::max_element(direct.begin(), direct.end()),
      *std::min_element(wrapped.begin(), wrapped.end()),
      *std::max_element(wrapped.begin(), wrapped.end()));
  return 0;
}
