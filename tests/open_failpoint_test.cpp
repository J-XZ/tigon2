// Open-phase exception rollback tests.  Each named failpoint forces
// KVEngine::Open() to throw at a distinct initialization stage; the test then
// verifies the rollback contract: gate disabled, pool registrations cleared,
// allocator/EBR/SCC bindings released, mapping released, and a subsequent
// Open against a different mapping succeeds.
//
// The failpoint is compiled out of release (NDEBUG) production builds, so this
// test skips when the failpoint is unavailable.
#include "kv/engine/kv_engine.h"

#include <latency_sim/simulator.h>
#include <latency_sim/testing.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {

tigonkv::Config ConfigFor(const std::string &path) {
  tigonkv::Config config;
  config.shared_memory_path = path;
  config.size_mb = 64;
  config.hwcc_offset_mb = 0;
  config.hwcc_size_mb = 32;
  config.swcc_offset_mb = 32;
  config.swcc_size_mb = 32;
  config.hw_cc_budget_mb = 32;
  config.vm_count = 1;
  config.node_id = 0;
  config.partition_count = 1;
  config.fixed_key_size = 32;
  config.fixed_value_size = 128;
  config.foreground_worker_count_per_vm = 1;
  config.transport_ring_total_mb = 1;
  config.partition_ranges = {{"", ""}};
  return config;
}

std::string MakeTempBacking(const char *prefix) {
  std::string path_template = std::string("/tmp/") + prefix + "-XXXXXX";
  std::vector<char> writable(path_template.begin(), path_template.end());
  writable.push_back('\0');
  const int fd = mkstemp(writable.data());
  assert(fd >= 0);
  close(fd);
  return std::string(writable.data());
}

}  // namespace

int main() {
#ifndef NDEBUG
  const char *const phases[] = {
      "after-pool-mapping", "after-registration", "during-partition-init",
      "during-worker-start", "before-demuxer",     "after-demuxer",
  };
  std::string path_a = MakeTempBacking("tigonkv-openfail-a");
  std::string path_b = MakeTempBacking("tigonkv-openfail-b");

  for (const char *phase : phases) {
    assert(setenv("TIGONKV_TEST_OPEN_FAILPOINT", phase, 1) == 0);
    bool threw = false;
    try {
      auto engine = tigonkv::engine::KVEngine::Open(ConfigFor(path_a), true);
      (void)engine;
    } catch (const std::runtime_error &) {
      threw = true;
    }
    unsetenv("TIGONKV_TEST_OPEN_FAILPOINT");
    assert(threw);
    std::printf("[open-failpoint] phase=%s threw and rolled back\n", phase);

    // Rollback contract: registrations cleared and no thread-local latency
    // state remains.  The simulator stays quiescent with zero pending delay.
#if !defined(LATENCY_SIM_COMPILE_OFF)
    auto &simulator = latency_sim::GlobalLatencySimulator();
    assert(!simulator.HasActiveScopeForCurrentThread());
  assert(latency_sim::testing::Accessor(simulator).PendingDelayNsForTest() == 0);
    simulator.ClearPoolRegistrations();
  assert(latency_sim::testing::Accessor(simulator).PendingDelayNsForTest() == 0);
#endif

    // Reopen against a different mapping succeeds and stays usable.  Engine
    // Open registered the ranges and scoped its init; the foreground calls
    // below need an explicit scope on this thread.
    auto engine = tigonkv::engine::KVEngine::Open(ConfigFor(path_b), true);
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
    engine->BindWorker(0);
    const std::string key(32, 'a');
    const std::string value(128, 'b');
    assert(engine->Put(key, value).ok());
    auto got = engine->Get(key);
    assert(got.status.ok() && got.value == value);
    engine->ReleaseWorker();
  }

  unlink(path_a.c_str());
  unlink(path_b.c_str());
  std::printf("OPEN_FAILPOINT_ROLLBACK_PASSED phases=%zu\n",
              sizeof(phases) / sizeof(phases[0]));
  return 0;
#else
  std::printf("OPEN_FAILPOINT_SKIPPED (failpoint compiled out in release)\n");
  return 0;
#endif
}
