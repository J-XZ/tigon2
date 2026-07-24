#include "kv/engine/latency_inject.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace latency_sim {
namespace {

std::atomic<bool> g_instrumentation_enabled{false};
std::atomic<uint64_t> g_simulator_generation{0};

std::once_flag g_tsc_calibration_once;
std::atomic<double> g_tsc_ticks_per_ns{0.0};

uint64_t NextSimulatorGeneration() {
  const uint64_t generation =
      g_simulator_generation.fetch_add(1, std::memory_order_relaxed) + 1;
  if (generation == 0) {
    std::abort();
  }
  return generation;
}

void CpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}

uint64_t ReadTsc() {
#if defined(__x86_64__) || defined(__i386__)
  return __rdtsc();
#else
  return 0;
#endif
}

void CalibrateTscOnce() {
  std::call_once(g_tsc_calibration_once, [] {
#if defined(__x86_64__) || defined(__i386__)
    constexpr auto kCalibrationWindow = std::chrono::milliseconds(4);
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t c0 = ReadTsc();
    auto t1 = t0;
    do {
      CpuRelax();
      t1 = std::chrono::steady_clock::now();
    } while (t1 - t0 < kCalibrationWindow);
    const uint64_t c1 = ReadTsc();
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    if (ns > 0 && c1 > c0) {
      g_tsc_ticks_per_ns.store(static_cast<double>(c1 - c0) /
                                   static_cast<double>(ns),
                               std::memory_order_release);
    }
#endif
  });
}

void DelaySpinNs(uint64_t ns) {
  if (ns == 0) {
    return;
  }
  CalibrateTscOnce();
  const double ticks_per_ns =
      g_tsc_ticks_per_ns.load(std::memory_order_acquire);
  if (ticks_per_ns <= 0.0) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    return;
  }
#if defined(__x86_64__) || defined(__i386__)
  const uint64_t start = ReadTsc();
  const uint64_t ticks =
      std::max<uint64_t>(1, static_cast<uint64_t>(ticks_per_ns * ns));
  const uint64_t target = start + ticks;
  while (ReadTsc() < target) {
    CpuRelax();
  }
#else
  std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
#endif
}

uint64_t RoundNs(double ns) {
  if (ns <= 0.0) {
    return 0;
  }
  return static_cast<uint64_t>(std::llround(ns));
}

uint64_t XorShift64(uint64_t *state) {
  uint64_t x = *state;
  if (x == 0) {
    x = 0x9e3779b97f4a7c15ULL;
  }
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

uint64_t LineDelayNs(const Config &cfg, PoolKind pool, AccessKind kind) {
  if (pool == PoolKind::kSwcc) {
    switch (kind) {
    case AccessKind::kRead:
    case AccessKind::kAtomicLoad:
      return RoundNs(cfg.swcc_read_ns_per_line);
    case AccessKind::kWrite:
    case AccessKind::kAtomicStore:
    case AccessKind::kAtomicRmw:
      return RoundNs(cfg.swcc_write_ns_per_line);
    case AccessKind::kFlush:
      return RoundNs(cfg.swcc_flush_ns_per_line);
    }
  }
  switch (kind) {
  case AccessKind::kRead:
    return RoundNs(cfg.hwcc_read_ns_per_line);
  case AccessKind::kWrite:
    return RoundNs(cfg.hwcc_write_ns_per_line);
  case AccessKind::kAtomicLoad:
    return RoundNs(cfg.hwcc_atomic_load_ns);
  case AccessKind::kAtomicStore:
    return RoundNs(cfg.hwcc_atomic_store_ns);
  case AccessKind::kAtomicRmw:
    return RoundNs(cfg.hwcc_atomic_rmw_ns);
  case AccessKind::kFlush:
    return RoundNs(cfg.hwcc_write_ns_per_line);
  }
  return 0;
}

struct CacheLineEntry {
  uint64_t tag = 0;
  bool valid = false;
};

struct CacheState {
  uint64_t capacity_lines = 0;
  uint64_t associativity = 0;
  uint64_t set_count = 0;
  uint64_t set_mask = 0;
  std::vector<CacheLineEntry> entries;

  void Reset(uint64_t capacity, uint64_t assoc) {
    capacity_lines = std::max<uint64_t>(1, capacity);
    associativity = std::max<uint64_t>(1, assoc);
    if (associativity > capacity_lines) {
      associativity = capacity_lines;
    }
    set_count = std::max<uint64_t>(1, capacity_lines / associativity);
    set_mask = ((set_count & (set_count - 1)) == 0) ? (set_count - 1) : 0;
    entries.assign(static_cast<size_t>(set_count * associativity), {});
  }

  bool Access(uint64_t tag) {
    if (entries.empty()) {
      Reset(1, 1);
    }
    const uint64_t set = set_mask != 0 ? (tag & set_mask) : (tag % set_count);
    CacheLineEntry *set_entries =
        entries.data() + static_cast<size_t>(set * associativity);
    uint64_t insert_pos = associativity - 1;
    for (uint64_t i = 0; i < associativity; ++i) {
      auto &e = set_entries[static_cast<size_t>(i)];
      if (e.valid && e.tag == tag) {
        if (i != 0) {
          const CacheLineEntry hit = e;
          for (uint64_t j = i; j > 0; --j) {
            set_entries[static_cast<size_t>(j)] =
                set_entries[static_cast<size_t>(j - 1)];
          }
          set_entries[0] = hit;
        }
        return true;
      }
      if (!e.valid) {
        insert_pos = i;
        break;
      }
    }
    for (uint64_t j = insert_pos; j > 0; --j) {
      set_entries[static_cast<size_t>(j)] =
          set_entries[static_cast<size_t>(j - 1)];
    }
    CacheLineEntry inserted;
    inserted.tag = tag;
    inserted.valid = true;
    set_entries[0] = inserted;
    return false;
  }
};

struct ThreadState {
  const LatencySimulator *active = nullptr;
  ScopeKind scope = ScopeKind::kOther;
  uint64_t scope_depth = 0;
  uint64_t generation = 0;
  uint64_t pending_delay_ns = 0;
  uint64_t rng = 0x6a09e667f3bcc909ULL;
  CacheState cache;
};

thread_local std::unordered_map<const LatencySimulator *, ThreadState> g_tls;

ThreadState &StateFor(const LatencySimulator *sim, const Config &cfg,
                      uint64_t generation) {
  ThreadState &state = g_tls[sim];
  if (state.generation != generation) {
    state = ThreadState{};
    state.generation = generation;
    if (cfg.cache_model == CacheModel::kPerThreadLru) {
      state.cache.Reset(cfg.cache_capacity_lines, cfg.cache_associativity);
    }
  }
  return state;
}

bool ScopeEnabled(const Config &cfg, ScopeKind scope) {
  switch (scope) {
  case ScopeKind::kForeground:
    return cfg.foreground_enabled;
  case ScopeKind::kMerge:
    return cfg.merge_enabled;
  case ScopeKind::kOther:
    return true;
  }
  return false;
}

uint64_t MakeTag(PoolKind pool, uint64_t line) {
  return (line << 1) | (pool == PoolKind::kHwcc ? 1ULL : 0ULL);
}

void AddStats(Stats *dst, PoolKind pool, uint64_t raw, uint64_t hits,
              uint64_t misses, uint64_t delayed_ns) {
  if (pool == PoolKind::kSwcc) {
    dst->swcc_raw_line_accesses += raw;
    dst->swcc_cache_hits += hits;
    dst->swcc_cache_misses += misses;
    dst->swcc_delayed_ns += delayed_ns;
  } else {
    dst->hwcc_raw_line_accesses += raw;
    dst->hwcc_cache_hits += hits;
    dst->hwcc_cache_misses += misses;
    dst->hwcc_delayed_ns += delayed_ns;
  }
}

std::mutex &StatsMutex() {
  static std::mutex mu;
  return mu;
}

} // namespace

CacheModel ParseCacheModel(const std::string &value) {
  if (value == "none") {
    return CacheModel::kNone;
  }
  if (value == "fixed_hit_rate") {
    return CacheModel::kFixedHitRate;
  }
  if (value == "per_thread_lru") {
    return CacheModel::kPerThreadLru;
  }
  throw std::invalid_argument("unknown latency cache model: " + value);
}

const char *CacheModelName(CacheModel model) {
  switch (model) {
  case CacheModel::kNone:
    return "none";
  case CacheModel::kFixedHitRate:
    return "fixed_hit_rate";
  case CacheModel::kPerThreadLru:
    return "per_thread_lru";
  }
  return "unknown";
}

LatencySimulator &GlobalLatencySimulator() {
  static LatencySimulator simulator;
  return simulator;
}

bool InstrumentationEnabledFast() {
  return g_instrumentation_enabled.load(std::memory_order_relaxed);
}

LatencySimulator::LatencySimulator(Config config)
    : config_(config), generation_(NextSimulatorGeneration()) {
  g_instrumentation_enabled.store(config.enabled, std::memory_order_relaxed);
}

void LatencySimulator::Configure(Config config) {
  if (config.cache_line_bytes == 0) {
    throw std::invalid_argument(
        "latency simulator cache_line_bytes must be > 0");
  }
  if (config.cache_fixed_hit_rate < 0.0 || config.cache_fixed_hit_rate > 1.0) {
    throw std::invalid_argument(
        "latency simulator cache_fixed_hit_rate must be in [0, 1]");
  }
  if (config.cache_capacity_lines == 0) {
    config.cache_capacity_lines = 1;
  }
  if (config.cache_associativity == 0) {
    config.cache_associativity = 1;
  }
  config_ = config;
  g_instrumentation_enabled.store(config.enabled, std::memory_order_relaxed);
  generation_ = NextSimulatorGeneration();
  if (config.enabled) {
    CalibrateTscOnce();
  }
}

void LatencySimulator::BeginScope(ScopeKind scope) {
  if (!g_instrumentation_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  ThreadState &state = StateFor(this, config_, generation_);
  if (state.active == this) {
    ++state.scope_depth;
    return;
  }
  state.active = ScopeEnabled(config_, scope) ? this : nullptr;
  state.scope = scope;
  state.pending_delay_ns = 0;
  state.scope_depth = state.active == this ? 1 : 0;
}

void LatencySimulator::EndScopeAndDelay() {
  if (!g_instrumentation_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  ThreadState &state = StateFor(this, config_, generation_);
  if (state.active != this) {
    return;
  }
  if (state.scope_depth > 1) {
    --state.scope_depth;
    return;
  }
  const uint64_t delay_ns = state.pending_delay_ns;
  state.pending_delay_ns = 0;
  state.scope_depth = 0;
  state.active = nullptr;
  DelaySpinNs(delay_ns);
}

void LatencySimulator::RecordLine(PoolKind pool, AccessKind kind,
                                  const void *addr) {
  RecordRange(pool, kind, addr, 1);
}

void LatencySimulator::RecordRange(PoolKind pool, AccessKind kind,
                                   const void *addr, uint64_t bytes) {
  if (!g_instrumentation_enabled.load(std::memory_order_relaxed) ||
      bytes == 0 || addr == nullptr) {
    return;
  }
  const Config &cfg = config_;
  ThreadState &state = StateFor(this, cfg, generation_);
  if (state.active != this) {
    return;
  }
  const uint64_t line_bytes = cfg.cache_line_bytes;
  const uintptr_t start_addr = reinterpret_cast<uintptr_t>(addr);
  const uintptr_t end_addr = start_addr + static_cast<uintptr_t>(bytes - 1);
  const uint64_t first_line = start_addr / line_bytes;
  const uint64_t last_line = end_addr / line_bytes;
  const uint64_t raw_count = last_line - first_line + 1;
  const uint64_t miss_delay_ns = LineDelayNs(cfg, pool, kind);
  const uint64_t hit_delay_ns = RoundNs(cfg.cache_hit_extra_ns);

  if (cfg.cache_model == CacheModel::kNone) {
    const uint64_t delayed_ns = raw_count * miss_delay_ns;
    state.pending_delay_ns += delayed_ns;
    if (cfg.stats_enabled) {
      std::lock_guard<std::mutex> lock(StatsMutex());
      AddStats(&stats_, pool, raw_count, 0, raw_count, delayed_ns);
    }
    return;
  }

  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t delayed_ns = 0;
  for (uint64_t line = first_line; line <= last_line; ++line) {
    bool hit = false;
    switch (cfg.cache_model) {
    case CacheModel::kNone:
      __builtin_unreachable();
    case CacheModel::kFixedHitRate: {
      const double unit = static_cast<double>(XorShift64(&state.rng) >> 11) *
                          (1.0 / 9007199254740992.0);
      hit = unit < cfg.cache_fixed_hit_rate;
      break;
    }
    case CacheModel::kPerThreadLru:
      hit = state.cache.Access(MakeTag(pool, line));
      break;
    }
    if (hit) {
      if (cfg.cache_hits_enabled) {
        ++hits;
        delayed_ns += hit_delay_ns;
      } else {
        ++misses;
        delayed_ns += miss_delay_ns;
      }
    } else {
      ++misses;
      delayed_ns += miss_delay_ns;
    }
  }
  state.pending_delay_ns += delayed_ns;
  if (cfg.stats_enabled) {
    std::lock_guard<std::mutex> lock(StatsMutex());
    AddStats(&stats_, pool, raw_count, hits, misses, delayed_ns);
  }
}

Stats LatencySimulator::SnapshotStats() const {
  std::lock_guard<std::mutex> lock(StatsMutex());
  return stats_;
}

Stats LatencySimulator::TakeStatsAndReset() {
  std::lock_guard<std::mutex> lock(StatsMutex());
  Stats out = stats_;
  stats_ = Stats{};
  return out;
}

uint64_t LatencySimulator::PendingDelayNsForTest() const {
  const auto it = g_tls.find(this);
  if (it == g_tls.end()) {
    return 0;
  }
  return it->second.pending_delay_ns;
}

} // namespace latency_sim
