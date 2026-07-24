#include "kv/engine/kv_partition.h"

#include <cstring>
#include <new>
#include <stdexcept>

namespace tigonkv::engine {

KVPartition::KVPartition(DualRegionAllocator &regions, star::CXL_EBR &ebr,
                         uint32_t partition_id, uint32_t owner_shard,
                         bool attach)
    : regions_(regions), ebr_(ebr), partition_id_(partition_id),
      owner_shard_(owner_shard), directory_(regions.layout().partitions.at(partition_id)),
      private_binding_{&regions, AllocationDomain::kOwnerPrivateSwcc, owner_shard, &ebr,
                       partition_id},
      shared_binding_{&regions, AllocationDomain::kHwccIndex, owner_shard, &ebr} {
  if (partition_id >= regions.layout().partition_count)
    throw std::invalid_argument("partition id outside persistent layout");
  if (attach) {
    if (directory_.private_root == kNullOffset ||
        directory_.shared_root == kNullOffset)
      throw std::runtime_error("partition attach missing tree root");
    private_tree_ = new PrivateTree(
        private_binding_, regions_.swcc().FromOffset(directory_.private_root));
    shared_tree_ = new SharedTree(
        shared_binding_, regions_.hwcc().FromOffset(directory_.shared_root));
  } else {
    private_tree_ = new PrivateTree(private_binding_);
    shared_tree_ = new SharedTree(shared_binding_);
    PersistRoots();
  }
}

FixedKey KVPartition::MakeKey(std::string_view key) const {
  return FixedKey::From(key, regions_.layout().fixed_key_size);
}

PrivateRow *KVPartition::RowFromOffset(RegionOffset offset) const {
  if (offset == kNullOffset) return nullptr;
  return static_cast<PrivateRow *>(regions_.swcc().FromOffset(offset));
}

PrivateRow *KVPartition::AllocateRow(const FixedKey &key, std::string_view value) {
  const uint64_t bytes = sizeof(PrivateRow) + regions_.layout().fixed_key_size +
                         regions_.layout().fixed_value_size;
  auto *row = new (regions_.AllocateOwnerPrivate(bytes, partition_id_, owner_shard_))
      PrivateRow;
  row->key_len = regions_.layout().fixed_key_size;
  row->value_len = static_cast<uint32_t>(value.size());
  std::memcpy(row->kv, key.bytes, row->key_len);
  std::memcpy(row->kv + row->key_len, value.data(), value.size());
  return row;
}

bool KVPartition::PutPrivate(std::string_view key, std::string_view value) {
  if (value.size() > regions_.layout().fixed_value_size)
    throw std::invalid_argument("private value exceeds fixed value size");
  const FixedKey fixed_key = MakeKey(key);
  RegionOffset row_offset = kNullOffset;
  if (private_tree_->lookup(fixed_key, row_offset)) {
    auto *row = RowFromOffset(row_offset);
    row->value_len = static_cast<uint32_t>(value.size());
    std::memcpy(row->kv + row->key_len, value.data(), value.size());
    ++row->version;
    row->is_tombstone = 0;
    return false;
  }
  auto *row = AllocateRow(fixed_key, value);
  row_offset = regions_.swcc().ToOffset(row);
  if (!private_tree_->insert(fixed_key, row_offset))
    throw std::runtime_error("private tree insert race without owner serialization");
  PersistRoots();
  return true;
}

bool KVPartition::GetPrivate(std::string_view key, std::string *value) const {
  RegionOffset row_offset = kNullOffset;
  if (!private_tree_->lookup(MakeKey(key), row_offset)) return false;
  const auto *row = RowFromOffset(row_offset);
  if (row->is_tombstone || row->is_migrated) return false;
  value->assign(row->kv + row->key_len, row->value_len);
  return true;
}

bool KVPartition::DeletePrivate(std::string_view key) {
  RegionOffset row_offset = kNullOffset;
  const FixedKey fixed_key = MakeKey(key);
  if (!private_tree_->lookup(fixed_key, row_offset)) return false;
  auto *row = RowFromOffset(row_offset);
  row->is_tombstone = 1;
  if (!private_tree_->remove(fixed_key)) return false;
  // Row reclamation is deferred until the partition's migration/EBR protocol
  // owns it; immediate free here could race a future shared-row transition.
  PersistRoots();
  return true;
}

void KVPartition::PersistRoots() {
  directory_.private_root = regions_.swcc().ToOffset(
      private_tree_->root_for_persistence());
  directory_.shared_root = regions_.hwcc().ToOffset(
      shared_tree_->root_for_persistence());
}

}  // namespace tigonkv::engine
