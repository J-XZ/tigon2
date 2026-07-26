#include "kv/kv_store.h"
#include "kv/engine/latency_inject.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace tigonkv;

namespace {

bool RelWithDebInfoBuild() {
#ifdef TIGONKV_CMAKE_BUILD_TYPE
  return std::string_view(TIGONKV_CMAKE_BUILD_TYPE) == "RelWithDebInfo";
#else
  return false;
#endif
}

bool ValidateThrows(const Config &config) {
  try {
    config.Validate();
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

}  // namespace

int main() {
  const std::string latency_config_path =
      "/tmp/tigonkv-latency-config-" + std::to_string(getpid()) + ".jsonc";
  {
    std::ifstream source(std::string(TIGONKV_SOURCE_DIR) +
                         "/experiment_config.jsonc");
    assert(source.good());
    std::string text((std::istreambuf_iterator<char>(source)),
                     std::istreambuf_iterator<char>());
    const std::string needle = "\"swcc_read_ns_per_line\": 0";
    const size_t position = text.find(needle);
    assert(position != std::string::npos);
    text.replace(position, needle.size(),
                 "\"swcc_read_ns_per_line\": 1.25");
    std::ofstream output(latency_config_path);
    output << text;
  }
  const Config fractional = Config::FromJsonc(latency_config_path);
  assert(fractional.swcc_read_ns == 1.25);
  assert(fractional.cpu_affinity);
  std::remove(latency_config_path.c_str());

  Config uneven;
  uneven.size_mb = 16;
  uneven.hwcc_size_mb = 4;
  uneven.swcc_offset_mb = 4;
  uneven.swcc_size_mb = 12;
  uneven.hw_cc_budget_mb = 4;
  uneven.vm_count = 2;
  uneven.node_id = 1;
  uneven.partition_count = 7;
  uneven.transport_ring_total_mb = 1;
  uneven.Validate();
  const std::string path = "/tmp/tigonkv-facade-" + std::to_string(getpid());
  std::remove(path.c_str());
  Config config;
  config.shared_memory_path = path;
  config.size_mb = 16;
  config.hwcc_size_mb = 4;
  config.swcc_offset_mb = 4;
  config.swcc_size_mb = 12;
  config.hw_cc_budget_mb = 4;
  config.vm_count = 1;
  config.partition_count = 8;
  config.fixed_key_size = 32;
  config.fixed_value_size = 128;
  config.foreground_worker_count_per_vm = 4;
  config.transport_ring_total_mb = 1;
  Config insufficient_cpu = config;
  insufficient_cpu.cpu_affinity = true;
  insufficient_cpu.vm_core_count_per_vm = 4;
  assert(ValidateThrows(insufficient_cpu));
  Config gated = config;
  gated.latency_enabled = true;
  if (RelWithDebInfoBuild()) {
    gated.verbose = true;
    assert(ValidateThrows(gated));
    gated.verbose = false;
    gated.extra_check = true;
    assert(ValidateThrows(gated));
  } else {
    assert(ValidateThrows(gated));
  }
  config.latency_enabled = RelWithDebInfoBuild();
  config.latency_foreground_enabled = true;
  config.latency_stats_enabled = true;
  config.swcc_read_ns = 1;
  config.swcc_write_ns = 1;
  auto store = KVStore::Create(config, true);
  assert(store->Put("alpha", "one").ok());
  assert(store->Put("beta", "two").ok());
  const auto scan = store->Scan("alpha", 0);
  assert(scan.status.ok() && scan.items.size() == 2);
  assert(store->CompareExchange("alpha", "one", "three").exchanged);
  assert(store->Increment("counter", 3).value == 3);
  assert(store->Get("alpha").value == "three");
  std::vector<std::thread> workers;
  for (uint32_t worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&, worker] {
      store->BindWorker(worker);
      for (uint32_t i = 0; i < 50; ++i) {
        const std::string key =
            "worker-" + std::to_string(worker) + "-" + std::to_string(i);
        assert(store->Put(key, "value").ok());
        const auto found = store->Get(key);
        assert(found.status.ok() && found.value == "value");
      }
    });
  }
  for (auto &worker : workers) worker.join();
  const RuntimeStats runtime = store->Runtime();
  assert(runtime.logical_ops == 406);
  assert(runtime.commits == 406);
  assert(runtime.aborts == 0);
  assert(runtime.private_puts == 204);
  assert(runtime.private_gets == 201);
  assert(store->Checkpoint().ok());
  assert(store->Memory().physical_region_split);
  const std::string stats = store->DumpStats();
  assert(stats.find("allocator_shared_overhead_bytes=") != std::string::npos);
  assert(stats.find("reclaimed_total_bytes=") != std::string::npos);
  assert(stats.find("network_tx_bytes=") != std::string::npos);
  if (RelWithDebInfoBuild()) {
    assert(stats.find("\nswcc_raw=0\n") == std::string::npos);
    assert(stats.find("\nswcc_misses=0\n") == std::string::npos);
  } else {
    assert(stats.find("\nswcc_raw=0\n") != std::string::npos);
    assert(stats.find("\nhwcc_raw=0\n") != std::string::npos);
  }
  store.reset();
  auto attached = KVStore::Create(config, false);
  assert(attached->Get("alpha").value == "three");
  assert(attached->Delete("beta").ok());
  std::remove(path.c_str());
  return 0;
}
