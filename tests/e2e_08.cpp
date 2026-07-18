#include "kv/kv_store.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

using namespace tigonkv;

namespace {
Config ConfigFor(const std::string &path, uint32_t node) {
  Config c;
  c.shared_memory_path = path;
  c.size_mb = 2048;
  c.hwcc_size_mb = 1024;
  c.swcc_offset_mb = 1024;
  c.swcc_size_mb = 1024;
  c.vm_count = 2;
  c.node_id = node;
  c.partition_count = 64;
  c.fixed_key_size = 8;
  c.fixed_value_size = 8;
  return c;
}
std::string Key8(uint32_t i) {
  char buf[9]; std::snprintf(buf, sizeof(buf), "%08u", i); return buf;
}
}

int main() {
  const std::string path = "/tmp/tigonkv-e2e-08-" + std::to_string(getpid());
  std::remove(path.c_str());
  auto owner = KVStore::Create(ConfigFor(path, 0), true);
  auto remote = KVStore::Create(ConfigFor(path, 1), false);
  constexpr uint32_t kKeys = 100000;
  const auto fill_begin = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < kKeys; ++i) {
    const std::string key = Key8(i);
    assert(owner->Put(key, key).ok());
  }
  const auto fill_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - fill_begin).count();

  std::mt19937 rng(8080);
  std::vector<uint64_t> sampled_latencies;
  const auto read_begin = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < kKeys; ++i) {
    uint32_t n = rng() % kKeys;
    const std::string key = Key8(n);
    const auto op_begin = std::chrono::steady_clock::now();
    auto result = (i & 1) ? remote->Get(key) : owner->Get(key);
    assert(result.status.ok() && result.value == key);
    if ((i % 100) == 0)
      sampled_latencies.push_back(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - op_begin).count()));
  }
  const auto read_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - read_begin).count();
  auto scan = owner->Scan("00000000", kKeys);
  assert(scan.status.ok() && scan.items.size() == kKeys);

  const auto stress_begin = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < kKeys; ++i) {
    auto status = (i & 1) ? remote->Delete(Key8(i)) : owner->Delete(Key8(i));
    assert(status.ok());
  }
  assert(owner->Memory().active_shared_rows == 0);
  assert(owner->Runtime().private_swcc_flushes == 0);
  assert(owner->Runtime().shared_swcc_flushes > 0);
  assert(owner->Memory().unclassified_shared_bytes == 0);
  assert(owner->Memory().logical_hwcc_used_bytes <= owner->Memory().logical_hwcc_capacity_bytes);
  const auto stress_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - stress_begin).count();
  std::sort(sampled_latencies.begin(), sampled_latencies.end());
  const auto percentile = [&](double p) {
    return sampled_latencies[static_cast<size_t>(p * (sampled_latencies.size() - 1))];
  };
  std::cout << "E2E_08_PHASE_TIME_US " << fill_us << "\n";
  std::cout << "E2E_08_OP_LATENCY_US " << read_us / kKeys << "\n";
  std::cout << "E2E_08_OP_P50_US " << percentile(0.50) << "\n";
  std::cout << "E2E_08_OP_P99_US " << percentile(0.99) << "\n";
  std::cout << "E2E_08_STRESS_TIME_US " << stress_us << "\n";
  std::cout << "E2E_08_MEMORY active_shared_rows=" << owner->Memory().active_shared_rows
            << " owner_private_swcc_used_bytes=" << owner->Memory().owner_private_swcc_used_bytes
            << " shared_payload_swcc_used_bytes=" << owner->Memory().shared_payload_swcc_used_bytes << "\n";
  std::cout << owner->DumpStats();
  remote.reset(); owner.reset(); std::remove(path.c_str());
  return 0;
}
