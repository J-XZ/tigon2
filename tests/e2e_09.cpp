#include "kv/kv_store.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

#include "e2e_vm_workflow.h"
#include <unistd.h>

using namespace tigonkv;

namespace {
std::string EnvOr(const char *name, const std::string &fallback) {
  const char *value = std::getenv(name);
  return value && *value ? value : fallback;
}

Config ConfigFor(const std::string &path, uint32_t node) {
  Config c;
  c.shared_memory_path = path;
  c.device_path = EnvOr("TIGONKV_E2E_DEVICE", "/dev/ivpci0");
  c.size_mb = std::stoull(EnvOr("TIGONKV_E2E_SIZE_MB", "2048"));
  c.hwcc_size_mb = 1024;
  c.swcc_offset_mb = 1024;
  c.swcc_size_mb = 1024;
  c.vm_count = 2;
  c.node_id = node;
  c.partition_count = 64;
  c.fixed_key_size = 32;
  c.fixed_value_size = 1000;
  return c;
}
std::string Key(uint32_t i) { return "key-" + std::to_string(i); }
std::string Value(uint32_t generation, uint32_t i) {
  std::string value(1000, static_cast<char>('a' + generation));
  std::string suffix = std::to_string(i);
  value.replace(0, suffix.size(), suffix);
  return value;
}
}

int main() {
  if (std::getenv("TIGONKV_E2E_MULTI_VM") != nullptr)
    return tigonkv::e2e_vm_workflow::RunE2E09MultiVm();
  const std::string path = EnvOr("TIGONKV_E2E_BACKING", "/tmp/tigonkv-e2e-09-" + std::to_string(getpid()));
  std::remove(path.c_str());
  auto owner = KVStore::Create(ConfigFor(path, 0), true);
  auto remote = KVStore::Create(ConfigFor(path, 1), false);
  constexpr uint32_t kKeys = 100000;
  for (uint32_t i = 0; i < kKeys; ++i) assert(owner->Put(Key(i), Value(0, i)).ok());

  const auto update_begin = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < kKeys; ++i) assert(remote->Put(Key(i), Value(1, i)).ok());
  const auto update_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - update_begin).count();
  std::mt19937 rng(9090);
  for (uint32_t i = 0; i < kKeys; ++i) {
    const uint32_t n = rng() % kKeys;
    auto result = remote->Get(Key(n));
    assert(result.status.ok() && result.value == Value(1, n));
  }
  auto shared = remote->Memory();
  assert(shared.active_shared_rows > 0 && shared.shared_payload_swcc_used_bytes > 0);
  for (uint32_t i = 0; i < kKeys; ++i) assert(owner->Delete(Key(i)).ok());
  assert(owner->Memory().active_shared_rows == 0);
  assert(owner->Memory().shared_payload_swcc_used_bytes == 0);
  assert(owner->Runtime().private_swcc_flushes == 0);
  assert(owner->Runtime().shared_swcc_flushes > 0);
  assert(owner->Memory().unclassified_shared_bytes == 0);
  assert(owner->Memory().logical_hwcc_used_bytes <= owner->Memory().logical_hwcc_capacity_bytes);
  std::cout << "E2E_09_PHASE_TIME_US " << update_us << "\n";
  std::cout << "E2E_09_MEMORY active_shared_rows=" << owner->Memory().active_shared_rows
            << " shared_payload_swcc_used_bytes=" << owner->Memory().shared_payload_swcc_used_bytes << "\n";
  std::cout << owner->DumpStats();
  remote.reset(); owner.reset(); std::remove(path.c_str());
  return 0;
}
