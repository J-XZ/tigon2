#pragma once

#include "common/CXL_EBR.h"
#include "common/btree_olc_cxl/BTreeOLC_CXL.h"
#include "core/CXLTable.h"
#include "protocol/TwoPLPasha/TwoPLPashaHelper.h"
#include "protocol/Pasha/MigrationManager.h"
#include "kv/engine/kv_types_layout.h"
#include "kv/engine/region_allocator.h"
#include "kv/kv_store.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <pthread.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tigonkv::engine {

class KvPartitionTable;
struct RegionOffsetRowStorage;

struct RegionOffsetSharedRowReference {
  using StoredRow = RegionOffset;
  DualRegionAllocator *regions = nullptr;
  uint32_t owner_shard = 0;

  StoredRow Null() const { return kNullOffset; }
  bool IsNull(StoredRow row) const { return row == kNullOffset; }
  StoredRow Encode(void *row) const {
    if (regions == nullptr) throw std::runtime_error("shared row resolver is unbound");
    return regions->ToDynamicHwccOffset(row, owner_shard);
  }
  void *Resolve(StoredRow row) const {
    if (row == kNullOffset) return nullptr;
    if (regions == nullptr) throw std::runtime_error("shared row resolver is unbound");
    return regions->ResolveDynamicHwcc(
        row, sizeof(star::TwoPLPashaMetadataShared), owner_shard);
  }
  bool Equal(StoredRow left, StoredRow right) const { return left == right; }
};

// One shared-tree probe result (§10.1). kRetry is contention; only kMissing may
// trigger migrate/Forward.
enum class SharedAccessState : uint8_t {
  kDone = 0,
  kMissing = 1,
  kRetry = 2,
};

// Owns the process-local handles for one persistent partition.  The B+tree
// nodes and rows themselves stay in the mapped dual-region pool; this class
// only reconstructs handles from PartitionDirectoryEntry during attach.
// Migration tracking is owned by the process-local PolicyClock
// (star::migration_manager); this class supplies the KV move-in/out callbacks.
class KVPartition {
 public:
  // Reuse the original CXLTableBTreeOLC leaf wrapper.  Its offset_ptr is
  // position independent because it is stored with the HWCC leaf; it never
  // persists a process VA.  The C++ table object below remains a non-owning
  // process-local handle.
  using SharedTable = star::CXLTableBTreeOLC<
      FixedKey, FixedKeyComparator, RegionOffsetSharedRowReference>;
  using SharedTreeValue = SharedTable::BTreeOLCValue;
  using SharedTree = SharedTable::CXLBTree;

  KVPartition(DualRegionAllocator &regions, star::CXL_EBR &ebr,
              uint32_t partition_id, uint32_t owner_shard, bool attach,
              bool materialize_private);
  ~KVPartition();

  uint32_t partition_id() const { return partition_id_; }
  uint32_t owner_shard() const { return owner_shard_; }
  KvPartitionTable *private_table() const { return private_table_.get(); }
  star::CXLTableBase *shared_cxl_table() const { return shared_table_; }
  star::RowOutcome PutPrivate(std::string_view key, std::string_view value);
  // Original REMOTE_INSERT owner half: install an invalid placeholder through
  // the ITable adjacent callback, move it into CXL with one requester ref,
  // then let the requester publish valid without another payload write.
  StatusCode InsertRemotePlaceholder(std::string_view key,
                                     std::string_view value,
                                     uint32_t requester_id);
  bool PublishRemotePlaceholder(std::string_view key, uint32_t requester_id);
  // Owner read: follows is_migrated to the shared SCC payload when present.
  star::RowOutcome GetPrivate(std::string_view key, std::string *value) const;
  star::RowOutcome DeletePrivate(std::string_view key);
  // PolicyClock callback. The caller holds this partition's Clock tracker.
  bool DeletePrivateForMigrationManager(
      std::string_view key, bool *need_untrack,
      void **migration_policy_meta, bool writer_prelocked = false,
      bool requester_prelocked = false);
  star::RowOutcome CompareExchangePrivate(std::string_view key,
                                           std::string_view expected,
                                           std::string_view desired,
                                           bool *exchanged,
                                           bool *inserted = nullptr);
  star::RowOutcome IncrementPrivate(std::string_view key, int64_t delta,
                                    int64_t *value,
                                    bool *inserted = nullptr);
  // Non-owner APIs never touch owner-private ValueStruct/local metadata. Point
  // ops use TryPinShared + SCC.
  // SharedAccessState distinguishes miss vs contention (§10.1); HasShared gone.
  SharedAccessState GetShared(std::string_view key, uint32_t host_id,
                              std::string *value,
                              bool record_clock_access = true) const;
  SharedAccessState PutShared(std::string_view key, uint32_t host_id,
                              std::string_view value,
                              bool record_clock_access = true);
  SharedAccessState CompareExchangeShared(std::string_view key, uint32_t host_id,
                                          std::string_view expected,
                                          std::string_view desired,
                                          bool *exchanged,
                                          bool record_clock_access = true);
  SharedAccessState IncrementShared(std::string_view key, uint32_t host_id,
                                    int64_t delta, int64_t *value,
                                    bool record_clock_access = true);
  // Original REMOTE_DELETE requester half: hold the shared write lock and
  // ref while publishing invalid; owner deletion consumes both on success.
  SharedAccessState PrepareRemoteDelete(
      std::string_view key, uint32_t host_id,
      star::TwoPLPashaMetadataShared **locked_row,
      bool record_clock_access = true);
  void AbortRemoteDelete(star::TwoPLPashaMetadataShared *locked_row,
                         uint32_t host_id);
  // Owner DATA_MIGRATION analogue: move_row_in(inc_ref=false). Returns Ok on
  // SUCCESS or FAIL_ALREADY_IN_CXL, NotFound if absent, OutOfMemory otherwise.
  // When non-null, *moved_in is set true only on fresh SUCCESS.
  StatusCode EnsureInShared(std::string_view key, uint32_t host_id,
                            bool *moved_in = nullptr);
  star::migration_result PromotePrivate(std::string_view key, uint32_t host_id);
  // Like PromotePrivate, but when the row is already shared pins payload
  // ref_cnt (FAIL_ALREADY_IN_CXL) and returns that smeta via *pinned_existing
  // for the caller to unpin after the request completes.
  star::migration_result PromotePrivate(
      std::string_view key, uint32_t host_id,
      star::TwoPLPashaMetadataShared **pinned_existing);
  // Original ITable local scan fragment. Engine calls it only for the owner.
  bool ScanLocalPartition(std::string_view start_key, uint64_t limit,
                          std::vector<std::pair<std::string, std::string>> *items,
                          std::string_view inclusive_max = {}) const;
  // Original CXLTable scan fragment: adjacency under leaf latch, values while
  // the fragment's reader/ref pins remain held.
  struct SharedScanResult {
    Status status = Status::Ok();
    bool scan_success = false;
    bool migration_required = false;
    std::vector<std::pair<std::string, std::string>> items;
  };
  // host_id is the requester (SCC cache bit / clflush identity), not the
  // partition owner. Matches original TwoPLPasha coordinator_id on remote scan.
  // allow_lower_bound_left_boundary is the K1 thin adapter: only the prescribed
  // post-move-in re-probe may set it. The first probe stays on master's
  // exact-min / size==limit / otherwise predicate so a distant migrated island
  // cannot be accepted while [min, island) is still private-only; otherwise
  // move_in(min, limit) burns its budget on the gap and never repairs the
  // island trailing edge (Busy livelock).
  SharedScanResult ScanSharedPartition(
      uint32_t host_id, std::string_view start_key, uint64_t output_limit,
      std::string_view inclusive_max = {},
      bool allow_lower_bound_left_boundary = false) const;
  // Partition-wide scan-range migrate single-flight (§3.9.1).
  bool TryBeginScanRangeMigrate();
  void EndScanRangeMigrate();
  bool ScanRangeMigrateInFlight() const;

  // PolicyClock owns the tracker algorithm; these methods only expose the
  // owner-private control and allocation/resolution primitives.
  OwnerPrivateClockTrackerControl *ClockTrackerControl() const {
    if (private_arena_ == nullptr)
      throw std::runtime_error("non-owner partition has no private Clock control");
    return &private_arena_->clock;
  }
  PrivateClockTrackerNode *ResolveClockTrackerNode(RegionOffset offset) const;
  PrivateClockTrackerNode *AllocateClockTrackerNode();
  void FreeClockTrackerNode(PrivateClockTrackerNode *node);
  RegionOffset ClockTrackerNodeOffset(const PrivateClockTrackerNode *node) const;
  RegionOffset ClockTrackerLocalRowOffset(
      const std::tuple<std::atomic<uint64_t> *, void *> &row) const;
  RegionOffset ClockTrackerSharedRowOffset(void *smeta) const;
  std::tuple<std::atomic<uint64_t> *, void *> ClockTrackerLocalRow(
      RegionOffset row_offset) const;
  star::TwoPLPashaMetadataShared *ClockTrackerSharedRow(
      RegionOffset smeta_offset) const;
  bool ClockTrackerNodeMatches(const PrivateClockTrackerNode &node) const;
  uint64_t shared_payload_used_bytes() const;
  uint64_t shared_payload_capacity_bytes() const;

  // KV-adapted Helper move-in/out bodies used as PolicyClock callbacks.
  // Caller (PolicyClock) already holds the per-partition Clock tracker lock.
  star::migration_result MoveInForMigrationManager(
      const void *key, bool inc_ref_cnt, void *&migration_policy_meta);
  bool MoveOutForMigrationManager(const void *key);

 private:
  friend class KvPartitionTable;
  friend struct RegionOffsetRowStorage;
  // Matches core/Executor: enter before observing shared tree/row/move paths.
  FixedKey MakeKey(std::string_view key) const;
  bool MoveOutPrivateRaw(std::string_view key, uint32_t host_id);
  bool LookupPrivateOffset(const FixedKey &key, RegionOffset *offset) const;
  bool LookupSharedReference(const FixedKey &key, RegionOffset *offset) const;
  PrivateValueStruct *ValueFromOffset(RegionOffset offset) const;
  PrivateMetadataLocal *MetadataFromValue(PrivateValueStruct *value) const;
  star::TwoPLPashaMetadataShared *SharedMetadataFromOffset(
      RegionOffset offset) const;
  uint64_t NextCommitTid(uint64_t observed_tid) const;
  static void LockRow(PrivateMetadataLocal *metadata);
  static void UnlockRow(PrivateMetadataLocal *metadata);
  std::string KeyString(const FixedKey &key) const;
  // Pin shared smeta so MoveOut cannot retire it between tree lookup and SCC
  // access. Clock access remains inside the original get_migrated_row body.
  SharedAccessState TryPinShared(const FixedKey &key,
                                 star::TwoPLPashaMetadataShared **smeta,
                                 bool record_clock_access) const;
  struct OwnerNextRowLock {
    PrivateValueStruct *value = nullptr;
    PrivateMetadataLocal *metadata = nullptr;
    uint64_t observed_tid = 0;
    bool shared = false;
  };
  // Offset-safe adapter for the original insert_and_update_next_key_info
  // next-row write lock.  It keeps the original placeholder → adjacency →
  // valid → release sequence without storing a process VA in ValueStruct.
  bool AcquireOwnerNextRowWriteLock(PrivateValueStruct *value,
                                    OwnerNextRowLock *locked_row);
  void ReleaseOwnerNextRowWriteLock(const OwnerNextRowLock &locked_row,
                                    uint64_t new_tid, bool commit);
  bool InsertOwnerPlaceholderWithNextLock(const FixedKey &key,
                                          std::string_view value,
                                          OwnerNextRowLock *locked_row);
  bool PublishOwnerPlaceholder(const FixedKey &key, uint64_t commit_tid);
  bool CreatePrivateWithOwnerInsert(const FixedKey &key,
                                    std::string_view value);
  void FreeUnpublishedPrivateValue(PrivateValueStruct *value);
  DualRegionAllocator &regions_;
  star::CXL_EBR &ebr_;
  uint32_t partition_id_;
  uint32_t owner_shard_;
  const uint32_t fixed_key_size_;
  const uint32_t fixed_value_size_;
  PartitionDirectoryEntry &directory_;
  // Only the partition owner materializes this non-owning SWCC handle.
  // Non-owners retain the shared-tree handle only and must never resolve an
  // owner-private arena during construction or lookup.
  OwnerPrivateArenaHeader *private_arena_ = nullptr;
  btreeolc_cxl::TreeNodeAllocation private_binding_;
  btreeolc_cxl::TreeNodeAllocation shared_binding_;
  // The owner-private TableBTreeOLC is a process-local handle.  It owns the
  // reconstructed B+Tree handle; persistent nodes and rows remain in SWCC.
  std::unique_ptr<KvPartitionTable> private_table_;
  SharedTree *shared_tree_ = nullptr;
  SharedTable *shared_table_ = nullptr;
};

}  // namespace tigonkv::engine
