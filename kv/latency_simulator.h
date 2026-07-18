#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tigonkv {
enum class PoolKind { kHwcc, kSwcc };
enum class AccessKind { kRead, kWrite, kAtomicLoad, kAtomicStore, kAtomicRmw, kFlush };

struct LatencyStats {
  uint64_t hwcc_reads = 0, hwcc_writes = 0, hwcc_atomics = 0;
  uint64_t swcc_reads = 0, swcc_writes = 0, swcc_flushes = 0;
  uint64_t delayed_ns = 0;
  uint64_t cache_hits = 0, cache_misses = 0;
};

class LatencySimulator {
 public:
  void Configure(bool enabled, uint64_t hwcc_ns, uint64_t swcc_read_ns,
                 uint64_t swcc_write_ns, uint64_t swcc_flush_ns);
  void ConfigureDetailed(bool enabled, uint64_t hwcc_read_ns,
                         uint64_t hwcc_write_ns, uint64_t hwcc_atomic_ns,
                         uint64_t swcc_read_ns, uint64_t swcc_write_ns,
                         uint64_t swcc_flush_ns,
                         const char *cache_model = "none",
                         double fixed_hit_rate = 0.0,
                         uint64_t cache_capacity_lines = 0);
  void Record(PoolKind pool, AccessKind kind, uint64_t bytes);
  // Sleep for latency accumulated by the current operation. Call only after
  // releasing protocol locks.
  void DrainPending();
  LatencyStats Snapshot() const;
  void Reset();

 private:
  std::atomic<bool> enabled_{false};
  std::atomic<uint64_t> hwcc_reads_{0}, hwcc_writes_{0}, hwcc_atomics_{0};
  std::atomic<uint64_t> swcc_reads_{0}, swcc_writes_{0}, swcc_flushes_{0};
  std::atomic<uint64_t> delayed_ns_{0};
  std::atomic<uint64_t> cache_hits_{0}, cache_misses_{0}, cache_sequence_{0};
  uint64_t hwcc_read_ns_ = 0, hwcc_write_ns_ = 0, hwcc_atomic_ns_ = 0;
  uint64_t swcc_read_ns_ = 0, swcc_write_ns_ = 0, swcc_flush_ns_ = 0;
  const char *cache_model_ = "none";
  double fixed_hit_rate_ = 0.0;
  uint64_t cache_capacity_lines_ = 0;
};

class NonCoherentSwccTestBackend {
 public:
  explicit NonCoherentSwccTestBackend(size_t bytes = 4096);
  void Write(uint32_t host, size_t offset, const void *data, size_t bytes);
  void WriteBack(uint32_t host, size_t offset, size_t bytes);
  void Invalidate(uint32_t host, size_t offset, size_t bytes);
  // Publication is deliberately rejected while the publishing host still
  // has dirty bytes. This is a deterministic fault-injection hook for the
  // shared-index publication ordering rule.
  bool Publish(uint32_t host, size_t offset, size_t bytes);
  bool IsPublished(size_t offset) const;
  void Read(uint32_t host, size_t offset, void *data, size_t bytes) const;
  const uint8_t *Visible() const;

 private:
  size_t bytes_;
  std::vector<uint8_t> visible_;
  std::vector<uint8_t> published_;
  std::vector<std::vector<uint8_t>> cache_;
  std::vector<std::vector<uint8_t>> dirty_;
};
}  // namespace tigonkv
