#include "kv/engine/latency_inject.h"
#include "kv/engine/mem_access.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>

namespace {

latency_sim::Config Fixed(double swcc, double hwcc) {
  latency_sim::Config config;
  config.fixed_latency.enabled = true;
  config.fixed_latency.cache_line_bytes = 64;
  config.fixed_latency.swcc_fixed_ns_per_line = swcc;
  config.fixed_latency.hwcc_fixed_ns_per_line = hwcc;
  config.fixed_latency.foreground_enabled = true;
  config.fixed_latency.background_enabled = true;
  return config;
}

void ThrowsOnEarlyExit() {
  tigonkv::engine::mem_access::LatencyScope scope(
      latency_sim::ScopeKind::kForeground);
  alignas(64) std::array<std::byte, 128> bytes{};
  tigonkv::engine::mem_access::Record(
      latency_sim::PoolKind::kHwcc, latency_sim::AccessKind::kWrite,
      bytes.data(), bytes.size());
  throw std::runtime_error("scope cleanup");
}

}  // namespace

int main() {
  auto &simulator = latency_sim::GlobalLatencySimulator();
  alignas(64) std::array<std::byte, 256> bytes{};

  // Disabled mode does not enter a scope or create a pending delay.
  simulator.Configure(latency_sim::Config{});
  assert(!latency_sim::FixedLatencyEnabledFast());
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                        latency_sim::AccessKind::kRead, bytes.data(),
                        bytes.size());
  assert(simulator.PendingDelayNsForTest() == 0);
  assert(!simulator.HasActiveScopeForCurrentThread());

  // Real range coverage: zero bytes are free, unaligned and repeated accesses
  // are charged by covered line on every call.
  simulator.Configure(Fixed(7, 13));
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, bytes.data(), 0);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, bytes.data() + 1, 1);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, bytes.data() + 63, 2);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, bytes.data() + 1, 1);
  simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                        latency_sim::AccessKind::kWrite, bytes.data() + 127, 2);
  assert(simulator.PendingDelayNsForTest() == 54);  // 4*7 + 2*13

  // Nested scopes share one worker-local pending delay and settle only at the
  // outer safe point.
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                        latency_sim::AccessKind::kRead, bytes.data(), 1);
  assert(simulator.PendingDelayNsForTest() == 67);
  simulator.EndScopeAndDelay();
  assert(simulator.HasActiveScopeForCurrentThread());
  assert(simulator.PendingDelayNsForTest() == 67);
  simulator.EndScopeAndDelay();
  assert(!simulator.HasActiveScopeForCurrentThread());
  assert(simulator.PendingDelayNsForTest() == 0);

  // Foreground and background workers have independent enable switches.
  auto background_only = Fixed(5, 9);
  background_only.fixed_latency.foreground_enabled = false;
  simulator.Configure(background_only);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, bytes.data(), 64);
  assert(simulator.PendingDelayNsForTest() == 0);
  simulator.EndScopeAndDelay();
  simulator.BeginScope(latency_sim::ScopeKind::kMerge);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, bytes.data(), 64);
  assert(simulator.PendingDelayNsForTest() == 5);
  simulator.EndScopeAndDelay();

  // Atomic wrappers preserve result/expected semantics and charge the actual
  // atomic operation exactly once, regardless of CAS success.
  simulator.Configure(Fixed(3, 11));
  std::atomic<uint64_t> value{7};
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  assert(latency_sim::FixedLatencyAtomicLoad(
             value, std::memory_order_acquire,
             latency_sim::AtomicDomain::kHwcc) == 7);
  latency_sim::FixedLatencyAtomicStore(
      value, uint64_t{8}, std::memory_order_release,
      latency_sim::AtomicDomain::kHwcc);
  assert(latency_sim::FixedLatencyAtomicExchange(
             value, uint64_t{9}, std::memory_order_acq_rel,
             latency_sim::AtomicDomain::kHwcc) == 8);
  assert(latency_sim::FixedLatencyAtomicFetchAdd(
             value, uint64_t{1}, std::memory_order_relaxed,
             latency_sim::AtomicDomain::kHwcc) == 9);
  assert(latency_sim::FixedLatencyAtomicFetchXor(
             value, uint64_t{3}, std::memory_order_relaxed,
             latency_sim::AtomicDomain::kHwcc) == 10);
  uint64_t expected = 100;
  assert(!latency_sim::FixedLatencyAtomicCompareExchangeStrong(
      value, expected, uint64_t{20}, std::memory_order_acq_rel,
      std::memory_order_acquire, latency_sim::AtomicDomain::kHwcc));
  assert(expected == 9);
  expected = 9;
  assert(latency_sim::FixedLatencyAtomicCompareExchangeStrong(
      value, expected, uint64_t{20}, std::memory_order_acq_rel,
      std::memory_order_acquire, latency_sim::AtomicDomain::kHwcc));
  assert(value.load(std::memory_order_relaxed) == 20);
  assert(simulator.PendingDelayNsForTest() == 77);  // seven HWCC operations
  simulator.EndScopeAndDelay();

  // A background worker gets its own scope and pending delay; it cannot leak
  // into the foreground thread.
  simulator.Configure(Fixed(17, 19));
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                        latency_sim::AccessKind::kRead, bytes.data(), 64);
  std::thread worker([&] {
    simulator.BeginScope(latency_sim::ScopeKind::kMerge);
    simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                          latency_sim::AccessKind::kRead, bytes.data(), 64);
    assert(simulator.PendingDelayNsForTest() == 17);
    simulator.EndScopeAndDelay();
  });
  worker.join();
  assert(simulator.PendingDelayNsForTest() == 19);
  simulator.EndScopeAndDelay();

  // RAII settles pending delay on an exception and leaves no active scope.
  simulator.Configure(Fixed(1, 1));
  try {
    ThrowsOnEarlyExit();
  } catch (const std::runtime_error &) {
  }
  assert(!simulator.HasActiveScopeForCurrentThread());
  assert(simulator.PendingDelayNsForTest() == 0);

  simulator.Configure(latency_sim::Config{});
  return 0;
}
