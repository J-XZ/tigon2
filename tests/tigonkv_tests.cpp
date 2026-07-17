#include "kv/kv_store.h"
#include "kv/latency_simulator.h"

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
  assert(a->Delete("alpha").ok());
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
  b.reset(); a.reset(); std::remove(path.c_str());
}

void TestNonCoherentBackend() {
  NonCoherentSwccTestBackend backend;
  const char first[] = "new";
  char readback[4] = {};
  backend.Write(0, 0, first, 3);
  backend.Read(1, 0, readback, 3);
  assert(std::string(readback, 3) != "new");
  backend.WriteBack(0, 0, 3);
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

void TestLatencyAndStrictAccess() {
  LatencySimulator simulator;
  simulator.Configure(true, 7, 3, 5, 11);
  simulator.Record(PoolKind::kSwcc, AccessKind::kRead, 64);
  simulator.Record(PoolKind::kSwcc, AccessKind::kFlush, 64);
  simulator.Record(PoolKind::kHwcc, AccessKind::kAtomicRmw, 64);
  const auto stats = simulator.Snapshot();
  assert(stats.swcc_reads == 1 && stats.swcc_flushes == 1 && stats.hwcc_atomics == 1);
  assert(stats.delayed_ns == 21);

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
  out << R"({"shared_memory":{"size_mb":16,"path":"/tmp/x","device_path":"/dev/ivpci0","hwcc":{"offset_mb":0,"size_mb":1},"swcc":{"offset_mb":1,"size_mb":15}},"vm":{"count":2},"tigon_kv":{"partition_count":8,"fixed_key_size":32,"fixed_value_size":128,"hwcc_budget_mb":1,"enable_scan":true}})";
  out.close();
  Config c = Config::FromJsonc(path);
  assert(c.size_mb == 16 && c.vm_count == 2 && c.hwcc_size_mb == 1);
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
  TestNonCoherentBackend();
  TestLatencyAndStrictAccess();
  std::cout << "tigonkv_tests: passed\n";
  return 0;
}
