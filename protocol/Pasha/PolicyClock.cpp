#include "protocol/Pasha/PolicyClock.h"

#include "kv/engine/kv_migration.h"
#include "kv/engine/kv_partition.h"

#include <cstddef>

namespace star
{

PolicyClock::PolicyClock(
    std::function<migration_result(ITable *, const void *,
                                   const std::tuple<std::atomic<uint64_t> *, void *> &,
                                   bool, void *&)>
        move_from_partition_to_shared_region,
    std::function<bool(ITable *, const void *,
                       const std::tuple<std::atomic<uint64_t> *, void *> &)>
        move_from_shared_region_to_partition,
    std::function<bool(ITable *, const void *, bool, bool &, void *&)>
        delete_and_update_next_key_info,
    uint64_t coordinator_id, uint64_t partition_num,
    const std::string when_to_move_out_str, uint64_t hw_cc_budget)
    : MigrationManager(move_from_partition_to_shared_region,
                       move_from_shared_region_to_partition,
                       delete_and_update_next_key_info, when_to_move_out_str),
      hw_cc_budget(hw_cc_budget),
      partition_num_(partition_num) {
  (void)coordinator_id;
}

void PolicyClock::init_migration_policy_metadata(
    void *migration_policy_meta, ITable *table, const void *key,
    const std::tuple<MetaDataType *, void *> &row, uint64_t metadata_size) {
  (void)table;
  (void)key;
  (void)row;
  (void)metadata_size;
  auto *smeta = PolicySmeta(migration_policy_meta);
  smeta->lock();
  smeta->clear_second_chance_bit();
  smeta->unlock();
}

void PolicyClock::access_row(void *migration_policy_meta, uint64_t partition_id) {
  auto *smeta = PolicySmeta(migration_policy_meta);
  smeta->lock();
  smeta->set_second_chance_bit();
  smeta->unlock();
  (void)partition_id;
}

bool PolicyClock::move_specific_row_out(ITable *table, const void *key) {
  auto *partition = PartitionOf(table);
  if (partition == nullptr) return false;
  // MoveOutForMigrationManager already untracks under a brief ClockLock.
  // Holding the spinlock across the heavy move-out path deadlocks on the
  // non-recursive pthread_spinlock (§11.15).
  (void)partition;
  return move_from_shared_region_to_partition(table, key, empty_row_);
}

migration_result PolicyClock::move_row_in(
    ITable *table, const void *key,
    const std::tuple<MetaDataType *, void *> &row, bool inc_ref_cnt) {
  auto *partition = PartitionOf(table);
  if (partition == nullptr) return migration_result::FAIL_OOM;
  void *migration_policy_meta = nullptr;
  // Allocation / SCC / adjacency run outside the Clock spinlock; only the
  // O(1) SWCC list update is locked (§11.14 / §11.15).
  const migration_result ret = move_from_partition_to_shared_region(
      table, key, row, inc_ref_cnt, migration_policy_meta);
  if (ret == migration_result::SUCCESS) {
    partition->ClockLock();
    partition->ClockTrackMigratedKey(key);
    partition->ClockUnlock();
  }
  return ret;
}

bool PolicyClock::move_row_out(uint64_t partition_id) {
  if (partition_id >= partition_num_) return false;
  auto *kv_table = tigonkv::engine::KvMigrationRuntime::Instance().TableFor(
      static_cast<uint32_t>(partition_id));
  if (kv_table == nullptr || kv_table->partition() == nullptr) return false;
  auto *partition = kv_table->partition();
  if (cxl_memory.get_stats(CXLMemory::TOTAL_HW_CC_USAGE) < hw_cc_budget)
    return false;
  return partition->ClockEvictUntilUnderBudget(hw_cc_budget);
}

bool PolicyClock::delete_specific_row_and_move_out(ITable *table, const void *key,
                                                   bool is_delete_local) {
  auto *partition = PartitionOf(table);
  if (partition == nullptr) return false;
  void *migration_policy_meta = nullptr;
  bool need_move_out = false;
  // DeletePrivateForMigrationManager untracks by row offset before EBR retire
  // when the shared row is removed; do not re-lock here (§11.14 / §11.15).
  (void)partition;
  return delete_and_update_next_key_info(
      table, key, is_delete_local, need_move_out, migration_policy_meta);
}

tigonkv::engine::KVPartition *PolicyClock::PartitionOf(ITable *table) {
  auto *kv_table = dynamic_cast<tigonkv::engine::KvPartitionTable *>(table);
  if (kv_table == nullptr) return nullptr;
  return kv_table->partition();
}

TwoPLPashaMetadataShared *PolicyClock::PolicySmeta(
    void *migration_policy_meta) {
  if (migration_policy_meta == nullptr)
    throw std::invalid_argument("null Clock migration policy metadata");
  auto *bytes = static_cast<char *>(migration_policy_meta);
  return reinterpret_cast<TwoPLPashaMetadataShared *>(
      bytes - offsetof(TwoPLPashaMetadataShared, migration_policy_meta));
}

}  // namespace star
