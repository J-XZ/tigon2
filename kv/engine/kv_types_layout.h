#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <pthread.h>
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
// v13: PolicyClock tracker state lives in owner-private SWCC.
// (owner-private SWCC), not process-heap ClockTrackerNode (§11.14).
// v15: layout identity includes the configured ordered range boundaries.
// v16: private-tree root and Clock control state live in owner-private SWCC.
// v17: shared metadata removes the KV-only writer-preference byte and restores
//      the original single-attempt TwoPLPasha row-lock semantics.
// v18: drops the unused Scan-certificate mutation generation from HWCC.
// v19: startup has a single Initializing→Ready publication; clean-exit and
//      checkpoint coordination are process-local, never shared layout state.
// v21: removes the current-only Clock migrated-key hot counter and records
//      the HWCC smeta offset in each owner-private Clock tracker node.
constexpr uint32_t kSharedLayoutVersion = 21;
constexpr size_t kMaxFixedKeyBytes = 32;
constexpr size_t kRootSlotCount = 8;
constexpr size_t kMaxPartitions = 256;
constexpr uint32_t kMaxAllocatorShards = 64;

// Frozen architecture contract (Scan原始Tigon对齐修改方案.md §11.1 / §14.1).
// Formal KV API and wire protocol accept exactly one logical table; partitions
// are ordered range-routing shards, not additional tables. Single-key APIs are
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

enum class LayoutState : uint32_t { kInitializing = 0, kReady = 1 };

struct alignas(64) DomainCounter {
  std::atomic<uint64_t> used_bytes{0};
  std::atomic<uint64_t> peak_bytes{0};
};

struct OwnerDynamicArenaDescriptor {
  RegionOffset hwcc_offset = kNullOffset;
  uint64_t hwcc_bytes = 0;
  RegionOffset shared_swcc_offset = kNullOffset;
  uint64_t shared_swcc_bytes = 0;
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

// Offset-adapted forms of the original TableBTreeOLC::ValueStruct and
// TwoPLPashaMetadataLocal.  They are separate owner-private allocations: the
// leaf carries the ValueStruct offset, its atomic meta carries this metadata
// offset, and no process virtual address survives an attach.
struct PrivateValueStruct {
  std::atomic<RegionOffset> meta{kNullOffset};
  char data[];
};

struct alignas(64) PrivateMetadataLocal {
  PrivateMetadataLocal() {
    pthread_spin_init(&latch, PTHREAD_PROCESS_PRIVATE);
  }

  void lock() { pthread_spin_lock(&latch); }
  void unlock() { pthread_spin_unlock(&latch); }

  pthread_spinlock_t latch;
  uint64_t tid{0};
  bool is_valid{false};
  bool is_migrated{false};
  bool is_data_modified_since_moved_out{true};
  uint8_t reserved{0};
  RegionOffset migrated_smeta_off{kNullOffset};
  RegionOffset scc_data_off{kNullOffset};
  RegionOffset clock_node_off{kNullOffset};
};
static_assert(alignof(PrivateMetadataLocal) == 64);

// Mechanical owner-private adaptation of PolicyClock::ClockTrackerNode.  The
// original node owns a key copy independently of ValueStruct; keeping it
// separate avoids smuggling a key into the local-row header.
struct PrivateClockTrackerNode {
  RegionOffset value_off{kNullOffset};
  RegionOffset smeta_off{kNullOffset};
  RegionOffset prev_off{kNullOffset};
  RegionOffset next_off{kNullOffset};
  FixedKey key{};
};

struct alignas(64) PartitionDirectoryEntry {
  // Shared-tree live root (HWCC). Updated on makeRoot/merge; every shared
  // tree op loads this atomically so already-attached peers see splits.
  std::atomic<RegionOffset> shared_root{kNullOffset};
  RegionOffset private_arena = kNullOffset;
  std::atomic<uint64_t> migration_in_seq{0};
};
static_assert(sizeof(PartitionDirectoryEntry) == 64,
              "directory contains only globally coherent shared-tree state");

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
  // Each owner completes its own private arena/root initialization before VM0
  // releases Ready. This bitmap is startup-only and never becomes a recovery
  // generation or a checkpoint protocol.
  std::atomic<uint64_t> owner_init_ready_bitmap{0};
  std::array<std::atomic<RegionOffset>, kRootSlotCount> roots{};
  // Immutable dynamic data ranges. Their allocator headers/counters live in
  // the owning VM's private SWCC arena, never in this HWCC layout.
  std::array<OwnerDynamicArenaDescriptor, kMaxAllocatorShards>
      owner_dynamic_arenas{};
  std::array<PartitionDirectoryEntry, kMaxPartitions> partitions{};
  std::array<DomainCounter, kAllocationDomainCount> domains{};

  bool IsCompatible(uint64_t expected_hash, uint64_t expected_pool_bytes,
                    uint32_t expected_vms, uint32_t expected_partitions) const {
    return magic == kSharedLayoutMagic && layout_version == kSharedLayoutVersion &&
           config_hash == expected_hash && total_pool_bytes == expected_pool_bytes &&
           vm_count == expected_vms && partition_count == expected_partitions &&
           state.load(std::memory_order_acquire) ==
               static_cast<uint32_t>(LayoutState::kReady);
  }
};

static_assert(alignof(SharedLayoutHeader) == 64);
static_assert(sizeof(FixedKey) == kMaxFixedKeyBytes);
static_assert(alignof(PartitionDirectoryEntry) == 64);

}  // namespace tigonkv::engine
