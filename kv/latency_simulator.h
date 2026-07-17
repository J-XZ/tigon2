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
};

class LatencySimulator {
 public:
  void Configure(bool enabled, uint64_t hwcc_ns, uint64_t swcc_read_ns,
                 uint64_t swcc_write_ns, uint64_t swcc_flush_ns);
  void Record(PoolKind pool, AccessKind kind, uint64_t bytes);
  LatencyStats Snapshot() const;
  void Reset();

 private:
  std::atomic<bool> enabled_{false};
  std::atomic<uint64_t> hwcc_reads_{0}, hwcc_writes_{0}, hwcc_atomics_{0};
  std::atomic<uint64_t> swcc_reads_{0}, swcc_writes_{0}, swcc_flushes_{0};
  std::atomic<uint64_t> delayed_ns_{0};
  uint64_t hwcc_ns_ = 0, swcc_read_ns_ = 0, swcc_write_ns_ = 0, swcc_flush_ns_ = 0;
};

class NonCoherentSwccTestBackend {
 public:
  explicit NonCoherentSwccTestBackend(size_t bytes = 4096);
  void Write(uint32_t host, size_t offset, const void *data, size_t bytes);
  void WriteBack(uint32_t host, size_t offset, size_t bytes);
  void Invalidate(uint32_t host, size_t offset, size_t bytes);
  void Read(uint32_t host, size_t offset, void *data, size_t bytes) const;
  const uint8_t *Visible() const;

 private:
  size_t bytes_;
  std::vector<uint8_t> visible_;
  std::vector<std::vector<uint8_t>> cache_;
  std::vector<std::vector<uint8_t>> dirty_;
};
}  // namespace tigonkv
