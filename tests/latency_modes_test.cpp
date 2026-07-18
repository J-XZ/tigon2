#include "kv/latency_simulator.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace tigonkv;

int main() {
  LatencySimulator simulator;

  simulator.Configure(false, 7, 3, 5, 11);
  simulator.Record(PoolKind::kSwcc, AccessKind::kRead, 64);
  assert(simulator.Snapshot().delayed_ns == 0);
  simulator.Reset();

  simulator.ConfigureDetailed(true, 1, 2, 3, 100, 200, 300, "none");
  simulator.Record(PoolKind::kHwcc, AccessKind::kRead, 64);
  simulator.Record(PoolKind::kHwcc, AccessKind::kAtomicRmw, 64);
  simulator.Record(PoolKind::kSwcc, AccessKind::kRead, 64);
  simulator.Record(PoolKind::kSwcc, AccessKind::kWrite, 64);
  simulator.Record(PoolKind::kSwcc, AccessKind::kFlush, 64);
  simulator.DrainPending();
  const LatencyStats none = simulator.Snapshot();
  assert(none.hwcc_reads == 1 && none.hwcc_atomics == 1);
  assert(none.swcc_reads == 1 && none.swcc_writes == 1 && none.swcc_flushes == 1);
  assert(none.cache_hits == 0 && none.cache_misses == 1);
  assert(none.delayed_ns == 604);

  simulator.Reset();
  simulator.ConfigureDetailed(true, 1, 2, 3, 100, 200, 300,
                              "fixed_hit_rate", 0.0, 0);
  simulator.Record(PoolKind::kSwcc, AccessKind::kRead, 64);
  assert(simulator.Snapshot().cache_misses == 1);
  simulator.Reset();
  simulator.ConfigureDetailed(true, 1, 2, 3, 100, 200, 300,
                              "fixed_hit_rate", 1.0, 0);
  simulator.Record(PoolKind::kSwcc, AccessKind::kRead, 64);
  assert(simulator.Snapshot().cache_hits == 1 && simulator.Snapshot().delayed_ns == 0);

  simulator.Reset();
  simulator.ConfigureDetailed(true, 1, 2, 3, 100, 200, 300,
                              "per_thread_lru", 0.0, 1);
  simulator.Record(PoolKind::kSwcc, AccessKind::kRead, 64, 1);
  simulator.Record(PoolKind::kSwcc, AccessKind::kRead, 64, 2);
  simulator.Record(PoolKind::kSwcc, AccessKind::kRead, 64, 1);
  const LatencyStats evicted = simulator.Snapshot();
  assert(evicted.cache_hits == 0 && evicted.cache_misses == 3);

  simulator.Reset();
  simulator.ConfigureDetailed(true, 1, 2, 3, 100, 200, 300,
                              "per_thread_lru", 0.0, 2);
  simulator.Record(PoolKind::kSwcc, AccessKind::kRead, 64, 1);
  simulator.Record(PoolKind::kSwcc, AccessKind::kRead, 64, 2);
  simulator.Record(PoolKind::kSwcc, AccessKind::kRead, 64, 1);
  const LatencyStats hit = simulator.Snapshot();
  assert(hit.cache_hits == 1 && hit.cache_misses == 2);
  simulator.Reset();

  return 0;
}
