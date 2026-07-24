#include "kv/engine/latency_inject.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

namespace {

latency_sim::Config BaseConfig() {
  latency_sim::Config config;
  config.enabled = true;
  config.stats_enabled = true;
  config.cache_line_bytes = 64;
  config.swcc_read_ns_per_line = 10;
  config.swcc_write_ns_per_line = 20;
  config.swcc_flush_ns_per_line = 30;
  config.hwcc_read_ns_per_line = 40;
  config.hwcc_write_ns_per_line = 50;
  config.hwcc_atomic_load_ns = 60;
  config.hwcc_atomic_store_ns = 70;
  config.hwcc_atomic_rmw_ns = 80;
  return config;
}

}  // namespace

int main() {
  auto disabled = BaseConfig();
  disabled.enabled = false;
  latency_sim::LatencySimulator simulator(disabled);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead,
                        reinterpret_cast<void *>(0x1001), 128);
  assert(simulator.PendingDelayNsForTest() == 0);
  assert(simulator.SnapshotStats().swcc_raw_line_accesses == 0);

  simulator.Configure(BaseConfig());
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead,
                        reinterpret_cast<void *>(0x1000), 1);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead,
                        reinterpret_cast<void *>(0x103f), 2);
  assert(simulator.PendingDelayNsForTest() == 30);
  const auto ranges = simulator.SnapshotStats();
  assert(ranges.swcc_raw_line_accesses == 3 && ranges.swcc_cache_misses == 3);
  simulator.EndScopeAndDelay();
  assert(simulator.PendingDelayNsForTest() == 0);
  simulator.TakeStatsAndReset();

  auto fixed = BaseConfig();
  fixed.cache_model = latency_sim::CacheModel::kFixedHitRate;
  fixed.cache_fixed_hit_rate = 1.0;
  fixed.cache_hit_extra_ns = 2.0;
  simulator.Configure(fixed);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kHwcc, latency_sim::AccessKind::kRead,
                        reinterpret_cast<void *>(0x3000), 128);
  auto stats = simulator.SnapshotStats();
  assert(stats.hwcc_cache_hits == 2 && stats.hwcc_cache_misses == 0);
  assert(simulator.PendingDelayNsForTest() == 4);
  simulator.EndScopeAndDelay();
  simulator.TakeStatsAndReset();

  auto lru = BaseConfig();
  lru.cache_model = latency_sim::CacheModel::kPerThreadLru;
  lru.cache_capacity_lines = 2;
  lru.cache_associativity = 1;
  lru.cache_hit_extra_ns = 1.0;
  simulator.Configure(lru);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordLine(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead,
                       reinterpret_cast<void *>(0x4000));
  simulator.RecordLine(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead,
                       reinterpret_cast<void *>(0x4000));
  simulator.RecordLine(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead,
                       reinterpret_cast<void *>(0x4080));
  stats = simulator.SnapshotStats();
  assert(stats.swcc_cache_hits == 1 && stats.swcc_cache_misses == 2);
  assert(simulator.TakeStatsAndReset().swcc_raw_line_accesses == 3);
  assert(simulator.SnapshotStats().swcc_raw_line_accesses == 0);

  lru.cache_hits_enabled = false;
  simulator.Configure(lru);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordLine(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead,
                       reinterpret_cast<void *>(0x4400));
  simulator.RecordLine(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead,
                       reinterpret_cast<void *>(0x4400));
  stats = simulator.SnapshotStats();
  assert(stats.swcc_cache_hits == 0 && stats.swcc_cache_misses == 2);
  return 0;
}
