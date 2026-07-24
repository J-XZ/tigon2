#include "kv/latency_simulator.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace tigonkv {

namespace {
thread_local std::unordered_map<const LatencySimulator *, uint64_t> pending_delay;
thread_local std::unordered_map<const LatencySimulator *, std::deque<uint64_t>> lru_lines;
std::once_flag tsc_calibration_once;
std::atomic<double> tsc_ticks_per_ns{0.0};

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
  std::call_once(tsc_calibration_once, [] {
#if defined(__x86_64__) || defined(__i386__)
    constexpr auto kCalibrationWindow = std::chrono::milliseconds(4);
    const auto begin = std::chrono::steady_clock::now();
    const uint64_t ticks_begin = ReadTsc();
    auto end = begin;
    do {
      CpuRelax();
      end = std::chrono::steady_clock::now();
    } while (end - begin < kCalibrationWindow);
    const uint64_t ticks_end = ReadTsc();
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
    if (elapsed_ns > 0 && ticks_end > ticks_begin)
      tsc_ticks_per_ns.store(static_cast<double>(ticks_end - ticks_begin) /
                                 static_cast<double>(elapsed_ns),
                             std::memory_order_release);
#endif
  });
}

void DelaySpinNs(uint64_t delay_ns) {
  if (delay_ns == 0) return;
  CalibrateTscOnce();
  const double ticks_per_ns = tsc_ticks_per_ns.load(std::memory_order_acquire);
#if defined(__x86_64__) || defined(__i386__)
  if (ticks_per_ns > 0.0) {
    const uint64_t start = ReadTsc();
    const uint64_t target = start + std::max<uint64_t>(
        1, static_cast<uint64_t>(ticks_per_ns * static_cast<double>(delay_ns)));
    while (ReadTsc() < target) CpuRelax();
    return;
  }
#endif
  std::this_thread::sleep_for(std::chrono::nanoseconds(delay_ns));
}
}

void LatencySimulator::Configure(bool enabled, uint64_t hwcc_ns,
                                 uint64_t swcc_read_ns, uint64_t swcc_write_ns,
                                 uint64_t swcc_flush_ns) {
  ConfigureDetailed(enabled, hwcc_ns, hwcc_ns, hwcc_ns, swcc_read_ns,
                    swcc_write_ns, swcc_flush_ns, "none", 0.0, 0);
}

void LatencySimulator::ConfigureDetailed(bool enabled, uint64_t hwcc_read_ns,
                                         uint64_t hwcc_write_ns,
                                         uint64_t hwcc_atomic_ns,
                                         uint64_t swcc_read_ns,
                                         uint64_t swcc_write_ns,
                                         uint64_t swcc_flush_ns,
                                         const char *cache_model,
                                         double fixed_hit_rate,
                                         uint64_t cache_capacity_lines) {
  enabled_.store(enabled, std::memory_order_release);
  hwcc_read_ns_ = hwcc_read_ns;
  hwcc_write_ns_ = hwcc_write_ns;
  hwcc_atomic_ns_ = hwcc_atomic_ns;
  swcc_read_ns_ = swcc_read_ns;
  swcc_write_ns_ = swcc_write_ns;
  swcc_flush_ns_ = swcc_flush_ns;
  cache_model_ = cache_model;
  fixed_hit_rate_ = fixed_hit_rate;
  cache_capacity_lines_ = cache_capacity_lines;
}

void LatencySimulator::Record(PoolKind pool, AccessKind kind, uint64_t bytes,
                              uint64_t cache_line_id) {
  const uint64_t lines = std::max<uint64_t>(1, (bytes + 63) / 64);
  if (pool == PoolKind::kHwcc) {
    if (kind == AccessKind::kRead) hwcc_reads_.fetch_add(lines);
    else if (kind == AccessKind::kWrite) hwcc_writes_.fetch_add(lines);
    else hwcc_atomics_.fetch_add(lines);
  } else if (kind == AccessKind::kRead) {
    swcc_reads_.fetch_add(lines);
  } else if (kind == AccessKind::kFlush) {
    swcc_flushes_.fetch_add(lines);
  } else {
    swcc_writes_.fetch_add(lines);
  }
  if (!enabled_.load(std::memory_order_acquire)) return;
  uint64_t cache_hits = 0;
  uint64_t cache_misses = 0;
  if (pool == PoolKind::kSwcc && kind == AccessKind::kRead) {
    if (std::strcmp(cache_model_, "fixed_hit_rate") == 0) {
      for (uint64_t line = 0; line < lines; ++line) {
        const uint64_t sample = cache_sequence_.fetch_add(1);
        if (static_cast<double>(sample % 10000) / 10000.0 < fixed_hit_rate_)
          ++cache_hits;
        else
          ++cache_misses;
      }
    } else if (std::strcmp(cache_model_, "per_thread_lru") == 0 &&
               cache_capacity_lines_ != 0) {
      auto &lru = lru_lines[this];
      // Without an address token, bytes is a stable synthetic line identity;
      // callers that instrument real objects should pass their line token.
      const uint64_t first_line = cache_line_id == 0 ? bytes / 64 : cache_line_id;
      for (uint64_t line = 0; line < lines; ++line) {
        const uint64_t token = first_line + line;
        auto found = std::find(lru.begin(), lru.end(), token);
        if (found != lru.end()) {
          ++cache_hits;
          lru.erase(found);
          lru.push_front(token);
        } else {
          ++cache_misses;
          lru.push_front(token);
          if (lru.size() > cache_capacity_lines_) lru.pop_back();
        }
      }
    } else {
      cache_misses = lines;
    }
    cache_hits_.fetch_add(cache_hits);
    cache_misses_.fetch_add(cache_misses);
  }
  uint64_t delay = 0;
  if (pool == PoolKind::kHwcc) {
    if (kind == AccessKind::kRead) delay = hwcc_read_ns_ * lines;
    else if (kind == AccessKind::kWrite) delay = hwcc_write_ns_ * lines;
    else delay = hwcc_atomic_ns_ * lines;
  }
  else if (kind == AccessKind::kRead) delay = swcc_read_ns_ * cache_misses;
  else if (kind == AccessKind::kFlush) delay = swcc_flush_ns_ * lines;
  else delay = swcc_write_ns_ * lines;
  delayed_ns_.fetch_add(delay);
  pending_delay[this] += delay;
}

void LatencySimulator::DrainPending() {
  auto it = pending_delay.find(this);
  if (it == pending_delay.end()) return;
  const uint64_t delay = it->second;
  pending_delay.erase(it);
  DelaySpinNs(delay);
}

LatencyStats LatencySimulator::Snapshot() const {
  return {hwcc_reads_.load(), hwcc_writes_.load(), hwcc_atomics_.load(),
          swcc_reads_.load(), swcc_writes_.load(), swcc_flushes_.load(),
          delayed_ns_.load(), cache_hits_.load(), cache_misses_.load()};
}

void LatencySimulator::Reset() {
  hwcc_reads_ = hwcc_writes_ = hwcc_atomics_ = 0;
  swcc_reads_ = swcc_writes_ = swcc_flushes_ = delayed_ns_ = 0;
  cache_hits_ = cache_misses_ = cache_sequence_ = 0;
  pending_delay.erase(this);
  lru_lines.erase(this);
}

NonCoherentSwccTestBackend::NonCoherentSwccTestBackend(size_t bytes)
    : bytes_(bytes), visible_(bytes, 0), published_(bytes, 0),
      cache_(2, std::vector<uint8_t>(bytes, 0)),
      dirty_(2, std::vector<uint8_t>(bytes, 0)) {}

void NonCoherentSwccTestBackend::Write(uint32_t host, size_t offset, const void *data,
                                       size_t bytes) {
  if (host >= cache_.size() || offset > bytes_ || bytes > bytes_ - offset)
    throw std::out_of_range("non-coherent SWCC write");
  std::memcpy(cache_[host].data() + offset, data, bytes);
  std::fill(dirty_[host].begin() + offset, dirty_[host].begin() + offset + bytes, 1);
}

void NonCoherentSwccTestBackend::WriteBack(uint32_t host, size_t offset, size_t bytes) {
  if (host >= cache_.size() || offset > bytes_ || bytes > bytes_ - offset)
    throw std::out_of_range("non-coherent SWCC write-back");
  for (size_t i = offset; i < offset + bytes; ++i) {
    if (dirty_[host][i]) { visible_[i] = cache_[host][i]; dirty_[host][i] = 0; }
  }
}

void NonCoherentSwccTestBackend::Invalidate(uint32_t host, size_t offset, size_t bytes) {
  if (host >= cache_.size() || offset > bytes_ || bytes > bytes_ - offset)
    throw std::out_of_range("non-coherent SWCC invalidate");
  std::memcpy(cache_[host].data() + offset, visible_.data() + offset, bytes);
  std::fill(dirty_[host].begin() + offset, dirty_[host].begin() + offset + bytes, 0);
}

bool NonCoherentSwccTestBackend::Publish(uint32_t host, size_t offset, size_t bytes) {
  if (host >= cache_.size() || offset > bytes_ || bytes > bytes_ - offset)
    throw std::out_of_range("non-coherent SWCC publish");
  for (size_t i = offset; i < offset + bytes; ++i)
    if (dirty_[host][i]) return false;
  std::fill(published_.begin() + offset, published_.begin() + offset + bytes, 1);
  return true;
}

bool NonCoherentSwccTestBackend::IsPublished(size_t offset) const {
  if (offset >= bytes_) throw std::out_of_range("non-coherent SWCC publication lookup");
  return published_[offset] != 0;
}

void NonCoherentSwccTestBackend::Read(uint32_t host, size_t offset, void *data,
                                      size_t bytes) const {
  if (host >= cache_.size() || offset > bytes_ || bytes > bytes_ - offset)
    throw std::out_of_range("non-coherent SWCC read");
  std::memcpy(data, cache_[host].data() + offset, bytes);
}

const uint8_t *NonCoherentSwccTestBackend::Visible() const { return visible_.data(); }
}  // namespace tigonkv
