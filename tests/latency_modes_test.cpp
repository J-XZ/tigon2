#include "kv/engine/latency_inject.h"
#include "kv/engine/mem_access.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <atomic>
#include <array>
#include <cstdint>

namespace {

latency_sim::Config AllModules() {
  latency_sim::Config config;
  config.fixed_latency.enabled = true;
  config.fixed_latency.cache_line_bytes = 64;
  config.fixed_latency.swcc_fixed_ns_per_line = 10;
  config.fixed_latency.hwcc_fixed_ns_per_line = 40;
  config.fixed_latency.foreground_enabled = true;
  config.hwcc_access_count.enabled = true;
  config.hwcc_access_count.cache_line_bytes = 64;
  config.hwcc_access_count.byte_count_enabled = true;
  config.atomic_count.enabled = true;
  config.atomic_count.hwcc_enabled = true;
  config.atomic_count.local_dram_enabled = true;
  config.remote_cache_invalidation.enabled = false;
  return config;
}

}  // namespace

int main() {
  // Production wrappers intentionally target the process-global simulator;
  // exercise that same object so the test cannot hide an instance/global
  // accounting split.
  auto& simulator = latency_sim::GlobalLatencySimulator();
  alignas(64) std::uint64_t words[32]{};

  // Disabled is a genuine fast path: no TLS state or counters are touched.
  simulator.Configure(latency_sim::Config{});
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                        latency_sim::AccessKind::kRead, words, sizeof(words));
  assert(simulator.PendingDelayNsForTest() == 0);
  assert(simulator.SnapshotStats().hwcc_read_ops == 0);

  simulator.Configure(AllModules());
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, words, 1);
  simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                        latency_sim::AccessKind::kRead, words + 8, 65, 3);
  auto stats = simulator.SnapshotStats();
  // Ordinary-access accounting is intentionally scoped to the HWCC module;
  // SWCC fixed-delay traffic is still charged above but is not folded into
  // the HWCC operation counters.
  assert(stats.swcc_read_ops == 0);
  assert(stats.hwcc_read_ops == 1 && stats.hwcc_read_lines == 2);
  assert(stats.hwcc_read_bytes == 65);
  assert(simulator.PendingDelayNsForTest() == 90);  // 1 SWCC + 2 HWCC lines
  simulator.EndScopeAndDelay();
  assert(!simulator.HasActiveScopeForCurrentThread());
  simulator.TakeStatsAndReset();

  // Ordinary accesses and atomics have independent accounting. Atomics are
  // counted only after the standard operation has actually executed.
  simulator.Configure(AllModules());
  std::atomic<std::uint64_t> atomic_value{7};
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  const auto loaded = latency_sim::CountedAtomicLoad(
      atomic_value, std::memory_order_acquire,
      latency_sim::AtomicDomain::kHwcc);
  assert(loaded == 7);
  latency_sim::CountedAtomicStore(atomic_value, std::uint64_t{9}, std::memory_order_release,
                                  latency_sim::AtomicDomain::kHwcc);
  const auto exchanged = latency_sim::CountedAtomicExchange(
      atomic_value, std::uint64_t{11}, std::memory_order_acq_rel,
      latency_sim::AtomicDomain::kHwcc);
  assert(exchanged == 9);
  std::uint64_t expected = 10;
  const bool failed = latency_sim::CountedCompareExchangeStrong(
      atomic_value, expected, std::uint64_t{12}, std::memory_order_acq_rel,
      std::memory_order_acquire, latency_sim::AtomicDomain::kHwcc);
  assert(!failed && expected == 11);
  expected = 11;
  const bool succeeded = latency_sim::CountedCompareExchangeWeak(
      atomic_value, expected, std::uint64_t{12}, std::memory_order_acq_rel,
      std::memory_order_acquire, latency_sim::AtomicDomain::kHwcc);
  assert(succeeded && atomic_value.load() == 12);
  simulator.EndScopeAndDelay();
  stats = simulator.TakeStatsAndReset();
  assert(stats.hwcc_atomic_ops == 5);
  assert(stats.hwcc_atomic_loads == 1 && stats.hwcc_atomic_stores == 1);
  assert(stats.hwcc_exchange_ops == 1 && stats.hwcc_cas_attempts == 2);
  assert(stats.hwcc_cas_successes == 1 && stats.hwcc_cas_failures == 1);
  assert(stats.hwcc_read_ops == 0 && stats.hwcc_write_ops == 0);

  // A module can be enabled without implicitly enabling the other modules.
  auto counts_only = AllModules();
  counts_only.fixed_latency.enabled = false;
  counts_only.atomic_count.enabled = false;
  counts_only.remote_cache_invalidation.enabled = false;
  simulator.Configure(counts_only);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                        latency_sim::AccessKind::kWrite, words, 64);
  simulator.EndScopeAndDelay();
  stats = simulator.TakeStatsAndReset();
  assert(stats.hwcc_write_ops == 1 && stats.hwcc_write_lines == 1);
  assert(simulator.PendingDelayNsForTest() == 0);

  // Atomic domains remain distinct; owner-private SWCC and local DRAM do not
  // enter the HWCC ordinary read/write totals.
  auto atomics_only = AllModules();
  atomics_only.fixed_latency.enabled = false;
  atomics_only.hwcc_access_count.enabled = false;
  atomics_only.atomic_count.owner_private_swcc_enabled = true;
  simulator.Configure(atomics_only);
  simulator.BeginScope(latency_sim::ScopeKind::kMerge);
  const auto owner_loaded = latency_sim::CountedAtomicLoad(
      atomic_value, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kOwnerPrivateSwcc);
  assert(owner_loaded == atomic_value.load(std::memory_order_relaxed));
  latency_sim::CountedAtomicFetchAdd(
      atomic_value, std::uint64_t{1}, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kOwnerPrivateSwcc);
  latency_sim::CountedAtomicFetchXor(
      atomic_value, std::uint64_t{1}, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  simulator.EndScopeAndDelay();
  stats = simulator.TakeStatsAndReset();
  assert(stats.owner_private_swcc_atomic_ops == 2);
  assert(stats.owner_private_swcc_atomic_loads == 1);
  assert(stats.local_dram_atomic_ops == 1);
  assert(stats.hwcc_read_ops == 0 && stats.hwcc_atomic_ops == 0);

  // The remote model is backed by one ordered shared log. A direct replay
  // validates the stable identity/sequence contract without relying on wall
  // time or a second per-process LRU.
  latency_sim::Config remote_config;
  remote_config.remote_cache_invalidation.enabled = true;
  remote_config.remote_cache_invalidation.node_count = 2;
  remote_config.remote_cache_invalidation.cache_size_bytes_per_node = 4096;
  remote_config.remote_cache_invalidation.event_log_capacity = 8;
  remote_config.remote_cache_invalidation.scope_breakdown_enabled = true;
  remote_config.remote_cache_invalidation.tag_breakdown_enabled = true;
  remote_config.remote_cache_invalidation.max_tags = 8;
  std::atomic<uint64_t> sequence{0};
  alignas(64) std::array<latency_sim::RemoteEventRecord, 8> event_log{};
  simulator.Configure(remote_config);
  simulator.SetNodeId(0);
  simulator.AttachSharedRemoteLog(&sequence, event_log.data(), event_log.size());
  // Owner-private SWCC accesses never become remote coherence events.
  tigonkv::engine::mem_access::PrivateRead(words, sizeof(std::uint64_t));
  assert(sequence.load(std::memory_order_acquire) == 0);
  simulator.RecordRemoteRead(
      latency_sim::MemoryIdentity{latency_sim::MemoryDomain::kHwcc, 0, 0},
      1, latency_sim::ScopeKind::kForeground, 3);
  simulator.ValidateSharedRemoteLog();
  assert(sequence.load(std::memory_order_acquire) == 1);
  assert(event_log[0].sequence == 1 && event_log[0].pool_id == 0);
  stats = simulator.SnapshotStats();
  assert(stats.remote_events == 1);
  simulator.DetachSharedRemoteLog(&sequence, event_log.data());
  simulator.Configure(latency_sim::Config{});
  return 0;
}
