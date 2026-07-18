#include "kv/kv_store.h"
#include "kv/latency_simulator.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace tigonkv;

namespace {
Config TestConfig(const std::string &path, uint32_t node = 0) {
  Config c;
  c.shared_memory_path = path;
  c.size_mb = 16;
  c.hwcc_size_mb = 1;
  c.swcc_offset_mb = 1;
  c.swcc_size_mb = 15;
  c.vm_count = 2;
  c.node_id = node;
  c.partition_count = 8;
  c.fixed_key_size = 32;
  c.fixed_value_size = 128;
  return c;
}

void TestStore() {
  const std::string path = "/tmp/tigonkv-unit-" + std::to_string(getpid());
  std::remove(path.c_str());
  auto c = TestConfig(path);
  auto a = KVStore::Create(c, true);
  assert(a->Put("alpha", "one").ok());
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    auto child_store = KVStore::Create(TestConfig(path, 1), false);
    const bool ok = child_store->Put("child", "process").ok();
    child_store.reset();
    _exit(ok ? 0 : 1);  // do not destruct the parent's inherited C++ object
  }
  int child_status = 0;
  assert(waitpid(child, &child_status, 0) == child);
  assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
  assert(a->Get("child").value == "process");
  assert(a->Get("alpha").value == "one");
  assert(a->Scan("a", 10).items.size() == 2);
  assert(a->CompareExchange("alpha", "one", "two").exchanged);
  assert(a->Get("alpha").value == "two");
  assert(a->Increment("counter", 3).value == 3);
  assert(a->Increment("counter", -1).value == 2);
  const uint64_t reclaimed_before = a->Memory().reclaimed_total_bytes;
  assert(a->Delete("alpha").ok());
  const auto reclaimed_after = a->Memory();
  assert(reclaimed_after.retired_pending_bytes == 0);
  assert(reclaimed_after.reclaimed_total_bytes > reclaimed_before);
  assert(a->Get("alpha").status.code == StatusCode::kNotFound);
  assert(a->Put("alpha", "reinsert").ok());
  assert(a->Get("alpha").value == "reinsert");
  std::vector<std::thread> workers;
  for (int t = 0; t < 4; ++t) workers.emplace_back([&a, t] {
    for (int i = 0; i < 25; ++i) {
      assert(a->Put("concurrent-" + std::to_string(t) + "-" + std::to_string(i), "v").ok());
    }
  });
  for (auto &worker : workers) worker.join();
  assert(a->Checkpoint().ok());
  auto before = a->Memory();
  assert(before.unclassified_shared_bytes == 0);
  a.reset();
  auto b = KVStore::Create(c, false);
  assert(b->Get("alpha").value == "reinsert");
  b.reset();
  std::remove(path.c_str());
}

void TestPromotionAndAccessClasses() {
  const std::string path = "/tmp/tigonkv-promotion-" + std::to_string(getpid());
  std::remove(path.c_str());
  auto c0 = TestConfig(path, 0);
  auto c1 = TestConfig(path, 1);
  auto a = KVStore::Create(c0, true);
  std::string key;
  for (int i = 0; i < 100; ++i) {
    std::string candidate = "private-" + std::to_string(i);
    if (a->OwnerForKey(candidate) == 0) { key = candidate; break; }
  }
  assert(!key.empty());
  assert(a->Put(key, "private-value").ok());
  const auto private_before = a->Runtime();
  assert(private_before.private_swcc_flushes == 0);
  auto b = KVStore::Create(c1, false);
  assert(b->Get(key).value == "private-value");
  assert(b->Runtime().migration_in == 1);
  assert(b->Put(key, "shared-value").ok());
  assert(b->Runtime().shared_swcc_flushes > 0);
  assert(a->Get(key).value == "shared-value");
  assert(a->Memory().active_shared_rows == 1);
  assert(a->MoveOut(key).ok());
  assert(a->Memory().active_shared_rows == 0);
  assert(a->Runtime().migration_out == 1);
  assert(a->Get(key).value == "shared-value");
  b.reset(); a.reset(); std::remove(path.c_str());
}

void TestNonOwnerPutStaysPrivateUntilRemoteRead() {
  const std::string path = "/tmp/tigonkv-forward-" + std::to_string(getpid());
  std::remove(path.c_str());
  auto requester = KVStore::Create(TestConfig(path, 0), true);
  std::string key;
  for (int i = 0; i < 100; ++i) {
    if (requester->OwnerForKey("forward-" + std::to_string(i)) == 1) {
      key = "forward-" + std::to_string(i);
      break;
    }
  }
  assert(!key.empty());
  assert(requester->Put(key, "private").ok());
  assert(requester->Memory().active_shared_rows == 0);
  assert(requester->Runtime().private_swcc_flushes == 0);
  assert(requester->Get(key).value == "private");
  assert(requester->Runtime().migration_in == 1);
  assert(requester->Memory().active_shared_rows == 1);
  auto remote = KVStore::Create(TestConfig(path, 1), false);
  assert(remote->Get(key).value == "private");
  assert(remote->Runtime().migration_in == 0);
  assert(remote->Memory().active_shared_rows == 1);
  remote.reset(); requester.reset(); std::remove(path.c_str());
}

void TestCrossProcessPromotionAndUpdate() {
  const std::string path = "/tmp/tigonkv-cross-process-" + std::to_string(getpid());
  std::remove(path.c_str());
  auto owner = KVStore::Create(TestConfig(path, 0), true);
  std::string key;
  for (int i = 0; i < 100; ++i) {
    const std::string candidate = "cross-process-" + std::to_string(i);
    if (owner->OwnerForKey(candidate) == 0) { key = candidate; break; }
  }
  assert(!key.empty());
  assert(owner->Put(key, "generation-0").ok());

  int stats_pipe[2];
  assert(pipe(stats_pipe) == 0);
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    close(stats_pipe[0]);
    try {
      auto remote = KVStore::Create(TestConfig(path, 1), false);
      if (remote->Get(key).status.code != StatusCode::kOk ||
          !remote->Put(key, "generation-1").ok()) {
        remote.reset();
        close(stats_pipe[1]);
        _exit(1);
      }
      const uint64_t shared_flushes = remote->Runtime().shared_swcc_flushes;
      if (write(stats_pipe[1], &shared_flushes, sizeof(shared_flushes)) !=
          static_cast<ssize_t>(sizeof(shared_flushes))) {
        remote.reset();
        close(stats_pipe[1]);
        _exit(1);
      }
      remote.reset();
      close(stats_pipe[1]);
      _exit(0);
    } catch (...) {
      close(stats_pipe[1]);
      _exit(2);
    }
  }
  close(stats_pipe[1]);
  int child_status = 0;
  assert(waitpid(child, &child_status, 0) == child);
  assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
  uint64_t child_shared_flushes = 0;
  assert(read(stats_pipe[0], &child_shared_flushes, sizeof(child_shared_flushes)) ==
         static_cast<ssize_t>(sizeof(child_shared_flushes)));
  close(stats_pipe[0]);
  assert(child_shared_flushes > 0);
  assert(owner->Get(key).value == "generation-1");
  assert(owner->Memory().active_shared_rows == 1);
  assert(owner->Runtime().private_swcc_flushes == 0);
  assert(owner->MoveOut(key).ok());
  assert(owner->Memory().active_shared_rows == 0);
  owner.reset();
  std::remove(path.c_str());
}

void TestDirtyCloseRequiresExplicitReset() {
  const std::string path = "/tmp/tigonkv-dirty-close-" + std::to_string(getpid());
  std::remove(path.c_str());
  auto config = TestConfig(path, 0);
  config.checkpoint_on_clean_exit = false;
  {
    auto store = KVStore::Create(config, true);
    assert(store->Put("dirty", "value").ok());
  }
  bool rejected = false;
  try {
    auto attach = KVStore::Create(config, false);
    (void)attach;
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  assert(rejected);
  auto reset = KVStore::Create(config, true);
  assert(reset->Put("clean-after-reset", "value").ok());
  reset.reset();
  std::remove(path.c_str());
}

void TestNonCoherentBackend() {
  NonCoherentSwccTestBackend backend;
  const char first[] = "new";
  char readback[4] = {};
  backend.Write(0, 0, first, 3);
  assert(!backend.Publish(0, 0, 3));  // early shared-index publication is rejected
  backend.Read(1, 0, readback, 3);
  assert(std::string(readback, 3) != "new");
  backend.WriteBack(0, 0, 3);
  assert(backend.Publish(0, 0, 3));
  assert(backend.IsPublished(0));
  backend.Invalidate(1, 0, 3);
  backend.Read(1, 0, readback, 3);
  assert(std::string(readback, 3) == "new");
  const char second[] = "old";
  backend.Write(0, 0, second, 3);
  backend.WriteBack(0, 0, 3);
  backend.Invalidate(1, 0, 3);
  backend.Read(1, 0, readback, 3);
  assert(std::string(readback, 3) == "old");
}

void TestSwccBudgetAndSortedScan() {
  const std::string path = "/tmp/tigonkv-budget-" + std::to_string(getpid());
  std::remove(path.c_str());
  auto c = TestConfig(path, 0);
  c.size_mb = 4;
  c.hwcc_size_mb = 1;
  c.swcc_offset_mb = 1;
  c.swcc_size_mb = 1;
  c.fixed_value_size = 4096;
  auto store = KVStore::Create(c, true);
  const std::string large(4096, 'b');
  bool saw_oom = false;
  for (int i = 0; i < 1000; ++i) {
    Status status = store->Put("budget-" + std::to_string(i), large);
    if (status.code == StatusCode::kOutOfMemory) { saw_oom = true; break; }
    assert(status.ok());
  }
  assert(saw_oom);
  auto memory = store->Memory();
  assert(memory.owner_private_swcc_used_bytes + memory.shared_payload_swcc_used_bytes <=
         memory.logical_swcc_capacity_bytes);

  store.reset();
  auto ordered = KVStore::Create(TestConfig(path, 0), true);
  for (int i = 0; i < 100; ++i)
    assert(ordered->Put("ordered-" + std::to_string(100 - i), "v").ok());
  auto scan = ordered->Scan("ordered-", 100);
  assert(scan.status.ok());
  for (size_t i = 1; i < scan.items.size(); ++i) assert(scan.items[i - 1].key < scan.items[i].key);
  ordered.reset();
  std::remove(path.c_str());
}

void TestLatencyAndStrictAccess() {
  LatencySimulator simulator;
  simulator.Configure(true, 7, 3, 5, 11);
  simulator.Record(PoolKind::kSwcc, AccessKind::kRead, 64);
  simulator.Record(PoolKind::kSwcc, AccessKind::kFlush, 64);
  simulator.Record(PoolKind::kHwcc, AccessKind::kAtomicRmw, 64);
  const auto stats = simulator.Snapshot();
  assert(stats.swcc_reads == 1 && stats.swcc_flushes == 1 && stats.hwcc_atomics == 1);
  assert(stats.delayed_ns == 21);
  simulator.Reset();
  simulator.ConfigureDetailed(true, 1, 2, 3, 100, 200, 300, "fixed_hit_rate", 1.0, 1);
  simulator.Record(PoolKind::kSwcc, AccessKind::kRead, 64);
  const auto hit_stats = simulator.Snapshot();
  assert(hit_stats.cache_hits == 1 && hit_stats.cache_misses == 0 && hit_stats.delayed_ns == 0);
  simulator.Reset();
  simulator.ConfigureDetailed(true, 1, 2, 3, 100, 200, 300, "per_thread_lru", 0.0, 1);
  simulator.Record(PoolKind::kSwcc, AccessKind::kRead, 64);
  simulator.Record(PoolKind::kSwcc, AccessKind::kRead, 64);
  const auto lru_stats = simulator.Snapshot();
  assert(lru_stats.cache_hits == 1 && lru_stats.cache_misses == 1 && lru_stats.delayed_ns == 100);

  const std::string path = "/tmp/tigonkv-strict-" + std::to_string(getpid());
  std::remove(path.c_str());
  auto owner_config = TestConfig(path, 0);
  auto strict_config = TestConfig(path, 1);
  strict_config.strict_swcc_access = true;
  auto owner = KVStore::Create(owner_config, true);
  std::string key;
  for (int i = 0; i < 100; ++i) if (owner->OwnerForKey(std::to_string(i)) == 0) { key = std::to_string(i); break; }
  assert(owner->Put(key, "v").ok());
  auto strict = KVStore::Create(strict_config, false);
  assert(strict->Get(key).status.code == StatusCode::kOwnerViolation);
  strict.reset(); owner.reset(); std::remove(path.c_str());
}

void TestConfig() {
  const std::string path = "/tmp/tigonkv-config-" + std::to_string(getpid()) + ".jsonc";
  std::ofstream out(path);
  out << R"({"shared_memory":{"size_mb":16,"path":"/tmp/x","device_path":"/dev/ivpci0","hwcc":{"offset_mb":0,"size_mb":1},"swcc":{"offset_mb":1,"size_mb":15}},"vm":{"count":2},"tigon_kv":{"partition_count":8,"fixed_key_size":32,"fixed_value_size":128,"hwcc_budget_mb":1,"enable_scan":true,"latency_inject":{"cache_model":"fixed_hit_rate","cache_fixed_hit_rate":0.5,"cache_capacity_lines":4}}})";
  out.close();
  Config c = Config::FromJsonc(path);
  assert(c.size_mb == 16 && c.vm_count == 2 && c.hwcc_size_mb == 1);
  assert(c.latency_cache_model == "fixed_hit_rate" && c.latency_cache_fixed_hit_rate == 0.5 && c.latency_cache_capacity_lines == 4);
  std::remove(path.c_str());

  std::ofstream bad(path);
  bad << R"({"unknown_field": 1})";
  bad.close();
  bool rejected = false;
  try { (void)Config::FromJsonc(path); } catch (const std::invalid_argument &) { rejected = true; }
  assert(rejected);
  std::remove(path.c_str());
}
}  // namespace

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  TestConfig();
  TestStore();
  TestPromotionAndAccessClasses();
  TestNonOwnerPutStaysPrivateUntilRemoteRead();
  TestCrossProcessPromotionAndUpdate();
  TestDirtyCloseRequiresExplicitReset();
  TestNonCoherentBackend();
  TestSwccBudgetAndSortedScan();
  TestLatencyAndStrictAccess();
  std::cout << "tigonkv_tests: passed\n";
  return 0;
}
