#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tigonkv::test {

// Deterministic SWCC model: each host reads its own cache; only WriteBack
// changes the visible copy and only Invalidate refreshes another host cache.
class NonCoherentSwccTestBackend {
 public:
  explicit NonCoherentSwccTestBackend(size_t bytes = 4096, uint32_t hosts = 2);
  void Write(uint32_t host, size_t offset, const void *data, size_t bytes);
  void WriteBack(uint32_t host, size_t offset, size_t bytes);
  void Invalidate(uint32_t host, size_t offset, size_t bytes);
  bool Publish(uint32_t host, size_t offset, size_t bytes);
  bool IsPublished(size_t offset) const;
  void Read(uint32_t host, size_t offset, void *data, size_t bytes) const;
  uint64_t flushes() const { return flushes_; }
  uint64_t invalidates() const { return invalidates_; }

 private:
  void Check(uint32_t host, size_t offset, size_t bytes) const;
  size_t bytes_;
  std::vector<uint8_t> visible_, published_;
  std::vector<std::vector<uint8_t>> cache_, dirty_;
  uint64_t flushes_ = 0, invalidates_ = 0;
};

}  // namespace tigonkv::test
