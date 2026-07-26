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

// Thin ITable adapter so the original PolicyClock can address one KV partition
// without rewriting the Clock tracker.  Only partitionID/key_size/value_size
// are exercised by PolicyClock; other methods are unsupported stubs.
class KvPartitionTable final : public star::ITable {
 public:
  KvPartitionTable(KVPartition *partition, uint32_t key_size, uint32_t value_size)
      : partition_(partition), key_size_(key_size), value_size_(value_size) {}

  KVPartition *partition() const { return partition_; }

  uint64_t get_plain_key(const void *) override { return 0; }
  int compare_key(const void *a, const void *b) override {
    return std::memcmp(a, b, key_size_);
  }
  std::tuple<MetaDataType *, void *> search(const void *) override {
    return {nullptr, nullptr};
  }
  void *search_value(const void *) override { return nullptr; }
  MetaDataType *search_metadata(const void *) override { return nullptr; }
  void scan(const void *,
            std::function<bool(const void *, MetaDataType *, void *, bool)>) override {
    throw std::logic_error("KV migration adapter has no adjacency-complete scan");
  }
  bool insert(const void *, const void *, bool = false) override { return false; }
  bool insert_lock_next_key(
      const void *, const void *,
      std::function<bool(const void *, MetaDataType *, void *)>,
      bool = false) override {
    throw std::logic_error("KV migration adapter does not maintain next-key state");
  }
  bool insert_and_process_adjacent_tuples(
      const void *, const void *,
      std::function<bool(const void *, MetaDataType *, void *, const void *,
                         MetaDataType *, void *)>,
      bool = false) override {
    throw std::logic_error("KV migration adapter does not maintain adjacency state");
  }
  bool remove(const void *) override { return false; }
  bool remove_and_process_adjacent_tuples(
      const void *,
      std::function<bool(const void *, void *, void *, const void *, void *,
                         void *, const void *, void *, void *)>) override {
    throw std::logic_error("KV migration adapter does not maintain adjacency state");
  }
  void update(const void *, const void *,
              std::function<void(const void *, const void *)> = {}) override {}
  bool search_and_update_next_key_info(
      const void *,
      std::function<void(const void *, void *, void *, const void *, void *,
                         void *, const void *, void *, void *)>) override {
    throw std::logic_error("KV migration adapter does not maintain next-key state");
  }
  void deserialize_value(const void *, star::StringPiece) override {}
  void serialize_value(star::Encoder &, const void *) override {}
  std::size_t key_size() override { return key_size_; }
  std::size_t value_size() override { return value_size_; }
  std::size_t field_size() override { return value_size_; }
  std::size_t tableID() override { return 0; }
  std::size_t partitionID() override { return partition_->partition_id(); }
  int tableType() override { return ITable::HASHMAP; }

 private:
  KVPartition *partition_;
  uint32_t key_size_;
  uint32_t value_size_;
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
  star::PolicyClock *clock() const { return clock_.get(); }

  // Sync CXLMemory TOTAL_HW_CC_USAGE from dual-region domain accounting so
  // PolicyClock's original budget check sees KV HWCC usage.
  static void SyncHwCcUsage(const KVPartition &partition);

 private:
  std::vector<std::unique_ptr<KvPartitionTable>> tables_;
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
