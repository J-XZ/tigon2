#include "kv/engine/kv_engine.h"
#include "common/CXLMemory.h"
#include "common/MPSCRingBuffer.h"
#include "kv/engine/kv_messages.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <sys/resource.h>
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
  char corrupt_template[] = "/tmp/tigonkv-engine-corrupt-XXXXXX";
  const int corrupt_fd = mkstemp(corrupt_template);
  assert(corrupt_fd >= 0);
  close(corrupt_fd);
  const pid_t corrupt_child = fork();
  assert(corrupt_child >= 0);
  if (corrupt_child == 0) {
    const rlimit no_core{0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    const auto corrupt_config = ConfigFor(corrupt_template);
    auto engine = tigonkv::engine::KVEngine::Open(corrupt_config, true);
    void *root = nullptr;
    star::CXLMemory::wait_and_retrieve_cxl_shared_data(
        star::CXLMemory::cxl_transport_root_index, &root);
    auto *rings = static_cast<star::MPSCRingBuffer *>(root);
    std::array<char, sizeof(tigonkv::engine::KvMessage) + 1> malformed{};
    while (!rings[0].enqueue(
        malformed.data(), malformed.size()))
      std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    _exit(90);
  }
  int corrupt_status = 0;
  assert(waitpid(corrupt_child, &corrupt_status, 0) == corrupt_child);
  assert(WIFSIGNALED(corrupt_status) && WTERMSIG(corrupt_status) == SIGABRT);
  unlink(corrupt_template);

  char misroute_template[] = "/tmp/tigonkv-engine-misroute-XXXXXX";
  const int misroute_fd = mkstemp(misroute_template);
  assert(misroute_fd >= 0);
  close(misroute_fd);
  const pid_t misroute_child = fork();
  assert(misroute_child >= 0);
  if (misroute_child == 0) {
    const rlimit no_core{0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    const auto config = ConfigFor(misroute_template, 2, 0);
    auto engine = tigonkv::engine::KVEngine::Open(config, true);
    std::string wrong_owner_key;
    for (uint32_t i = 0; i < 1000; ++i) {
      wrong_owner_key = "misroute-" + std::to_string(i);
      if (engine->OwnerForKey(wrong_owner_key) == 1) break;
    }
    void *root = nullptr;
    star::CXLMemory::wait_and_retrieve_cxl_shared_data(
        star::CXLMemory::cxl_transport_root_index, &root);
    auto *rings = static_cast<star::MPSCRingBuffer *>(root);
    auto request = tigonkv::engine::MakeRequest(
        tigonkv::engine::KvMessageType::kPut, 1, 0, 7,
        wrong_owner_key, "value");
    while (!rings[0].enqueue(
        reinterpret_cast<char *>(&request), sizeof(request)))
      std::this_thread::yield();
    for (;;) engine->PollTransport();
  }
  int misroute_status = 0;
  assert(waitpid(misroute_child, &misroute_status, 0) ==
         misroute_child);
  assert(WIFSIGNALED(misroute_status) &&
         WTERMSIG(misroute_status) == SIGABRT);
  unlink(misroute_template);

  char full_template[] = "/tmp/tigonkv-engine-full-XXXXXX";
  const int full_fd = mkstemp(full_template);
  assert(full_fd >= 0);
  close(full_fd);
  const pid_t full_child = fork();
  assert(full_child >= 0);
  if (full_child == 0) {
    const rlimit no_core{0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    auto full_config = ConfigFor(full_template, 2, 0);
    full_config.sync_timeout_sec = 1;
    auto engine = tigonkv::engine::KVEngine::Open(full_config, true);
    void *root = nullptr;
    star::CXLMemory::wait_and_retrieve_cxl_shared_data(
        star::CXLMemory::cxl_transport_root_index, &root);
    auto *rings = static_cast<star::MPSCRingBuffer *>(root);
    tigonkv::engine::KvMessage frame{};
    while (rings[1].enqueue(
        reinterpret_cast<char *>(&frame), sizeof(frame))) {
    }
    std::string remote_key;
    for (uint32_t i = 0; i < 1000; ++i) {
      remote_key = "full-ring-" + std::to_string(i);
      if (engine->OwnerForKey(remote_key) == 1) break;
    }
    (void)engine->Put(remote_key, "value");
    _exit(91);
  }
  int full_status = 0;
  assert(waitpid(full_child, &full_status, 0) == full_child);
  assert(WIFSIGNALED(full_status) && WTERMSIG(full_status) == SIGABRT);
  unlink(full_template);

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
    std::atomic<bool> worker_bound{false};
    std::atomic<bool> release_worker{false};
    std::thread worker_owner([&] {
      engine->BindWorker(0);
      worker_bound.store(true, std::memory_order_release);
      while (!release_worker.load(std::memory_order_acquire))
        std::this_thread::yield();
      engine->ReleaseWorker();
    });
    while (!worker_bound.load(std::memory_order_acquire))
      std::this_thread::yield();
    bool duplicate_worker_rejected = false;
    try {
      engine->BindWorker(0);
    } catch (const std::runtime_error &) {
      duplicate_worker_rejected = true;
    }
    assert(duplicate_worker_rejected);
    release_worker.store(true, std::memory_order_release);
    worker_owner.join();
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
  auto node_zero = ConfigFor(routed_path, 2, 0);
  node_zero.foreground_worker_count_per_vm = 4;
  auto node_one_config = ConfigFor(routed_path, 2, 1);
  node_one_config.foreground_worker_count_per_vm = 4;
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
    // Populate one remote partition past a Scan page boundary. The owner pins
    // each authoritative prefix; the requester reads it only through CXL.
    std::vector<std::string> promoted_scan_keys;
    uint32_t promoted_partition = UINT32_MAX;
    for (uint32_t i = 0; promoted_scan_keys.size() < 130; ++i) {
      char key[32];
      std::snprintf(key, sizeof(key), "hybrid-%08u", i);
      if (engine->OwnerForKey(key) != 0) continue;
      const uint32_t partition = engine->PartitionForKey(key);
      if (promoted_partition == UINT32_MAX) promoted_partition = partition;
      if (partition != promoted_partition) continue;
      assert(engine->Put(key, "owner-authority").ok());
      promoted_scan_keys.emplace_back(key);
    }
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
      auto node_one = tigonkv::engine::KVEngine::Open(node_one_config, false);
      if (!node_one->Put(owner_one_key, "owner-one").ok()) _exit(1);
      for (const auto &key : promoted_scan_keys) {
        const auto promoted = node_one->Get(key);
        if (!promoted.status.ok() || promoted.value != "owner-authority") _exit(14);
      }
      const uint64_t tx_before_authoritative_scan = node_one->NetworkTxBytes();
      const auto authoritative_scan = node_one->Scan("hybrid-", 100);
      const uint64_t authoritative_scan_tx =
          node_one->NetworkTxBytes() - tx_before_authoritative_scan;
      if (!authoritative_scan.status.ok() || authoritative_scan.items.size() != 100)
        _exit(15);
      for (const auto &item : authoritative_scan.items) {
        if (item.key.rfind("hybrid-", 0) != 0 || item.value != "owner-authority")
          _exit(16);
      }
      // One range-migrate request plus one cursor range-migrate request. Values
      // travel through CXL, so no ScanItem frames are sent by this requester.
      if (authoritative_scan_tx != 2 * sizeof(tigonkv::engine::KvMessage)) _exit(17);

      std::atomic<bool> start_concurrent_scans{false};
      std::atomic<bool> concurrent_scan_failed{false};
      std::vector<std::thread> scan_threads;
      for (uint32_t worker = 0; worker < 4; ++worker) {
        scan_threads.emplace_back([&, worker] {
          node_one->BindWorker(worker);
          while (!start_concurrent_scans.load(std::memory_order_acquire))
            std::this_thread::yield();
          const auto scan = node_one->Scan("hybrid-", 100);
          if (!scan.status.ok() || scan.items.size() != 100)
            concurrent_scan_failed.store(true, std::memory_order_release);
          node_one->ReleaseWorker();
        });
      }
      start_concurrent_scans.store(true, std::memory_order_release);
      for (auto &thread : scan_threads) thread.join();
      if (concurrent_scan_failed.load(std::memory_order_acquire)) _exit(18);

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
      const uint64_t tx_after_remote_update = node_one->NetworkTxBytes();
      const auto read = node_one->Get(owner_zero_key);
      if (!read.status.ok() || read.value != "forwarded") _exit(4);
      if (node_one->NetworkTxBytes() != tx_after_remote_update) _exit(24);
      const uint64_t tx_after_promotion = node_one->NetworkTxBytes();
      const auto shared_read = node_one->Get(owner_zero_key);
      if (!shared_read.status.ok() || shared_read.value != "forwarded") _exit(19);
      if (!node_one->Put(owner_zero_key, "shared-put").ok()) _exit(20);
      if (node_one->NetworkTxBytes() != tx_after_promotion) _exit(21);
      const auto cas = node_one->CompareExchange(owner_zero_key, "shared-put", "cas-forwarded");
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
      const uint64_t tx_after_remote_increment = node_one->NetworkTxBytes();
      const auto promoted_counter = node_one->Get(counter_key);
      if (!promoted_counter.status.ok() || promoted_counter.value != "3") _exit(22);
      if (node_one->NetworkTxBytes() != tx_after_remote_increment) _exit(23);
      std::string cas_create_key;
      for (uint32_t i = 0; i < 100; ++i) {
        const std::string candidate = "cross-cas-create-" + std::to_string(i);
        if (node_one->OwnerForKey(candidate) == 0) {
          cas_create_key = candidate;
          break;
        }
      }
      const auto cas_create =
          node_one->CompareExchange(cas_create_key, "", "created");
      if (!cas_create.status.ok() || !cas_create.exchanged) _exit(25);
      const auto created = node_one->Get(cas_create_key);
      if (!created.status.ok() || created.value != "created") _exit(26);
      std::string cas_race_key;
      for (uint32_t i = 0; i < 100; ++i) {
        const std::string candidate = "cross-cas-race-" + std::to_string(i);
        if (node_one->OwnerForKey(candidate) == 0) {
          cas_race_key = candidate;
          break;
        }
      }
      std::atomic<uint32_t> cas_winners{0};
      std::atomic<bool> cas_protocol_failed{false};
      std::vector<std::thread> cas_threads;
      for (uint32_t worker = 0; worker < 4; ++worker) {
        cas_threads.emplace_back([&, worker] {
          node_one->BindWorker(worker);
          const auto result =
              node_one->CompareExchange(cas_race_key, "", "winner");
          if (result.status.ok() && result.exchanged)
            cas_winners.fetch_add(1, std::memory_order_relaxed);
          else if (result.status.code != tigonkv::StatusCode::kCompareFailed)
            cas_protocol_failed.store(true, std::memory_order_relaxed);
          node_one->ReleaseWorker();
        });
      }
      for (auto &thread : cas_threads) thread.join();
      if (cas_winners.load() != 1 || cas_protocol_failed.load()) _exit(27);
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
