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
  star::CXLMemory::bind_dual_region_allocator(&regions, owner_shard);
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

void KVPartition::LockRow(PrivateRow *row) {
  uint32_t expected = 0;
  while (!row->latch.compare_exchange_weak(expected, 1, std::memory_order_acquire,
                                            std::memory_order_relaxed))
    expected = 0;
}

void KVPartition::UnlockRow(PrivateRow *row) {
  row->latch.store(0, std::memory_order_release);
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
  auto *row = RowFromOffset(row_offset);
  LockRow(row);
  if (row->is_tombstone) {
    UnlockRow(row);
    return false;
  }
  if (!row->is_migrated) {
    value->assign(row->kv + row->key_len, row->value_len);
    UnlockRow(row);
    return true;
  }
  const RegionOffset smeta_offset = row->migrated_smeta_off;
  const uint32_t value_len = row->value_len;
  UnlockRow(row);
  if (smeta_offset == kNullOffset) return false;
  RegionOffset shared_value = kNullOffset;
  if (!shared_tree_->lookup(MakeKey(key), shared_value) || shared_value != smeta_offset)
    return false;
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(smeta_offset));
  std::string shared(value_len, '\0');
  if (!star::TwoPLPashaHelper::kv_shared_read(smeta, owner_shard_, shared.data(), value_len))
    return false;
  *value = std::move(shared);
  return true;
}

bool KVPartition::PromotePrivate(std::string_view key, uint32_t host_id) {
  if (star::scc_manager == nullptr) return false;
  RegionOffset row_offset = kNullOffset;
  const FixedKey fixed_key = MakeKey(key);
  if (!private_tree_->lookup(fixed_key, row_offset)) return false;
  auto *row = RowFromOffset(row_offset);
  LockRow(row);
  if (row->is_tombstone || row->is_migrated) {
    UnlockRow(row);
    return false;
  }
  const uint64_t payload_bytes = sizeof(star::TwoPLPashaSharedDataSCC) +
                                 regions_.layout().fixed_value_size;
  auto *payload = new (regions_.Allocate(payload_bytes,
      AllocationDomain::kSharedPayloadSwcc, owner_shard_)) star::TwoPLPashaSharedDataSCC;
  auto *smeta = new (regions_.Allocate(sizeof(star::TwoPLPashaMetadataShared),
      AllocationDomain::kHwccMetadata, owner_shard_)) star::TwoPLPashaMetadataShared(payload);
  star::scc_manager->init_scc_metadata(smeta, host_id);
  smeta->lock();
  star::scc_manager->do_write(smeta, host_id, payload->data, row->kv + row->key_len,
                               row->value_len);
  payload->set_flag(star::TwoPLPashaSharedDataSCC::valid_flag_index);
  const RegionOffset smeta_offset = regions_.hwcc().ToOffset(smeta);
  if (!shared_tree_->insert(fixed_key, smeta_offset)) {
    smeta->unlock();
    UnlockRow(row);
    return false;
  }
  row->migrated_smeta_off = smeta_offset;
  row->is_migrated = 1;
  star::scc_manager->finish_write(smeta, host_id, payload,
                                  sizeof(star::TwoPLPashaSharedDataSCC) + row->value_len);
  smeta->unlock();
  UnlockRow(row);
  PersistRoots();
  directory_.migration_in_seq.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool KVPartition::MoveOutPrivate(std::string_view key, uint32_t host_id) {
  if (star::scc_manager == nullptr) return false;
  RegionOffset row_offset = kNullOffset;
  const FixedKey fixed_key = MakeKey(key);
  if (!private_tree_->lookup(fixed_key, row_offset)) return false;
  auto *row = RowFromOffset(row_offset);
  LockRow(row);
  if (!row->is_migrated || row->migrated_smeta_off == kNullOffset) {
    UnlockRow(row);
    return false;
  }
  const RegionOffset smeta_offset = row->migrated_smeta_off;
  RegionOffset indexed_offset = kNullOffset;
  if (!shared_tree_->lookup(fixed_key, indexed_offset) || indexed_offset != smeta_offset) {
    UnlockRow(row);
    return false;
  }
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(smeta_offset));
  smeta->lock();
  auto *payload = smeta->get_scc_data();
  if (payload->ref_cnt != 0 || smeta->get_reader_count() != 0 || smeta->is_write_locked()) {
    smeta->unlock();
    UnlockRow(row);
    return false;
  }
  smeta->set_write_locked();
  star::scc_manager->prepare_read(smeta, host_id, payload,
                                  sizeof(star::TwoPLPashaSharedDataSCC) + row->value_len);
  if (!payload->get_flag(star::TwoPLPashaSharedDataSCC::valid_flag_index)) {
    smeta->clear_write_locked();
    smeta->unlock();
    UnlockRow(row);
    return false;
  }
  star::scc_manager->do_read(smeta, host_id, row->kv + row->key_len, payload->data,
                             row->value_len);
  row->is_migrated = 0;
  row->migrated_smeta_off = kNullOffset;
  if (!shared_tree_->remove(fixed_key)) {
    // Restore the only published owner state before exposing the failure.
    row->is_migrated = 1;
    row->migrated_smeta_off = smeta_offset;
    smeta->clear_write_locked();
    smeta->unlock();
    UnlockRow(row);
    return false;
  }
  smeta->clear_write_locked();
  smeta->unlock();
  ebr_.add_retired_object(smeta, sizeof(star::TwoPLPashaMetadataShared),
                          star::CXLMemory::METADATA_FREE, owner_shard_);
  ebr_.add_retired_object(payload,
                          sizeof(star::TwoPLPashaSharedDataSCC) +
                              regions_.layout().fixed_value_size,
                          star::CXLMemory::DATA_FREE, owner_shard_);
  UnlockRow(row);
  PersistRoots();
  directory_.migration_out_seq.fetch_add(1, std::memory_order_relaxed);
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
