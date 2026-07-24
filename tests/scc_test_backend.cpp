#include "tests/scc_test_backend.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace tigonkv::test {

NonCoherentSwccTestBackend::NonCoherentSwccTestBackend(size_t bytes, uint32_t hosts)
    : bytes_(bytes), visible_(bytes), published_(bytes),
      cache_(hosts, std::vector<uint8_t>(bytes)),
      dirty_(hosts, std::vector<uint8_t>(bytes)) {
  if (bytes == 0 || hosts == 0) throw std::invalid_argument("invalid non-coherent model");
}

void NonCoherentSwccTestBackend::Check(uint32_t host, size_t offset, size_t bytes) const {
  if (host >= cache_.size() || offset > bytes_ || bytes > bytes_ - offset)
    throw std::out_of_range("non-coherent SWCC range");
}

void NonCoherentSwccTestBackend::Write(uint32_t host, size_t offset,
                                       const void *data, size_t bytes) {
  Check(host, offset, bytes);
  std::memcpy(cache_[host].data() + offset, data, bytes);
  std::fill(dirty_[host].begin() + offset, dirty_[host].begin() + offset + bytes, 1);
}

void NonCoherentSwccTestBackend::WriteBack(uint32_t host, size_t offset, size_t bytes) {
  Check(host, offset, bytes);
  for (size_t i = offset; i < offset + bytes; ++i) {
    if (dirty_[host][i]) { visible_[i] = cache_[host][i]; dirty_[host][i] = 0; }
  }
  ++flushes_;
}

void NonCoherentSwccTestBackend::Invalidate(uint32_t host, size_t offset, size_t bytes) {
  Check(host, offset, bytes);
  std::memcpy(cache_[host].data() + offset, visible_.data() + offset, bytes);
  std::fill(dirty_[host].begin() + offset, dirty_[host].begin() + offset + bytes, 0);
  ++invalidates_;
}

bool NonCoherentSwccTestBackend::Publish(uint32_t host, size_t offset, size_t bytes) {
  Check(host, offset, bytes);
  for (size_t i = offset; i < offset + bytes; ++i)
    if (dirty_[host][i]) return false;
  std::fill(published_.begin() + offset, published_.begin() + offset + bytes, 1);
  return true;
}

bool NonCoherentSwccTestBackend::IsPublished(size_t offset) const {
  if (offset >= bytes_) throw std::out_of_range("non-coherent publication lookup");
  return published_[offset] != 0;
}

void NonCoherentSwccTestBackend::Read(uint32_t host, size_t offset, void *data,
                                      size_t bytes) const {
  Check(host, offset, bytes);
  std::memcpy(data, cache_[host].data() + offset, bytes);
}

}  // namespace tigonkv::test
