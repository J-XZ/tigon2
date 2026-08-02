#include "kv/engine/mem_access.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

int main() {
  alignas(64) std::atomic<uint64_t> value{0};
  constexpr uint64_t kIterations = 2'000'000;
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    checksum += tigonkv::engine::mem_access::HwccAtomicFetchAdd(
        value, uint64_t{1}, std::memory_order_relaxed);
    if ((i & 63u) == 0)
      checksum += tigonkv::engine::mem_access::HwccAtomicLoad(
          value, std::memory_order_relaxed);
  }
  std::printf("hardware_sim_disabled_benchmark features=%u value=%llu checksum=%llu\n",
              latency_sim::HardwareSimulationFeaturesFast(),
              static_cast<unsigned long long>(value.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(checksum));
  return value.load(std::memory_order_relaxed) == kIterations ? 0 : 1;
}
