#include "kv/engine/latency_inject.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace latency_sim {

struct ThreadState {
  uint64_t generation = 0;
  uint32_t scope_depth = 0;
  bool scope_enabled = false;
  uint64_t pending_delay_ns = 0;
};

namespace {

std::atomic<uint32_t> g_features{0};
std::atomic<uint64_t> g_generation{0};
thread_local std::unordered_map<const LatencySimulator*, ThreadState*> g_tls;

uint64_t NextGeneration() {
  const uint64_t generation =
      g_generation.fetch_add(1, std::memory_order_relaxed) + 1;
  if (generation == 0) std::abort();
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
    const auto begin = std::chrono::steady_clock::now();
    const uint64_t start_tsc = ReadTsc();
    auto now = begin;
    while (now - begin < std::chrono::milliseconds(2)) {
      CpuRelax();
      now = std::chrono::steady_clock::now();
    }
    const uint64_t end_tsc = ReadTsc();
    const uint64_t elapsed_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - begin)
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
  if (ns == 0) return;
  CalibrateTscOnce();
#if defined(__x86_64__) || defined(__i386__)
  if (!HasCalibratedTsc()) {
    throw std::runtime_error("fixed latency requires a calibrated TSC");
  }
  const uint64_t start = ReadTsc();
  const long double requested_ticks =
      static_cast<long double>(g_ticks_per_ns) * ns;
  const uint64_t ticks = requested_ticks >=
                                 std::numeric_limits<uint64_t>::max()
                             ? std::numeric_limits<uint64_t>::max()
                             : std::max<uint64_t>(
                                   1, static_cast<uint64_t>(requested_ticks));
  while (ReadTsc() - start < ticks) CpuRelax();
#else
  throw std::runtime_error("fixed latency requires x86 TSC support");
#endif
}

uint64_t RoundNs(double ns) {
  if (!(ns > 0.0) || !std::isfinite(ns)) return 0;
  if (ns >= static_cast<double>(std::numeric_limits<uint64_t>::max()))
    return std::numeric_limits<uint64_t>::max();
  return static_cast<uint64_t>(std::llround(ns));
}

uint64_t LineCount(const void* address, uint64_t bytes, uint64_t line_bytes) {
  if (address == nullptr || bytes == 0) return 0;
  const uintptr_t start = reinterpret_cast<uintptr_t>(address);
  const uintptr_t length = static_cast<uintptr_t>(bytes - 1);
  if (length > std::numeric_limits<uintptr_t>::max() - start) std::abort();
  const uintptr_t last = start + length;
  return static_cast<uint64_t>(last / line_bytes - start / line_bytes + 1);
}

uint64_t AddSaturating(uint64_t left, uint64_t right) {
  if (right > std::numeric_limits<uint64_t>::max() - left)
    return std::numeric_limits<uint64_t>::max();
  return left + right;
}

uint64_t MultiplySaturating(uint64_t left, uint64_t right) {
  if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left)
    return std::numeric_limits<uint64_t>::max();
  return left * right;
}

bool IsChargeableDomain(PoolKind pool) {
  return pool == PoolKind::kSwcc || pool == PoolKind::kHwcc ||
         pool == PoolKind::kOwnerPrivateSwcc;
}

uint64_t FixedDelayPerLine(const Config& config, PoolKind pool) {
  if (pool == PoolKind::kHwcc)
    return RoundNs(config.fixed_latency.hwcc_fixed_ns_per_line);
  if (pool == PoolKind::kSwcc || pool == PoolKind::kOwnerPrivateSwcc)
    return RoundNs(config.fixed_latency.swcc_fixed_ns_per_line);
  return 0;
}

}  // namespace

#if !defined(TIGONKV_DISABLE_HARDWARE_SIMULATION)
uint32_t FixedLatencyFeaturesFast() noexcept {
  return g_features.load(std::memory_order_relaxed);
}
#endif

LatencySimulator& GlobalLatencySimulator() {
  static LatencySimulator simulator;
  return simulator;
}

LatencySimulator::LatencySimulator(Config config)
    : config_(std::move(config)), generation_(NextGeneration()) {
  Configure(config_);
}

LatencySimulator::~LatencySimulator() = default;

void LatencySimulator::Configure(Config config) {
  const auto& fixed = config.fixed_latency;
  if (fixed.cache_line_bytes == 0 ||
      (fixed.cache_line_bytes & (fixed.cache_line_bytes - 1)) != 0) {
    throw std::invalid_argument(
        "fixed_latency.cache_line_bytes must be a power of two");
  }
  for (double value : {fixed.swcc_fixed_ns_per_line,
                       fixed.hwcc_fixed_ns_per_line}) {
    if (!std::isfinite(value) || value < 0.0)
      throw std::invalid_argument(
          "fixed latency values must be finite and non-negative");
  }
  if (fixed.enabled) {
    CalibrateTscOnce();
    if (!HasCalibratedTsc()) {
      throw std::runtime_error(
          "fixed latency requires a calibrated x86 TSC; no clock fallback is permitted");
    }
  }
  config_ = std::move(config);
  generation_ = NextGeneration();
#if defined(TIGONKV_DISABLE_HARDWARE_SIMULATION)
  feature_mask_ = 0;
#else
  feature_mask_ = config_.fixed_latency.enabled
                      ? static_cast<uint32_t>(kFixedLatency)
                      : 0u;
#endif
  g_features.store(feature_mask_, std::memory_order_relaxed);
}

ThreadState& LatencySimulator::GetThreadState() {
  auto it = g_tls.find(this);
  if (it == g_tls.end()) {
    auto* state = new ThreadState;
    it = g_tls.emplace(this, state).first;
  }
  ThreadState& state = *it->second;
  if (state.generation != generation_) {
    state = ThreadState{};
    state.generation = generation_;
  }
  return state;
}

void LatencySimulator::BeginScope(ScopeKind scope) {
  if (!FixedLatencyEnabledFast()) return;
  ThreadState& state = GetThreadState();
  if (state.scope_depth != 0) {
    ++state.scope_depth;
    return;
  }
  state.scope_depth = 1;
  state.scope_enabled =
      scope == ScopeKind::kForeground
          ? config_.fixed_latency.foreground_enabled
          : config_.fixed_latency.background_enabled;
  state.pending_delay_ns = 0;
}

void LatencySimulator::EndScopeAndDelay() {
  if (!FixedLatencyEnabledFast()) return;
  auto it = g_tls.find(this);
  if (it == g_tls.end() || it->second->generation != generation_) return;
  ThreadState& state = *it->second;
  if (state.scope_depth == 0) return;
  if (state.scope_depth > 1) {
    --state.scope_depth;
    return;
  }
  const uint64_t delay = state.pending_delay_ns;
  state = ThreadState{};
  state.generation = generation_;
  DelaySpinNs(delay);
}

bool LatencySimulator::HasActiveScopeForCurrentThread() const {
  if (!FixedLatencyEnabledFast()) return false;
  const auto it = g_tls.find(this);
  return it != g_tls.end() && it->second != nullptr &&
         it->second->generation == generation_ && it->second->scope_depth != 0;
}

void LatencySimulator::RecordRange(PoolKind pool, AccessKind,
                                   const void* address, uint64_t bytes) {
  if (!FixedLatencyEnabledFast() || !IsChargeableDomain(pool) ||
      address == nullptr || bytes == 0)
    return;
  auto it = g_tls.find(this);
  if (it == g_tls.end() || it->second->generation != generation_ ||
      it->second->scope_depth == 0 || !it->second->scope_enabled)
    return;
  const uint64_t lines =
      LineCount(address, bytes, config_.fixed_latency.cache_line_bytes);
  const uint64_t delay = FixedDelayPerLine(config_, pool);
  it->second->pending_delay_ns = AddSaturating(
      it->second->pending_delay_ns, MultiplySaturating(lines, delay));
}

void LatencySimulator::RecordAtomicAccess(AtomicDomain domain,
                                          AccessKind kind,
                                          const void* address,
                                          uint64_t bytes) {
  RecordRange(static_cast<PoolKind>(domain), kind, address, bytes);
}

uint64_t LatencySimulator::PendingDelayNsForTest() const {
  if (!FixedLatencyEnabledFast()) return 0;
  const auto it = g_tls.find(this);
  if (it == g_tls.end() || it->second == nullptr ||
      it->second->generation != generation_)
    return 0;
  return it->second->pending_delay_ns;
}

}  // namespace latency_sim
