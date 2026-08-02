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
    pthread_spin_lock(&control->lock);
  }
  void unlock() {
    auto *control = partition_.ClockTrackerControl();
    pthread_spin_unlock(&control->lock);
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
    node->value_off = row_offset;
    node->smeta_off = smeta_offset;
    node->prev_off = tigonkv::engine::kNullOffset;
    node->next_off = tigonkv::engine::kNullOffset;
    std::memcpy(&node->key, key, sizeof(node->key));
    const auto node_offset = partition_.ClockTrackerNodeOffset(node);
    auto *control = partition_.ClockTrackerControl();
    tigonkv::engine::mem_access::PrivateRead(&control->head,
                                             sizeof(control->head));
    tigonkv::engine::mem_access::PrivateRead(&control->tail,
                                             sizeof(control->tail));
    if (control->head == tigonkv::engine::kNullOffset &&
        control->tail == tigonkv::engine::kNullOffset) {
      tigonkv::engine::mem_access::PrivateWrite(&control->head,
                                                sizeof(control->head));
      tigonkv::engine::mem_access::PrivateWrite(&control->tail,
                                                sizeof(control->tail));
      control->head = node_offset;
      control->tail = node_offset;
    } else {
      auto *tail = partition_.ResolveClockTrackerNode(control->tail);
      if (tail == nullptr) throw std::runtime_error("Clock tail offset is invalid");
      tigonkv::engine::mem_access::PrivateRead(tail, sizeof(*tail));
      tigonkv::engine::mem_access::PrivateWrite(tail, sizeof(*tail));
      tail->next_off = node_offset;
      tigonkv::engine::mem_access::PrivateWrite(node, sizeof(*node));
      node->prev_off = control->tail;
      tigonkv::engine::mem_access::PrivateWrite(&control->tail,
                                                sizeof(control->tail));
      control->tail = node_offset;
    }
    tigonkv::engine::mem_access::PrivateWrite(node, sizeof(*node));
  }

  ClockTrackerNode *move_forward_and_get_cursor(ITable *table) {
    if (table == nullptr) throw std::invalid_argument("null Clock table");
    auto *control = partition_.ClockTrackerControl();
    tigonkv::engine::mem_access::PrivateRead(&control->cursor,
                                             sizeof(control->cursor));
    if (control->cursor == tigonkv::engine::kNullOffset) {
      tigonkv::engine::mem_access::PrivateRead(&control->head,
                                               sizeof(control->head));
      control->cursor = control->head;
    } else {
      auto *current = partition_.ResolveClockTrackerNode(control->cursor);
      if (current == nullptr) throw std::runtime_error("Clock cursor offset is invalid");
      tigonkv::engine::mem_access::PrivateRead(current, sizeof(*current));
      control->cursor = current->next_off;
    }
    tigonkv::engine::mem_access::PrivateWrite(&control->cursor,
                                              sizeof(control->cursor));
    if (control->cursor == tigonkv::engine::kNullOffset) return nullptr;
    auto *node = partition_.ResolveClockTrackerNode(control->cursor);
    if (node == nullptr) throw std::runtime_error("Clock candidate offset is invalid");
    tigonkv::engine::mem_access::PrivateRead(node, sizeof(*node));
    if (!partition_.ClockTrackerNodeMatches(*node))
      throw std::runtime_error("Clock tracker node/local-row mismatch");
    node_.node_offset = control->cursor;
    node_.row_entity = MigrationManager::migrated_row_entity(
        table, node->key.bytes, partition_.ClockTrackerLocalRow(node->value_off),
        /*metadata_size=*/0);
    node_.row_entity.migration_manager_meta =
        partition_.ClockTrackerSharedRow(node->smeta_off);
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
    tigonkv::engine::mem_access::PrivateRead(&control->head,
                                             sizeof(control->head));
    for (auto offset = control->head; offset != tigonkv::engine::kNullOffset;) {
      auto *node = partition_.ResolveClockTrackerNode(offset);
      if (node == nullptr) throw std::runtime_error("Clock node offset is invalid");
      tigonkv::engine::mem_access::PrivateRead(node, sizeof(*node));
      const auto next = node->next_off;
      if (node->key.Compare(fixed_key) == 0) {
        unlink_and_free(offset);
        return;
      }
      offset = next;
    }
  }

  void reset_cursor() {
    auto *control = partition_.ClockTrackerControl();
    tigonkv::engine::mem_access::PrivateWrite(&control->cursor,
                                              sizeof(control->cursor));
    control->cursor = tigonkv::engine::kNullOffset;
  }

 private:
  void unlink_and_free(tigonkv::engine::RegionOffset node_offset) {
    auto *control = partition_.ClockTrackerControl();
    auto *node = partition_.ResolveClockTrackerNode(node_offset);
    if (node == nullptr) throw std::runtime_error("Clock untrack offset is invalid");
    tigonkv::engine::mem_access::PrivateRead(node, sizeof(*node));
    tigonkv::engine::mem_access::PrivateRead(control, sizeof(*control));
    if (control->cursor == node_offset) control->cursor = node->prev_off;
    if (control->head == control->tail) {
      if (control->head != node_offset)
        throw std::runtime_error("Clock singleton unlink mismatch");
      control->head = tigonkv::engine::kNullOffset;
      control->tail = tigonkv::engine::kNullOffset;
    } else {
      if (node->prev_off != tigonkv::engine::kNullOffset) {
        auto *prev = partition_.ResolveClockTrackerNode(node->prev_off);
        if (prev == nullptr) throw std::runtime_error("Clock previous offset is invalid");
        tigonkv::engine::mem_access::PrivateWrite(prev, sizeof(*prev));
        prev->next_off = node->next_off;
      }
      if (node->next_off != tigonkv::engine::kNullOffset) {
        auto *next = partition_.ResolveClockTrackerNode(node->next_off);
        if (next == nullptr) throw std::runtime_error("Clock next offset is invalid");
        tigonkv::engine::mem_access::PrivateWrite(next, sizeof(*next));
        next->prev_off = node->prev_off;
      }
      if (control->head == node_offset) control->head = node->next_off;
      if (control->tail == node_offset) control->tail = node->prev_off;
    }
    tigonkv::engine::mem_access::PrivateWrite(control, sizeof(*control));
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
