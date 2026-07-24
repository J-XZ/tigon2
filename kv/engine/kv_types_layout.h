#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace tigonkv::engine {

// tigonkv: persistent references are offsets from their physical region, never
// process virtual addresses. Zero is reserved as the null offset.
using RegionOffset = uint64_t;
constexpr RegionOffset kNullOffset = 0;
constexpr uint64_t kSharedLayoutMagic = 0x5449474f4e4b5633ULL;  // TIGONKV3
constexpr uint32_t kSharedLayoutVersion = 1;
constexpr size_t kMaxFixedKeyBytes = 32;
constexpr size_t kRootSlotCount = 8;

enum class AllocationDomain : uint32_t {
  kHwccIndex = 0,
  kHwccMetadata,
  kHwccEbr,
  kHwccLayout,
  kTransport,
  kOwnerPrivateSwcc,
  kSharedPayloadSwcc,
  kAllocatorMetadata,
  kCount,
};

constexpr size_t kAllocationDomainCount =
    static_cast<size_t>(AllocationDomain::kCount);

enum class LayoutState : uint32_t { kInitializing = 0, kClean = 1, kDirty = 2 };

struct alignas(64) DomainCounter {
  std::atomic<uint64_t> used_bytes{0};
  std::atomic<uint64_t> peak_bytes{0};
};

// This key format is deliberately bytewise and does not assume integral keys.
struct FixedKey {
  std::array<char, kMaxFixedKeyBytes> bytes{};

  static FixedKey From(std::string_view key, uint32_t fixed_size) {
    if (fixed_size == 0 || fixed_size > kMaxFixedKeyBytes || key.size() > fixed_size)
      throw std::invalid_argument("invalid fixed key size");
    FixedKey result;
    std::memcpy(result.bytes.data(), key.data(), key.size());
    return result;
  }

  int Compare(const FixedKey &other) const {
    return std::memcmp(bytes.data(), other.bytes.data(), bytes.size());
  }
};

struct FixedKeyLess {
  bool operator()(const FixedKey &left, const FixedKey &right) const {
    return left.Compare(right) < 0;
  }
};

// The first object in the HWCC region. Fields are fixed-width so an attach in a
// separately mapped process can validate the complete layout before dereference.
struct alignas(64) SharedLayoutHeader {
  uint64_t magic = kSharedLayoutMagic;
  uint32_t layout_version = kSharedLayoutVersion;
  std::atomic<uint32_t> state{static_cast<uint32_t>(LayoutState::kInitializing)};
  uint64_t config_hash = 0;
  uint64_t total_pool_bytes = 0;
  uint64_t hwcc_offset_bytes = 0;
  uint64_t hwcc_size_bytes = 0;
  uint64_t swcc_offset_bytes = 0;
  uint64_t swcc_size_bytes = 0;
  uint32_t vm_count = 0;
  uint32_t partition_count = 0;
  uint32_t fixed_key_size = 0;
  uint32_t fixed_value_size = 0;
  std::atomic<uint64_t> clean_epoch{0};
  std::array<std::atomic<RegionOffset>, kRootSlotCount> roots{};
  std::array<DomainCounter, kAllocationDomainCount> domains{};

  bool IsCompatible(uint64_t expected_hash, uint64_t expected_pool_bytes,
                    uint32_t expected_vms, uint32_t expected_partitions) const {
    return magic == kSharedLayoutMagic && layout_version == kSharedLayoutVersion &&
           config_hash == expected_hash && total_pool_bytes == expected_pool_bytes &&
           vm_count == expected_vms && partition_count == expected_partitions &&
           state.load(std::memory_order_acquire) !=
               static_cast<uint32_t>(LayoutState::kInitializing);
  }
};

static_assert(alignof(SharedLayoutHeader) == 64);
static_assert(sizeof(FixedKey) == kMaxFixedKeyBytes);

}  // namespace tigonkv::engine
