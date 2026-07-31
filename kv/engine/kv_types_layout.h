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
// v22: Clock tracker membership is represented only by independent
// owner-private tracker nodes; local row metadata no longer persists a reverse
// node link.
// v23: owner allocator controls are fixed slots before partition arenas and
// private roots are atomic RegionOffsets.
// v24: partition-wide scan-range migrate single-flight flag.
// v26: same single-flight semantics (v25 multi-slot experiment dropped).
constexpr uint32_t kSharedLayoutVersion = 26;
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

// The original TwoPLPasha local metadata is one storage body. Only the two
// references vary between the legacy DRAM representation and the persistent
// KV representation; all latch/state fields and their order stay identical.
template <typename MigratedRowRef, typename SccDataRef>
struct TwoPLPashaMetadataLocalStorage {
  TwoPLPashaMetadataLocalStorage()
      : tid(0),
        is_valid(false),
        is_migrated(false),
        is_data_modified_since_moved_out(true),
        migrated_row{},
        scc_data{} {
    pthread_spin_init(&latch, PTHREAD_PROCESS_PRIVATE);
  }

  void lock() { pthread_spin_lock(&latch); }
  void unlock() { pthread_spin_unlock(&latch); }

  pthread_spinlock_t latch;
  uint64_t tid{0};
  bool is_valid{false};
  bool is_migrated{false};
  bool is_data_modified_since_moved_out{true};
  // The second spelling is retained as a source-level adapter for the
  // offset-backed KV call sites. Both names are the same one storage slot;
  // there is no second locator or state field.
  union {
    MigratedRowRef migrated_row;
    MigratedRowRef migrated_smeta_off;
  };
  union {
    SccDataRef scc_data;
    SccDataRef scc_data_off;
  };
};

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
    if (fixed_size == 0 || fixed_size > kMaxFixedKeyBytes ||
        key.size() != fixed_size)
      throw std::invalid_argument("invalid fixed key size");
    FixedKey result{};
    std::memcpy(result.bytes, key.data(), key.size());
    return result;
  }

  static FixedKey InternalMax(uint32_t fixed_size) {
    if (fixed_size == 0 || fixed_size > kMaxFixedKeyBytes)
      throw std::invalid_argument("invalid internal max key size");
    FixedKey result{};
    std::memset(result.bytes, 0xff, fixed_size);
    return result;
  }

  int Compare(const FixedKey &other) const {
    return std::memcmp(bytes, other.bytes, kMaxFixedKeyBytes);
  }

  uint64_t get_plain_key() const {
    uint64_t result = 0;
    for (size_t i = 0; i < sizeof(result); ++i)
      result = (result << 8) | static_cast<unsigned char>(bytes[i]);
    return result;
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

// Offset-adapted form of the original TableBTreeOLC::ValueStruct. The lmeta
// storage itself is defined once in the TwoPLPasha helper and is specialized
// there for RegionOffset references.
struct PrivateValueStruct {
  std::atomic<RegionOffset> meta{kNullOffset};
  char data[];
};

using PrivateMetadataLocal =
    TwoPLPashaMetadataLocalStorage<RegionOffset, RegionOffset>;

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

struct OwnerPrivateClockTrackerControl {
  pthread_spinlock_t lock{};
  RegionOffset head{kNullOffset};
  RegionOffset tail{kNullOffset};
  RegionOffset cursor{kNullOffset};
};

struct alignas(64) PartitionDirectoryEntry {
  // Shared-tree live root (HWCC). Updated on makeRoot/merge; every shared
  // tree op loads this atomically so already-attached peers see splits.
  std::atomic<RegionOffset> shared_root{kNullOffset};
  RegionOffset private_arena = kNullOffset;
  // Owner sets for scan-migrate through move_in (not whole move_out; §3.9.1).
  // One in-flight DATA_MIGRATION_REQUEST_FOR_SCAN per owner partition.
  std::atomic<uint32_t> scan_range_migrate_inflight{0};
};
static_assert(sizeof(PartitionDirectoryEntry) == 64,
              "directory contains only globally coherent shared-tree state");

// The first object in the HWCC region. Fields are fixed-width so an attach in a
// separately mapped process can validate the complete layout before dereference.
struct alignas(64) SharedLayoutHeader {
  // The reset VM publishes this only after every immutable layout field is
  // initialized.  Joining VMs use it as the acquire/release attachment gate.
  std::atomic<uint64_t> magic{0};
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
};

static_assert(alignof(SharedLayoutHeader) == 64);
static_assert(sizeof(FixedKey) == kMaxFixedKeyBytes);
static_assert(alignof(PartitionDirectoryEntry) == 64);

}  // namespace tigonkv::engine
