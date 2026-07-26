#include "kv/engine/kv_migration.h"

#include "common/CXLMemory.h"

#include <stdexcept>

namespace tigonkv::engine {

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
  (void)table;
  (void)key;
  (void)is_delete_local;
  (void)need_move_out;
  (void)migration_policy_meta;
  // KV deletes deliberately bypass PolicyClock because this adapter has no
  // next/previous-key metadata. Reaching the Clock delete hook is a protocol
  // misuse, not a successful no-op.
  throw std::logic_error(
      "KV migration adapter does not implement Clock-managed delete");
}

}  // namespace tigonkv::engine
