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
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace tigonkv;


int main() {
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
  config.transport_ring_total_mb = 1;
  config.latency_enabled = true;
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
  assert(store->Checkpoint().ok());
  assert(store->Memory().physical_region_split);
  const std::string stats = store->DumpStats();
  assert(stats.find("allocator_shared_overhead_bytes=") != std::string::npos);
  assert(stats.find("reclaimed_total_bytes=") != std::string::npos);
  assert(stats.find("network_tx_bytes=") != std::string::npos);
  assert(stats.find("\nswcc_reads=0\n") == std::string::npos);
  assert(stats.find("\nswcc_writes=0\n") == std::string::npos);
  assert(stats.find("\nswcc_flushes=0\n") == std::string::npos);
  store.reset();
  auto attached = KVStore::Create(config, false);
  assert(attached->Get("alpha").value == "three");
  assert(attached->Delete("beta").ok());
  std::remove(path.c_str());
  return 0;
}
