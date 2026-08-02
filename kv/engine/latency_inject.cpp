#include "kv/engine/latency_inject.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <iterator>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace latency_sim {

struct ThreadState {
  uint64_t generation = 0;
  uint64_t scope_depth = 0;
  ScopeKind scope = ScopeKind::kOther;
  bool scope_enabled = false;
  uint64_t pending_delay_ns = 0;
  Stats stats;
};

namespace {

std::atomic<uint32_t> g_features{0};
std::atomic<uint64_t> g_generation{0};

uint64_t NextGeneration() {
  const uint64_t generation =
      g_generation.fetch_add(1, std::memory_order_relaxed) + 1;
  if (generation == 0) {
    std::abort();
  }
  return generation;
}

void CpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

uint64_t ReadTsc() {
#if defined(__x86_64__) || defined(__i386__)
  return __rdtsc();
#else
  return 0;
#endif
}

std::once_flag g_calibration_once;
double g_ticks_per_ns = 0.0;

void CalibrateTscOnce() {
  std::call_once(g_calibration_once, [] {
#if defined(__x86_64__) || defined(__i386__)
    const auto start_time = std::chrono::steady_clock::now();
    const uint64_t start_tsc = ReadTsc();
    auto now = start_time;
    while (now - start_time < std::chrono::milliseconds(2)) {
      CpuRelax();
      now = std::chrono::steady_clock::now();
    }
    const uint64_t end_tsc = ReadTsc();
    const uint64_t elapsed_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time)
            .count());
    if (elapsed_ns != 0 && end_tsc > start_tsc) {
      g_ticks_per_ns = static_cast<double>(end_tsc - start_tsc) /
                       static_cast<double>(elapsed_ns);
    }
#endif
  });
}

bool HasCalibratedTsc() {
#if defined(__x86_64__) || defined(__i386__)
  return std::isfinite(g_ticks_per_ns) && g_ticks_per_ns > 0.0;
#else
  return false;
#endif
}

void DelaySpinNs(uint64_t ns) {
  if (ns == 0) {
    return;
  }
  CalibrateTscOnce();
#if defined(__x86_64__) || defined(__i386__)
  if (!HasCalibratedTsc()) {
    std::abort();
  }
  const uint64_t start = ReadTsc();
  const uint64_t ticks = std::max<uint64_t>(
      1, static_cast<uint64_t>(g_ticks_per_ns * static_cast<double>(ns)));
  while (ReadTsc() - start < ticks) {
    CpuRelax();
  }
#else
  std::abort();
#endif
}

uint64_t RoundNs(double ns) {
  if (!(ns > 0.0) || !std::isfinite(ns)) {
    return 0;
  }
  if (ns >= static_cast<double>(std::numeric_limits<uint64_t>::max())) {
    return std::numeric_limits<uint64_t>::max();
  }
  return static_cast<uint64_t>(std::llround(ns));
}

uint64_t LineCount(const void* address, uint64_t bytes, uint64_t line_bytes) {
  if (address == nullptr || bytes == 0 || line_bytes == 0) {
    return 0;
  }
  const uintptr_t start = reinterpret_cast<uintptr_t>(address);
  const uintptr_t last = start + static_cast<uintptr_t>(bytes - 1);
  if (last < start) {
    std::abort();
  }
  return last / line_bytes - start / line_bytes + 1;
}

uint64_t AddSaturating(uint64_t left, uint64_t right) {
  if (right > std::numeric_limits<uint64_t>::max() - left) {
    return std::numeric_limits<uint64_t>::max();
  }
  return left + right;
}

uint64_t MultiplySaturating(uint64_t left, uint64_t right) {
  if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) {
    return std::numeric_limits<uint64_t>::max();
  }
  return left * right;
}

uint64_t PoolLineDelay(const Config& cfg, PoolKind pool) {
  if (pool == PoolKind::kSwcc || pool == PoolKind::kOwnerPrivateSwcc) {
    return RoundNs(cfg.fixed_latency.swcc_fixed_ns_per_line);
  }
  if (pool == PoolKind::kHwcc) {
    return RoundNs(cfg.fixed_latency.hwcc_fixed_ns_per_line);
  }
  return 0;
}

struct IdentityHash {
  size_t operator()(const MemoryIdentity& identity) const {
    uint64_t h = static_cast<uint64_t>(identity.domain);
    h ^= identity.pool_id + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= identity.pool_offset + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return static_cast<size_t>(h);
  }
};

// Entries are intentionally leaked until process exit.  This keeps pointers
// in the aggregation registry valid when a worker thread exits, and allocation
// only occurs after a non-zero feature mask has taken the slow path.
thread_local std::unordered_map<const LatencySimulator*, ThreadState*> g_tls;

struct RangeMapping {
  uintptr_t begin = 0;
  uintptr_t end = 0;
  MemoryIdentity first;
};

struct RemoteLineState {
  int32_t dirty_owner = -1;
  std::vector<uint32_t> sharers;
  uint64_t frequency = 0;
  uint64_t last_touch = 0;
};

struct CacheEntry {
  uint64_t frequency = 0;
  uint64_t last_touch = 0;
  bool dirty = false;
};

using CacheSet =
    std::unordered_map<MemoryIdentity, CacheEntry, IdentityHash>;
using CacheSetsByNode = std::vector<std::vector<CacheSet>>;
using RemoteDirectory =
    std::unordered_map<MemoryIdentity, RemoteLineState, IdentityHash>;

size_t ScopeIndex(ScopeKind scope) {
  const auto value = static_cast<size_t>(scope);
  if (value >= kScopeCount) std::abort();
  return value;
}

size_t AtomicDomainIndex(AtomicDomain domain) {
  switch (domain) {
    case AtomicDomain::kHwcc:
      return 0;
    case AtomicDomain::kOwnerPrivateSwcc:
      return 1;
    case AtomicDomain::kLocalDram:
      return 2;
  }
  std::abort();
}

size_t MemoryOrderIndex(std::memory_order order) {
  switch (order) {
    case std::memory_order_relaxed:
      return 0;
    case std::memory_order_consume:
    case std::memory_order_acquire:
      return 1;
    case std::memory_order_release:
      return 2;
    case std::memory_order_acq_rel:
      return 3;
    case std::memory_order_seq_cst:
      return 4;
  }
  std::abort();
}

template <size_t N>
void AddArray(std::array<uint64_t, N>* destination,
              const std::array<uint64_t, N>& source) {
  for (size_t i = 0; i < N; ++i) {
    (*destination)[i] += source[i];
  }
}

template <size_t N>
void SubtractArray(std::array<uint64_t, N>* destination,
                   const std::array<uint64_t, N>& current,
                   const std::array<uint64_t, N>& baseline) {
  for (size_t i = 0; i < N; ++i) {
    (*destination)[i] = current[i] - baseline[i];
  }
}

uint64_t CacheSetIndex(const MemoryIdentity& identity, uint64_t set_count,
                       uint64_t instance_count) {
  if (set_count == 0 || instance_count == 0) {
    std::abort();
  }
  const uint64_t hash = IdentityHash{}(identity);
  const uint64_t instance = (hash / set_count) % instance_count;
  const uint64_t set = hash % set_count;
  if (instance > (std::numeric_limits<uint64_t>::max() - set) /
                    set_count) {
    std::abort();
  }
  return instance * set_count + set;
}

// Apply one already sequenced remote event.  The same state-machine function
// is used by online accounting and by shared-log replay; otherwise the two
// paths would quietly drift and a multi-VM result could not be audited.
void ApplyRemoteModelEvent(const Config& config, uint64_t* access_number,
                           uint64_t cache_ways,
                           const std::vector<uint64_t>& set_counts,
                           CacheSetsByNode* cache_sets,
                           RemoteDirectory* directory,
                           const MemoryIdentity& identity, uint32_t node,
                           RemoteEventKind event, Stats* stats,
                           uint32_t source_node = UINT32_MAX,
                           ScopeKind scope = ScopeKind::kOther,
                           uint32_t tag = 0) {
  if (access_number == nullptr || cache_sets == nullptr ||
      directory == nullptr || stats == nullptr ||
      node >= config.remote_cache_invalidation.node_count) {
    std::abort();
  }
  if (*access_number == std::numeric_limits<uint64_t>::max()) {
    std::abort();
  }
  ++*access_number;
  const auto& remote = config.remote_cache_invalidation;
  const size_t scope_index = ScopeIndex(scope);
  if (tag >= kMaxBreakdownTags ||
      (remote.tag_breakdown_enabled && tag >= remote.max_tags)) {
    std::abort();
  }
  auto increment_metric = [&](uint64_t* total, auto* by_scope,
                              auto* by_tag) {
    ++*total;
    if (remote.scope_breakdown_enabled) ++(*by_scope)[scope_index];
    if (remote.tag_breakdown_enabled) ++(*by_tag)[tag];
  };
  if (remote.replacement_policy == "lfu" &&
      remote.lfu_aging_interval_accesses != 0 &&
      *access_number % remote.lfu_aging_interval_accesses == 0) {
    for (auto& node_sets : *cache_sets) {
      for (auto& set : node_sets) {
        for (auto& entry : set) {
          entry.second.frequency = (entry.second.frequency + 1) / 2;
        }
      }
    }
  }

  auto erase_cache_copy = [&](const MemoryIdentity& key,
                              uint32_t cache_node) {
    if (cache_sets->empty() || cache_node >= cache_sets->size()) return;
    const uint64_t set_count = set_counts[cache_node];
    const uint64_t instance_count =
        config.remote_cache_invalidation.cache_instances_per_node;
    (*cache_sets)[cache_node][CacheSetIndex(key, set_count, instance_count)]
        .erase(key);
  };
  auto mark_cache_copy_clean = [&](const MemoryIdentity& key,
                                   uint32_t cache_node) {
    if (cache_sets->empty() || cache_node >= cache_sets->size()) return;
    const uint64_t set_count = set_counts[cache_node];
    const uint64_t instance_count =
        config.remote_cache_invalidation.cache_instances_per_node;
    auto& set = (*cache_sets)[cache_node]
        [CacheSetIndex(key, set_count, instance_count)];
    auto it = set.find(key);
    if (it != set.end()) {
      it->second.dirty = false;
    }
  };
  auto touch_cache = [&](const MemoryIdentity& key, uint32_t cache_node,
                         bool dirty) {
    if (cache_sets->empty()) return;
    if (cache_node >= cache_sets->size()) std::abort();
    const uint64_t set_count = set_counts[cache_node];
    const uint64_t instance_count =
        config.remote_cache_invalidation.cache_instances_per_node;
    auto& set = (*cache_sets)[cache_node]
        [CacheSetIndex(key, set_count, instance_count)];
    auto found = set.find(key);
    if (found != set.end()) {
      const uint64_t bits = std::min<uint64_t>(remote.lfu_counter_bits, 63);
      const uint64_t max_frequency = (uint64_t{1} << bits) - 1;
      found->second.frequency =
          std::min<uint64_t>(found->second.frequency + 1, max_frequency);
      found->second.last_touch = *access_number;
      found->second.dirty = found->second.dirty || dirty;
      return;
    }
    if (set.size() >= cache_ways) {
      auto victim = set.begin();
      for (auto candidate = std::next(set.begin()); candidate != set.end();
           ++candidate) {
        const bool candidate_before =
            remote.replacement_policy == "lfu"
                ? (candidate->second.frequency < victim->second.frequency ||
                   (candidate->second.frequency == victim->second.frequency &&
                    candidate->second.last_touch < victim->second.last_touch))
                : candidate->second.last_touch < victim->second.last_touch;
        if (candidate_before) victim = candidate;
      }
      if (victim->second.dirty && remote.dirty_eviction_writeback_enabled) {
        increment_metric(&stats->remote_dirty_capacity_evictions,
                         &stats->remote_dirty_capacity_evictions_by_scope,
                         &stats->remote_dirty_capacity_evictions_by_tag);
      }
      if (victim->second.dirty &&
          victim->first.domain == MemoryDomain::kHwcc) {
        auto line_it = directory->find(victim->first);
        if (line_it != directory->end() &&
            line_it->second.dirty_owner ==
                static_cast<int32_t>(cache_node)) {
          line_it->second.dirty_owner = -1;
        }
      }
      set.erase(victim);
    }
    set.emplace(key, CacheEntry{1, *access_number, dirty});
  };

  increment_metric(&stats->remote_events, &stats->remote_events_by_scope,
                   &stats->remote_events_by_tag);
  if (event == RemoteEventKind::kRead) touch_cache(identity, node, false);
  if (event == RemoteEventKind::kWrite || event == RemoteEventKind::kAtomicRmw)
    touch_cache(identity, node, true);
  if (event == RemoteEventKind::kExplicitSwccHandoff)
    touch_cache(identity, node, false);

  if (identity.domain == MemoryDomain::kSwcc &&
      event != RemoteEventKind::kExplicitSwccHandoff) {
    // SWCC ordinary access is deliberately not a hardware-coherence event.
    return;
  }
  auto& line = (*directory)[identity];
  ++line.frequency;
  line.last_touch = *access_number;
  if (event == RemoteEventKind::kRead) {
    if (line.dirty_owner >= 0 &&
        line.dirty_owner != static_cast<int32_t>(node)) {
      const uint32_t old_owner = static_cast<uint32_t>(line.dirty_owner);
      if (remote.dirty_handoff_enabled) {
        increment_metric(&stats->remote_dirty_handoffs,
                         &stats->remote_dirty_handoffs_by_scope,
                         &stats->remote_dirty_handoffs_by_tag);
      }
      mark_cache_copy_clean(identity, old_owner);
      if (std::find(line.sharers.begin(), line.sharers.end(), old_owner) ==
          line.sharers.end()) {
        line.sharers.push_back(old_owner);
      }
      line.dirty_owner = -1;
    }
    if (std::find(line.sharers.begin(), line.sharers.end(), node) ==
        line.sharers.end()) {
      line.sharers.push_back(node);
    }
    return;
  }
  if (event == RemoteEventKind::kWrite || event == RemoteEventKind::kAtomicRmw) {
    bool caused_invalidation = false;
    if (line.dirty_owner >= 0 &&
        line.dirty_owner != static_cast<int32_t>(node)) {
      const uint32_t old_owner = static_cast<uint32_t>(line.dirty_owner);
      if (remote.dirty_handoff_enabled) {
        increment_metric(&stats->remote_dirty_handoffs,
                         &stats->remote_dirty_handoffs_by_scope,
                         &stats->remote_dirty_handoffs_by_tag);
      }
      erase_cache_copy(identity, old_owner);
      line.dirty_owner = -1;
      caused_invalidation = true;
    }
    for (uint32_t sharer : line.sharers) {
      if (sharer != node) {
        if (remote.clean_copy_invalidation_enabled) {
          increment_metric(
              &stats->remote_clean_copy_invalidations,
              &stats->remote_clean_copy_invalidations_by_scope,
              &stats->remote_clean_copy_invalidations_by_tag);
        }
        caused_invalidation = true;
        erase_cache_copy(identity, sharer);
      }
    }
    line.sharers.clear();
    line.dirty_owner = static_cast<int32_t>(node);
    if (caused_invalidation) {
      increment_metric(
          &stats->remote_write_transactions_causing_invalidation,
          &stats->remote_write_transactions_causing_invalidation_by_scope,
          &stats->remote_write_transactions_causing_invalidation_by_tag);
    }
    return;
  }
  if (event == RemoteEventKind::kExplicitSwccHandoff) {
    if (source_node >= config.remote_cache_invalidation.node_count ||
        source_node == node) {
      std::abort();
    }
    if (remote.swcc_explicit_visibility_handoff_enabled) {
      increment_metric(&stats->remote_swcc_explicit_handoffs,
                       &stats->remote_swcc_explicit_handoffs_by_scope,
                       &stats->remote_swcc_explicit_handoffs_by_tag);
    }
    return;
  }
  if (event == RemoteEventKind::kDirtyCapacityEviction) {
    erase_cache_copy(identity, node);
    if (remote.dirty_eviction_writeback_enabled) {
      increment_metric(&stats->remote_dirty_capacity_evictions,
                       &stats->remote_dirty_capacity_evictions_by_scope,
                       &stats->remote_dirty_capacity_evictions_by_tag);
    }
    if (line.dirty_owner == static_cast<int32_t>(node)) {
      line.dirty_owner = -1;
    }
  }
}

}  // namespace

struct LatencySimulator::Impl {
  mutable std::mutex registry_mu;
  std::vector<ThreadState*> registered_states;

  mutable std::mutex mapping_mu;
  std::vector<RangeMapping> mappings;

  mutable std::mutex remote_mu;
  std::unordered_map<MemoryIdentity, RemoteLineState, IdentityHash> remote;
  uint64_t cache_set_count = 1;
  uint64_t cache_ways = 1;
  std::vector<uint64_t> cache_set_count_by_node;
  std::vector<std::vector<std::unordered_map<MemoryIdentity, CacheEntry,
                                               IdentityHash>>>
      cache_sets_by_node;
  uint64_t next_sequence = 0;
  uint64_t last_sequence = 0;
  uint64_t remote_access_number = 0;
  uint32_t node_id = 0;
  std::atomic<uint64_t>* shared_sequence = nullptr;
  RemoteEventRecord* shared_event_log = nullptr;
  uint64_t shared_event_capacity = 0;
  uint64_t shared_replay_base_sequence = 0;
  Stats shared_replay_baseline_stats;
  Stats shared_replay_cumulative_stats;
  bool shared_replay_baseline_initialized = false;
  mutable Stats shared_replay_stats;
  mutable bool shared_replay_valid = false;
};

Stats SubtractRemoteStats(const Stats& current, const Stats& baseline) {
  Stats result;
  result.remote_dirty_handoffs = current.remote_dirty_handoffs -
                                 baseline.remote_dirty_handoffs;
  result.remote_clean_copy_invalidations =
      current.remote_clean_copy_invalidations -
      baseline.remote_clean_copy_invalidations;
  result.remote_write_transactions_causing_invalidation =
      current.remote_write_transactions_causing_invalidation -
      baseline.remote_write_transactions_causing_invalidation;
  result.remote_dirty_capacity_evictions =
      current.remote_dirty_capacity_evictions -
      baseline.remote_dirty_capacity_evictions;
  result.remote_swcc_explicit_handoffs =
      current.remote_swcc_explicit_handoffs -
      baseline.remote_swcc_explicit_handoffs;
  result.remote_events = current.remote_events - baseline.remote_events;
  SubtractArray(&result.remote_dirty_handoffs_by_scope,
                current.remote_dirty_handoffs_by_scope,
                baseline.remote_dirty_handoffs_by_scope);
  SubtractArray(&result.remote_clean_copy_invalidations_by_scope,
                current.remote_clean_copy_invalidations_by_scope,
                baseline.remote_clean_copy_invalidations_by_scope);
  SubtractArray(&result.remote_write_transactions_causing_invalidation_by_scope,
                current.remote_write_transactions_causing_invalidation_by_scope,
                baseline.remote_write_transactions_causing_invalidation_by_scope);
  SubtractArray(&result.remote_dirty_capacity_evictions_by_scope,
                current.remote_dirty_capacity_evictions_by_scope,
                baseline.remote_dirty_capacity_evictions_by_scope);
  SubtractArray(&result.remote_swcc_explicit_handoffs_by_scope,
                current.remote_swcc_explicit_handoffs_by_scope,
                baseline.remote_swcc_explicit_handoffs_by_scope);
  SubtractArray(&result.remote_events_by_scope, current.remote_events_by_scope,
                baseline.remote_events_by_scope);
  SubtractArray(&result.remote_dirty_handoffs_by_tag,
                current.remote_dirty_handoffs_by_tag,
                baseline.remote_dirty_handoffs_by_tag);
  SubtractArray(&result.remote_clean_copy_invalidations_by_tag,
                current.remote_clean_copy_invalidations_by_tag,
                baseline.remote_clean_copy_invalidations_by_tag);
  SubtractArray(&result.remote_write_transactions_causing_invalidation_by_tag,
                current.remote_write_transactions_causing_invalidation_by_tag,
                baseline.remote_write_transactions_causing_invalidation_by_tag);
  SubtractArray(&result.remote_dirty_capacity_evictions_by_tag,
                current.remote_dirty_capacity_evictions_by_tag,
                baseline.remote_dirty_capacity_evictions_by_tag);
  SubtractArray(&result.remote_swcc_explicit_handoffs_by_tag,
                current.remote_swcc_explicit_handoffs_by_tag,
                baseline.remote_swcc_explicit_handoffs_by_tag);
  SubtractArray(&result.remote_events_by_tag, current.remote_events_by_tag,
                baseline.remote_events_by_tag);
  return result;
}

uint64_t Stats::RawLineAccesses(PoolKind pool) const {
  if (pool == PoolKind::kSwcc) {
    return swcc_read_lines + swcc_write_lines;
  }
  if (pool == PoolKind::kHwcc) {
    return hwcc_read_lines + hwcc_write_lines;
  }
  return 0;
}

uint64_t Stats::DelayedNs(PoolKind pool) const {
  return (pool == PoolKind::kSwcc || pool == PoolKind::kOwnerPrivateSwcc)
             ? swcc_delayed_ns
             : hwcc_delayed_ns;
}

#if !defined(TIGONKV_DISABLE_HARDWARE_SIMULATION)
uint32_t HardwareSimulationFeaturesFast() {
  return g_features.load(std::memory_order_relaxed);
}
#endif

LatencySimulator& GlobalLatencySimulator() {
  static LatencySimulator simulator;
  return simulator;
}

LatencySimulator::LatencySimulator(Config config)
    : config_(std::move(config)), generation_(NextGeneration()), impl_(new Impl) {
  Configure(config_);
}

LatencySimulator::~LatencySimulator() = default;

void LatencySimulator::Configure(Config config) {
  if (config.enabled) {
    config.fixed_latency.enabled = true;
  }
  if (config.fixed_latency.cache_line_bytes == 0 ||
      (config.fixed_latency.cache_line_bytes &
       (config.fixed_latency.cache_line_bytes - 1)) != 0) {
    throw std::invalid_argument("fixed_latency.cache_line_bytes must be a power of two");
  }
  if (config.remote_cache_invalidation.cache_line_bytes == 0 ||
      (config.remote_cache_invalidation.cache_line_bytes &
       (config.remote_cache_invalidation.cache_line_bytes - 1)) != 0) {
    throw std::invalid_argument(
        "remote_cache_invalidation.cache_line_bytes must be a power of two");
  }
  if (config.hwcc_access_count.cache_line_bytes == 0 ||
      (config.hwcc_access_count.cache_line_bytes &
       (config.hwcc_access_count.cache_line_bytes - 1)) != 0) {
    throw std::invalid_argument(
        "hwcc_access_count.cache_line_bytes must be a power of two");
  }
  if (config.hwcc_access_count.max_tags == 0 ||
      config.atomic_count.max_tags == 0 ||
      config.remote_cache_invalidation.max_tags == 0 ||
      config.hwcc_access_count.max_tags > kMaxBreakdownTags ||
      config.atomic_count.max_tags > kMaxBreakdownTags ||
      config.remote_cache_invalidation.max_tags > kMaxBreakdownTags) {
    throw std::invalid_argument(
        "hardware simulation breakdown max_tags must be in [1, 64]");
  }
  if (config.remote_cache_invalidation.node_count == 0 ||
      config.remote_cache_invalidation.associativity == 0 ||
      config.remote_cache_invalidation.cache_instances_per_node == 0) {
    throw std::invalid_argument("remote cache topology values must be > 0");
  }
  if (config.remote_cache_invalidation.associativity >
      std::numeric_limits<uint64_t>::max() /
          config.remote_cache_invalidation.cache_instances_per_node) {
    throw std::invalid_argument("remote cache ways overflow");
  }
  for (double value : {config.fixed_latency.swcc_fixed_ns_per_line,
                       config.fixed_latency.hwcc_fixed_ns_per_line}) {
    if (value < 0.0 || !std::isfinite(value)) {
      throw std::invalid_argument("fixed latency must be finite and non-negative");
    }
  }
  if (config.remote_cache_invalidation.replacement_policy != "lru" &&
      config.remote_cache_invalidation.replacement_policy != "lfu") {
    throw std::invalid_argument("remote replacement_policy must be lru or lfu");
  }
  if (config.remote_cache_invalidation.lfu_counter_bits == 0 ||
      config.remote_cache_invalidation.lfu_counter_bits > 63) {
    throw std::invalid_argument(
        "remote lfu_counter_bits must be in the range [1, 63]");
  }
  if (config.remote_cache_invalidation.lfu_aging_interval_accesses == 0) {
    throw std::invalid_argument(
        "remote lfu_aging_interval_accesses must be > 0");
  }
  {
    std::lock_guard<std::mutex> lock(impl_->remote_mu);
    if (impl_->shared_sequence != nullptr) {
      if (!config.remote_cache_invalidation.enabled) {
        // Runtime reconfiguration is only valid at a quiescent boundary.  Do
        // not leave a stale pointer into a pool that the caller believes is
        // no longer instrumented.
        impl_->shared_sequence = nullptr;
        impl_->shared_event_log = nullptr;
        impl_->shared_event_capacity = 0;
      } else if (config.remote_cache_invalidation.event_log_capacity !=
                     impl_->shared_event_capacity) {
        throw std::invalid_argument(
            "remote event_log_capacity cannot change while a shared log is attached");
      }
    }
  }
  config_ = std::move(config);
  {
    std::lock_guard<std::mutex> lock(impl_->remote_mu);
    impl_->remote.clear();
    impl_->remote_access_number = 0;
    impl_->next_sequence = 0;
    impl_->last_sequence = 0;
    impl_->shared_replay_stats = Stats{};
    impl_->shared_replay_cumulative_stats = Stats{};
    impl_->shared_replay_baseline_stats = Stats{};
    impl_->shared_replay_base_sequence = 0;
    impl_->shared_replay_baseline_initialized = false;
    impl_->shared_replay_valid = false;
    if (config_.remote_cache_invalidation.enabled) {
      const auto& remote = config_.remote_cache_invalidation;
      if (remote.capacity_mode != "per_node" &&
          remote.capacity_mode != "total_equal_split" &&
          remote.capacity_mode != "explicit_by_node") {
        throw std::invalid_argument("remote capacity_mode is invalid");
      }
      std::vector<uint64_t> bytes_by_node(
          static_cast<size_t>(remote.node_count),
          remote.cache_size_bytes_per_node);
      if (remote.capacity_mode == "total_equal_split") {
        if (remote.total_cpu_cache_size_bytes % remote.node_count != 0) {
          throw std::invalid_argument(
              "total remote cache capacity must divide by node count");
        }
        const uint64_t bytes_per_node =
            remote.total_cpu_cache_size_bytes / remote.node_count;
        std::fill(bytes_by_node.begin(), bytes_by_node.end(), bytes_per_node);
      } else if (remote.capacity_mode == "explicit_by_node") {
        if (remote.cache_size_bytes_by_node.size() != remote.node_count) {
          throw std::invalid_argument("explicit remote capacity node count mismatch");
        }
        bytes_by_node = remote.cache_size_bytes_by_node;
      } else if (remote.total_cpu_cache_size_bytes != 0 ||
                 !remote.cache_size_bytes_by_node.empty()) {
        throw std::invalid_argument(
            "per_node capacity cannot specify total or explicit sizes");
      }
      const uint64_t divisor = remote.associativity *
                               remote.cache_instances_per_node;
      if (divisor == 0) {
        throw std::invalid_argument("remote cache ways must be non-zero");
      }
      impl_->cache_set_count_by_node.clear();
      for (uint64_t bytes : bytes_by_node) {
        const uint64_t line_count = bytes / remote.cache_line_bytes;
        if (line_count == 0 || line_count % divisor != 0) {
          throw std::invalid_argument(
              "remote cache capacity must divide into cache lines and ways");
        }
        impl_->cache_set_count_by_node.push_back(
            line_count / divisor);
      }
      impl_->cache_set_count = impl_->cache_set_count_by_node.front();
      impl_->cache_ways = remote.associativity;
      impl_->cache_sets_by_node.clear();
      impl_->cache_sets_by_node.reserve(bytes_by_node.size());
      for (uint64_t set_count : impl_->cache_set_count_by_node) {
        if (set_count > std::numeric_limits<uint64_t>::max() /
                            remote.cache_instances_per_node) {
          throw std::invalid_argument("remote cache set count overflow");
        }
        impl_->cache_sets_by_node.emplace_back(
            static_cast<size_t>(set_count *
                                remote.cache_instances_per_node));
      }
    } else {
      impl_->cache_sets_by_node.clear();
      impl_->cache_set_count_by_node.clear();
      impl_->cache_set_count = 1;
      impl_->cache_ways = 1;
    }
  }
  feature_mask_ = 0;
  if (config_.fixed_latency.enabled) feature_mask_ |= kFixedLatency;
  if (config_.hwcc_access_count.enabled) feature_mask_ |= kHwccAccessCount;
  if (config_.atomic_count.enabled) feature_mask_ |= kAtomicCount;
  if (config_.remote_cache_invalidation.enabled)
    feature_mask_ |= kRemoteInvalidation;
  generation_ = NextGeneration();
  // Configure is a startup/quiescent operation.  Clear every registered
  // phase-local accumulator here rather than relying on each thread to take
  // the next slow-path event; otherwise a worker that stays idle across a
  // generation boundary would leak its previous stats into SnapshotStats.
  {
    std::lock_guard<std::mutex> lock(impl_->registry_mu);
    for (ThreadState* state : impl_->registered_states) {
      *state = ThreadState{};
      state->generation = generation_;
    }
  }
  g_features.store(feature_mask_, std::memory_order_relaxed);
  if (feature_mask_ & kFixedLatency) {
    CalibrateTscOnce();
    if (!HasCalibratedTsc()) {
      throw std::runtime_error(
          "fixed latency requires a calibrated x86 TSC; no clock fallback is permitted");
    }
  }
}

ThreadState& LatencySimulator::GetThreadState() {
  auto it = g_tls.find(this);
  if (it == g_tls.end()) {
    auto* state = new ThreadState;
    it = g_tls.emplace(this, state).first;
    std::lock_guard<std::mutex> lock(impl_->registry_mu);
    impl_->registered_states.push_back(state);
  }
  ThreadState& state = *it->second;
  if (state.generation != generation_) {
    state = ThreadState{};
    state.generation = generation_;
    // A stack/local simulator may reuse the same address on a later test or
    // component lifetime.  The TLS map key alone cannot distinguish the new
    // Impl registry, so register the reused state with this simulator too.
    std::lock_guard<std::mutex> lock(impl_->registry_mu);
    if (std::find(impl_->registered_states.begin(),
                  impl_->registered_states.end(), &state) ==
        impl_->registered_states.end()) {
      impl_->registered_states.push_back(&state);
    }
  }
  return state;
}

void LatencySimulator::BeginScope(ScopeKind scope) {
  const uint32_t features = HardwareSimulationFeaturesFast();
  const bool needs_scope =
      (features & kFixedLatency) ||
      ((features & kHwccAccessCount) &&
       config_.hwcc_access_count.breakdown_by_scope_enabled) ||
      ((features & kAtomicCount) &&
       config_.atomic_count.scope_breakdown_enabled) ||
      ((features & kRemoteInvalidation) &&
       config_.remote_cache_invalidation.scope_breakdown_enabled);
  if (features == 0 || !needs_scope) return;
  ThreadState& state = GetThreadState();
  if (state.scope_depth != 0) {
    ++state.scope_depth;
    return;
  }
  state.scope = scope;
  state.scope_enabled = (features & kFixedLatency) &&
                        config_.fixed_latency.enabled &&
                        (scope == ScopeKind::kForeground
                             ? config_.fixed_latency.foreground_enabled
                             : scope == ScopeKind::kMerge
                                   ? config_.fixed_latency.background_enabled
                                   : true);
  state.scope_depth = 1;
  state.pending_delay_ns = 0;
}

void LatencySimulator::EndScopeAndDelay() {
  const uint32_t features = HardwareSimulationFeaturesFast();
  const bool needs_scope =
      (features & kFixedLatency) ||
      ((features & kHwccAccessCount) &&
       config_.hwcc_access_count.breakdown_by_scope_enabled) ||
      ((features & kAtomicCount) &&
       config_.atomic_count.scope_breakdown_enabled) ||
      ((features & kRemoteInvalidation) &&
       config_.remote_cache_invalidation.scope_breakdown_enabled);
  if (features == 0 || !needs_scope) return;
  ThreadState& state = GetThreadState();
  if (state.scope_depth == 0) return;
  if (state.scope_depth > 1) {
    --state.scope_depth;
    return;
  }
  const uint64_t delay = state.pending_delay_ns;
  state.pending_delay_ns = 0;
  state.scope_depth = 0;
  state.scope_enabled = false;
  // This is the caller's boundary after its guards and publication state have
  // been released.  The simulator itself owns no business lock here.
  DelaySpinNs(delay);
}

bool LatencySimulator::HasActiveScopeForCurrentThread() const {
  const auto it = g_tls.find(this);
  return it != g_tls.end() && it->second != nullptr &&
         it->second->generation == generation_ &&
         it->second->scope_depth != 0;
}

void LatencySimulator::RecordRange(PoolKind pool, AccessKind kind,
                                   const void* addr, uint64_t bytes,
                                   uint32_t tag) {
  const uint32_t features = HardwareSimulationFeaturesFast();
  if (features == 0 ||
      !(features & (kFixedLatency | kHwccAccessCount |
                    kRemoteInvalidation)) ||
      addr == nullptr || bytes == 0) {
    return;
  }
  ThreadState& state = GetThreadState();
  const auto& hwcc_cfg = config_.hwcc_access_count;
  if (hwcc_cfg.breakdown_by_tag_enabled && tag >= hwcc_cfg.max_tags) {
    std::abort();
  }
  const bool ordinary_read = kind == AccessKind::kRead;
  const bool ordinary_write = kind == AccessKind::kWrite;
  const bool needs_fixed_lines =
      (features & kFixedLatency) && config_.fixed_latency.enabled &&
      state.scope_enabled && pool != PoolKind::kLocalDram &&
      kind != AccessKind::kFlush && kind != AccessKind::kInvalidate;
  const bool needs_hwcc_count_lines =
      (features & kHwccAccessCount) && pool == PoolKind::kHwcc &&
      config_.hwcc_access_count.line_count_enabled &&
      (ordinary_read || ordinary_write);
  const uint64_t fixed_lines =
      needs_fixed_lines
          ? LineCount(addr, bytes, config_.fixed_latency.cache_line_bytes)
          : 0;
  // HWCC access-count line granularity follows the configured platform line.
  // If fixed latency is disabled, use the remote model's line size when that
  // module is enabled; otherwise the fixed-module default remains the stable
  // 64-byte platform line.
  const uint64_t count_line_bytes =
      config_.hwcc_access_count.cache_line_bytes;
  const uint64_t count_lines =
      needs_hwcc_count_lines ? LineCount(addr, bytes, count_line_bytes) : 0;

  if ((features & kHwccAccessCount) && pool == PoolKind::kHwcc &&
      (ordinary_read || ordinary_write)) {
    auto& c = state.stats;
    const size_t scope_index = ScopeIndex(state.scope);
    if (ordinary_read && hwcc_cfg.read_enabled) {
      if (hwcc_cfg.operation_count_enabled) {
        ++c.hwcc_read_ops;
        if (hwcc_cfg.breakdown_by_scope_enabled)
          ++c.hwcc_read_ops_by_scope[scope_index];
        if (hwcc_cfg.breakdown_by_tag_enabled) ++c.hwcc_read_ops_by_tag[tag];
      }
      if (hwcc_cfg.line_count_enabled) {
        c.hwcc_read_lines += count_lines;
        if (hwcc_cfg.breakdown_by_scope_enabled)
          c.hwcc_read_lines_by_scope[scope_index] += count_lines;
        if (hwcc_cfg.breakdown_by_tag_enabled)
          c.hwcc_read_lines_by_tag[tag] += count_lines;
      }
      if (hwcc_cfg.byte_count_enabled) {
        c.hwcc_read_bytes += bytes;
        if (hwcc_cfg.breakdown_by_scope_enabled)
          c.hwcc_read_bytes_by_scope[scope_index] += bytes;
        if (hwcc_cfg.breakdown_by_tag_enabled)
          c.hwcc_read_bytes_by_tag[tag] += bytes;
      }
    } else if (ordinary_write && hwcc_cfg.write_enabled) {
      if (hwcc_cfg.operation_count_enabled) {
        ++c.hwcc_write_ops;
        if (hwcc_cfg.breakdown_by_scope_enabled)
          ++c.hwcc_write_ops_by_scope[scope_index];
        if (hwcc_cfg.breakdown_by_tag_enabled) ++c.hwcc_write_ops_by_tag[tag];
      }
      if (hwcc_cfg.line_count_enabled) {
        c.hwcc_write_lines += count_lines;
        if (hwcc_cfg.breakdown_by_scope_enabled)
          c.hwcc_write_lines_by_scope[scope_index] += count_lines;
        if (hwcc_cfg.breakdown_by_tag_enabled)
          c.hwcc_write_lines_by_tag[tag] += count_lines;
      }
      if (hwcc_cfg.byte_count_enabled) {
        c.hwcc_write_bytes += bytes;
        if (hwcc_cfg.breakdown_by_scope_enabled)
          c.hwcc_write_bytes_by_scope[scope_index] += bytes;
        if (hwcc_cfg.breakdown_by_tag_enabled)
          c.hwcc_write_bytes_by_tag[tag] += bytes;
      }
    }
  }
  if ((features & kFixedLatency) && state.scope_enabled &&
      kind != AccessKind::kFlush && kind != AccessKind::kInvalidate &&
      pool != PoolKind::kLocalDram) {
    const uint64_t delay = PoolLineDelay(config_, pool);
    if (delay != 0) {
      const uint64_t range_delay = MultiplySaturating(fixed_lines, delay);
      state.pending_delay_ns =
          AddSaturating(state.pending_delay_ns, range_delay);
      if (config_.fixed_latency.delayed_time_stats_enabled) {
        if (pool == PoolKind::kSwcc || pool == PoolKind::kOwnerPrivateSwcc)
          state.stats.swcc_delayed_ns =
              AddSaturating(state.stats.swcc_delayed_ns, range_delay);
        else if (pool == PoolKind::kHwcc)
          state.stats.hwcc_delayed_ns =
              AddSaturating(state.stats.hwcc_delayed_ns, range_delay);
      }
    }
  }
  if ((features & kRemoteInvalidation) &&
      (pool == PoolKind::kHwcc || pool == PoolKind::kSwcc)) {
    const MemoryIdentity first = ResolveIdentity(addr, pool);
    const uint64_t remote_line = config_.remote_cache_invalidation.cache_line_bytes;
    const uint64_t count = LineCount(addr, bytes, remote_line);
    for (uint64_t i = 0; i < count; ++i) {
      MemoryIdentity identity = first;
      identity.pool_offset = (first.pool_offset / remote_line + i) * remote_line;
      if (ordinary_read)
        RecordRemoteRead(identity, impl_->node_id, state.scope, tag);
      if (ordinary_write)
        RecordRemoteWrite(identity, impl_->node_id, false, state.scope, tag);
    }
  }
}

void LatencySimulator::RecordLine(PoolKind pool, AccessKind kind,
                                  const void* addr, uint32_t tag) {
  RecordRange(pool, kind, addr, 1, tag);
}

void LatencySimulator::RecordAtomicCompleted(AtomicDomain domain,
                                             AccessKind kind,
                                             AtomicOperation operation,
                                             const void* addr, uint64_t bytes,
                                             bool cas_success,
                                             std::memory_order order,
                                             uint32_t tag) {
  const uint32_t features = HardwareSimulationFeaturesFast();
  if (features == 0) return;
  ThreadState& state = GetThreadState();
  const auto atomic_cfg = config_.atomic_count;
  if (atomic_cfg.tag_breakdown_enabled && tag >= atomic_cfg.max_tags) {
    std::abort();
  }
  if ((features & kAtomicCount) && atomic_cfg.enabled) {
    const bool domain_enabled =
        domain == AtomicDomain::kHwcc
            ? atomic_cfg.hwcc_enabled
            : domain == AtomicDomain::kOwnerPrivateSwcc
                  ? atomic_cfg.owner_private_swcc_enabled
                  : atomic_cfg.local_dram_enabled;
    const bool operation_enabled = [&] {
      switch (operation) {
        case AtomicOperation::kLoad:
          return atomic_cfg.load_enabled;
        case AtomicOperation::kStore:
          return atomic_cfg.store_enabled;
        case AtomicOperation::kCas:
        case AtomicOperation::kCasWeak:
        case AtomicOperation::kCasStrong:
          return atomic_cfg.cas_enabled;
        case AtomicOperation::kExchange:
          return atomic_cfg.exchange_enabled;
        case AtomicOperation::kFetchAdd:
        case AtomicOperation::kFetchSub:
          return atomic_cfg.fetch_arithmetic_enabled;
        case AtomicOperation::kFetchOr:
        case AtomicOperation::kFetchAnd:
        case AtomicOperation::kFetchXor:
          return atomic_cfg.fetch_bitwise_enabled;
        case AtomicOperation::kFence:
          return atomic_cfg.fence_enabled;
        case AtomicOperation::kWaitNotify:
          return atomic_cfg.wait_notify_enabled;
      }
      return false;
    }();
    if (domain_enabled && operation_enabled) {
      auto& c = state.stats;
      const bool hwcc = domain == AtomicDomain::kHwcc;
      const bool owner = domain == AtomicDomain::kOwnerPrivateSwcc;
      const size_t domain_index = AtomicDomainIndex(domain);

      auto increment_load = [&] {
        if (hwcc) {
          ++c.hwcc_atomic_loads;
        } else if (owner) {
          ++c.owner_private_swcc_atomic_loads;
        } else {
          ++c.local_dram_atomic_loads;
        }
      };
      auto increment_store = [&] {
        if (hwcc) {
          ++c.hwcc_atomic_stores;
        } else if (owner) {
          ++c.owner_private_swcc_atomic_stores;
        } else {
          ++c.local_dram_atomic_stores;
        }
      };
      auto increment_rmw = [&] {
        if (hwcc) {
          ++c.hwcc_atomic_rmw;
        } else if (owner) {
          ++c.owner_private_swcc_atomic_rmw;
        } else {
          ++c.local_dram_atomic_rmw;
        }
      };
      auto increment_cas_attempt = [&] {
        if (hwcc) {
          ++c.hwcc_cas_attempts;
        } else if (owner) {
          ++c.owner_private_swcc_cas_attempts;
        } else {
          ++c.local_dram_cas_attempts;
        }
      };
      auto increment_cas_result = [&](bool success) {
        if (hwcc) {
          if (success) {
            ++c.hwcc_cas_successes;
          } else {
            ++c.hwcc_cas_failures;
          }
        } else if (owner) {
          if (success) {
            ++c.owner_private_swcc_cas_successes;
          } else {
            ++c.owner_private_swcc_cas_failures;
          }
        } else if (success) {
          ++c.local_dram_cas_successes;
        } else {
          ++c.local_dram_cas_failures;
        }
      };
      auto increment_fence = [&] {
        if (hwcc) {
          ++c.hwcc_fence_ops;
        } else if (owner) {
          ++c.owner_private_swcc_fence_ops;
        } else {
          ++c.local_dram_fence_ops;
        }
      };
      auto increment_wait_notify = [&] {
        if (hwcc) {
          ++c.hwcc_wait_notify_ops;
        } else if (owner) {
          ++c.owner_private_swcc_wait_notify_ops;
        } else {
          ++c.local_dram_wait_notify_ops;
        }
      };

      switch (operation) {
        case AtomicOperation::kLoad:
          increment_load();
          break;
        case AtomicOperation::kStore:
          increment_store();
          break;
        case AtomicOperation::kCas:
        case AtomicOperation::kCasWeak:
        case AtomicOperation::kCasStrong:
          increment_rmw();
          increment_cas_attempt();
          if (operation == AtomicOperation::kCasWeak) {
            if (hwcc) {
              ++c.hwcc_cas_weak_attempts;
            } else if (owner) {
              ++c.owner_private_swcc_cas_weak_attempts;
            } else {
              ++c.local_dram_cas_weak_attempts;
            }
          } else if (operation == AtomicOperation::kCasStrong) {
            if (hwcc) {
              ++c.hwcc_cas_strong_attempts;
            } else if (owner) {
              ++c.owner_private_swcc_cas_strong_attempts;
            } else {
              ++c.local_dram_cas_strong_attempts;
            }
          }
          if (atomic_cfg.result_breakdown_enabled) {
            increment_cas_result(cas_success);
          }
          break;
        case AtomicOperation::kExchange:
          increment_rmw();
          if (hwcc) {
            ++c.hwcc_exchange_ops;
          } else if (owner) {
            ++c.owner_private_swcc_exchange_ops;
          } else {
            ++c.local_dram_exchange_ops;
          }
          break;
        case AtomicOperation::kFetchAdd:
        case AtomicOperation::kFetchSub:
        case AtomicOperation::kFetchOr:
        case AtomicOperation::kFetchAnd:
        case AtomicOperation::kFetchXor:
          increment_rmw();
          if (operation == AtomicOperation::kFetchAdd) {
            if (hwcc) ++c.hwcc_fetch_add_ops;
            else if (owner) ++c.owner_private_swcc_fetch_add_ops;
            else ++c.local_dram_fetch_add_ops;
          } else if (operation == AtomicOperation::kFetchSub) {
            if (hwcc) ++c.hwcc_fetch_sub_ops;
            else if (owner) ++c.owner_private_swcc_fetch_sub_ops;
            else ++c.local_dram_fetch_sub_ops;
          } else if (operation == AtomicOperation::kFetchOr) {
            if (hwcc) ++c.hwcc_fetch_or_ops;
            else if (owner) ++c.owner_private_swcc_fetch_or_ops;
            else ++c.local_dram_fetch_or_ops;
          } else if (operation == AtomicOperation::kFetchAnd) {
            if (hwcc) ++c.hwcc_fetch_and_ops;
            else if (owner) ++c.owner_private_swcc_fetch_and_ops;
            else ++c.local_dram_fetch_and_ops;
          } else {
            if (hwcc) ++c.hwcc_fetch_xor_ops;
            else if (owner) ++c.owner_private_swcc_fetch_xor_ops;
            else ++c.local_dram_fetch_xor_ops;
          }
          break;
        case AtomicOperation::kFence:
          increment_fence();
          break;
        case AtomicOperation::kWaitNotify:
          increment_wait_notify();
          break;
      }
      if (atomic_cfg.memory_order_breakdown_enabled) {
        c.atomic_ops_by_domain_memory_order
            [domain_index * kMemoryOrderBucketCount +
             MemoryOrderIndex(order)]++;
      }
      if (atomic_cfg.scope_breakdown_enabled) {
        c.atomic_ops_by_domain_scope[domain_index * kScopeCount +
                                     ScopeIndex(state.scope)]++;
      }
      if (atomic_cfg.tag_breakdown_enabled) {
        c.atomic_ops_by_domain_tag[domain_index * kMaxBreakdownTags + tag]++;
      }
      // Fences and wait/notify are synchronization events, not operations on
      // a memory object.  They have dedicated counters and must not inflate
      // the inclusive object-atomic total.
      if (operation != AtomicOperation::kFence &&
          operation != AtomicOperation::kWaitNotify) {
        if (hwcc) {
          ++c.hwcc_atomic_ops;
        } else if (owner) {
          ++c.owner_private_swcc_atomic_ops;
        } else {
          ++c.local_dram_atomic_ops;
        }
      }
    }
  }
  const PoolKind pool = static_cast<PoolKind>(domain);
  if (features & kFixedLatency) {
    RecordRange(pool, kind, addr, bytes, tag);
  }
  if ((features & kRemoteInvalidation) &&
      (pool == PoolKind::kHwcc || pool == PoolKind::kSwcc)) {
    const MemoryIdentity identity = ResolveIdentity(addr, pool);
    if (kind == AccessKind::kAtomicLoad)
      RecordRemoteRead(identity, impl_->node_id, state.scope, tag);
    if (kind == AccessKind::kAtomicStore)
      RecordRemoteWrite(identity, impl_->node_id, false, state.scope, tag);
    if (kind == AccessKind::kAtomicRmw)
      RecordRemoteWrite(identity, impl_->node_id, true, state.scope, tag);
  }
}

MemoryIdentity ResolveIdentityFor(const std::vector<RangeMapping>& mappings,
                                  const void* address, MemoryDomain domain) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  for (const auto& mapping : mappings) {
    if (value >= mapping.begin && value < mapping.end) {
      MemoryIdentity identity = mapping.first;
      identity.domain = domain;
      identity.pool_offset += static_cast<uint64_t>(value - mapping.begin);
      return identity;
    }
  }
  // This fallback is used only by tests and by paths not yet attached through
  // CxlBasic.  It is deliberately not used as a cross-VM identity in the pool
  // layer; attached pools always register their stable pool id and offset.
  return MemoryIdentity{domain, 0, static_cast<uint64_t>(value)};
}

MemoryIdentity LatencySimulator::ResolveIdentity(const void* address,
                                                 MemoryDomain domain) const {
  std::lock_guard<std::mutex> lock(impl_->mapping_mu);
  return ResolveIdentityFor(impl_->mappings, address, domain);
}

void LatencySimulator::RecordRemoteEvent(const MemoryIdentity& identity,
                                         uint32_t node,
                                         RemoteEventKind event,
                                         uint32_t source_node,
                                         ScopeKind scope, uint32_t tag) {
  if (!(HardwareSimulationFeaturesFast() & kRemoteInvalidation)) return;
  if (node >= config_.remote_cache_invalidation.node_count) {
    std::abort();
  }
  if (static_cast<uint8_t>(identity.domain) >
          static_cast<uint8_t>(MemoryDomain::kHwcc) ||
      static_cast<uint8_t>(event) >
          static_cast<uint8_t>(RemoteEventKind::kDirtyCapacityEviction)) {
    std::abort();
  }
  if (event == RemoteEventKind::kExplicitSwccHandoff) {
    if (identity.domain != MemoryDomain::kSwcc ||
        source_node >= config_.remote_cache_invalidation.node_count ||
        source_node == node) {
      std::abort();
    }
  } else if (source_node != UINT32_MAX) {
    std::abort();
  }
  if (static_cast<size_t>(scope) >= kScopeCount || tag >= kMaxBreakdownTags) {
    std::abort();
  }
  if (!config_.remote_cache_invalidation.scope_breakdown_enabled) {
    scope = ScopeKind::kOther;
  }
  if (!config_.remote_cache_invalidation.tag_breakdown_enabled) {
    tag = 0;
  } else if (tag >= config_.remote_cache_invalidation.max_tags) {
    std::abort();
  }
  MemoryIdentity canonical = identity;
  const uint64_t line_bytes =
      config_.remote_cache_invalidation.cache_line_bytes;
  canonical.pool_offset = (canonical.pool_offset / line_bytes) * line_bytes;
  std::lock_guard<std::mutex> lock(impl_->remote_mu);
  uint64_t sequence = 0;
  if (impl_->shared_sequence != nullptr) {
    sequence = impl_->shared_sequence->fetch_add(1, std::memory_order_acq_rel) + 1;
    if (sequence == 0 || sequence > impl_->shared_event_capacity) {
      std::abort();
    }
    RemoteEventRecord& record = impl_->shared_event_log[sequence - 1];
    record.pool_id = canonical.pool_id;
    record.pool_offset = canonical.pool_offset;
    record.domain_and_kind =
        static_cast<uint64_t>(canonical.domain) |
        (static_cast<uint64_t>(event) << 8);
    record.node = node;
    record.reserved[0] = source_node;
    record.reserved[1] = static_cast<uint64_t>(scope);
    record.reserved[2] = tag;
    reinterpret_cast<std::atomic<uint64_t>*>(&record.sequence)->store(
        sequence, std::memory_order_release);
  } else {
    sequence = ++impl_->next_sequence;
    if (sequence != impl_->last_sequence + 1) std::abort();
    impl_->last_sequence = sequence;
  }
  ThreadState& state = GetThreadState();
  ApplyRemoteModelEvent(
      config_, &impl_->remote_access_number, impl_->cache_ways,
      impl_->cache_set_count_by_node, &impl_->cache_sets_by_node,
      &impl_->remote, canonical, node, event, &state.stats, source_node,
      scope, tag);
}

void LatencySimulator::RecordRemoteRead(const MemoryIdentity& identity,
                                        uint32_t node, ScopeKind scope,
                                        uint32_t tag) {
  RecordRemoteEvent(identity, node, RemoteEventKind::kRead, UINT32_MAX, scope,
                    tag);
}

void LatencySimulator::RecordRemoteWrite(const MemoryIdentity& identity,
                                         uint32_t node, bool atomic_rmw,
                                         ScopeKind scope, uint32_t tag) {
  RecordRemoteEvent(identity, node,
                    atomic_rmw ? RemoteEventKind::kAtomicRmw
                               : RemoteEventKind::kWrite,
                    UINT32_MAX, scope, tag);
}

void LatencySimulator::RecordSwccExplicitHandoff(const MemoryIdentity& identity,
                                                 uint32_t from_node,
                                                 uint32_t to_node,
                                                 ScopeKind scope,
                                                 uint32_t tag) {
  if (from_node == to_node) return;
  RecordRemoteEvent(identity, to_node, RemoteEventKind::kExplicitSwccHandoff,
                    from_node, scope, tag);
}

void LatencySimulator::RecordSwccExplicitHandoff(
    PoolKind pool, const void* addr, uint64_t bytes, uint32_t from_node,
    uint32_t to_node, uint32_t tag) {
  if (pool != PoolKind::kSwcc || addr == nullptr || bytes == 0 ||
      !(HardwareSimulationFeaturesFast() & kRemoteInvalidation)) {
    return;
  }
  const MemoryIdentity first = ResolveIdentity(addr, pool);
  const uint64_t line_bytes =
      config_.remote_cache_invalidation.cache_line_bytes;
  const uint64_t count = LineCount(addr, bytes, line_bytes);
  for (uint64_t i = 0; i < count; ++i) {
    MemoryIdentity identity = first;
    identity.pool_offset = (first.pool_offset / line_bytes + i) * line_bytes;
    RecordSwccExplicitHandoff(identity, from_node, to_node,
                              GetThreadState().scope, tag);
  }
}

void LatencySimulator::RecordDirtyCapacityEviction(
    const MemoryIdentity& identity, uint32_t node, ScopeKind scope,
    uint32_t tag) {
  RecordRemoteEvent(identity, node, RemoteEventKind::kDirtyCapacityEviction,
                    UINT32_MAX, scope, tag);
}

void LatencySimulator::RegisterMemoryRange(const void* base, uint64_t bytes,
                                           MemoryDomain domain, uint64_t pool_id,
                                           uint64_t pool_offset) {
  if (base == nullptr || bytes == 0) return;
  const uintptr_t begin = reinterpret_cast<uintptr_t>(base);
  const uintptr_t end = begin + static_cast<uintptr_t>(bytes);
  if (end <= begin) std::abort();
  std::lock_guard<std::mutex> lock(impl_->mapping_mu);
  impl_->mappings.push_back({begin, end, {domain, pool_id, pool_offset}});
}

void LatencySimulator::AttachSharedRemoteLog(void* sequence_word,
                                             void* event_log,
                                             uint64_t event_capacity) {
  if (sequence_word == nullptr || event_log == nullptr || event_capacity == 0 ||
      reinterpret_cast<uintptr_t>(sequence_word) % alignof(uint64_t) != 0 ||
      reinterpret_cast<uintptr_t>(event_log) % alignof(RemoteEventRecord) != 0) {
    throw std::invalid_argument("invalid shared remote instrumentation area");
  }
  std::lock_guard<std::mutex> lock(impl_->remote_mu);
  impl_->shared_sequence =
      reinterpret_cast<std::atomic<uint64_t>*>(sequence_word);
  impl_->shared_event_log = reinterpret_cast<RemoteEventRecord*>(event_log);
  impl_->shared_event_capacity = event_capacity;
  const uint64_t count = impl_->shared_sequence->load(std::memory_order_acquire);
  if (count > event_capacity) std::abort();
  // An attached process must replay the complete global log, including
  // events emitted by an earlier VM during startup.  A nonzero sequence is
  // not a reason to discard history; phase boundaries are established by
  // TakeStatsAndReset after all participants have attached.
  impl_->shared_replay_base_sequence = 0;
  impl_->shared_replay_baseline_stats = Stats{};
  impl_->shared_replay_cumulative_stats = Stats{};
  impl_->shared_replay_baseline_initialized = true;
  impl_->shared_replay_stats = Stats{};
  impl_->shared_replay_valid = false;
}

void LatencySimulator::ValidateSharedRemoteLog() const {
  std::lock_guard<std::mutex> lock(impl_->remote_mu);
  if (impl_->shared_sequence == nullptr) {
    impl_->shared_replay_valid = false;
    return;
  }
  const uint64_t count =
      impl_->shared_sequence->load(std::memory_order_acquire);
  if (count > impl_->shared_event_capacity) std::abort();
  if (count < impl_->shared_replay_base_sequence) std::abort();
  for (uint64_t i = 0; i < count; ++i) {
    const uint64_t committed = reinterpret_cast<const std::atomic<uint64_t>*>(
                                   &impl_->shared_event_log[i].sequence)
                                   ->load(std::memory_order_acquire);
    if (committed != i + 1) {
      // A sequence reservation without a committed record is an event loss;
      // formal results must fail instead of silently reporting a prefix.
      std::abort();
    }
  }
  CacheSetsByNode replay_cache_sets;
  replay_cache_sets.reserve(impl_->cache_set_count_by_node.size());
  for (uint64_t set_count : impl_->cache_set_count_by_node) {
    const uint64_t instances =
        config_.remote_cache_invalidation.cache_instances_per_node;
    if (set_count > std::numeric_limits<uint64_t>::max() / instances) {
      std::abort();
    }
    replay_cache_sets.emplace_back(
        static_cast<size_t>(set_count * instances));
  }
  RemoteDirectory replay_directory;
  uint64_t replay_access_number = 0;
  Stats replay_stats;
  Stats attach_baseline_stats;
  if (impl_->shared_replay_baseline_initialized) {
    attach_baseline_stats = impl_->shared_replay_baseline_stats;
  }
  for (uint64_t i = 0; i < count; ++i) {
    const RemoteEventRecord& record = impl_->shared_event_log[i];
    const uint64_t raw_domain = record.domain_and_kind & 0xffULL;
    const uint64_t raw_event = (record.domain_and_kind >> 8) & 0xffULL;
    if ((record.domain_and_kind >> 16) != 0 ||
        record.reserved[1] >= kScopeCount ||
        record.reserved[2] >= kMaxBreakdownTags) {
      std::abort();
    }
    const ScopeKind scope = static_cast<ScopeKind>(record.reserved[1]);
    const uint32_t tag = static_cast<uint32_t>(record.reserved[2]);
    if ((!config_.remote_cache_invalidation.scope_breakdown_enabled &&
         scope != ScopeKind::kOther) ||
        (config_.remote_cache_invalidation.tag_breakdown_enabled
             ? tag >= config_.remote_cache_invalidation.max_tags
             : tag != 0)) {
      std::abort();
    }
    if (raw_domain > static_cast<uint64_t>(MemoryDomain::kHwcc) ||
        raw_event > static_cast<uint64_t>(RemoteEventKind::kDirtyCapacityEviction) ||
        record.node >= config_.remote_cache_invalidation.node_count) {
      std::abort();
    }
    if (record.pool_offset % config_.remote_cache_invalidation.cache_line_bytes !=
        0) {
      std::abort();
    }
    const MemoryIdentity identity{
        static_cast<MemoryDomain>(raw_domain), record.pool_id,
        record.pool_offset};
    const uint32_t source_node =
        static_cast<uint32_t>(record.reserved[0]);
    if (raw_event ==
            static_cast<uint64_t>(RemoteEventKind::kExplicitSwccHandoff) &&
        (raw_domain != static_cast<uint64_t>(MemoryDomain::kSwcc) ||
         source_node >= config_.remote_cache_invalidation.node_count ||
         source_node == record.node)) {
      std::abort();
    }
    if (raw_event !=
            static_cast<uint64_t>(RemoteEventKind::kExplicitSwccHandoff) &&
        source_node != UINT32_MAX) {
      std::abort();
    }
    ApplyRemoteModelEvent(
        config_, &replay_access_number, impl_->cache_ways,
        impl_->cache_set_count_by_node, &replay_cache_sets,
        &replay_directory, identity, static_cast<uint32_t>(record.node),
        static_cast<RemoteEventKind>(raw_event), &replay_stats, source_node,
        scope, tag);
    if (!impl_->shared_replay_baseline_initialized &&
        i + 1 == impl_->shared_replay_base_sequence) {
      attach_baseline_stats = replay_stats;
    }
  }
  if (!impl_->shared_replay_baseline_initialized) {
    if (impl_->shared_replay_base_sequence != 0 &&
        impl_->shared_replay_base_sequence > count) {
      std::abort();
    }
    impl_->shared_replay_baseline_stats = attach_baseline_stats;
    impl_->shared_replay_baseline_initialized = true;
  }
  impl_->shared_replay_cumulative_stats = replay_stats;
  impl_->shared_replay_stats =
      SubtractRemoteStats(replay_stats, impl_->shared_replay_baseline_stats);
  impl_->shared_replay_valid = true;
}

void LatencySimulator::DetachSharedRemoteLog(void* sequence_word,
                                             void* event_log) {
  std::lock_guard<std::mutex> lock(impl_->remote_mu);
  if (impl_->shared_sequence == sequence_word &&
      impl_->shared_event_log == event_log) {
    impl_->shared_sequence = nullptr;
    impl_->shared_event_log = nullptr;
    impl_->shared_event_capacity = 0;
    impl_->shared_replay_base_sequence = 0;
    impl_->shared_replay_baseline_stats = Stats{};
    impl_->shared_replay_cumulative_stats = Stats{};
    impl_->shared_replay_baseline_initialized = false;
    impl_->shared_replay_stats = Stats{};
    impl_->shared_replay_valid = false;
  }
}

void LatencySimulator::UnregisterMemoryRange(const void* base, uint64_t bytes) {
  if (base == nullptr || bytes == 0) return;
  const uintptr_t begin = reinterpret_cast<uintptr_t>(base);
  const uintptr_t end = begin + static_cast<uintptr_t>(bytes);
  std::lock_guard<std::mutex> lock(impl_->mapping_mu);
  impl_->mappings.erase(
      std::remove_if(impl_->mappings.begin(), impl_->mappings.end(),
                     [begin, end](const RangeMapping& mapping) {
                       return mapping.begin == begin && mapping.end == end;
                     }),
      impl_->mappings.end());
}

void LatencySimulator::SetNodeId(uint32_t node_id) {
  if (node_id >= config_.remote_cache_invalidation.node_count) {
    throw std::invalid_argument("remote node id outside configured node_count");
  }
  std::lock_guard<std::mutex> lock(impl_->remote_mu);
  impl_->node_id = node_id;
}

uint32_t LatencySimulator::node_id() const {
  std::lock_guard<std::mutex> lock(impl_->remote_mu);
  return impl_->node_id;
}

Stats LatencySimulator::SnapshotStats() const {
  ValidateSharedRemoteLog();
  Stats result;
  Stats replay_stats;
  bool replay_valid = false;
  {
    std::lock_guard<std::mutex> lock(impl_->remote_mu);
    replay_valid = impl_->shared_replay_valid;
    replay_stats = impl_->shared_replay_stats;
  }
  std::lock_guard<std::mutex> lock(impl_->registry_mu);
  for (const ThreadState* state : impl_->registered_states) {
    const Stats& c = state->stats;
#define ADD_FIELD(field) result.field += c.field
    ADD_FIELD(swcc_read_ops); ADD_FIELD(swcc_write_ops);
    ADD_FIELD(swcc_read_lines); ADD_FIELD(swcc_write_lines);
    ADD_FIELD(swcc_read_bytes); ADD_FIELD(swcc_write_bytes);
    ADD_FIELD(hwcc_read_ops); ADD_FIELD(hwcc_write_ops);
    ADD_FIELD(hwcc_read_lines); ADD_FIELD(hwcc_write_lines);
    ADD_FIELD(hwcc_read_bytes); ADD_FIELD(hwcc_write_bytes);
    AddArray(&result.hwcc_read_ops_by_scope, c.hwcc_read_ops_by_scope);
    AddArray(&result.hwcc_write_ops_by_scope, c.hwcc_write_ops_by_scope);
    AddArray(&result.hwcc_read_lines_by_scope, c.hwcc_read_lines_by_scope);
    AddArray(&result.hwcc_write_lines_by_scope, c.hwcc_write_lines_by_scope);
    AddArray(&result.hwcc_read_bytes_by_scope, c.hwcc_read_bytes_by_scope);
    AddArray(&result.hwcc_write_bytes_by_scope, c.hwcc_write_bytes_by_scope);
    AddArray(&result.hwcc_read_ops_by_tag, c.hwcc_read_ops_by_tag);
    AddArray(&result.hwcc_write_ops_by_tag, c.hwcc_write_ops_by_tag);
    AddArray(&result.hwcc_read_lines_by_tag, c.hwcc_read_lines_by_tag);
    AddArray(&result.hwcc_write_lines_by_tag, c.hwcc_write_lines_by_tag);
    AddArray(&result.hwcc_read_bytes_by_tag, c.hwcc_read_bytes_by_tag);
    AddArray(&result.hwcc_write_bytes_by_tag, c.hwcc_write_bytes_by_tag);
    ADD_FIELD(hwcc_atomic_ops); ADD_FIELD(hwcc_atomic_loads);
    ADD_FIELD(hwcc_atomic_stores);
    ADD_FIELD(hwcc_atomic_rmw); ADD_FIELD(hwcc_exchange_ops);
    ADD_FIELD(hwcc_fetch_add_ops); ADD_FIELD(hwcc_fetch_sub_ops);
    ADD_FIELD(hwcc_fetch_or_ops); ADD_FIELD(hwcc_fetch_and_ops);
    ADD_FIELD(hwcc_fetch_xor_ops); ADD_FIELD(hwcc_cas_attempts);
    ADD_FIELD(hwcc_cas_weak_attempts); ADD_FIELD(hwcc_cas_strong_attempts);
    ADD_FIELD(hwcc_cas_successes); ADD_FIELD(hwcc_cas_failures);
    ADD_FIELD(owner_private_swcc_atomic_ops); ADD_FIELD(local_dram_atomic_ops);
    ADD_FIELD(owner_private_swcc_atomic_loads);
    ADD_FIELD(owner_private_swcc_atomic_stores);
    ADD_FIELD(owner_private_swcc_atomic_rmw);
    ADD_FIELD(local_dram_atomic_loads); ADD_FIELD(local_dram_atomic_stores);
    ADD_FIELD(local_dram_atomic_rmw);
    ADD_FIELD(owner_private_swcc_exchange_ops);
    ADD_FIELD(owner_private_swcc_fetch_add_ops);
    ADD_FIELD(owner_private_swcc_fetch_sub_ops);
    ADD_FIELD(owner_private_swcc_fetch_or_ops);
    ADD_FIELD(owner_private_swcc_fetch_and_ops);
    ADD_FIELD(owner_private_swcc_fetch_xor_ops);
    ADD_FIELD(local_dram_exchange_ops); ADD_FIELD(local_dram_fetch_add_ops);
    ADD_FIELD(local_dram_fetch_sub_ops); ADD_FIELD(local_dram_fetch_or_ops);
    ADD_FIELD(local_dram_fetch_and_ops); ADD_FIELD(local_dram_fetch_xor_ops);
    ADD_FIELD(owner_private_swcc_cas_attempts);
    ADD_FIELD(owner_private_swcc_cas_weak_attempts);
    ADD_FIELD(owner_private_swcc_cas_strong_attempts);
    ADD_FIELD(owner_private_swcc_cas_successes);
    ADD_FIELD(owner_private_swcc_cas_failures);
    ADD_FIELD(local_dram_cas_attempts);
    ADD_FIELD(local_dram_cas_weak_attempts);
    ADD_FIELD(local_dram_cas_strong_attempts);
    ADD_FIELD(local_dram_cas_successes);
    ADD_FIELD(local_dram_cas_failures);
    ADD_FIELD(hwcc_fence_ops); ADD_FIELD(owner_private_swcc_fence_ops);
    ADD_FIELD(local_dram_fence_ops); ADD_FIELD(hwcc_wait_notify_ops);
    ADD_FIELD(owner_private_swcc_wait_notify_ops);
    ADD_FIELD(local_dram_wait_notify_ops);
    AddArray(&result.atomic_ops_by_domain_scope,
             c.atomic_ops_by_domain_scope);
    AddArray(&result.atomic_ops_by_domain_memory_order,
             c.atomic_ops_by_domain_memory_order);
    AddArray(&result.atomic_ops_by_domain_tag, c.atomic_ops_by_domain_tag);
    ADD_FIELD(swcc_delayed_ns); ADD_FIELD(hwcc_delayed_ns);
#undef ADD_FIELD
  }
  if (replay_valid) {
    result.remote_dirty_handoffs = replay_stats.remote_dirty_handoffs;
    result.remote_clean_copy_invalidations =
        replay_stats.remote_clean_copy_invalidations;
    result.remote_write_transactions_causing_invalidation =
        replay_stats.remote_write_transactions_causing_invalidation;
    result.remote_dirty_capacity_evictions =
        replay_stats.remote_dirty_capacity_evictions;
    result.remote_swcc_explicit_handoffs =
        replay_stats.remote_swcc_explicit_handoffs;
    result.remote_events = replay_stats.remote_events;
    result.remote_dirty_handoffs_by_scope =
        replay_stats.remote_dirty_handoffs_by_scope;
    result.remote_clean_copy_invalidations_by_scope =
        replay_stats.remote_clean_copy_invalidations_by_scope;
    result.remote_write_transactions_causing_invalidation_by_scope =
        replay_stats.remote_write_transactions_causing_invalidation_by_scope;
    result.remote_dirty_capacity_evictions_by_scope =
        replay_stats.remote_dirty_capacity_evictions_by_scope;
    result.remote_swcc_explicit_handoffs_by_scope =
        replay_stats.remote_swcc_explicit_handoffs_by_scope;
    result.remote_events_by_scope = replay_stats.remote_events_by_scope;
    result.remote_dirty_handoffs_by_tag = replay_stats.remote_dirty_handoffs_by_tag;
    result.remote_clean_copy_invalidations_by_tag =
        replay_stats.remote_clean_copy_invalidations_by_tag;
    result.remote_write_transactions_causing_invalidation_by_tag =
        replay_stats.remote_write_transactions_causing_invalidation_by_tag;
    result.remote_dirty_capacity_evictions_by_tag =
        replay_stats.remote_dirty_capacity_evictions_by_tag;
    result.remote_swcc_explicit_handoffs_by_tag =
        replay_stats.remote_swcc_explicit_handoffs_by_tag;
    result.remote_events_by_tag = replay_stats.remote_events_by_tag;
  } else {
    for (const ThreadState* state : impl_->registered_states) {
      result.remote_dirty_handoffs += state->stats.remote_dirty_handoffs;
      result.remote_clean_copy_invalidations +=
          state->stats.remote_clean_copy_invalidations;
      result.remote_write_transactions_causing_invalidation +=
          state->stats.remote_write_transactions_causing_invalidation;
      result.remote_dirty_capacity_evictions +=
          state->stats.remote_dirty_capacity_evictions;
      result.remote_swcc_explicit_handoffs +=
          state->stats.remote_swcc_explicit_handoffs;
      result.remote_events += state->stats.remote_events;
      AddArray(&result.remote_dirty_handoffs_by_scope,
               state->stats.remote_dirty_handoffs_by_scope);
      AddArray(&result.remote_clean_copy_invalidations_by_scope,
               state->stats.remote_clean_copy_invalidations_by_scope);
      AddArray(&result.remote_write_transactions_causing_invalidation_by_scope,
               state->stats.remote_write_transactions_causing_invalidation_by_scope);
      AddArray(&result.remote_dirty_capacity_evictions_by_scope,
               state->stats.remote_dirty_capacity_evictions_by_scope);
      AddArray(&result.remote_swcc_explicit_handoffs_by_scope,
               state->stats.remote_swcc_explicit_handoffs_by_scope);
      AddArray(&result.remote_events_by_scope,
               state->stats.remote_events_by_scope);
      AddArray(&result.remote_dirty_handoffs_by_tag,
               state->stats.remote_dirty_handoffs_by_tag);
      AddArray(&result.remote_clean_copy_invalidations_by_tag,
               state->stats.remote_clean_copy_invalidations_by_tag);
      AddArray(&result.remote_write_transactions_causing_invalidation_by_tag,
               state->stats.remote_write_transactions_causing_invalidation_by_tag);
      AddArray(&result.remote_dirty_capacity_evictions_by_tag,
               state->stats.remote_dirty_capacity_evictions_by_tag);
      AddArray(&result.remote_swcc_explicit_handoffs_by_tag,
               state->stats.remote_swcc_explicit_handoffs_by_tag);
      AddArray(&result.remote_events_by_tag, state->stats.remote_events_by_tag);
    }
  }
  return result;
}

Stats LatencySimulator::TakeStatsAndReset() {
  ValidateSharedRemoteLog();
  Stats result;
  Stats replay_stats;
  bool replay_valid = false;
  {
    std::lock_guard<std::mutex> lock(impl_->remote_mu);
    replay_valid = impl_->shared_replay_valid;
    replay_stats = impl_->shared_replay_stats;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->registry_mu);
    for (ThreadState* state : impl_->registered_states) {
    const Stats& c = state->stats;
#define TAKE_FIELD(field) result.field += c.field
    TAKE_FIELD(swcc_read_ops); TAKE_FIELD(swcc_write_ops);
    TAKE_FIELD(swcc_read_lines); TAKE_FIELD(swcc_write_lines);
    TAKE_FIELD(swcc_read_bytes); TAKE_FIELD(swcc_write_bytes);
    TAKE_FIELD(hwcc_read_ops); TAKE_FIELD(hwcc_write_ops);
    TAKE_FIELD(hwcc_read_lines); TAKE_FIELD(hwcc_write_lines);
    TAKE_FIELD(hwcc_read_bytes); TAKE_FIELD(hwcc_write_bytes);
    AddArray(&result.hwcc_read_ops_by_scope, c.hwcc_read_ops_by_scope);
    AddArray(&result.hwcc_write_ops_by_scope, c.hwcc_write_ops_by_scope);
    AddArray(&result.hwcc_read_lines_by_scope, c.hwcc_read_lines_by_scope);
    AddArray(&result.hwcc_write_lines_by_scope, c.hwcc_write_lines_by_scope);
    AddArray(&result.hwcc_read_bytes_by_scope, c.hwcc_read_bytes_by_scope);
    AddArray(&result.hwcc_write_bytes_by_scope, c.hwcc_write_bytes_by_scope);
    AddArray(&result.hwcc_read_ops_by_tag, c.hwcc_read_ops_by_tag);
    AddArray(&result.hwcc_write_ops_by_tag, c.hwcc_write_ops_by_tag);
    AddArray(&result.hwcc_read_lines_by_tag, c.hwcc_read_lines_by_tag);
    AddArray(&result.hwcc_write_lines_by_tag, c.hwcc_write_lines_by_tag);
    AddArray(&result.hwcc_read_bytes_by_tag, c.hwcc_read_bytes_by_tag);
    AddArray(&result.hwcc_write_bytes_by_tag, c.hwcc_write_bytes_by_tag);
    TAKE_FIELD(hwcc_atomic_ops); TAKE_FIELD(hwcc_atomic_loads);
    TAKE_FIELD(hwcc_atomic_stores);
    TAKE_FIELD(hwcc_atomic_rmw); TAKE_FIELD(hwcc_exchange_ops);
    TAKE_FIELD(hwcc_fetch_add_ops); TAKE_FIELD(hwcc_fetch_sub_ops);
    TAKE_FIELD(hwcc_fetch_or_ops); TAKE_FIELD(hwcc_fetch_and_ops);
    TAKE_FIELD(hwcc_fetch_xor_ops); TAKE_FIELD(hwcc_cas_attempts);
    TAKE_FIELD(hwcc_cas_weak_attempts); TAKE_FIELD(hwcc_cas_strong_attempts);
    TAKE_FIELD(hwcc_cas_successes); TAKE_FIELD(hwcc_cas_failures);
    TAKE_FIELD(owner_private_swcc_atomic_ops); TAKE_FIELD(local_dram_atomic_ops);
    TAKE_FIELD(owner_private_swcc_atomic_loads);
    TAKE_FIELD(owner_private_swcc_atomic_stores);
    TAKE_FIELD(owner_private_swcc_atomic_rmw);
    TAKE_FIELD(local_dram_atomic_loads); TAKE_FIELD(local_dram_atomic_stores);
    TAKE_FIELD(local_dram_atomic_rmw);
    TAKE_FIELD(owner_private_swcc_exchange_ops);
    TAKE_FIELD(owner_private_swcc_fetch_add_ops);
    TAKE_FIELD(owner_private_swcc_fetch_sub_ops);
    TAKE_FIELD(owner_private_swcc_fetch_or_ops);
    TAKE_FIELD(owner_private_swcc_fetch_and_ops);
    TAKE_FIELD(owner_private_swcc_fetch_xor_ops);
    TAKE_FIELD(local_dram_exchange_ops); TAKE_FIELD(local_dram_fetch_add_ops);
    TAKE_FIELD(local_dram_fetch_sub_ops); TAKE_FIELD(local_dram_fetch_or_ops);
    TAKE_FIELD(local_dram_fetch_and_ops); TAKE_FIELD(local_dram_fetch_xor_ops);
    TAKE_FIELD(owner_private_swcc_cas_attempts);
    TAKE_FIELD(owner_private_swcc_cas_weak_attempts);
    TAKE_FIELD(owner_private_swcc_cas_strong_attempts);
    TAKE_FIELD(owner_private_swcc_cas_successes);
    TAKE_FIELD(owner_private_swcc_cas_failures);
    TAKE_FIELD(local_dram_cas_attempts);
    TAKE_FIELD(local_dram_cas_weak_attempts);
    TAKE_FIELD(local_dram_cas_strong_attempts);
    TAKE_FIELD(local_dram_cas_successes);
    TAKE_FIELD(local_dram_cas_failures);
    TAKE_FIELD(hwcc_fence_ops); TAKE_FIELD(owner_private_swcc_fence_ops);
    TAKE_FIELD(local_dram_fence_ops); TAKE_FIELD(hwcc_wait_notify_ops);
    TAKE_FIELD(owner_private_swcc_wait_notify_ops);
    TAKE_FIELD(local_dram_wait_notify_ops);
    AddArray(&result.atomic_ops_by_domain_scope,
             c.atomic_ops_by_domain_scope);
    AddArray(&result.atomic_ops_by_domain_memory_order,
             c.atomic_ops_by_domain_memory_order);
    AddArray(&result.atomic_ops_by_domain_tag, c.atomic_ops_by_domain_tag);
    TAKE_FIELD(swcc_delayed_ns); TAKE_FIELD(hwcc_delayed_ns);
#undef TAKE_FIELD
      state->stats = Stats{};
    }
  }
  if (replay_valid) {
    result.remote_dirty_handoffs = replay_stats.remote_dirty_handoffs;
    result.remote_clean_copy_invalidations =
        replay_stats.remote_clean_copy_invalidations;
    result.remote_write_transactions_causing_invalidation =
        replay_stats.remote_write_transactions_causing_invalidation;
    result.remote_dirty_capacity_evictions =
        replay_stats.remote_dirty_capacity_evictions;
    result.remote_swcc_explicit_handoffs =
        replay_stats.remote_swcc_explicit_handoffs;
    result.remote_events = replay_stats.remote_events;
    result.remote_dirty_handoffs_by_scope =
        replay_stats.remote_dirty_handoffs_by_scope;
    result.remote_clean_copy_invalidations_by_scope =
        replay_stats.remote_clean_copy_invalidations_by_scope;
    result.remote_write_transactions_causing_invalidation_by_scope =
        replay_stats.remote_write_transactions_causing_invalidation_by_scope;
    result.remote_dirty_capacity_evictions_by_scope =
        replay_stats.remote_dirty_capacity_evictions_by_scope;
    result.remote_swcc_explicit_handoffs_by_scope =
        replay_stats.remote_swcc_explicit_handoffs_by_scope;
    result.remote_events_by_scope = replay_stats.remote_events_by_scope;
    result.remote_dirty_handoffs_by_tag = replay_stats.remote_dirty_handoffs_by_tag;
    result.remote_clean_copy_invalidations_by_tag =
        replay_stats.remote_clean_copy_invalidations_by_tag;
    result.remote_write_transactions_causing_invalidation_by_tag =
        replay_stats.remote_write_transactions_causing_invalidation_by_tag;
    result.remote_dirty_capacity_evictions_by_tag =
        replay_stats.remote_dirty_capacity_evictions_by_tag;
    result.remote_swcc_explicit_handoffs_by_tag =
        replay_stats.remote_swcc_explicit_handoffs_by_tag;
    result.remote_events_by_tag = replay_stats.remote_events_by_tag;
  } else {
    for (const ThreadState* state : impl_->registered_states) {
      result.remote_dirty_handoffs += state->stats.remote_dirty_handoffs;
      result.remote_clean_copy_invalidations +=
          state->stats.remote_clean_copy_invalidations;
      result.remote_write_transactions_causing_invalidation +=
          state->stats.remote_write_transactions_causing_invalidation;
      result.remote_dirty_capacity_evictions +=
          state->stats.remote_dirty_capacity_evictions;
      result.remote_swcc_explicit_handoffs +=
          state->stats.remote_swcc_explicit_handoffs;
      result.remote_events += state->stats.remote_events;
      AddArray(&result.remote_dirty_handoffs_by_scope,
               state->stats.remote_dirty_handoffs_by_scope);
      AddArray(&result.remote_clean_copy_invalidations_by_scope,
               state->stats.remote_clean_copy_invalidations_by_scope);
      AddArray(&result.remote_write_transactions_causing_invalidation_by_scope,
               state->stats.remote_write_transactions_causing_invalidation_by_scope);
      AddArray(&result.remote_dirty_capacity_evictions_by_scope,
               state->stats.remote_dirty_capacity_evictions_by_scope);
      AddArray(&result.remote_swcc_explicit_handoffs_by_scope,
               state->stats.remote_swcc_explicit_handoffs_by_scope);
      AddArray(&result.remote_events_by_scope,
               state->stats.remote_events_by_scope);
      AddArray(&result.remote_dirty_handoffs_by_tag,
               state->stats.remote_dirty_handoffs_by_tag);
      AddArray(&result.remote_clean_copy_invalidations_by_tag,
               state->stats.remote_clean_copy_invalidations_by_tag);
      AddArray(&result.remote_write_transactions_causing_invalidation_by_tag,
               state->stats.remote_write_transactions_causing_invalidation_by_tag);
      AddArray(&result.remote_dirty_capacity_evictions_by_tag,
               state->stats.remote_dirty_capacity_evictions_by_tag);
      AddArray(&result.remote_swcc_explicit_handoffs_by_tag,
               state->stats.remote_swcc_explicit_handoffs_by_tag);
      AddArray(&result.remote_events_by_tag, state->stats.remote_events_by_tag);
    }
  }
  if (replay_valid) {
    std::lock_guard<std::mutex> remote_lock(impl_->remote_mu);
    const uint64_t count =
        impl_->shared_sequence->load(std::memory_order_acquire);
    if (count < impl_->shared_replay_base_sequence) std::abort();
    // The shared log is append-only so the coherence state must be replayed
    // from sequence one on every validation. Advancing the baseline here
    // resets phase counters without discarding warmup cache state.
    impl_->shared_replay_base_sequence = count;
    impl_->shared_replay_baseline_stats = impl_->shared_replay_cumulative_stats;
    impl_->shared_replay_valid = false;
  }
  return result;
}

uint64_t LatencySimulator::PendingDelayNsForTest() const {
  auto it = g_tls.find(this);
  if (it == g_tls.end() || it->second->generation != generation_) return 0;
  return it->second->pending_delay_ns;
}

}  // namespace latency_sim
