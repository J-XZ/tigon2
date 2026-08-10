#include "kv/engine/kv_migration.h"

#include "common/CXLMemory.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace tigonkv::engine {
namespace {

std::string_view TableValue(const void *value, uint32_t value_size) {
  if (value == nullptr) throw std::invalid_argument("null table value");
  return std::string_view(static_cast<const char *>(value), value_size);
}

}  // namespace

RegionOffsetRowStorage::StoredRow RegionOffsetRowStorage::AllocateAndConstruct(
    const void *value, bool is_placeholder) const {
  if (partition == nullptr) throw std::runtime_error("null owner partition");
  const std::string_view fixed_value = TableValue(value, value_size);
  void *metadata_storage = partition->regions_.AllocateOwnerPrivate(
      sizeof(PrivateMetadataLocal), partition->partition_id_,
      partition->owner_shard_);
  auto *metadata = ::new (metadata_storage)
      PrivateMetadataLocal(OwnerPrivateSharedInitTag{});
  const uint64_t row_bytes = sizeof(PrivateValueStruct) + value_size;
  PrivateValueStruct *row = nullptr;
  try {
    void *row_storage = partition->regions_.AllocateOwnerPrivate(
        row_bytes, partition->partition_id_, partition->owner_shard_);
    row = latency_sim::FixedLatencyConstructShared<PrivateValueStruct>(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, row_storage);
  } catch (const std::bad_alloc &) {
    partition->regions_.FreeOwnerPrivate(
        metadata, sizeof(PrivateMetadataLocal), partition->partition_id_,
        partition->owner_shard_);
    throw;
  }
  mem_access::PrivateAtomicStore(
      row->meta,
      partition->regions_.ToOwnerPrivateOffset(metadata,
                                               partition->partition_id_),
      std::memory_order_release);
  mem_access::PrivateCopyLocalToShared(row->data, fixed_value.data(), value_size);
  latency_sim::FixedLatencyMemoryStore(
      latency_sim::MemoryDomain::kOwnerPrivateSwcc, &metadata->is_valid,
      !is_placeholder);
  const uint64_t observed_tid = latency_sim::FixedLatencyMemoryLoad(
      latency_sim::MemoryDomain::kOwnerPrivateSwcc, &metadata->tid);
  latency_sim::FixedLatencyMemoryStore(
      latency_sim::MemoryDomain::kOwnerPrivateSwcc, &metadata->tid,
      partition->NextCommitTid(observed_tid));
  return partition->regions_.ToOwnerPrivateOffset(row, partition->partition_id_);
}

void RegionOffsetRowStorage::DestroyUnpublished(StoredRow row) const {
  auto *value = partition->ValueFromOffset(row);
  if (value == nullptr) throw std::runtime_error("invalid unpublished row offset");
  partition->FreeUnpublishedPrivateValue(value);
}

void RegionOffsetRowStorage::Retire(StoredRow row) const {
  auto *value = partition->ValueFromOffset(row);
  if (value == nullptr) throw std::runtime_error("invalid retired row offset");
  auto *metadata = partition->MetadataFromValue(value);
  partition->ebr_.add_retired_object(
      value, sizeof(PrivateValueStruct) + value_size,
      star::CXLMemory::MISC_FREE, partition->owner_shard_,
      partition->partition_id_);
  partition->ebr_.add_retired_object(
      metadata, sizeof(PrivateMetadataLocal), star::CXLMemory::MISC_FREE,
      partition->owner_shard_, partition->partition_id_);
}

RegionOffsetRowStorage::MetaDataType *RegionOffsetRowStorage::Meta(
    StoredRow row) const {
  auto *value = partition->ValueFromOffset(row);
  return value == nullptr ? nullptr : &value->meta;
}

void *RegionOffsetRowStorage::Data(StoredRow row) const {
  auto *value = partition->ValueFromOffset(row);
  return value == nullptr ? nullptr : value->data;
}

void *RegionOffsetRowStorage::AdjacentMeta(StoredRow row) const {
  auto *value = partition->ValueFromOffset(row);
  return value == nullptr ? nullptr : partition->MetadataFromValue(value);
}

void RegionOffsetRowStorage::Update(StoredRow row, const void *value) const {
  auto *target = partition->ValueFromOffset(row);
  if (target == nullptr) throw std::runtime_error("invalid row offset");
  const auto fixed_value = TableValue(value, value_size);
  mem_access::PrivateCopyLocalToShared(target->data, fixed_value.data(), value_size);
}

void RegionOffsetRowStorage::Deserialize(StoredRow row,
                                         star::StringPiece bytes) const {
  if (bytes.size() != value_size)
    throw std::invalid_argument("fixed table value has unexpected size");
  Update(row, bytes.data());
}

void RegionOffsetRowStorage::Serialize(star::Encoder &encoder,
                                       const void *value) const {
  encoder.write_n_bytes(TableValue(value, value_size).data(), value_size);
}

KvPartitionTable::KvPartitionTable(
    KVPartition *partition, uint32_t key_size, uint32_t value_size,
    const btreeolc_cxl::TreeNodeAllocation &binding, void *persisted_root,
    bool create_max_sentinel)
    : KvTableBase(
          kSingleTableId, partition->partition_id(),
          RegionOffsetRowStorage{partition, key_size, value_size, binding,
                                 persisted_root}),
      partition_(partition) {
  if (partition_ == nullptr) throw std::invalid_argument("null owner partition");
  if (!create_max_sentinel) return;
  const FixedKey sentinel = FixedKey::InternalMax(key_size);
  const std::string zero_value(value_size, '\0');
  if (!insert(&sentinel, zero_value.data(), false))
    throw std::runtime_error("private tree internal max sentinel duplicate");
}

bool KvPartitionTable::LookupOffset(const FixedKey &key,
                                    RegionOffset *offset) const {
  if (offset == nullptr) throw std::invalid_argument("null table offset output");
  RegionOffset row = kNullOffset;
  if (!lookup_stored_row(key, &row)) return false;
  *offset = row;
  return true;
}

KvMigrationRuntime &KvMigrationRuntime::Instance() {
  static KvMigrationRuntime runtime;
  return runtime;
}

void KvMigrationRuntime::Reset() {
  if (star::migration_manager == clock_.get()) star::migration_manager = nullptr;
  if (star::twopl_pasha_global_helper == helper_.get())
    star::twopl_pasha_global_helper = nullptr;
  clock_.reset();
  helper_.reset();
  cxl_tbl_vecs_.clear();
  tables_.clear();
}

void KvMigrationRuntime::Install(std::vector<KVPartition *> partitions,
                                 uint32_t key_size, uint32_t value_size,
                                 uint32_t coordinator_id,
                                 uint32_t partition_count,
                                 uint64_t hw_cc_budget_per_host) {
  Reset();
  if (partition_count == 0) throw std::invalid_argument("partition_count == 0");
  tables_.assign(partition_count, nullptr);
  cxl_tbl_vecs_.assign(1, std::vector<star::CXLTableBase *>(partition_count,
                                                               nullptr));
  for (KVPartition *partition : partitions) {
    if (partition == nullptr) continue;
    if (partition->partition_id() >= partition_count)
      throw std::invalid_argument("partition id exceeds partition_count");
    auto *table = partition->private_table();
    // Non-owner VMs deliberately have no owner-private SWCC handle.  Their
    // shared CXL table is installed separately in the original helper's
    // cxl_tbl_vecs; only the owner needs an ITable/OLC handle here.
    if (table == nullptr) continue;
    if (table->key_size() != key_size || table->value_size() != value_size)
      throw std::runtime_error("partition table fixed KV size mismatch");
    tables_[partition->partition_id()] = table;
    // Fall through: owner entries also need their shared lookup wrapper.
  }
  for (KVPartition *partition : partitions) {
    if (partition == nullptr) continue;
    const uint32_t partition_id = partition->partition_id();
    if (partition_id >= partition_count)
      throw std::invalid_argument("partition id exceeds partition_count");
    auto *shared_table = partition->shared_cxl_table();
    if (shared_table == nullptr || shared_table->tableID() != kSingleTableId ||
        shared_table->partitionID() != partition_id)
      throw std::runtime_error("invalid shared CXL table wrapper");
    cxl_tbl_vecs_[kSingleTableId][partition_id] = shared_table;
  }
  star::Context helper_context;
  helper_context.coordinator_id = coordinator_id;
  helper_context.partition_num = partition_count;
  helper_context.protocol = "TwoPLPasha";
  helper_context.enable_phantom_detection = true;
  helper_context.enable_scc = true;
  helper_context.enable_migration_optimization = true;
  helper_context.model_cxl_search_overhead = false;
  helper_ = std::make_unique<star::TwoPLPashaHelper>(
      coordinator_id, helper_context, cxl_tbl_vecs_);
  star::twopl_pasha_global_helper = helper_.get();
  clock_ = std::make_unique<star::PolicyClock>(
      KvMoveFromPartitionToShared, KvMoveFromSharedToPartition,
      KvDeleteAndUpdateNextKeyInfo, coordinator_id, partition_count, "OnDemand",
      hw_cc_budget_per_host);
  star::migration_manager = clock_.get();
}

KvPartitionTable *KvMigrationRuntime::TableFor(uint32_t partition_id) const {
  if (partition_id >= tables_.size()) return nullptr;
  return tables_[partition_id];
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
