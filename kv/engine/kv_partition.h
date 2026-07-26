#pragma once

#include "common/CXL_EBR.h"
#include "common/btree_olc_cxl/BTreeOLC_CXL.h"
#include "protocol/TwoPLPasha/TwoPLPashaHelper.h"
#include "protocol/Pasha/MigrationManager.h"
#include "kv/engine/kv_types_layout.h"
#include "kv/engine/region_allocator.h"

#include <cstdint>
#include <functional>
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
  bool CompareExchangePrivate(std::string_view key, std::string_view expected,
                              std::string_view desired, bool *exchanged);
  bool IncrementPrivate(std::string_view key, int64_t delta, int64_t *value);
  // Non-owner APIs never touch PrivateRow. Get/Put/INCR Shared currently
  // Forward (YCSB-A liveness). CompareExchangeShared + ScanSharedOnly use
  // TryPinShared CXL paths.
  bool GetShared(std::string_view key, uint32_t host_id, std::string *value) const;
  bool PutShared(std::string_view key, uint32_t host_id, std::string_view value);
  bool CompareExchangeShared(std::string_view key, uint32_t host_id,
                             std::string_view expected, std::string_view desired,
                             bool *exchanged);
  bool IncrementShared(std::string_view key, uint32_t host_id, int64_t delta,
                       int64_t *value);
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
  // Non-owner-safe CXL shared-tree scan (no PrivateRow). Used as CXL-first
  // probe before owner Scan RPC, matching TwoPLPasha remote CXL table scan.
  bool ScanSharedOnly(std::string_view start_key, uint64_t limit,
                      std::vector<std::pair<std::string, std::string>> *items,
                      uint32_t host_id) const;
  // Invokes PolicyClock::move_row_out for this partition.
  bool MoveOutClockVictim(uint32_t host_id);
  // Rebuild DRAM Clock tracker entries from the shared tree after attach.
  void RebuildClockTracker();
  uint64_t shared_payload_used_bytes() const;
  uint64_t shared_payload_capacity_bytes() const;
  uint64_t hwcc_used_bytes() const;
  uint64_t migrated_key_count() const;

  // Must be called after an operation which might split or collapse a root.
  // It writes only region-relative offsets into the persistent directory.
  void PersistRoots();

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
  static void UnlockRow(PrivateRow *row);
  std::string KeyString(const FixedKey &key) const;
  void NoteSharedAccess(star::TwoPLPashaMetadataShared *smeta) const;
  // Pin shared smeta so MoveOut cannot retire it between tree lookup and SCC
  // access (replaces the old non-owner PrivateRow LockRow quiescence window).
  bool TryPinShared(const FixedKey &key, star::TwoPLPashaMetadataShared **smeta,
                    RegionOffset *smeta_offset) const;

  DualRegionAllocator &regions_;
  star::CXL_EBR &ebr_;
  uint32_t partition_id_;
  uint32_t owner_shard_;
  PartitionDirectoryEntry &directory_;
  btreeolc_cxl::TreeNodeAllocation private_binding_;
  btreeolc_cxl::TreeNodeAllocation shared_binding_;
  PrivateTree *private_tree_ = nullptr;
  SharedTree *shared_tree_ = nullptr;
};

}  // namespace tigonkv::engine
