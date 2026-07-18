#include "kv/kv_store.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <unistd.h>

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
  const auto read_begin = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < kKeys; ++i) {
    uint32_t n = rng() % kKeys;
    const std::string key = Key8(n);
    auto result = (i & 1) ? remote->Get(key) : owner->Get(key);
    assert(result.status.ok() && result.value == key);
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
  const auto stress_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - stress_begin).count();
  std::cout << "E2E_08_PHASE_TIME_US " << fill_us << "\n";
  std::cout << "E2E_08_OP_LATENCY_US " << read_us / kKeys << "\n";
  std::cout << "E2E_08_STRESS_TIME_US " << stress_us << "\n";
  std::cout << "E2E_08_MEMORY active_shared_rows=" << owner->Memory().active_shared_rows
            << " owner_private_swcc_used_bytes=" << owner->Memory().owner_private_swcc_used_bytes
            << " shared_payload_swcc_used_bytes=" << owner->Memory().shared_payload_swcc_used_bytes << "\n";
  std::cout << owner->DumpStats();
  remote.reset(); owner.reset(); std::remove(path.c_str());
  return 0;
}
