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
constexpr uint64_t kSharedLayoutMagic = 0x5449474f4e4b5638ULL;  // TIGONKV8
// v9: allocator metadata accounting is split by its physical HWCC/SWCC pool.
// v10: remote Scan validates concurrent shared removals without persistent pins.
// v11: the Scan generation also covers logical-key insertion/deletion.
// v12: original TwoPLPasha next/prev adjacency bits make shared visibility
// changes self-validating; the generation now certifies logical EOF only.
constexpr uint32_t kSharedLayoutVersion = 12;
constexpr size_t kMaxFixedKeyBytes = 32;
constexpr size_t kRootSlotCount = 8;
constexpr size_t kMaxPartitions = 256;
constexpr uint32_t kMaxAllocatorShards = 64;

// Frozen architecture contract (Scan原始Tigon对齐修改方案.md §11.1 / §14.1).
// Formal KV API and wire protocol accept exactly one logical table; partitions
// are hash routing shards, not additional tables. Single-key APIs are
// linearizable; Scan is not a global cross-partition snapshot.
constexpr uint32_t kSingleTableId = 0;
static_assert(kSingleTableId == 0, "TigonKV exposes exactly one logical table");

enum class AllocationDomain : uint32_t {
  kHwccIndex = 0,
  kHwccMetadata,
  kHwccEbr,
  kHwccLayout,
  kTransport,
  kHwccAllocatorMetadata,
  kOwnerPrivateSwcc,
  kSharedPayloadSwcc,
  kSwccAllocatorMetadata,
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
  char bytes[kMaxFixedKeyBytes];

  static FixedKey From(std::string_view key, uint32_t fixed_size) {
    if (fixed_size == 0 || fixed_size > kMaxFixedKeyBytes || key.size() > fixed_size)
      throw std::invalid_argument("invalid fixed key size");
    FixedKey result{};
    std::memcpy(result.bytes, key.data(), key.size());
    return result;
  }

  int Compare(const FixedKey &other) const {
    return std::memcmp(bytes, other.bytes, kMaxFixedKeyBytes);
  }
};

struct FixedKeyLess {
  bool operator()(const FixedKey &left, const FixedKey &right) const {
    return left.Compare(right) < 0;
  }
};

struct FixedKeyComparator {
  int operator()(const FixedKey &left, const FixedKey &right) const {
    return left.Compare(right);
  }
};

// Owner-private SWCC row header. The key bytes are followed by value bytes in
// kv[]; no raw process pointer is persisted here.
struct alignas(64) PrivateRow {
  std::atomic<uint32_t> latch{0};
  uint8_t is_migrated = 0;
  uint8_t is_tombstone = 0;
  uint16_t key_len = 0;
  uint32_t value_len = 0;
  uint64_t version = 0;
  RegionOffset migrated_smeta_off = kNullOffset;
  char kv[];
};

struct alignas(64) PartitionDirectoryEntry {
  RegionOffset private_root = kNullOffset;
  // Shared-tree live root (HWCC). Updated on makeRoot/merge; every shared
  // tree op loads this atomically so already-attached peers see splits.
  std::atomic<RegionOffset> shared_root{kNullOffset};
  RegionOffset private_arena = kNullOffset;
  std::atomic<uint64_t> migration_in_seq{0};
  // High 32 bits count completed logical-key mutations; low 32 bits count
  // mutations in flight. Remote CXL range readers use this only to certify an
  // exhausted private-tree tail; shared move-in/out uses next/prev bits.
  std::atomic<uint64_t> shared_mutation_state{0};
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
  std::array<std::atomic<RegionOffset>, kMaxAllocatorShards>
      swcc_remote_free_heads{};
  std::array<std::atomic<uint64_t>, kMaxAllocatorShards>
      checkpoint_ready_epoch{};
  std::array<DomainCounter, kMaxAllocatorShards>
      owner_migration_hwcc{};
  std::array<PartitionDirectoryEntry, kMaxPartitions> partitions{};
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
static_assert(alignof(PrivateRow) == 64);
static_assert(alignof(PartitionDirectoryEntry) == 64);

}  // namespace tigonkv::engine
