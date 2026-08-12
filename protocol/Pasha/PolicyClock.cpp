#include "protocol/Pasha/PolicyClock.h"

#include "kv/engine/kv_migration.h"
#include "kv/engine/kv_partition.h"

#include <cstring>
#include <stdexcept>
#include <string_view>

namespace star
{

// Position-independent storage adapter for the original ClockTracker API.
// The list algorithm lives here; KVPartition only supplies owner-private
// control/node allocation and offset resolvers.
class PolicyClock::ClockTracker {
 public:
  struct ClockTrackerNode {
    MigrationManager::migrated_row_entity row_entity;
    tigonkv::engine::RegionOffset node_offset = tigonkv::engine::kNullOffset;
  };

  explicit ClockTracker(tigonkv::engine::KVPartition &partition)
      : partition_(partition) {}

  void lock() {
    auto *control = partition_.ClockTrackerControl();
    (void)latency_sim::FixedLatencyPthreadSpinLockShared(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->lock);
  }
  void unlock() {
    auto *control = partition_.ClockTrackerControl();
    (void)latency_sim::FixedLatencyPthreadSpinUnlockShared(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->lock);
  }

  tigonkv::engine::PrivateClockTrackerNode *allocate() {
    return partition_.AllocateClockTrackerNode();
  }
  void discard(tigonkv::engine::PrivateClockTrackerNode *node) {
    partition_.FreeClockTrackerNode(node);
  }

  void track(tigonkv::engine::PrivateClockTrackerNode *node, ITable *table,
             const void *key,
             const std::tuple<ITable::MetaDataType *, void *> &row,
             void *migration_policy_meta) {
    if (node == nullptr || table == nullptr || key == nullptr ||
        migration_policy_meta == nullptr)
      throw std::runtime_error("Clock move-in returned invalid tracker state");
    const auto row_offset = partition_.ClockTrackerLocalRowOffset(row);
    const auto smeta_offset = partition_.ClockTrackerSharedRowOffset(
        migration_policy_meta);
    latency_sim::FixedLatencyMemoryStore(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &node->value_off,
        row_offset);
    latency_sim::FixedLatencyMemoryStore(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &node->smeta_off,
        smeta_offset);
    latency_sim::FixedLatencyMemoryStore(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &node->prev_off,
        tigonkv::engine::kNullOffset);
    latency_sim::FixedLatencyMemoryStore(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &node->next_off,
        tigonkv::engine::kNullOffset);
    latency_sim::FixedLatencyCopyLocalToShared(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &node->key, key,
        sizeof(node->key));
    const auto node_offset = partition_.ClockTrackerNodeOffset(node);
    auto *control = partition_.ClockTrackerControl();
    const auto head = latency_sim::FixedLatencyMemoryLoad(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->head);
    const auto tail_offset = latency_sim::FixedLatencyMemoryLoad(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->tail);
    if (head == tigonkv::engine::kNullOffset &&
        tail_offset == tigonkv::engine::kNullOffset) {
      latency_sim::FixedLatencyMemoryStore(
          latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->head,
          node_offset);
      latency_sim::FixedLatencyMemoryStore(
          latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->tail,
          node_offset);
    } else {
      auto *tail = partition_.ResolveClockTrackerNode(tail_offset);
      if (tail == nullptr) throw std::runtime_error("Clock tail offset is invalid");
      latency_sim::FixedLatencyMemoryStore(
          latency_sim::MemoryDomain::kOwnerPrivateSwcc, &tail->next_off,
          node_offset);
      latency_sim::FixedLatencyMemoryStore(
          latency_sim::MemoryDomain::kOwnerPrivateSwcc, &node->prev_off,
          tail_offset);
      latency_sim::FixedLatencyMemoryStore(
          latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->tail,
          node_offset);
    }
  }

  ClockTrackerNode *move_forward_and_get_cursor(ITable *table) {
    if (table == nullptr) throw std::invalid_argument("null Clock table");
    auto *control = partition_.ClockTrackerControl();
    const auto cursor = latency_sim::FixedLatencyMemoryLoad(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->cursor);
    tigonkv::engine::RegionOffset next_cursor =
        tigonkv::engine::kNullOffset;
    if (cursor == tigonkv::engine::kNullOffset) {
      next_cursor = latency_sim::FixedLatencyMemoryLoad(
          latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->head);
    } else {
      auto *current = partition_.ResolveClockTrackerNode(cursor);
      if (current == nullptr) throw std::runtime_error("Clock cursor offset is invalid");
      next_cursor = latency_sim::FixedLatencyMemoryLoad(
          latency_sim::MemoryDomain::kOwnerPrivateSwcc, &current->next_off);
    }
    latency_sim::FixedLatencyMemoryStore(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->cursor,
        next_cursor);
    if (next_cursor == tigonkv::engine::kNullOffset) return nullptr;
    auto *node = partition_.ResolveClockTrackerNode(next_cursor);
    if (node == nullptr) throw std::runtime_error("Clock candidate offset is invalid");
    const auto node_value_offset = latency_sim::FixedLatencyMemoryLoad(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &node->value_off);
    const auto node_smeta_offset = latency_sim::FixedLatencyMemoryLoad(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &node->smeta_off);
    if (!partition_.ClockTrackerNodeMatches(node_value_offset,
                                             node_smeta_offset))
      throw std::runtime_error("Clock tracker node/local-row mismatch");
    node_.node_offset = next_cursor;
    node_.row_entity.table = table;
    node_.row_entity.metadata_size = 0;
    node_.row_entity.local_row =
        partition_.ClockTrackerLocalRow(node_value_offset);
    node_.row_entity.migration_manager_meta =
        partition_.ClockTrackerSharedRow(node_smeta_offset);
    latency_sim::FixedLatencyCopySharedToLocal(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, node_.row_entity.key,
        node->key.bytes, table->key_size());
    return &node_;
  }

  void untrack(ClockTrackerNode *node) {
    if (node == nullptr) throw std::invalid_argument("null Clock victim");
    unlink_and_free(node->node_offset);
  }

  void untrack_key(const void *key) {
    if (key == nullptr) throw std::invalid_argument("null Clock key");
    tigonkv::engine::FixedKey fixed_key =
        tigonkv::engine::FixedKey::From(
            std::string_view(static_cast<const char *>(key), 32), 32);
    auto *control = partition_.ClockTrackerControl();
    auto offset = latency_sim::FixedLatencyMemoryLoad(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->head);
    for (; offset != tigonkv::engine::kNullOffset;) {
      auto *node = partition_.ResolveClockTrackerNode(offset);
      if (node == nullptr) throw std::runtime_error("Clock node offset is invalid");
      const auto next = latency_sim::FixedLatencyMemoryLoad(
          latency_sim::MemoryDomain::kOwnerPrivateSwcc, &node->next_off);
      if (latency_sim::FixedLatencyMemcmpSharedToLocal(
              latency_sim::MemoryDomain::kOwnerPrivateSwcc, node->key.bytes,
              fixed_key.bytes, sizeof(node->key.bytes)) == 0) {
        unlink_and_free(offset);
        return;
      }
      offset = next;
    }
  }

  void reset_cursor() {
    auto *control = partition_.ClockTrackerControl();
    latency_sim::FixedLatencyMemoryStore(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->cursor,
        tigonkv::engine::kNullOffset);
  }

 private:
  void unlink_and_free(tigonkv::engine::RegionOffset node_offset) {
    auto *control = partition_.ClockTrackerControl();
    auto *node = partition_.ResolveClockTrackerNode(node_offset);
    if (node == nullptr) throw std::runtime_error("Clock untrack offset is invalid");
    const auto node_prev = latency_sim::FixedLatencyMemoryLoad(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &node->prev_off);
    const auto node_next = latency_sim::FixedLatencyMemoryLoad(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &node->next_off);
    const auto cursor = latency_sim::FixedLatencyMemoryLoad(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->cursor);
    const auto head = latency_sim::FixedLatencyMemoryLoad(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->head);
    const auto tail = latency_sim::FixedLatencyMemoryLoad(
        latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->tail);
    if (cursor == node_offset) {
      latency_sim::FixedLatencyMemoryStore(
          latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->cursor,
          node_prev);
    }
    if (head == tail) {
      if (head != node_offset)
        throw std::runtime_error("Clock singleton unlink mismatch");
      latency_sim::FixedLatencyMemoryStore(
          latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->head,
          tigonkv::engine::kNullOffset);
      latency_sim::FixedLatencyMemoryStore(
          latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->tail,
          tigonkv::engine::kNullOffset);
    } else {
      if (node_prev != tigonkv::engine::kNullOffset) {
        auto *prev = partition_.ResolveClockTrackerNode(node_prev);
        if (prev == nullptr) throw std::runtime_error("Clock previous offset is invalid");
        latency_sim::FixedLatencyMemoryStore(
            latency_sim::MemoryDomain::kOwnerPrivateSwcc, &prev->next_off,
            node_next);
      }
      if (node_next != tigonkv::engine::kNullOffset) {
        auto *next = partition_.ResolveClockTrackerNode(node_next);
        if (next == nullptr) throw std::runtime_error("Clock next offset is invalid");
        latency_sim::FixedLatencyMemoryStore(
            latency_sim::MemoryDomain::kOwnerPrivateSwcc, &next->prev_off,
            node_prev);
      }
      if (head == node_offset) {
        latency_sim::FixedLatencyMemoryStore(
            latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->head,
            node_next);
      }
      if (tail == node_offset) {
        latency_sim::FixedLatencyMemoryStore(
            latency_sim::MemoryDomain::kOwnerPrivateSwcc, &control->tail,
            node_prev);
      }
    }
    partition_.FreeClockTrackerNode(node);
  }

  tigonkv::engine::KVPartition &partition_;
  ClockTrackerNode node_;
};

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

migration_result PolicyClock::move_row_in(
    ITable *table, const void *key,
    const std::tuple<MetaDataType *, void *> &row, bool inc_ref_cnt) {
  // Master: lock → move_from_partition_to_shared_region → track → unlock.
  // Offset adapter allocates the tracker node before the move so OOM can
  // return FAIL_OOM without leaking a half-published shared row.
  auto *partition = PartitionOf(table);
  if (partition == nullptr) return migration_result::FAIL_OOM;
  ClockTracker clock_tracker(*partition);
  void *migration_policy_meta = nullptr;
  clock_tracker.lock();
  auto *tracker_node = static_cast<tigonkv::engine::PrivateClockTrackerNode *>(
      nullptr);
  try {
    try {
      tracker_node = clock_tracker.allocate();
    } catch (const std::bad_alloc &) {
      clock_tracker.unlock();
      return migration_result::FAIL_OOM;
    }
    const migration_result ret = move_from_partition_to_shared_region(
        table, key, row, inc_ref_cnt, migration_policy_meta);
    if (ret == migration_result::SUCCESS) {
      clock_tracker.track(tracker_node, table, key, row,
                          migration_policy_meta);
    } else {
      clock_tracker.discard(tracker_node);
    }
    clock_tracker.unlock();
    return ret;
  } catch (...) {
    if (tracker_node != nullptr) clock_tracker.discard(tracker_node);
    clock_tracker.unlock();
    throw;
  }
}

bool PolicyClock::move_row_out(uint64_t partition_id) {
  // Same control flow as master PolicyClock::move_row_out: blocking tracker
  // lock, unbounded second-chance walk, cursor advance before untrack.
  if (partition_id >= partition_num_) return false;
  auto *kv_table = tigonkv::engine::KvMigrationRuntime::Instance().TableFor(
      static_cast<uint32_t>(partition_id));
  if (kv_table == nullptr || kv_table->partition() == nullptr) return false;
  auto *partition = kv_table->partition();
  ClockTracker clock_tracker(*partition);
  bool ret = false;
  clock_tracker.lock();
  try {
    if (cxl_memory.get_stats(CXLMemory::TOTAL_HW_CC_USAGE) < hw_cc_budget) {
      clock_tracker.unlock();
      return ret;
    }
    while (true) {
      auto *victim = clock_tracker.move_forward_and_get_cursor(kv_table);
      if (victim == nullptr) break;
      auto *smeta = PolicySmeta(victim->row_entity.migration_manager_meta);
      smeta->lock();
      const bool second_chance = smeta->get_second_chance_bit();
      if (second_chance) smeta->clear_second_chance_bit();
      smeta->unlock();
      if (second_chance) continue;
      const bool moved = move_from_shared_region_to_partition(
          victim->row_entity.table, victim->row_entity.key,
          victim->row_entity.local_row);
      if (moved) {
        // Scratch view is overwritten by the master-prescribed cursor advance;
        // keep a stable copy for untrack like master's victim pointer.
        ClockTracker::ClockTrackerNode original_victim = *victim;
        (void)clock_tracker.move_forward_and_get_cursor(kv_table);
        clock_tracker.untrack(&original_victim);
        if (cxl_memory.get_stats(CXLMemory::TOTAL_HW_CC_USAGE) < hw_cc_budget) {
          ret = true;
          break;
        }
      }
    }
    clock_tracker.unlock();
    return ret;
  } catch (...) {
    clock_tracker.unlock();
    throw;
  }
}

bool PolicyClock::delete_specific_row_and_move_out(ITable *table, const void *key,
                                                   bool is_delete_local) {
  // Master locks the tracker around delete_and_update_next_key_info.  The
  // documented E-class fix also frees the tracker node when need_move_out.
  auto *partition = PartitionOf(table);
  if (partition == nullptr) return false;
  ClockTracker clock_tracker(*partition);
  void *migration_policy_meta = nullptr;
  bool need_move_out = false;
  clock_tracker.lock();
  try {
    const bool ret = delete_and_update_next_key_info(
        table, key, is_delete_local, need_move_out, migration_policy_meta);
    if (ret && need_move_out) clock_tracker.untrack_key(key);
    clock_tracker.unlock();
    return ret;
  } catch (...) {
    clock_tracker.unlock();
    throw;
  }
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
  // Clock uses the existing HWCC smeta bit 37 directly.  The opaque
  // MigrationManager argument is the smeta itself, not a pointer into an
  // SCC-resident policy blob.
  return static_cast<TwoPLPashaMetadataShared *>(migration_policy_meta);
}

}  // namespace star
