#pragma once

#include "core/Table.h"
#include "protocol/Pasha/MigrationManager.h"
#include "protocol/Pasha/PolicyClock.h"
#include "kv/engine/kv_partition.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace tigonkv::engine {

// Storage policy for the original core/Table.h TableBTreeOLC body.  It keeps
// only offsets in the B+Tree value and resolves the owner-private row through
// the partition's existing allocator binding.
struct RegionOffsetRowStorage {
  using MetaDataType = std::atomic<uint64_t>;
  using ValueStruct = PrivateValueStruct;
  using StoredRow = RegionOffset;

  KVPartition *partition = nullptr;
  uint32_t key_size = 0;
  uint32_t value_size = 0;
  btreeolc_cxl::TreeNodeAllocation binding{};
  void *persisted_root = nullptr;

  StoredRow AllocateAndConstruct(const void *value, bool is_placeholder) const;
  void DestroyUnpublished(StoredRow row) const;
  void Retire(StoredRow row) const;
  StoredRow Null() const { return kNullOffset; }
  MetaDataType *Meta(StoredRow row) const;
  void *Data(StoredRow row) const;
  void *AdjacentMeta(StoredRow row) const;
  void Update(StoredRow row, const void *value) const;
  void Deserialize(StoredRow row, star::StringPiece bytes) const;
  void Serialize(star::Encoder &encoder, const void *value) const;
  std::size_t KeySize(std::size_t) const { return key_size; }
  std::size_t ValueSize() const { return value_size; }
  std::size_t FieldSize() const { return value_size; }

  template <typename BTree>
  std::unique_ptr<BTree> MakeTree() const {
    if (persisted_root != nullptr)
      return std::make_unique<BTree>(binding, persisted_root);
    return std::make_unique<BTree>(binding);
  }
  template <typename BTree>
  void BindPublishedRoot(BTree &tree, std::atomic<uint64_t> *slot) const {
    tree.bind_published_root(reinterpret_cast<std::atomic<RegionOffset> *>(slot));
  }
  template <typename BTree>
  uint64_t RootOffset(const BTree &tree) const {
    return tree.root_offset_for_persistence();
  }
  bool IsNull(StoredRow row) const { return row == kNullOffset; }
};

using KvTableBase = star::TableBTreeOLC<
    FixedKey, FixedKey, FixedKeyComparator, FixedKeyComparator,
    star::MetaInitFuncNothing, btreeolc_cxl::BPlusTree,
    RegionOffsetRowStorage>;

// This is only the process-local non-owning binding expected by the existing
// KV facade.  All B+Tree operations and callbacks remain in the master-derived
// TableBTreeOLC body.
class KvPartitionTable final : public KvTableBase {
 public:
  KvPartitionTable(KVPartition *partition, uint32_t key_size,
                   uint32_t value_size,
                   const btreeolc_cxl::TreeNodeAllocation &binding,
                   void *persisted_root, bool create_max_sentinel);

  KVPartition *partition() const { return partition_; }
  bool LookupOffset(const FixedKey &key, RegionOffset *offset) const;
  RegionOffset root_offset_for_persistence() const {
    return static_cast<RegionOffset>(KvTableBase::root_offset_for_persistence());
  }
  void BindPublishedRoot(std::atomic<RegionOffset> *slot) {
    KvTableBase::bind_published_root(slot);
  }

 private:
  KVPartition *partition_;
};

// Owns the process-local PolicyClock and per-partition ITable adapters.
class KvMigrationRuntime {
 public:
  static KvMigrationRuntime &Instance();

  void Reset();
  void Install(std::vector<KVPartition *> partitions, uint32_t key_size,
               uint32_t value_size, uint32_t coordinator_id,
               uint32_t partition_count, uint64_t hw_cc_budget_per_host);

  KvPartitionTable *TableFor(uint32_t partition_id) const;
  star::TwoPLPashaHelper *helper() const { return helper_.get(); }
  star::PolicyClock *clock() const { return clock_.get(); }

 private:
  std::vector<KvPartitionTable *> tables_;
  // Original helper shape, deliberately fixed to one logical table.  The
  // entries are non-owning process-local CXLTable wrappers reconstructed for
  // every visible partition.
  std::vector<std::vector<star::CXLTableBase *>> cxl_tbl_vecs_;
  std::unique_ptr<star::TwoPLPashaHelper> helper_;
  std::unique_ptr<star::PolicyClock> clock_;
};

// Callbacks bound into PolicyClock (KV-adapted Helper move-in/out).
star::migration_result KvMoveFromPartitionToShared(
    star::ITable *table, const void *key,
    const std::tuple<std::atomic<uint64_t> *, void *> &row, bool inc_ref_cnt,
    void *&migration_policy_meta);
bool KvMoveFromSharedToPartition(
    star::ITable *table, const void *key,
    const std::tuple<std::atomic<uint64_t> *, void *> &row);
bool KvDeleteAndUpdateNextKeyInfo(star::ITable *table, const void *key,
                                  bool is_delete_local, bool &need_move_out,
                                  void *&migration_policy_meta);

}  // namespace tigonkv::engine
