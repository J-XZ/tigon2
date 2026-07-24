#include "kv/engine/kv_engine.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

tigonkv::Config ConfigFor(const std::string &path, uint32_t vm_count = 1,
                           uint32_t node_id = 0) {
  tigonkv::Config config;
  config.shared_memory_path = path;
  config.size_mb = 16;
  config.hwcc_offset_mb = 0;
  config.hwcc_size_mb = 4;
  config.swcc_offset_mb = 4;
  config.swcc_size_mb = 12;
  config.hw_cc_budget_mb = 4;
  config.vm_count = vm_count;
  config.node_id = node_id;
  config.partition_count = 8;
  config.fixed_key_size = 32;
  config.fixed_value_size = 128;
  config.foreground_worker_count_per_vm = 1;
  config.transport_ring_total_mb = 1;
  return config;
}

}  // namespace

int main() {
  char path_template[] = "/tmp/tigonkv-engine-XXXXXX";
  const int fd = mkstemp(path_template);
  assert(fd >= 0);
  close(fd);
  const std::string path(path_template);

  const auto single_owner = ConfigFor(path);
  {
    auto engine = tigonkv::engine::KVEngine::Open(single_owner, true);
    assert(engine->Put("alpha", "one").ok());
    assert(engine->Put("alpha", "updated").ok());
    const auto found = engine->Get("alpha");
    assert(found.status.ok() && found.value == "updated");
    const auto cas = engine->CompareExchange("alpha", "updated", "cas-value");
    assert(cas.status.ok() && cas.exchanged);
    const auto cas_failed = engine->CompareExchange("alpha", "updated", "ignored");
    assert(cas_failed.status.code == tigonkv::StatusCode::kCompareFailed && !cas_failed.exchanged);
    assert(engine->Put("counter", "1").ok());
    const auto incremented = engine->Increment("counter", 2);
    assert(incremented.status.ok() && incremented.value == 3);
    const auto scan = engine->Scan("alpha", 0);
    assert(scan.status.ok() && scan.items.size() == 2);
    assert(scan.items[0].key == "alpha" && scan.items[0].value == "cas-value");
    assert(scan.items[1].key == "counter" && scan.items[1].value == "3");
    assert(engine->Delete("alpha").ok());
    assert(engine->Get("alpha").status.code == tigonkv::StatusCode::kNotFound);
    assert(engine->Put("persist", "value").ok());
    assert(engine->Checkpoint().ok());
  }
  {
    auto attached = tigonkv::engine::KVEngine::Open(single_owner, false);
    const auto found = attached->Get("persist");
    assert(found.status.ok() && found.value == "value");
  }
  unlink(path.c_str());

  char routed_template[] = "/tmp/tigonkv-engine-route-XXXXXX";
  const int routed_fd = mkstemp(routed_template);
  assert(routed_fd >= 0);
  close(routed_fd);
  const std::string routed_path(routed_template);
  const auto node_zero = ConfigFor(routed_path, 2, 0);
  {
    auto engine = tigonkv::engine::KVEngine::Open(node_zero, true);
    for (uint32_t i = 0; i < 100; ++i) {
      const std::string key = "route-" + std::to_string(i);
      const uint32_t partition = engine->PartitionForKey(key);
      assert(engine->OwnerForKey(key) == partition % node_zero.vm_count);
      if (engine->OwnerForKey(key) == 1) break;
    }

    std::string owner_zero_key;
    for (uint32_t i = 0; i < 100; ++i) {
      const std::string key = "cross-node-" + std::to_string(i);
      if (engine->OwnerForKey(key) == 0) { owner_zero_key = key; break; }
    }
    assert(!owner_zero_key.empty());
    std::string owner_one_key;
    for (uint32_t i = 0; i < 100; ++i) {
      const std::string key = "scan-node-one-" + std::to_string(i);
      if (engine->OwnerForKey(key) == 1) { owner_one_key = key; break; }
    }
    assert(!owner_one_key.empty());
    assert(engine->Put(owner_zero_key, "owner-zero").ok());
    constexpr uint32_t kRemoteScanRows = 1024;
    uint32_t remote_scan_rows = 0;
    for (uint32_t i = 0; remote_scan_rows < kRemoteScanRows; ++i) {
      const std::string key = "scan-bulk-" + std::to_string(i);
      if (engine->OwnerForKey(key) != 0) continue;
      assert(engine->Put(key, "bulk").ok());
      ++remote_scan_rows;
    }
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
      auto node_one = tigonkv::engine::KVEngine::Open(ConfigFor(routed_path, 2, 1), false);
      if (!node_one->Put(owner_one_key, "owner-one").ok()) _exit(1);
      const auto distributed_scan = node_one->Scan("", 0);
      const auto limited_scan = node_one->Scan("", 17);
      bool saw_owner_zero = false;
      bool saw_owner_one = false;
      for (const auto &item : distributed_scan.items) {
        saw_owner_zero = saw_owner_zero || (item.key == owner_zero_key && item.value == "owner-zero");
        saw_owner_one = saw_owner_one || (item.key == owner_one_key && item.value == "owner-one");
      }
      uint32_t bulk_seen = 0;
      for (const auto &item : distributed_scan.items)
        bulk_seen += item.value == "bulk";
      if (!distributed_scan.status.ok() || !saw_owner_zero || !saw_owner_one ||
          bulk_seen != kRemoteScanRows) _exit(2);
      if (!limited_scan.status.ok() || limited_scan.items.size() != 17) _exit(12);
      for (size_t i = 1; i < limited_scan.items.size(); ++i) {
        if (limited_scan.items[i - 1].key >= limited_scan.items[i].key) _exit(13);
      }
      if (!node_one->Put(owner_zero_key, "forwarded").ok()) _exit(3);
      const auto read = node_one->Get(owner_zero_key);
      if (!read.status.ok() || read.value != "forwarded") _exit(4);
      const uint64_t tx_after_promotion = node_one->NetworkTxBytes();
      const auto cas = node_one->CompareExchange(owner_zero_key, "forwarded", "cas-forwarded");
      if (!cas.status.ok() || !cas.exchanged) _exit(5);
      const auto cas_miss = node_one->CompareExchange(owner_zero_key, "forwarded", "ignored");
      if (cas_miss.status.code != tigonkv::StatusCode::kCompareFailed || cas_miss.exchanged) _exit(6);
      // The GET above promoted this row.  Both CAS operations must use the
      // non-owner shared fast path rather than send another fixed transport frame.
      if (node_one->NetworkTxBytes() != tx_after_promotion) _exit(11);
      std::string counter_key;
      for (uint32_t i = 0; i < 100; ++i) {
        const std::string candidate = "cross-counter-" + std::to_string(i);
        if (node_one->OwnerForKey(candidate) == 0) { counter_key = candidate; break; }
      }
      if (counter_key.empty() || !node_one->Put(counter_key, "1").ok()) _exit(7);
      const auto increment = node_one->Increment(counter_key, 2);
      if (!increment.status.ok() || increment.value != 3) _exit(8);
      if (!node_one->Delete(owner_zero_key).ok()) _exit(9);
      if (node_one->Get(owner_zero_key).status.code != tigonkv::StatusCode::kNotFound) _exit(10);
      _exit(0);
    }
    int status = 0;
    for (;;) {
      engine->PollTransport();
      const pid_t done = waitpid(child, &status, WNOHANG);
      if (done == child) break;
      assert(done == 0);
    }
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(engine->NetworkTxBytes() > 0 && engine->NetworkRxBytes() > 0);
    const auto engine_runtime = engine->EngineRuntime();
    assert(engine_runtime.migration_in > 0);
    assert(engine_runtime.shared_swcc_flushes >= engine_runtime.migration_in);
  }
  unlink(routed_path.c_str());
  return 0;
}
