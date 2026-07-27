#pragma once

#include "common/CXL_EBR.h"
#include "common/btree_olc_cxl/BTreeOLC_CXL.h"
#include "protocol/TwoPLPasha/TwoPLPashaHelper.h"
#include "protocol/Pasha/MigrationManager.h"
#include "kv/engine/kv_types_layout.h"
#include "kv/engine/region_allocator.h"
#include "kv/kv_store.h"

#include <cstdint>
#include <functional>
#include <pthread.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tigonkv::engine {

// Owns the process-local handles for one persistent partition.  The B+tree
// nodes and rows themselves stay in the mapped dual-region pool; this class
// only reconstructs handles from PartitionDirectoryEntry during attach.
// Migration tracking is owned by the process-local PolicyClock
// (star::migration_manager); this class supplies the KV move-in/out callbacks.
class KVPartition {
 public:
  using PrivateTree = btreeolc_cxl::BPlusTree<FixedKey, RegionOffset,
                                              FixedKeyComparator,
                                              std::equal_to<RegionOffset>>;
  // Shared leaf = smeta RegionOffset only (PLAN / TwoPLPasha CXL table). Length
  // Shared leaf stores RegionOffset to HWCC smeta. Logical length lives in
  // TwoPLPashaMetadataShared::value_len (HWCC) for non-owner CXL access.
  using SharedTree = PrivateTree;

  KVPartition(DualRegionAllocator &regions, star::CXL_EBR &ebr,
              uint32_t partition_id, uint32_t owner_shard, bool attach);

  uint32_t partition_id() const { return partition_id_; }
  uint32_t owner_shard() const { return owner_shard_; }
  bool PutPrivate(std::string_view key, std::string_view value);
  // Owner read: follows is_migrated to the shared SCC payload when present.
  bool GetPrivate(std::string_view key, std::string *value) const;
  bool DeletePrivate(std::string_view key);
  // PolicyClock callback. The caller holds this partition's Clock tracker.
  bool DeletePrivateForMigrationManager(
      std::string_view key, bool *need_untrack,
      void **migration_policy_meta);
  bool CompareExchangePrivate(std::string_view key, std::string_view expected,
                              std::string_view desired, bool *exchanged,
                              bool *inserted = nullptr);
  bool IncrementPrivate(std::string_view key, int64_t delta, int64_t *value,
                        bool *inserted = nullptr);
  // Non-owner APIs never touch PrivateRow. Point ops use TryPinShared + SCC
  // (aligned with get_migrated_row / CompareExchangeShared).
  // True iff shared_tree_ currently indexes the key (no pin / SCC). Used by
  // Get to distinguish tree-miss (migrate) from SCC contention (retry).
  bool HasShared(std::string_view key) const;
  bool GetShared(std::string_view key, uint32_t host_id, std::string *value) const;
  bool PutShared(std::string_view key, uint32_t host_id, std::string_view value);
  bool CompareExchangeShared(std::string_view key, uint32_t host_id,
                             std::string_view expected, std::string_view desired,
                             bool *exchanged);
  bool IncrementShared(std::string_view key, uint32_t host_id, int64_t delta,
                       int64_t *value);
  // Owner DATA_MIGRATION analogue: move_row_in(inc_ref=false). Returns Ok on
  // SUCCESS or FAIL_ALREADY_IN_CXL, NotFound if absent, OutOfMemory otherwise.
  // When non-null, *moved_in is set true only on fresh SUCCESS.
  StatusCode EnsureInShared(std::string_view key, uint32_t host_id,
                            bool *moved_in = nullptr);
  bool PromotePrivate(std::string_view key, uint32_t host_id);
  // Like PromotePrivate, but when the row is already shared pins payload
  // ref_cnt (FAIL_ALREADY_IN_CXL) and returns that smeta via *pinned_existing
  // for the caller to unpin after the request completes.
  bool PromotePrivate(std::string_view key, uint32_t host_id,
                      star::TwoPLPashaMetadataShared **pinned_existing);
  bool MoveOutPrivate(std::string_view key, uint32_t host_id);
  // Owner-only dual-tree merge (private + shared). Engine must not call this
  // for non-owned partitions.
  bool ScanOwned(std::string_view start_key, uint64_t limit,
                 std::vector<std::pair<std::string, std::string>> *items,
                 const std::function<void()> *progress = nullptr) const;
  // Key-only owner locator walk used by TwoPLPasha-style range move-in. It
  // deliberately avoids reading values that the requester will read via CXL.
  bool ScanOwnedKeys(
      std::string_view start_key, uint64_t limit,
      std::vector<std::string> *keys,
      const std::function<void()> *progress = nullptr) const;
  bool ScanShared(
      std::string_view start_key, uint64_t limit,
      std::vector<std::pair<std::string, std::string>> *items) const;
  // Thin CXLTable::scan-style entry: shared_tree_->scanForUpdate only.
  // Processor returns true to stop (BTreeOLC_CXL end semantics). Adapter does
  // ValueType→RegionOffset passthrough; no adjacency/migration logic (§4.3).
  void ScanSharedForUpdate(
      const FixedKey &min_key,
      const std::function<bool(const FixedKey &key, RegionOffset smeta_off,
                               bool is_last_tuple)> &processor) const;
  // TwoPLPasha range proof: rows through cutoff plus one right boundary must
  // form the logical private-tree adjacency chain.  For an exhausted range,
  // expected_count anchors both endpoints; expected_generation linearizes
  // logical EOF where adjacency has no right boundary.
  bool ScanSharedComplete(
      std::string_view start_key, const FixedKey &cutoff, bool exhausted,
      bool no_predecessor, uint32_t expected_count,
      uint32_t expected_generation,
      std::vector<std::pair<std::string, std::string>> *items) const;
  bool PrivatePredecessorKey(std::string_view key, std::string *predecessor) const;
  uint64_t SharedMutationState() const;
  // Invokes PolicyClock::move_row_out for this partition.
  bool MoveOutClockVictim(uint32_t host_id);
  void ClockLock();
  void ClockUnlock();
  void ClockTrackMigratedKey(const void *key_bytes);
  void ClockUntrackMigratedKey(const void *key_bytes);
  void ClockUntrackRowOffset(RegionOffset row_off);
  // Returns the PrivateRow offset under the Clock cursor (or kNullOffset).
  RegionOffset ClockAdvanceCursor();
  bool ClockMoveOutRow(RegionOffset row_off);
  // Second-chance eviction loop used by PolicyClock::move_row_out (§11.14).
  bool ClockEvictUntilUnderBudget(uint64_t hw_cc_budget);
  uint64_t shared_payload_used_bytes() const;
  uint64_t shared_payload_capacity_bytes() const;
  uint64_t hwcc_used_bytes() const;
  uint64_t migrated_key_count() const;

  // Must be called after an operation which might split or collapse a root.
  // It writes only region-relative offsets into the persistent directory.
  void PersistPrivateRootIfChanged();
  uint64_t PrivateRootPublishCount() const { return private_root_publishes_; }

  // KV-adapted Helper move-in/out bodies used as PolicyClock callbacks.
  // Caller (PolicyClock) already holds the per-partition Clock tracker lock.
  star::migration_result MoveInForMigrationManager(
      const void *key, bool inc_ref_cnt, void *&migration_policy_meta);
  bool MoveOutForMigrationManager(const void *key);

 private:
  // Matches core/Executor: enter before observing shared tree/row/move paths.
  void EnterEbr() const { ebr_.enter_critical_section(); }
  // Upstream Tigon leave is unused (CHECK(0)); keep as empty no-op for pairing.
  void LeaveEbr() const { ebr_.leave_critical_section(); }
  FixedKey MakeKey(std::string_view key) const;
  PrivateRow *RowFromOffset(RegionOffset offset) const;
  PrivateRow *AllocateRow(const FixedKey &key, std::string_view value);
  static void LockRow(PrivateRow *row);
  static bool TryLockRow(PrivateRow *row);
  static void UnlockRow(PrivateRow *row);
  std::string KeyString(const FixedKey &key) const;
  void NoteSharedAccess(star::TwoPLPashaMetadataShared *smeta) const;
  // Pin shared smeta so MoveOut cannot retire it between tree lookup and SCC
  // access (replaces the old non-owner PrivateRow LockRow quiescence window).
  bool TryPinShared(const FixedKey &key, star::TwoPLPashaMetadataShared **smeta,
                    RegionOffset *smeta_offset) const;
  bool TryPinSharedEntry(
      const FixedKey &key, RegionOffset expected_offset,
      star::TwoPLPashaMetadataShared **smeta) const;
  struct RowRef {
    FixedKey key{};
    RegionOffset offset = kNullOffset;
    PrivateRow *row = nullptr;
  };
  struct Neighborhood {
    bool has_prev = false;
    bool has_current = false;
    bool has_next = false;
    RowRef prev;
    RowRef current;
    RowRef next;
  };
  void LockNeighborhood(const FixedKey &key, Neighborhood *neighborhood) const;
  static void UnlockNeighborhood(Neighborhood *neighborhood);
  bool SameNeighborhood(const Neighborhood &left,
                        const Neighborhood &right) const;
  void SetNextReal(const RowRef &row, bool real);
  void SetPrevReal(const RowRef &row, bool real);
  void RefreshAdjacencyLocked(const Neighborhood &neighborhood);
  void BreakAdjacencyLocked(const Neighborhood &neighborhood);
  bool InsertPrivateRow(const FixedKey &key, PrivateRow *row);
  void FreeUnpublishedPrivateRow(PrivateRow *row);
  void BeginSharedMutation();
  void EndSharedMutation();
  class SharedMutationGuard {
   public:
    explicit SharedMutationGuard(KVPartition &partition)
        : partition_(partition) {
      partition_.BeginSharedMutation();
    }
    ~SharedMutationGuard() { partition_.EndSharedMutation(); }
    SharedMutationGuard(const SharedMutationGuard &) = delete;
    SharedMutationGuard &operator=(const SharedMutationGuard &) = delete;

   private:
    KVPartition &partition_;
  };
  DualRegionAllocator &regions_;
  star::CXL_EBR &ebr_;
  uint32_t partition_id_;
  uint32_t owner_shard_;
  const uint32_t fixed_key_size_;
  const uint32_t fixed_value_size_;
  PartitionDirectoryEntry &directory_;
  btreeolc_cxl::TreeNodeAllocation private_binding_;
  btreeolc_cxl::TreeNodeAllocation shared_binding_;
  PrivateTree *private_tree_ = nullptr;
  SharedTree *shared_tree_ = nullptr;
  // Process-local cache of the last published private root offset (§11.6).
  RegionOffset persisted_private_root_offset_ = kNullOffset;
  uint64_t private_root_publishes_ = 0;
  // Process-local Clock list lock; list nodes live in SWCC PrivateRow (§11.14).
  pthread_spinlock_t clock_lock_{};
  bool clock_lock_inited_ = false;
};

}  // namespace tigonkv::engine
