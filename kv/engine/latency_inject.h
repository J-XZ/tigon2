#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace latency_sim {

enum class PoolKind : uint8_t { kSwcc = 0, kHwcc = 1 };
enum class AccessKind : uint8_t {
  kRead = 0,
  kWrite = 1,
  kAtomicLoad = 2,
  kAtomicStore = 3,
  kAtomicRmw = 4,
  kFlush = 5,
};
enum class CacheModel : uint8_t {
  kNone = 0,
  kFixedHitRate = 1,
  kPerThreadLru = 2,
};
enum class ScopeKind : uint8_t {
  kForeground = 0,
  kMerge = 1,
  kOther = 2,
};
class LatencySimulator;

struct Config {
  bool enabled = false;
  bool foreground_enabled = true;
  bool merge_enabled = true;
  bool stats_enabled = false;
  uint64_t cache_line_bytes = 64;

  double swcc_read_ns_per_line = 0.0;
  double swcc_write_ns_per_line = 0.0;
  double swcc_flush_ns_per_line = 0.0;
  double hwcc_read_ns_per_line = 0.0;
  double hwcc_write_ns_per_line = 0.0;
  double hwcc_atomic_load_ns = 0.0;
  double hwcc_atomic_store_ns = 0.0;
  double hwcc_atomic_rmw_ns = 0.0;

  CacheModel cache_model = CacheModel::kNone;
  bool cache_hits_enabled = true;
  uint64_t cache_capacity_lines = 4096;
  uint64_t cache_associativity = 8;
  double cache_fixed_hit_rate = 0.0;
  double cache_hit_extra_ns = 0.0;
};

struct Stats {
  uint64_t swcc_raw_line_accesses = 0;
  uint64_t hwcc_raw_line_accesses = 0;
  uint64_t swcc_cache_hits = 0;
  uint64_t hwcc_cache_hits = 0;
  uint64_t swcc_cache_misses = 0;
  uint64_t hwcc_cache_misses = 0;
  uint64_t swcc_delayed_ns = 0;
  uint64_t hwcc_delayed_ns = 0;

  uint64_t RawLineAccesses(PoolKind pool) const {
    return pool == PoolKind::kSwcc ? swcc_raw_line_accesses
                                   : hwcc_raw_line_accesses;
  }

  uint64_t CacheHits(PoolKind pool) const {
    return pool == PoolKind::kSwcc ? swcc_cache_hits : hwcc_cache_hits;
  }

  uint64_t CacheMisses(PoolKind pool) const {
    return pool == PoolKind::kSwcc ? swcc_cache_misses : hwcc_cache_misses;
  }

  uint64_t TotalLineAccesses(PoolKind pool) const {
    return CacheHits(pool) + CacheMisses(pool);
  }

  uint64_t DelayedNs(PoolKind pool) const {
    return pool == PoolKind::kSwcc ? swcc_delayed_ns : hwcc_delayed_ns;
  }

  uint64_t TotalDelayedNs() const {
    return swcc_delayed_ns + hwcc_delayed_ns;
  }
};

CacheModel ParseCacheModel(const std::string &value);
const char *CacheModelName(CacheModel model);
LatencySimulator &GlobalLatencySimulator();

// Process-wide relaxed fast gate. Set by LatencySimulator::Configure().
// All hot-path recorders should check this before touching thread-local state.
bool InstrumentationEnabledFast();

class LatencySimulator {
public:
  explicit LatencySimulator(Config config = {});

  void Configure(Config config);
  const Config &config() const { return config_; }

  void BeginScope(ScopeKind scope);
  void EndScopeAndDelay();

  void RecordRange(PoolKind pool, AccessKind kind, const void *addr,
                   uint64_t bytes);
  void RecordLine(PoolKind pool, AccessKind kind, const void *addr);

  Stats SnapshotStats() const;
  Stats TakeStatsAndReset();
  uint64_t PendingDelayNsForTest() const;

private:
  Config config_;
  uint64_t generation_ = 0;
  mutable Stats stats_;
};

} // namespace latency_sim
