#include "kv/engine/kv_partition.h"

#include <cstring>
#include <new>
#include <stdexcept>
#include <vector>

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
  RebuildClockTracker();
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
    LockRow(row);
    if (row->is_tombstone) {
      UnlockRow(row);
      return false;
    }
    if (row->is_migrated) {
      const RegionOffset smeta_offset = row->migrated_smeta_off;
      RegionOffset indexed_offset = kNullOffset;
      const bool shared_present = smeta_offset != kNullOffset &&
          shared_tree_->lookup(fixed_key, indexed_offset) && indexed_offset == smeta_offset;
      auto *smeta = shared_present
          ? static_cast<star::TwoPLPashaMetadataShared *>(regions_.hwcc().FromOffset(smeta_offset))
          : nullptr;
      // Keep the private latch through the shared operation so move-out cannot
      // remove the shared authority between lookup and the SCC write.
      const bool written = star::TwoPLPashaHelper::kv_shared_write(
          smeta, owner_shard_, value.data(), value.size());
      if (written) {
        row->value_len = static_cast<uint32_t>(value.size());
        ++row->version;
        smeta->lock();
        smeta->set_bit(37);
        smeta->unlock();
      }
      UnlockRow(row);
      if (!written) throw std::runtime_error("migrated row shared write rejected");
      return false;
    }
    row->value_len = static_cast<uint32_t>(value.size());
    std::memcpy(row->kv + row->key_len, value.data(), value.size());
    ++row->version;
    UnlockRow(row);
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
  if (smeta_offset == kNullOffset) {
    UnlockRow(row);
    return false;
  }
  RegionOffset shared_value = kNullOffset;
  if (!shared_tree_->lookup(MakeKey(key), shared_value) || shared_value != smeta_offset) {
    UnlockRow(row);
    return false;
  }
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(smeta_offset));
  std::string shared(value_len, '\0');
  const bool read = star::TwoPLPashaHelper::kv_shared_read(
      smeta, owner_shard_, shared.data(), value_len);
  if (read) {
    smeta->lock();
    smeta->set_bit(37);
    smeta->unlock();
  }
  UnlockRow(row);
  if (!read)
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
  smeta->set_bit(37);
  star::scc_manager->finish_write(smeta, host_id, payload,
                                  sizeof(star::TwoPLPashaSharedDataSCC) + row->value_len);
  smeta->unlock();
  UnlockRow(row);
  PersistRoots();
  directory_.migration_in_seq.fetch_add(1, std::memory_order_relaxed);
  TrackMigratedKey(fixed_key);
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
  UntrackMigratedKey(fixed_key);
  return true;
}

bool KVPartition::DeletePrivate(std::string_view key) {
  RegionOffset row_offset = kNullOffset;
  const FixedKey fixed_key = MakeKey(key);
  if (!private_tree_->lookup(fixed_key, row_offset)) return false;
  auto *row = RowFromOffset(row_offset);
  LockRow(row);
  row->is_tombstone = 1;
  const uint64_t row_bytes = sizeof(PrivateRow) + regions_.layout().fixed_key_size +
                             regions_.layout().fixed_value_size;
  if (row->is_migrated) {
    const RegionOffset smeta_offset = row->migrated_smeta_off;
    RegionOffset indexed_offset = kNullOffset;
    if (smeta_offset == kNullOffset || !shared_tree_->lookup(fixed_key, indexed_offset) ||
        indexed_offset != smeta_offset) {
      row->is_tombstone = 0;
      UnlockRow(row);
      return false;
    }
    auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
        regions_.hwcc().FromOffset(smeta_offset));
    smeta->lock();
    auto *payload = smeta->get_scc_data();
    if (payload->ref_cnt != 0 || smeta->get_reader_count() != 0 || smeta->is_write_locked()) {
      smeta->unlock();
      row->is_tombstone = 0;
      UnlockRow(row);
      return false;
    }
    smeta->set_write_locked();
    payload->clear_flag(star::TwoPLPashaSharedDataSCC::valid_flag_index);
    if (!shared_tree_->remove(fixed_key)) {
      payload->set_flag(star::TwoPLPashaSharedDataSCC::valid_flag_index);
      smeta->clear_write_locked();
      smeta->unlock();
      row->is_tombstone = 0;
      UnlockRow(row);
      return false;
    }
    row->is_migrated = 0;
    row->migrated_smeta_off = kNullOffset;
    smeta->clear_write_locked();
    smeta->unlock();
    ebr_.add_retired_object(smeta, sizeof(star::TwoPLPashaMetadataShared),
                            star::CXLMemory::METADATA_FREE, owner_shard_);
    ebr_.add_retired_object(payload,
                            sizeof(star::TwoPLPashaSharedDataSCC) +
                                regions_.layout().fixed_value_size,
                            star::CXLMemory::DATA_FREE, owner_shard_);
  }
  if (!private_tree_->remove(fixed_key)) {
    UnlockRow(row);
    throw std::runtime_error("private tree remove failed after shared delete");
  }
  ebr_.add_retired_object(row, row_bytes, star::CXLMemory::MISC_FREE,
                          owner_shard_, partition_id_);
  UnlockRow(row);
  PersistRoots();
  UntrackMigratedKey(fixed_key);
  return true;
}

void KVPartition::TrackMigratedKey(const FixedKey &key) {
  std::lock_guard<std::mutex> lock(clock_mutex_);
  for (const auto &candidate : clock_keys_)
    if (candidate.Compare(key) == 0) return;
  clock_keys_.push_back(key);
}

void KVPartition::UntrackMigratedKey(const FixedKey &key) {
  std::lock_guard<std::mutex> lock(clock_mutex_);
  for (size_t i = 0; i < clock_keys_.size(); ++i) {
    if (clock_keys_[i].Compare(key) != 0) continue;
    clock_keys_.erase(clock_keys_.begin() + static_cast<std::ptrdiff_t>(i));
    if (clock_keys_.empty()) clock_cursor_ = 0;
    else if (clock_cursor_ >= clock_keys_.size()) clock_cursor_ = 0;
    return;
  }
}

void KVPartition::RebuildClockTracker() {
  FixedKey low{};
  FixedKey high{};
  std::memset(high.bytes, 0xff, sizeof(high.bytes));
  std::vector<SharedTree::KeyValuePair> entries;
  shared_tree_->scan(low, high, true, true, 0, entries);
  std::lock_guard<std::mutex> lock(clock_mutex_);
  clock_keys_.clear();
  clock_cursor_ = 0;
  for (const auto &entry : entries) clock_keys_.push_back(entry.first);
}

bool KVPartition::MoveOutClockVictim(uint32_t host_id) {
  FixedKey victim{};
  bool selected = false;
  {
    std::lock_guard<std::mutex> lock(clock_mutex_);
    if (clock_keys_.empty()) return false;
    const size_t attempts = clock_keys_.size();
    for (size_t attempt = 0; attempt < attempts; ++attempt) {
      if (clock_cursor_ >= clock_keys_.size()) clock_cursor_ = 0;
      const FixedKey key = clock_keys_[clock_cursor_++];
      RegionOffset smeta_offset = kNullOffset;
      if (!shared_tree_->lookup(key, smeta_offset) || smeta_offset == kNullOffset) continue;
      auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
          regions_.hwcc().FromOffset(smeta_offset));
      smeta->lock();
      const bool second_chance = smeta->is_bit_set(37);
      if (second_chance) smeta->clear_bit(37);
      smeta->unlock();
      if (second_chance) continue;
      victim = key;
      selected = true;
      break;
    }
  }
  if (!selected) return false;
  return MoveOutPrivate(std::string_view(victim.bytes, regions_.layout().fixed_key_size),
                        host_id);
}

uint64_t KVPartition::shared_payload_used_bytes() const {
  return regions_.layout().domains[static_cast<size_t>(
      AllocationDomain::kSharedPayloadSwcc)].used_bytes.load(std::memory_order_relaxed);
}

uint64_t KVPartition::shared_payload_capacity_bytes() const {
  return regions_.SharedPayloadCapacityBytes();
}

uint64_t KVPartition::hwcc_used_bytes() const {
  uint64_t total = 0;
  for (size_t i = 0; i < static_cast<size_t>(AllocationDomain::kOwnerPrivateSwcc); ++i)
    total += regions_.layout().domains[i].used_bytes.load(std::memory_order_relaxed);
  return total;
}

void KVPartition::PersistRoots() {
  directory_.private_root = regions_.swcc().ToOffset(
      private_tree_->root_for_persistence());
  directory_.shared_root = regions_.hwcc().ToOffset(
      shared_tree_->root_for_persistence());
}

}  // namespace tigonkv::engine
