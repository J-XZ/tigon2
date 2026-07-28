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

class KvPartitionTable;

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
  using PrivateTree = btreeolc_cxl::BPlusTree<FixedKey, RegionOffset,
                                              FixedKeyComparator,
                                              std::equal_to<RegionOffset>>;
  // Shared leaf = smeta RegionOffset only (PLAN / TwoPLPasha CXL table). Length
  // Shared leaf stores RegionOffset to HWCC smeta. Payload width is the
  // partition's fixed_value_size_, so no logical value length is persisted.
  using SharedTree = PrivateTree;

  KVPartition(DualRegionAllocator &regions, star::CXL_EBR &ebr,
              uint32_t partition_id, uint32_t owner_shard, bool attach,
              bool materialize_private);
  ~KVPartition();

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
  // Non-owner APIs never touch owner-private ValueStruct/local metadata. Point
  // ops use TryPinShared + SCC.
  // SharedAccessState distinguishes miss vs contention (§10.1); HasShared gone.
  SharedAccessState GetShared(std::string_view key, uint32_t host_id,
                              std::string *value) const;
  SharedAccessState PutShared(std::string_view key, uint32_t host_id,
                              std::string_view value);
  SharedAccessState CompareExchangeShared(std::string_view key, uint32_t host_id,
                                          std::string_view expected,
                                          std::string_view desired,
                                          bool *exchanged);
  SharedAccessState IncrementShared(std::string_view key, uint32_t host_id,
                                    int64_t delta, int64_t *value);
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
  // Test/legacy helper; formal Scan uses ProbeSharedScanPage. host_id is the
  // SCC reader identity (requester), not owner_shard_.
  bool ScanShared(
      std::string_view start_key, uint64_t limit, uint32_t host_id,
      std::vector<std::pair<std::string, std::string>> *items) const;
  // Thin CXLTable::scan-style entry: shared_tree_->scanForUpdate only.
  // Processor returns true to stop (BTreeOLC_CXL end semantics). Adapter does
  // ValueType→RegionOffset passthrough; no adjacency/migration logic (§4.3).
  void ScanSharedForUpdate(
      const FixedKey &min_key,
      const std::function<bool(const FixedKey &key, RegionOffset smeta_off,
                               bool is_last_tuple)> &processor) const;
  // CXL-first page probe (§4.3–§4.5): adjacency under leaf latch; values after.
  struct SharedScanProbeResult {
    Status status = Status::Ok();
    bool scan_success = false;
    bool migration_required = false;
    bool more = false;
    std::vector<std::pair<std::string, std::string>> items;
  };
  // host_id is the requester (SCC cache bit / clflush identity), not the
  // partition owner. Matches original TwoPLPasha coordinator_id on remote scan.
  SharedScanProbeResult ProbeSharedScanPage(
      uint32_t host_id, std::string_view start_key, uint64_t output_limit,
      bool owner_exhausted_for_cursor, bool cursor_is_duplicate,
      bool owner_no_predecessor_for_cursor = false) const;
  bool PrivatePredecessorKey(std::string_view key, std::string *predecessor) const;
  void ClockLock();
  void ClockUnlock();
  void ClockTrackMigratedKey(const void *key_bytes);
  void ClockUntrackMigratedKey(const void *key_bytes);
  void ClockUntrackRowOffset(RegionOffset row_off);
  // Returns the private ValueStruct offset under the Clock cursor (or null).
  RegionOffset ClockAdvanceCursor();
  bool ClockMoveOutRow(RegionOffset row_off);
  // Second-chance eviction loop used by the original PolicyClock gate.
  bool ClockEvictUntilUnderBudget(uint64_t hw_cc_budget);
  bool MoveOutClockVictim(uint32_t host_id);
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
  friend class KvPartitionTable;
  // Matches core/Executor: enter before observing shared tree/row/move paths.
  void EnterEbr() const { ebr_.enter_critical_section(); }
  FixedKey MakeKey(std::string_view key) const;
  PrivateValueStruct *ValueFromOffset(RegionOffset offset) const;
  PrivateMetadataLocal *MetadataFromValue(PrivateValueStruct *value) const;
  PrivateClockTrackerNode *ClockNodeFromOffset(RegionOffset offset) const;
  PrivateValueStruct *AllocateValue(std::string_view value);
  PrivateMetadataLocal *AllocateMetadata();
  static void LockRow(PrivateMetadataLocal *metadata);
  static void UnlockRow(PrivateMetadataLocal *metadata);
  std::string KeyString(const FixedKey &key) const;
  void NoteSharedAccess(star::TwoPLPashaMetadataShared *smeta) const;
  // Pin shared smeta so MoveOut cannot retire it between tree lookup and SCC
  // access.
  SharedAccessState TryPinShared(const FixedKey &key,
                                 star::TwoPLPashaMetadataShared **smeta,
                                 RegionOffset *smeta_offset) const;
  bool TryPinSharedEntry(
      const FixedKey &key, RegionOffset expected_offset,
      star::TwoPLPashaMetadataShared **smeta) const;
  // Pin without shared-tree re-lookup; only safe while scanForUpdate holds the
  // leaf write lock (lookup would self-deadlock on that leaf).
  bool TryPinSharedEntryUnderScan(
      RegionOffset expected_offset,
      star::TwoPLPashaMetadataShared **smeta) const;
  struct RowRef {
    FixedKey key{};
    RegionOffset offset = kNullOffset;
    PrivateValueStruct *value = nullptr;
    PrivateMetadataLocal *metadata = nullptr;
  };
  // Ephemeral arguments supplied by one B+Tree leaf callback.  This is not a
  // second lookup/retry state machine; all three entries are valid only while
  // that callback retains the original leaf latches.
  struct AdjacentRows {
    bool has_prev = false;
    bool has_current = false;
    bool has_next = false;
    RowRef prev;
    RowRef current;
    RowRef next;
  };
  static void UnlockAdjacentRows(AdjacentRows *rows);
  void SetNextReal(const RowRef &row, bool real);
  void SetPrevReal(const RowRef &row, bool real);
  void ApplySharedAdjacency(const AdjacentRows &rows);
  void ClearSharedAdjacency(const AdjacentRows &rows);
  bool InsertPrivateValue(const FixedKey &key, PrivateValueStruct *value);
  void FreeUnpublishedPrivateValue(PrivateValueStruct *value);
  DualRegionAllocator &regions_;
  star::CXL_EBR &ebr_;
  uint32_t partition_id_;
  uint32_t owner_shard_;
  const uint32_t fixed_key_size_;
  const uint32_t fixed_value_size_;
  PartitionDirectoryEntry &directory_;
  OwnerPrivateArenaHeader &private_arena_;
  btreeolc_cxl::TreeNodeAllocation private_binding_;
  btreeolc_cxl::TreeNodeAllocation shared_binding_;
  PrivateTree *private_tree_ = nullptr;
  SharedTree *shared_tree_ = nullptr;
  // Process-local cache of the last published private root offset (§11.6).
  RegionOffset persisted_private_root_offset_ = kNullOffset;
  uint64_t private_root_publishes_ = 0;
  // Process-local Clock list lock; list nodes live in owner-private metadata.
  pthread_spinlock_t clock_lock_{};
  bool clock_lock_inited_ = false;
};

}  // namespace tigonkv::engine
