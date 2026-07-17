#include "kv/latency_simulator.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

namespace tigonkv {

void LatencySimulator::Configure(bool enabled, uint64_t hwcc_ns,
                                 uint64_t swcc_read_ns, uint64_t swcc_write_ns,
                                 uint64_t swcc_flush_ns) {
  enabled_.store(enabled, std::memory_order_release);
  hwcc_ns_ = hwcc_ns;
  swcc_read_ns_ = swcc_read_ns;
  swcc_write_ns_ = swcc_write_ns;
  swcc_flush_ns_ = swcc_flush_ns;
}

void LatencySimulator::Record(PoolKind pool, AccessKind kind, uint64_t bytes) {
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
  uint64_t delay = 0;
  if (pool == PoolKind::kHwcc) delay = hwcc_ns_ * lines;
  else if (kind == AccessKind::kRead) delay = swcc_read_ns_ * lines;
  else if (kind == AccessKind::kFlush) delay = swcc_flush_ns_ * lines;
  else delay = swcc_write_ns_ * lines;
  delayed_ns_.fetch_add(delay);
  if (delay != 0) std::this_thread::sleep_for(std::chrono::nanoseconds(delay));
}

LatencyStats LatencySimulator::Snapshot() const {
  return {hwcc_reads_.load(), hwcc_writes_.load(), hwcc_atomics_.load(),
          swcc_reads_.load(), swcc_writes_.load(), swcc_flushes_.load(),
          delayed_ns_.load()};
}

void LatencySimulator::Reset() {
  hwcc_reads_ = hwcc_writes_ = hwcc_atomics_ = 0;
  swcc_reads_ = swcc_writes_ = swcc_flushes_ = delayed_ns_ = 0;
}

NonCoherentSwccTestBackend::NonCoherentSwccTestBackend(size_t bytes)
    : bytes_(bytes), visible_(bytes, 0), cache_(2, std::vector<uint8_t>(bytes, 0)),
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

void NonCoherentSwccTestBackend::Read(uint32_t host, size_t offset, void *data,
                                      size_t bytes) const {
  if (host >= cache_.size() || offset > bytes_ || bytes > bytes_ - offset)
    throw std::out_of_range("non-coherent SWCC read");
  std::memcpy(data, cache_[host].data() + offset, bytes);
}

const uint8_t *NonCoherentSwccTestBackend::Visible() const { return visible_.data(); }
}  // namespace tigonkv
