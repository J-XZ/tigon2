#pragma once

#include "common/CXL_EBR.h"
#include "common/btree_olc_cxl/BTreeOLC_CXL.h"
#include "protocol/TwoPLPasha/TwoPLPashaHelper.h"
#include "kv/engine/kv_types_layout.h"
#include "kv/engine/region_allocator.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace tigonkv::engine {

// Owns the process-local handles for one persistent partition.  The B+tree
// nodes and rows themselves stay in the mapped dual-region pool; this class
// only reconstructs handles from PartitionDirectoryEntry during attach.
class KVPartition {
 public:
  using PrivateTree = btreeolc_cxl::BPlusTree<FixedKey, RegionOffset,
                                              FixedKeyComparator,
                                              std::equal_to<RegionOffset>>;
  using SharedTree = PrivateTree;

  KVPartition(DualRegionAllocator &regions, star::CXL_EBR &ebr,
              uint32_t partition_id, uint32_t owner_shard, bool attach);

  uint32_t partition_id() const { return partition_id_; }
  uint32_t owner_shard() const { return owner_shard_; }
  bool PutPrivate(std::string_view key, std::string_view value);
  // Owner read: follows is_migrated to the shared SCC payload when present.
  bool GetPrivate(std::string_view key, std::string *value) const;
  bool DeletePrivate(std::string_view key);
  bool PromotePrivate(std::string_view key, uint32_t host_id);

  // Must be called after an operation which might split or collapse a root.
  // It writes only region-relative offsets into the persistent directory.
  void PersistRoots();

 private:
  FixedKey MakeKey(std::string_view key) const;
  PrivateRow *RowFromOffset(RegionOffset offset) const;
  PrivateRow *AllocateRow(const FixedKey &key, std::string_view value);
  static void LockRow(PrivateRow *row);
  static void UnlockRow(PrivateRow *row);

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
