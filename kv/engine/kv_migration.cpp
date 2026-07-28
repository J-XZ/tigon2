#include "kv/engine/kv_migration.h"

#include "common/CXLMemory.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace tigonkv::engine {
namespace {

const FixedKey &TableKey(const void *key) {
  if (key == nullptr) throw std::invalid_argument("null table key");
  return *static_cast<const FixedKey *>(key);
}

std::string_view TableValue(const void *value, uint32_t value_size) {
  if (value == nullptr) throw std::invalid_argument("null table value");
  return std::string_view(static_cast<const char *>(value), value_size);
}

}  // namespace

std::tuple<star::ITable::MetaDataType *, void *> KvPartitionTable::Row(
    RegionOffset offset) const {
  if (offset == kNullOffset) return std::make_tuple(nullptr, nullptr);
  auto *value = partition_->ValueFromOffset(offset);
  if (value == nullptr) return std::make_tuple(nullptr, nullptr);
  return std::make_tuple(&value->meta, value->data);
}

void KvPartitionTable::FillAdjacent(RegionOffset *offset, void **meta,
                                     void **data) const {
  *meta = nullptr;
  *data = nullptr;
  if (offset == nullptr || *offset == kNullOffset) return;
  auto *value = partition_->ValueFromOffset(*offset);
  // Match TableBTreeOLC's adjacent callbacks: search() exposes the
  // ValueStruct meta slot, while adjacent callbacks expose the resolved local
  // metadata object stored in that slot.
  *meta = partition_->MetadataFromValue(value);
  *data = value->data;
}

uint64_t KvPartitionTable::get_plain_key(const void *key) {
  const auto &fixed = TableKey(key);
  uint64_t result = 0;
  const size_t bytes = std::min<size_t>(sizeof(result), key_size_);
  for (size_t i = 0; i < bytes; ++i)
    result = (result << 8) | static_cast<unsigned char>(fixed.bytes[i]);
  return result;
}

int KvPartitionTable::compare_key(const void *a, const void *b) {
  return TableKey(a).Compare(TableKey(b));
}

std::tuple<star::ITable::MetaDataType *, void *> KvPartitionTable::search(
    const void *key) {
  partition_->EnterEbr();
  RegionOffset offset = kNullOffset;
  if (!partition_->LookupPrivateOffset(TableKey(key), &offset))
    return std::make_tuple(nullptr, nullptr);
  return Row(offset);
}

void *KvPartitionTable::search_value(const void *key) {
  return std::get<1>(search(key));
}

star::ITable::MetaDataType *KvPartitionTable::search_metadata(const void *key) {
  return std::get<0>(search(key));
}

void KvPartitionTable::scan(
    const void *min_key,
    std::function<bool(const void *, MetaDataType *, void *, bool)> processor) {
  if (!processor) throw std::invalid_argument("empty table scan processor");
  partition_->EnterEbr();
  const FixedKey &start = TableKey(min_key);
  partition_->private_tree_->scanForUpdate(
      start, [&](const FixedKey &key, KVPartition::PrivateTreeValue &offset,
                 bool is_last) {
        auto [meta, data] = Row(offset.row);
        return processor(&key, meta, data, is_last);
      });
}

bool KvPartitionTable::insert(const void *key, const void *value,
                              bool is_placeholder) {
  partition_->EnterEbr();
  auto *row = partition_->AllocateValue(TableValue(value, value_size_));
  auto *metadata = partition_->MetadataFromValue(row);
  metadata->is_valid = !is_placeholder;
  const bool inserted = partition_->private_tree_->insert(
      TableKey(key), KVPartition::PrivateTreeValue{
                         partition_->regions_.swcc().ToOffset(row)});
  if (!inserted) {
    partition_->FreeUnpublishedPrivateValue(row);
    return false;
  }
  partition_->PersistPrivateRootIfChanged();
  return true;
}

bool KvPartitionTable::insert_lock_next_key(
    const void *key, const void *value,
    std::function<bool(const void *, MetaDataType *, void *)> processor,
    bool is_placeholder) {
  if (!processor) throw std::invalid_argument("empty next-key processor");
  partition_->EnterEbr();
  auto *row = partition_->AllocateValue(TableValue(value, value_size_));
  partition_->MetadataFromValue(row)->is_valid = !is_placeholder;
  const KVPartition::PrivateTreeValue row_offset{
      partition_->regions_.swcc().ToOffset(row)};
  const bool inserted = partition_->private_tree_->insert_lock_next_key(
      TableKey(key), row_offset,
      [&](const FixedKey *next_key, KVPartition::PrivateTreeValue *next_offset) {
        auto [meta, data] = next_offset == nullptr ? std::make_tuple(nullptr, nullptr)
                                                    : Row(next_offset->row);
        return processor(next_key, meta, data);
      });
  if (!inserted) {
    partition_->FreeUnpublishedPrivateValue(row);
    return false;
  }
  partition_->PersistPrivateRootIfChanged();
  return true;
}

bool KvPartitionTable::insert_and_process_adjacent_tuples(
    const void *key, const void *value,
    std::function<bool(const void *, MetaDataType *, void *, const void *,
                       MetaDataType *, void *)> processor,
    bool is_placeholder) {
  if (!processor) throw std::invalid_argument("empty adjacent-insert processor");
  partition_->EnterEbr();
  auto *row = partition_->AllocateValue(TableValue(value, value_size_));
  partition_->MetadataFromValue(row)->is_valid = !is_placeholder;
  const KVPartition::PrivateTreeValue row_offset{
      partition_->regions_.swcc().ToOffset(row)};
  const bool inserted = partition_->private_tree_->insert_and_process_adjacent_tuples(
      TableKey(key), row_offset,
      [&](const FixedKey *prev_key, KVPartition::PrivateTreeValue *prev_offset,
          const FixedKey *next_key, KVPartition::PrivateTreeValue *next_offset) {
        auto [prev_meta, prev_data] = prev_offset == nullptr
                                          ? std::make_tuple(nullptr, nullptr)
                                          : Row(prev_offset->row);
        auto [next_meta, next_data] = next_offset == nullptr
                                          ? std::make_tuple(nullptr, nullptr)
                                          : Row(next_offset->row);
        return processor(prev_key, prev_meta, prev_data, next_key, next_meta,
                         next_data);
      });
  if (!inserted) {
    partition_->FreeUnpublishedPrivateValue(row);
    return false;
  }
  partition_->PersistPrivateRootIfChanged();
  return true;
}

bool KvPartitionTable::remove(const void *key) {
  partition_->EnterEbr();
  const bool removed = partition_->private_tree_->remove(TableKey(key));
  if (removed) partition_->PersistPrivateRootIfChanged();
  return removed;
}

bool KvPartitionTable::remove_and_process_adjacent_tuples(
    const void *key,
    std::function<bool(const void *, void *, void *, const void *, void *,
                       void *, const void *, void *, void *)> processor) {
  if (!processor) throw std::invalid_argument("empty adjacent-delete processor");
  partition_->EnterEbr();
  const bool removed = partition_->private_tree_->remove_and_process_adjacent_keys(
      TableKey(key),
      [&](const FixedKey *prev_key, KVPartition::PrivateTreeValue *prev_offset,
          const FixedKey *cur_key, KVPartition::PrivateTreeValue *cur_offset,
          const FixedKey *next_key, KVPartition::PrivateTreeValue *next_offset) {
        void *prev_meta = nullptr;
        void *prev_data = nullptr;
        void *cur_meta = nullptr;
        void *cur_data = nullptr;
        void *next_meta = nullptr;
        void *next_data = nullptr;
        FillAdjacent(prev_offset == nullptr ? nullptr : &prev_offset->row,
                     &prev_meta, &prev_data);
        FillAdjacent(cur_offset == nullptr ? nullptr : &cur_offset->row,
                     &cur_meta, &cur_data);
        FillAdjacent(next_offset == nullptr ? nullptr : &next_offset->row,
                     &next_meta, &next_data);
        return processor(prev_key, prev_meta, prev_data, cur_key, cur_meta,
                         cur_data, next_key, next_meta, next_data);
      });
  if (removed) partition_->PersistPrivateRootIfChanged();
  return removed;
}

void KvPartitionTable::update(
    const void *key, const void *value,
    std::function<void(const void *, const void *)> on_update) {
  partition_->EnterEbr();
  RegionOffset offset = kNullOffset;
  if (!partition_->LookupPrivateOffset(TableKey(key), &offset))
    throw std::runtime_error("table update missing key");
  auto *row = partition_->ValueFromOffset(offset);
  if (on_update) on_update(key, row->data);
  std::memcpy(row->data, TableValue(value, value_size_).data(), value_size_);
  mem_access::PrivateWrite(row->data, value_size_);
}

bool KvPartitionTable::search_and_update_next_key_info(
    const void *key,
    std::function<void(const void *, void *, void *, const void *, void *,
                       void *, const void *, void *, void *)> processor) {
  if (!processor) throw std::invalid_argument("empty next-key update processor");
  partition_->EnterEbr();
  return partition_->private_tree_->lookupForNextKeyUpdate(
      TableKey(key),
      [&](const FixedKey *prev_key, KVPartition::PrivateTreeValue *prev_offset,
          const FixedKey *cur_key, KVPartition::PrivateTreeValue *cur_offset,
          const FixedKey *next_key, KVPartition::PrivateTreeValue *next_offset) {
        void *prev_meta = nullptr;
        void *prev_data = nullptr;
        void *cur_meta = nullptr;
        void *cur_data = nullptr;
        void *next_meta = nullptr;
        void *next_data = nullptr;
        FillAdjacent(prev_offset == nullptr ? nullptr : &prev_offset->row,
                     &prev_meta, &prev_data);
        FillAdjacent(cur_offset == nullptr ? nullptr : &cur_offset->row,
                     &cur_meta, &cur_data);
        FillAdjacent(next_offset == nullptr ? nullptr : &next_offset->row,
                     &next_meta, &next_data);
        processor(prev_key, prev_meta, prev_data, cur_key, cur_meta, cur_data,
                  next_key, next_meta, next_data);
      });
}

void KvPartitionTable::deserialize_value(const void *key,
                                         star::StringPiece bytes) {
  if (bytes.size() != value_size_)
    throw std::invalid_argument("fixed table value has unexpected size");
  update(key, bytes.data());
}

void KvPartitionTable::serialize_value(star::Encoder &encoder,
                                       const void *value) {
  encoder.write_n_bytes(TableValue(value, value_size_).data(), value_size_);
}

KvMigrationRuntime &KvMigrationRuntime::Instance() {
  static KvMigrationRuntime runtime;
  return runtime;
}

void KvMigrationRuntime::Reset() {
  if (star::migration_manager == clock_.get()) star::migration_manager = nullptr;
  clock_.reset();
  tables_.clear();
}

void KvMigrationRuntime::Install(std::vector<KVPartition *> partitions,
                                 uint32_t key_size, uint32_t value_size,
                                 uint32_t coordinator_id,
                                 uint32_t partition_count,
                                 uint64_t hw_cc_budget_per_host) {
  Reset();
  if (partition_count == 0) throw std::invalid_argument("partition_count == 0");
  tables_.clear();
  tables_.resize(partition_count);
  for (KVPartition *partition : partitions) {
    if (partition == nullptr) continue;
    if (partition->partition_id() >= partition_count)
      throw std::invalid_argument("partition id exceeds partition_count");
    tables_[partition->partition_id()] = std::make_unique<KvPartitionTable>(
        partition, key_size, value_size);
  }
  clock_ = std::make_unique<star::PolicyClock>(
      KvMoveFromPartitionToShared, KvMoveFromSharedToPartition,
      KvDeleteAndUpdateNextKeyInfo, coordinator_id, partition_count, "OnDemand",
      hw_cc_budget_per_host);
  star::migration_manager = clock_.get();
}

KvPartitionTable *KvMigrationRuntime::TableFor(uint32_t partition_id) const {
  if (partition_id >= tables_.size()) return nullptr;
  return tables_[partition_id].get();
}

void KvMigrationRuntime::SyncHwCcUsage(const KVPartition &partition) {
  star::cxl_memory.set_total_hw_cc_usage(partition.hwcc_used_bytes());
}

star::migration_result KvMoveFromPartitionToShared(
    star::ITable *table, const void *key,
    const std::tuple<std::atomic<uint64_t> *, void *> &row, bool inc_ref_cnt,
    void *&migration_policy_meta) {
  (void)row;
  auto *kv_table = dynamic_cast<KvPartitionTable *>(table);
  if (kv_table == nullptr || kv_table->partition() == nullptr)
    return star::migration_result::FAIL_OOM;
  return kv_table->partition()->MoveInForMigrationManager(key, inc_ref_cnt,
                                                          migration_policy_meta);
}

bool KvMoveFromSharedToPartition(
    star::ITable *table, const void *key,
    const std::tuple<std::atomic<uint64_t> *, void *> &row) {
  (void)row;
  auto *kv_table = dynamic_cast<KvPartitionTable *>(table);
  if (kv_table == nullptr || kv_table->partition() == nullptr) return false;
  return kv_table->partition()->MoveOutForMigrationManager(key);
}

bool KvDeleteAndUpdateNextKeyInfo(star::ITable *table, const void *key,
                                  bool is_delete_local, bool &need_move_out,
                                  void *&migration_policy_meta) {
  auto *kv_table = dynamic_cast<KvPartitionTable *>(table);
  if (kv_table == nullptr || kv_table->partition() == nullptr)
    return false;
  return kv_table->partition()->DeletePrivateForMigrationManager(
      std::string_view(static_cast<const char *>(key), table->key_size()),
      &need_move_out, &migration_policy_meta,
      /*writer_prelocked=*/is_delete_local,
      /*requester_prelocked=*/!is_delete_local);
}

}  // namespace tigonkv::engine
