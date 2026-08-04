#include "kv/engine/latency_inject.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace latency_sim {

// Fixed-point scale: one picosecond per fixed-point unit.  Fractional
// nanoseconds per line are converted once at configuration time, accumulated
// without rounding for the whole scope, and rounded to nanoseconds only at the
// scope exit.  This makes e.g. 0.4ns x 5 lines exactly 2ns instead of 0ns.
constexpr uint64_t kPsPerNs = 1000;

struct ThreadState {
  uint64_t generation = 0;
  uint32_t scope_depth = 0;
  ScopeKind scope = ScopeKind::kOther;
  bool scope_enabled = false;
  bool suspended = false;
  uint64_t pending_delay_ps = 0;
  std::vector<uint64_t> deferred_delay_ps;
};

namespace {

uint64_t g_generation = 0;
thread_local std::unordered_map<const LatencySimulator*,
                                std::unique_ptr<ThreadState>>
    g_tls;

uint64_t NextGeneration() {
  const uint64_t generation = ++g_generation;
  if (generation == 0) std::abort();
  return generation;
}

[[noreturn]] void HardFail(const char* detail) {
  std::fprintf(stderr, "[fixed latency hard fail] %s\n", detail);
  void* frames[24];
  const int frame_count = ::backtrace(frames, 24);
  if (frame_count > 0) ::backtrace_symbols_fd(frames, frame_count, STDERR_FILENO);
  std::fflush(stderr);
  std::abort();
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
  const uint64_t ticks = TicksForDelayNsForTest(g_ticks_per_ns, ns);
  const uint64_t start = ReadTsc();
  while (ReadTsc() - start < ticks) CpuRelax();
#else
  throw std::runtime_error("fixed latency requires x86 TSC support");
#endif
}

uint64_t FixedPointPerLine(double ns) {
  if (ns == 0.0) return 0;
  if (!std::isfinite(ns) || ns < 0.0 ||
      ns > static_cast<double>(std::numeric_limits<uint64_t>::max()) /
               static_cast<double>(kPsPerNs))
    throw std::invalid_argument(
        "fixed latency nanoseconds per line must be finite and fit uint64");
  const long double scaled =
      static_cast<long double>(ns) * static_cast<long double>(kPsPerNs);
  if (scaled >
      static_cast<long double>(std::numeric_limits<uint64_t>::max()))
    throw std::invalid_argument(
        "fixed latency nanoseconds per line overflows the fixed-point scale");
  const long double rounded = std::round(scaled);
  if (rounded < 0.0L ||
      rounded > static_cast<long double>(std::numeric_limits<uint64_t>::max()))
    throw std::invalid_argument(
        "fixed latency nanoseconds per line overflows the fixed-point scale");
  return static_cast<uint64_t>(rounded);
}

uint64_t CheckedAdd(uint64_t left, uint64_t right) {
  if (right > std::numeric_limits<uint64_t>::max() - left)
    HardFail("fixed latency accumulator overflow; refusing to saturate to "
             "UINT64_MAX");
  return left + right;
}

uint64_t CheckedMultiply(uint64_t left, uint64_t right) {
  if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left)
    HardFail("fixed latency delay overflow; refusing to saturate to UINT64_MAX");
  return left * right;
}

uint64_t LineCountFromOffset(uint64_t offset, uint64_t bytes,
                             uint64_t line_bytes) {
  if (bytes == 0) return 0;
  const uint64_t first_line = offset / line_bytes;
  const uint64_t last_byte = offset + (bytes - 1);  // caller validated
  const uint64_t last_line = last_byte / line_bytes;
  return last_line - first_line + 1;
}

uint64_t CheckedOffsetBytesEnd(uint64_t offset, uint64_t bytes,
                               uint64_t pool_size) {
  if (bytes == 0) return offset;
  if (offset >= pool_size || bytes > pool_size - offset)
    HardFail("fixed latency range [offset, offset+bytes) falls outside the "
             "registered pool");
  if (bytes - 1 > std::numeric_limits<uint64_t>::max() - offset)
    HardFail("fixed latency address-range addition overflow");
  return offset + bytes - 1;
}

bool ScopeEnabled(const FixedLatencyConfig& config, ScopeKind scope) {
  if (scope == ScopeKind::kForeground)
    return config.foreground_enabled;
  if (scope == ScopeKind::kMerge)
    return config.background_enabled;
  return config.foreground_enabled || config.background_enabled;
}

bool IsChargeableDomain(PoolKind pool) {
  return pool == PoolKind::kSwcc || pool == PoolKind::kHwcc ||
         pool == PoolKind::kOwnerPrivateSwcc;
}

}  // namespace

uint64_t RoundDelayPsToNsForTest(uint64_t pending_ps) {
  return pending_ps / kPsPerNs + ((pending_ps % kPsPerNs) >= kPsPerNs / 2);
}

uint64_t TicksForDelayNsForTest(double ticks_per_ns, uint64_t ns) {
  if (!std::isfinite(ticks_per_ns) || ticks_per_ns <= 0.0)
    HardFail("fixed latency TSC calibration is invalid");
  if (ns == 0) return 0;
  const long double requested_ticks =
      static_cast<long double>(ticks_per_ns) * static_cast<long double>(ns);
  if (!std::isfinite(static_cast<double>(requested_ticks)) ||
      requested_ticks <= 0.0L ||
      requested_ticks >
          static_cast<long double>(std::numeric_limits<uint64_t>::max()))
    HardFail("fixed latency delay exceeds the uint64 tick budget");
  // Ceil so a fractional tick budget never waits one tick too few.
  return static_cast<uint64_t>(std::ceil(requested_ticks));
}

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
  swcc_delay_ps_per_line_ =
      FixedPointPerLine(fixed.swcc_fixed_ns_per_line);
  hwcc_delay_ps_per_line_ =
      FixedPointPerLine(fixed.hwcc_fixed_ns_per_line);
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
  g_hardware_simulation_features = feature_mask_;
}

void LatencySimulator::RegisterPool(PoolKind pool, const void* base,
                                    uint64_t size) {
  const size_t index = static_cast<size_t>(pool);
  if (index >= pool_ranges_.size() ||
      (pool != PoolKind::kSwcc && pool != PoolKind::kHwcc &&
       pool != PoolKind::kOwnerPrivateSwcc))
    throw std::invalid_argument(
        "fixed latency pool registration only accepts HWCC/SWCC domains");
  if (base == nullptr || size == 0)
    throw std::invalid_argument(
        "fixed latency pool registration requires non-null base and size");
  pool_ranges_[index].push_back(
      PoolRange{reinterpret_cast<uintptr_t>(base), size});
}

void LatencySimulator::ClearPoolRegistrations() {
  for (auto& ranges : pool_ranges_) ranges.clear();
}

ThreadState& LatencySimulator::GetThreadState() {
  auto it = g_tls.find(this);
  if (it == g_tls.end()) {
    it = g_tls.emplace(this, std::make_unique<ThreadState>()).first;
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
  state.scope = scope;
  state.suspended = false;
  state.scope_depth = 1;
  state.scope_enabled = ScopeEnabled(config_.fixed_latency, scope);
  state.pending_delay_ps = 0;
}

void LatencySimulator::EndScopeAndDelay() {
  if (!FixedLatencyEnabledFast()) return;
  auto it = g_tls.find(this);
  if (it == g_tls.end() || it->second->generation != generation_) return;
  ThreadState& state = *it->second;
  if (state.scope_depth == 0) {
    // A suspended or never-begun scope has no live budget.  Deferred segments
    // are only settled by the outermost active scope exit.
    return;
  }
  if (state.scope_depth > 1) {
    --state.scope_depth;
    return;
  }
  uint64_t total_ps = state.pending_delay_ps;
  state.pending_delay_ps = 0;
  for (const uint64_t deferred : state.deferred_delay_ps)
    total_ps = CheckedAdd(total_ps, deferred);
  state.deferred_delay_ps.clear();
  state.scope_depth = 0;
  state.scope_enabled = false;
  state.suspended = false;
  DelaySpinNs(RoundDelayPsToNsForTest(total_ps));
}

bool LatencySimulator::HasActiveScopeForCurrentThread() const {
  if (!FixedLatencyEnabledFast()) return false;
  const auto it = g_tls.find(this);
  return it != g_tls.end() && it->second != nullptr &&
         it->second->generation == generation_ && it->second->scope_depth != 0;
}

bool LatencySimulator::HasActiveForegroundScopeForCurrentThread() const {
  if (!FixedLatencyEnabledFast()) return false;
  const auto it = g_tls.find(this);
  return it != g_tls.end() && it->second != nullptr &&
         it->second->generation == generation_ &&
         it->second->scope_depth != 0 &&
         it->second->scope == ScopeKind::kForeground;
}

bool LatencySimulator::HasTopLevelForegroundScopeForCurrentThread() const {
  if (!FixedLatencyEnabledFast()) return false;
  const auto it = g_tls.find(this);
  return it != g_tls.end() && it->second != nullptr &&
         it->second->generation == generation_ &&
         it->second->scope_depth == 1 &&
         it->second->scope == ScopeKind::kForeground;
}

bool LatencySimulator::SuspendScopeAndDelayLater() {
  if (!FixedLatencyEnabledFast()) return false;
  auto it = g_tls.find(this);
  if (it == g_tls.end() || it->second->generation != generation_) return false;
  ThreadState& state = *it->second;
  if (state.scope_depth == 0) return false;
  if (state.scope_depth > 1) {
    --state.scope_depth;
    return false;
  }
  if (state.pending_delay_ps != 0)
    state.deferred_delay_ps.push_back(state.pending_delay_ps);
  state.pending_delay_ps = 0;
  state.scope_depth = 0;
  state.scope_enabled = false;
  state.suspended = true;
  return true;
}

void LatencySimulator::ResumeScope() {
  if (!FixedLatencyEnabledFast()) return;
  ThreadState& state = GetThreadState();
  if (state.scope_depth != 0) {
    ++state.scope_depth;
    return;
  }
  state.scope = ScopeKind::kForeground;
  state.suspended = false;
  state.scope_depth = 1;
  state.scope_enabled =
      ScopeEnabled(config_.fixed_latency, ScopeKind::kForeground);
  state.pending_delay_ps = 0;
}

void LatencySimulator::RecordRange(PoolKind pool, AccessKind kind,
                                   const void* address, uint64_t bytes) {
  if (!FixedLatencyEnabledFast()) return;
  // Flush/invalidate are audit labels for real protocol visibility
  // operations; they never add a second fixed-latency charge.
  if (kind == AccessKind::kFlush || kind == AccessKind::kInvalidate)
    return;
  if (!IsChargeableDomain(pool) || bytes == 0) return;
  auto it = g_tls.find(this);
  if (it == g_tls.end() || it->second->generation != generation_ ||
      it->second->scope_depth == 0) {
    HardFail("fixed latency shared-memory access outside an explicit "
             "foreground/background scope");
  }
  if (!it->second->scope_enabled) return;
  if (address == nullptr)
    HardFail("fixed latency access with non-zero bytes must have a valid "
             "address");

  const uintptr_t raw = reinterpret_cast<uintptr_t>(address);
  const PoolKind validation_pool =
      pool == PoolKind::kOwnerPrivateSwcc ? PoolKind::kSwcc : pool;
  const size_t pool_index = static_cast<size_t>(validation_pool);
  if (pool_index >= pool_ranges_.size() || pool_ranges_[pool_index].empty())
    HardFail("fixed latency access on a pool that was never registered");
  const size_t other_index =
      validation_pool == PoolKind::kHwcc
          ? static_cast<size_t>(PoolKind::kSwcc)
          : static_cast<size_t>(PoolKind::kHwcc);
  bool in_own = false;
  for (const PoolRange& range : pool_ranges_[pool_index]) {
    if (raw >= range.base && raw - range.base < range.size) {
      in_own = true;
      break;
    }
  }
  // A mislabeled access sits inside the other physical domain but not inside
  // its own domain.  Test-only pools may register the same mapping under both
  // domains; those addresses are in their own domain as well and stay valid.
  if (!in_own) {
    for (const PoolRange& other : pool_ranges_[other_index]) {
      if (raw >= other.base && raw - other.base < other.size) {
      std::fprintf(
          stderr,
          "[fixed latency hard fail] access address %#lx (bytes %lu) charged "
          "as %s belongs to the other memory domain; own domain ranges:",
          static_cast<unsigned long>(raw), static_cast<unsigned long>(bytes),
          validation_pool == PoolKind::kHwcc ? "HWCC" : "SWCC");
      for (const PoolRange& own : pool_ranges_[pool_index]) {
        std::fprintf(stderr, " [%#lx, %#lx)",
                     static_cast<unsigned long>(own.base),
                     static_cast<unsigned long>(own.base + own.size));
      }
      std::fprintf(
          stderr, " other=%s [%#lx, %#lx)\n",
          validation_pool == PoolKind::kHwcc ? "SWCC" : "HWCC",
          static_cast<unsigned long>(other.base),
          static_cast<unsigned long>(other.base + other.size));
      std::fflush(stderr);
      HardFail(
          "fixed latency access address belongs to the other memory domain");
      }
    }
  }
  uint64_t offset = 0;
  bool contained = false;
  for (const PoolRange& range : pool_ranges_[pool_index]) {
    if (raw < range.base) continue;
    const uint64_t candidate = raw - range.base;
    if (candidate >= range.size) continue;
    CheckedOffsetBytesEnd(candidate, bytes, range.size);
    offset = candidate;
    contained = true;
    break;
  }
  if (!contained)
    HardFail("fixed latency access address is outside every registered pool "
             "range of its domain");
  const uint64_t lines =
      LineCountFromOffset(offset, bytes, config_.fixed_latency.cache_line_bytes);
  const uint64_t delay_ps =
      CheckedMultiply(lines, LineDelayPs(validation_pool));
  it->second->pending_delay_ps =
      CheckedAdd(it->second->pending_delay_ps, delay_ps);
}

void LatencySimulator::RecordAtomicAccess(AtomicDomain domain, AccessKind kind,
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
  return RoundDelayPsToNsForTest(it->second->pending_delay_ps);
}

uint64_t LatencySimulator::PendingDelayPsForTest() const {
  if (!FixedLatencyEnabledFast()) return 0;
  const auto it = g_tls.find(this);
  if (it == g_tls.end() || it->second == nullptr ||
      it->second->generation != generation_)
    return 0;
  return it->second->pending_delay_ps;
}

size_t LatencySimulator::ThreadStateCountForTest() const {
  return g_tls.count(this);
}

uint64_t LatencySimulator::LineDelayPs(PoolKind pool) const {
  if (pool == PoolKind::kHwcc) return hwcc_delay_ps_per_line_;
  if (pool == PoolKind::kSwcc || pool == PoolKind::kOwnerPrivateSwcc)
    return swcc_delay_ps_per_line_;
  return 0;
}

}  // namespace latency_sim
