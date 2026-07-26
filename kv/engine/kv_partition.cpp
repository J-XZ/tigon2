#include "kv/engine/kv_partition.h"
#include "kv/engine/kv_migration.h"
#include "kv/engine/mem_access.h"

#include <charconv>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
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
    mem_access::HwccAtomicLoad(&directory_.shared_root);
    if (directory_.private_root == kNullOffset ||
        directory_.shared_root.load(std::memory_order_acquire) == kNullOffset)
      throw std::runtime_error("partition attach missing tree root");
    private_tree_ = new PrivateTree(
        private_binding_, regions_.swcc().FromOffset(directory_.private_root));
    mem_access::HwccAtomicLoad(&directory_.shared_root);
    shared_tree_ = new SharedTree(
        shared_binding_,
        regions_.hwcc().FromOffset(
            directory_.shared_root.load(std::memory_order_acquire)));
    shared_tree_->bind_published_root(&directory_.shared_root);
  } else {
    private_tree_ = new PrivateTree(private_binding_);
    shared_tree_ = new SharedTree(shared_binding_);
    shared_tree_->bind_published_root(&directory_.shared_root);
    PersistRoots();
  }
  // Clock tracker rebuild runs after KvMigrationRuntime::Install so the
  // process-local PolicyClock exists (PLAN §4.5 attach rebuild).
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
  mem_access::PrivateWrite(row->kv, row->key_len + value.size());
  return row;
}

bool KVPartition::PutPrivate(std::string_view key, std::string_view value) {
  EnterEbr();
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
      RegionOffset indexed = kNullOffset;
      const bool shared_present = smeta_offset != kNullOffset &&
          shared_tree_->lookup(fixed_key, indexed) && indexed == smeta_offset;
      auto *smeta = shared_present
          ? static_cast<star::TwoPLPashaMetadataShared *>(regions_.hwcc().FromOffset(smeta_offset))
          : nullptr;
      auto *payload = smeta != nullptr ? smeta->get_scc_data() : nullptr;
      // Keep the private latch through the shared operation so move-out cannot
      // remove the shared authority between lookup and the SCC write.
      if (payload != nullptr) mem_access::SharedPayloadWrite(payload->data, value.size());
      bool written = false;
      // Exclusive SCC write must wait for reader_count==0. Under YCSB-A the
      // CXL Get path keeps readers arriving; bounded yield counts starve and
      // throw. Spin until a short deadline instead (2PL write-lock wait analogue).
      const auto write_deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (smeta != nullptr &&
             std::chrono::steady_clock::now() < write_deadline) {
        written = star::TwoPLPashaHelper::kv_shared_write(
            smeta, owner_shard_, value.data(), value.size());
        if (written) break;
        UnlockRow(row);
        std::this_thread::sleep_for(std::chrono::microseconds(20));
        LockRow(row);
        if (row->is_tombstone || !row->is_migrated ||
            row->migrated_smeta_off != smeta_offset) {
          UnlockRow(row);
          return PutPrivate(key, value);
        }
      }
      if (written) {
        row->value_len = static_cast<uint32_t>(value.size());
        ++row->version;
        // Length for non-owner CXL lookups lives in SCC, not the shared tree.
        if (smeta != nullptr) smeta->set_value_len(row->value_len);
        NoteSharedAccess(smeta);
      }
      UnlockRow(row);
      if (!written) throw std::runtime_error("migrated row shared write rejected");
      return false;
    }
    row->value_len = static_cast<uint32_t>(value.size());
    std::memcpy(row->kv + row->key_len, value.data(), value.size());
    mem_access::PrivateWrite(row->kv + row->key_len, value.size());
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
  EnterEbr();
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
    mem_access::PrivateRead(row->kv + row->key_len, row->value_len);
    UnlockRow(row);
    return true;
  }
  const RegionOffset smeta_offset = row->migrated_smeta_off;
  if (smeta_offset == kNullOffset) {
    UnlockRow(row);
    return false;
  }
  RegionOffset shared_ref = kNullOffset;
  if (!shared_tree_->lookup(MakeKey(key), shared_ref) || shared_ref != smeta_offset) {
    UnlockRow(row);
    return false;
  }
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(smeta_offset));
  auto *payload = smeta->get_scc_data();
  std::string shared(regions_.layout().fixed_value_size, '\0');
  // Row is migrated: treat SCC refusal (writer_waiting / write lock) as
  // contention, not NotFound. Spin like PutPrivate's shared write wait.
  bool read = false;
  const auto read_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < read_deadline) {
    uint32_t value_len = 0;
    read = star::TwoPLPashaHelper::kv_shared_read_value(
        smeta, owner_shard_, shared.data(), shared.size(), &value_len);
    if (read) {
      shared.resize(value_len);
      mem_access::SharedPayloadRead(payload->data, value_len);
    }
    if (read) break;
    UnlockRow(row);
    std::this_thread::sleep_for(std::chrono::microseconds(20));
    LockRow(row);
    if (row->is_tombstone || !row->is_migrated ||
        row->migrated_smeta_off != smeta_offset) {
      UnlockRow(row);
      return GetPrivate(key, value);
    }
  }
  if (read) {
    NoteSharedAccess(smeta);
  }
  UnlockRow(row);
  if (!read)
    return false;
  *value = std::move(shared);
  return true;
}

bool KVPartition::HasShared(std::string_view key) const {
  EnterEbr();
  RegionOffset offset = kNullOffset;
  return shared_tree_->lookup(MakeKey(key), offset) && offset != kNullOffset;
}

bool KVPartition::GetShared(std::string_view key, uint32_t host_id,
                            std::string *value) const {
  EnterEbr();
  if (value == nullptr) throw std::invalid_argument("null shared GET output");
  // CXL-first: shared_tree_ only (original get_migrated_row). Never PrivateRow.
  const FixedKey fixed_key = MakeKey(key);
  star::TwoPLPashaMetadataShared *smeta = nullptr;
  RegionOffset smeta_offset = kNullOffset;
  if (!TryPinShared(fixed_key, &smeta, &smeta_offset)) return false;
  auto *payload = smeta->get_scc_data();
  std::string shared(regions_.layout().fixed_value_size, '\0');
  // Pin proves the row is in CXL. kv_shared_read may refuse while a writer
  // waits; retry instead of returning miss (which would Migrate-storm).
  bool read = false;
  const auto read_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < read_deadline) {
    uint32_t value_len = 0;
    read = star::TwoPLPashaHelper::kv_shared_read_value(
        smeta, host_id, shared.data(), shared.size(), &value_len);
    if (read) {
      shared.resize(value_len);
      mem_access::SharedPayloadRead(payload->data, value_len);
    }
    if (read) break;
    if (!smeta->get_flag(star::TwoPLPashaMetadataShared::valid_flag_index))
      break;
    std::this_thread::sleep_for(std::chrono::microseconds(20));
  }
  if (read) NoteSharedAccess(smeta);
  star::TwoPLPashaHelper::kv_unpin_shared_ref(smeta);
  if (!read) return false;
  *value = std::move(shared);
  return true;
}

bool KVPartition::PutShared(std::string_view key, uint32_t host_id,
                            std::string_view value) {
  EnterEbr();
  if (value.size() > regions_.layout().fixed_value_size)
    throw std::invalid_argument("shared value exceeds fixed value size");
  const FixedKey fixed_key = MakeKey(key);
  star::TwoPLPashaMetadataShared *smeta = nullptr;
  RegionOffset smeta_offset = kNullOffset;
  if (!TryPinShared(fixed_key, &smeta, &smeta_offset)) return false;
  auto *payload = smeta->get_scc_data();
  mem_access::SharedPayloadWrite(payload->data, value.size());
  bool written = false;
  const auto write_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < write_deadline) {
    written = star::TwoPLPashaHelper::kv_shared_write(
        smeta, host_id, value.data(), value.size());
    if (written) break;
    // Pin keeps move-out out; sleep so concurrent readers can drop reader_count.
    std::this_thread::sleep_for(std::chrono::microseconds(20));
  }
  if (written) {
    smeta->set_value_len(static_cast<uint32_t>(value.size()));
    NoteSharedAccess(smeta);
  }
  star::TwoPLPashaHelper::kv_unpin_shared_ref(smeta);
  return written;
}

bool KVPartition::CompareExchangeShared(std::string_view key, uint32_t host_id,
                                        std::string_view expected,
                                        std::string_view desired, bool *exchanged) {
  EnterEbr();
  if (exchanged == nullptr) throw std::invalid_argument("null shared CAS result");
  if (desired.size() > regions_.layout().fixed_value_size)
    throw std::invalid_argument("shared CAS desired value exceeds fixed value size");
  *exchanged = false;
  const FixedKey fixed_key = MakeKey(key);
  star::TwoPLPashaMetadataShared *smeta = nullptr;
  RegionOffset smeta_offset = kNullOffset;
  if (!TryPinShared(fixed_key, &smeta, &smeta_offset)) return false;
  bool changed = false;
  const bool updated = star::TwoPLPashaHelper::kv_shared_update(
      smeta, host_id, regions_.layout().fixed_value_size,
      [&](const std::string &current, std::string *replacement) {
        if (current != expected) return false;
        replacement->assign(desired);
        return true;
      },
      &changed);
  if (!updated) {
    star::TwoPLPashaHelper::kv_unpin_shared_ref(smeta);
    return false;
  }
  if (changed)
    mem_access::SharedPayloadWrite(smeta->get_scc_data()->data, desired.size());
  *exchanged = changed;
  NoteSharedAccess(smeta);
  star::TwoPLPashaHelper::kv_unpin_shared_ref(smeta);
  return true;
}

bool KVPartition::IncrementShared(std::string_view key, uint32_t host_id,
                                  int64_t delta, int64_t *value) {
  EnterEbr();
  if (value == nullptr) throw std::invalid_argument("null shared increment output");
  const FixedKey fixed_key = MakeKey(key);
  star::TwoPLPashaMetadataShared *smeta = nullptr;
  RegionOffset smeta_offset = kNullOffset;
  if (!TryPinShared(fixed_key, &smeta, &smeta_offset)) return false;
  int64_t next = 0;
  bool changed = false;
  const bool updated = star::TwoPLPashaHelper::kv_shared_update(
      smeta, host_id, regions_.layout().fixed_value_size,
      [&](const std::string &current, std::string *replacement) {
        int64_t previous = 0;
        const auto parsed =
            std::from_chars(current.data(), current.data() + current.size(), previous);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != current.data() + current.size() ||
            (delta > 0 && previous > std::numeric_limits<int64_t>::max() - delta) ||
            (delta < 0 && previous < std::numeric_limits<int64_t>::min() - delta))
          throw std::invalid_argument(
              "increment requires a non-overflowing int64 value");
        next = previous + delta;
        *replacement = std::to_string(next);
        return true;
      },
      &changed);
  if (!updated) {
    star::TwoPLPashaHelper::kv_unpin_shared_ref(smeta);
    return false;
  }
  DCHECK(changed);
  mem_access::SharedPayloadWrite(smeta->get_scc_data()->data,
                                std::to_string(next).size());
  *value = next;
  NoteSharedAccess(smeta);
  star::TwoPLPashaHelper::kv_unpin_shared_ref(smeta);
  return true;
}

bool KVPartition::CompareExchangePrivate(std::string_view key,
                                         std::string_view expected,
                                         std::string_view desired,
                                         bool *exchanged) {
  EnterEbr();
  if (exchanged == nullptr) throw std::invalid_argument("null CAS result");
  if (desired.size() > regions_.layout().fixed_value_size)
    throw std::invalid_argument("CAS desired value exceeds fixed value size");
  *exchanged = false;
  RegionOffset row_offset = kNullOffset;
  const FixedKey fixed_key = MakeKey(key);
  if (!private_tree_->lookup(fixed_key, row_offset)) {
    if (!expected.empty()) return false;
    auto *row = AllocateRow(fixed_key, desired);
    if (!private_tree_->insert(fixed_key, regions_.swcc().ToOffset(row)))
      throw std::runtime_error("private tree CAS insert race without owner serialization");
    PersistRoots();
    *exchanged = true;
    return true;
  }
  auto *row = RowFromOffset(row_offset);
  LockRow(row);
  if (row->is_tombstone) {
    UnlockRow(row);
    return false;
  }
  if (!row->is_migrated) {
    const std::string_view current(row->kv + row->key_len, row->value_len);
    if (current == expected) {
      row->value_len = static_cast<uint32_t>(desired.size());
      std::memcpy(row->kv + row->key_len, desired.data(), desired.size());
      mem_access::PrivateWrite(row->kv + row->key_len, desired.size());
      ++row->version;
      *exchanged = true;
    }
    UnlockRow(row);
    return true;
  }
  const RegionOffset smeta_offset = row->migrated_smeta_off;
  RegionOffset indexed = kNullOffset;
  if (smeta_offset == kNullOffset || !shared_tree_->lookup(fixed_key, indexed) ||
      indexed != smeta_offset) {
    UnlockRow(row);
    return false;
  }
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(smeta_offset));
  auto *payload = smeta->get_scc_data();
  bool changed = false;
  const bool read = star::TwoPLPashaHelper::kv_shared_update(
      smeta, owner_shard_, regions_.layout().fixed_value_size,
      [&](const std::string &current, std::string *replacement) {
        if (current != expected) return false;
        replacement->assign(desired);
        return true;
      },
      &changed);
  if (read && changed) {
    mem_access::SharedPayloadWrite(payload->data, desired.size());
    row->value_len = static_cast<uint32_t>(desired.size());
    ++row->version;
    NoteSharedAccess(smeta);
    *exchanged = true;
  }
  UnlockRow(row);
  return read;
}

bool KVPartition::IncrementPrivate(std::string_view key, int64_t delta,
                                   int64_t *value) {
  EnterEbr();
  if (value == nullptr) throw std::invalid_argument("null increment result");
  RegionOffset row_offset = kNullOffset;
  const FixedKey fixed_key = MakeKey(key);
  if (!private_tree_->lookup(fixed_key, row_offset)) {
    const std::string encoded = std::to_string(delta);
    auto *row = AllocateRow(fixed_key, encoded);
    if (!private_tree_->insert(fixed_key, regions_.swcc().ToOffset(row)))
      throw std::runtime_error("private tree increment insert race without owner serialization");
    PersistRoots();
    *value = delta;
    return true;
  }
  auto *row = RowFromOffset(row_offset);
  LockRow(row);
  if (row->is_tombstone) {
    UnlockRow(row);
    return false;
  }
  std::string current;
  star::TwoPLPashaMetadataShared *smeta = nullptr;
  if (!row->is_migrated) {
    current.assign(row->kv + row->key_len, row->value_len);
    mem_access::PrivateRead(row->kv + row->key_len, row->value_len);
  } else {
    const RegionOffset smeta_offset = row->migrated_smeta_off;
    RegionOffset indexed = kNullOffset;
    if (smeta_offset == kNullOffset || !shared_tree_->lookup(fixed_key, indexed) ||
        indexed != smeta_offset) {
      UnlockRow(row);
      return false;
    }
    smeta = static_cast<star::TwoPLPashaMetadataShared *>(
        regions_.hwcc().FromOffset(smeta_offset));
    current.clear();
  }
  if (smeta != nullptr) {
    int64_t next = 0;
    bool changed = false;
    if (!star::TwoPLPashaHelper::kv_shared_update(
            smeta, owner_shard_, regions_.layout().fixed_value_size,
            [&](const std::string &value_before, std::string *replacement) {
              int64_t previous = 0;
              const auto parsed = std::from_chars(
                  value_before.data(), value_before.data() + value_before.size(),
                  previous);
              if (parsed.ec != std::errc{} ||
                  parsed.ptr != value_before.data() + value_before.size() ||
                  (delta > 0 &&
                   previous > std::numeric_limits<int64_t>::max() - delta) ||
                  (delta < 0 &&
                   previous < std::numeric_limits<int64_t>::min() - delta))
                throw std::invalid_argument(
                    "increment requires a non-overflowing int64 value");
              next = previous + delta;
              *replacement = std::to_string(next);
              return true;
            },
            &changed)) {
      UnlockRow(row);
      throw std::runtime_error("migrated increment shared write rejected");
    }
    DCHECK(changed);
    const std::string encoded = std::to_string(next);
    mem_access::SharedPayloadWrite(smeta->get_scc_data()->data, encoded.size());
    row->value_len = static_cast<uint32_t>(encoded.size());
    NoteSharedAccess(smeta);
    ++row->version;
    *value = next;
    UnlockRow(row);
    return true;
  } else {
    int64_t previous = 0;
    const auto parsed =
        std::from_chars(current.data(), current.data() + current.size(), previous);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != current.data() + current.size() ||
        (delta > 0 && previous > std::numeric_limits<int64_t>::max() - delta) ||
        (delta < 0 && previous < std::numeric_limits<int64_t>::min() - delta)) {
      UnlockRow(row);
      throw std::invalid_argument(
          "increment requires a non-overflowing int64 value");
    }
    const int64_t next = previous + delta;
    const std::string encoded = std::to_string(next);
    std::memcpy(row->kv + row->key_len, encoded.data(), encoded.size());
    mem_access::PrivateWrite(row->kv + row->key_len, encoded.size());
    row->value_len = static_cast<uint32_t>(encoded.size());
    ++row->version;
    *value = next;
    UnlockRow(row);
    return true;
  }
}

StatusCode KVPartition::EnsureInShared(std::string_view key, uint32_t host_id,
                                       bool *moved_in) {
  EnterEbr();
  (void)host_id;
  if (moved_in != nullptr) *moved_in = false;
  if (star::scc_manager == nullptr) return StatusCode::kOutOfMemory;
  const FixedKey fixed_key = MakeKey(key);
  star::migration_result result = star::migration_result::FAIL_OOM;
  if (star::migration_manager != nullptr) {
    auto *table = KvMigrationRuntime::Instance().TableFor(partition_id_);
    if (table == nullptr) return StatusCode::kOutOfMemory;
    std::tuple<std::atomic<uint64_t> *, void *> row{nullptr, nullptr};
    // DATA_MIGRATION_REQUEST uses move_row_in(..., inc_ref=false).
    result = star::migration_manager->move_row_in(table, fixed_key.bytes, row, false);
  } else {
    void *migration_policy_meta = nullptr;
    result = MoveInForMigrationManager(fixed_key.bytes, false, migration_policy_meta);
  }
  if (result == star::migration_result::SUCCESS) {
    if (moved_in != nullptr) *moved_in = true;
    return StatusCode::kOk;
  }
  if (result == star::migration_result::FAIL_ALREADY_IN_CXL)
    return StatusCode::kOk;
  RegionOffset row_offset = kNullOffset;
  if (!private_tree_->lookup(fixed_key, row_offset)) return StatusCode::kNotFound;
  auto *row = RowFromOffset(row_offset);
  LockRow(row);
  const bool absent = row->is_tombstone;
  UnlockRow(row);
  return absent ? StatusCode::kNotFound : StatusCode::kOutOfMemory;
}

bool KVPartition::PromotePrivate(std::string_view key, uint32_t host_id) {
  return PromotePrivate(key, host_id, nullptr);
}

bool KVPartition::PromotePrivate(std::string_view key, uint32_t host_id,
                                 star::TwoPLPashaMetadataShared **pinned_existing) {
  EnterEbr();
  (void)host_id;
  if (pinned_existing != nullptr) *pinned_existing = nullptr;
  if (star::scc_manager == nullptr) return false;
  const FixedKey fixed_key = MakeKey(key);
  const bool inc_ref = pinned_existing != nullptr;
  star::migration_result result = star::migration_result::FAIL_OOM;
  if (star::migration_manager != nullptr) {
    auto *table = KvMigrationRuntime::Instance().TableFor(partition_id_);
    if (table == nullptr) return false;
    std::tuple<std::atomic<uint64_t> *, void *> row{nullptr, nullptr};
    result = star::migration_manager->move_row_in(table, fixed_key.bytes, row, inc_ref);
  } else {
    void *migration_policy_meta = nullptr;
    result = MoveInForMigrationManager(fixed_key.bytes, inc_ref, migration_policy_meta);
  }
  // SUCCESS always holds an inc_ref from MoveIn; FAIL_ALREADY_IN_CXL only when
  // MoveIn actually pinned (see MoveInForMigrationManager). Never publish
  // smeta for FAIL_OOM / failed pin — ServeTransportRequest would unpin and
  // underflow ref_cnt under NDEBUG.
  if (inc_ref && (result == star::migration_result::SUCCESS ||
                  result == star::migration_result::FAIL_ALREADY_IN_CXL)) {
    RegionOffset row_offset = kNullOffset;
    if (private_tree_->lookup(fixed_key, row_offset)) {
      auto *private_row = RowFromOffset(row_offset);
      if (private_row->is_migrated && private_row->migrated_smeta_off != kNullOffset) {
        *pinned_existing = static_cast<star::TwoPLPashaMetadataShared *>(
            regions_.hwcc().FromOffset(private_row->migrated_smeta_off));
      }
    }
  }
  return result == star::migration_result::SUCCESS;
}

star::migration_result KVPartition::MoveInForMigrationManager(
    const void *key, bool inc_ref_cnt, void *&migration_policy_meta) {
  migration_policy_meta = nullptr;
  if (star::scc_manager == nullptr) return star::migration_result::FAIL_OOM;
  const FixedKey fixed_key = MakeKey(std::string_view(
      static_cast<const char *>(key), regions_.layout().fixed_key_size));
  RegionOffset row_offset = kNullOffset;
  if (!private_tree_->lookup(fixed_key, row_offset))
    return star::migration_result::FAIL_OOM;
  auto *row = RowFromOffset(row_offset);
  LockRow(row);
  if (row->is_tombstone) {
    UnlockRow(row);
    return star::migration_result::FAIL_OOM;
  }
  if (row->is_migrated) {
    if (inc_ref_cnt) {
      // Caller (PromotePrivate with pinned_existing) will unpin exactly once.
      // Only report FAIL_ALREADY_IN_CXL when the pin is actually held; a failed
      // pin must not hand back smeta or RelWithDebInfo uint8_t ref_cnt wraps
      // 0→255 and saturates every later shared read/write (YCSB Forward stall).
      if (row->migrated_smeta_off == kNullOffset) {
        UnlockRow(row);
        return star::migration_result::FAIL_OOM;
      }
      auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
          regions_.hwcc().FromOffset(row->migrated_smeta_off));
      auto *payload = smeta->get_scc_data();
      if (!star::TwoPLPashaHelper::kv_pin_shared_ref(smeta)) {
        UnlockRow(row);
        return star::migration_result::FAIL_OOM;
      }
      migration_policy_meta = &smeta->migration_policy_meta;
    }
    UnlockRow(row);
    return star::migration_result::FAIL_ALREADY_IN_CXL;
  }
  const uint64_t payload_bytes = regions_.layout().fixed_value_size;
  void *payload_mem = nullptr;
  void *smeta_mem = nullptr;
  try {
    payload_mem = regions_.Allocate(payload_bytes,
        AllocationDomain::kSharedPayloadSwcc, owner_shard_);
    smeta_mem = regions_.Allocate(sizeof(star::TwoPLPashaMetadataShared),
        AllocationDomain::kHwccMetadata, owner_shard_);
  } catch (const std::bad_alloc &) {
    UnlockRow(row);
    return star::migration_result::FAIL_OOM;
  }
  auto *payload = new (payload_mem) star::TwoPLPashaSharedDataSCC;
  auto *smeta = new (smeta_mem) star::TwoPLPashaMetadataShared(payload);
  star::scc_manager->init_scc_metadata(smeta, owner_shard_);
  smeta->lock();
  if (star::migration_manager != nullptr) {
    star::migration_manager->init_migration_policy_metadata(
        &smeta->migration_policy_meta, nullptr, key,
        std::tuple<std::atomic<uint64_t> *, void *>{nullptr, nullptr},
        sizeof(star::TwoPLPashaMetadataShared));
  }
  mem_access::SharedPayloadWrite(payload->data, row->value_len);
  star::scc_manager->do_write(smeta, owner_shard_, payload->data, row->kv + row->key_len,
                               row->value_len);
  smeta->set_value_len(row->value_len);
  smeta->set_flag(star::TwoPLPashaMetadataShared::valid_flag_index);
  if (inc_ref_cnt) {
    if (smeta->get_ref_cnt() == std::numeric_limits<uint8_t>::max()) {
      smeta->unlock();
      UnlockRow(row);
      return star::migration_result::FAIL_OOM;
    }
    smeta->increment_ref_cnt();
  }
  const RegionOffset smeta_offset = regions_.hwcc().ToOffset(smeta);
  if (!shared_tree_->insert(fixed_key, smeta_offset)) {
    if (inc_ref_cnt) smeta->decrement_ref_cnt();
    smeta->unlock();
    UnlockRow(row);
    return star::migration_result::FAIL_OOM;
  }
  row->migrated_smeta_off = smeta_offset;
  row->is_migrated = 1;
  star::scc_manager->finish_write(smeta, owner_shard_, payload, row->value_len);
  migration_policy_meta = &smeta->migration_policy_meta;
  smeta->unlock();
  UnlockRow(row);
  PersistRoots();
  mem_access::HwccAtomicRmw(&directory_.migration_in_seq);
  directory_.migration_in_seq.fetch_add(1, std::memory_order_relaxed);
  star::num_data_move_in.fetch_add(1, std::memory_order_relaxed);
  return star::migration_result::SUCCESS;
}

bool KVPartition::MoveOutForMigrationManager(const void *key) {
  return MoveOutPrivate(
      std::string_view(static_cast<const char *>(key), regions_.layout().fixed_key_size),
      owner_shard_);
}

bool KVPartition::MoveOutPrivate(std::string_view key, uint32_t host_id) {
  EnterEbr();
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
  RegionOffset indexed = kNullOffset;
  if (!shared_tree_->lookup(fixed_key, indexed) || indexed != smeta_offset) {
    UnlockRow(row);
    return false;
  }
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(smeta_offset));
  smeta->lock();
  auto *payload = smeta->get_scc_data();
  if (smeta->get_ref_cnt() != 0 || smeta->get_reader_count() != 0 ||
      smeta->is_write_locked()) {
    smeta->unlock();
    UnlockRow(row);
    return false;
  }
  smeta->set_write_locked();
  const uint32_t value_len = smeta->get_value_len();
  if (value_len > regions_.layout().fixed_value_size) {
    smeta->clear_write_locked();
    smeta->unlock();
    UnlockRow(row);
    return false;
  }
  star::scc_manager->prepare_read(smeta, host_id, payload, value_len);
  if (!smeta->get_flag(star::TwoPLPashaMetadataShared::valid_flag_index)) {
    smeta->clear_write_locked();
    smeta->unlock();
    UnlockRow(row);
    return false;
  }
  mem_access::SharedPayloadRead(payload->data, value_len);
  mem_access::PrivateWrite(row->kv + row->key_len, value_len);
  star::scc_manager->do_read(smeta, host_id, row->kv + row->key_len, payload->data,
                             value_len);
  row->value_len = value_len;
  // Invalidate before tree remove so concurrent TryPinShared cannot pin a
  // row that is about to be EBR-retired. Drop the HWCC latch across the
  // shared-tree remove so GetShared/PutShared (tree then smeta) cannot
  // deadlock against MoveOut (smeta then tree).
  smeta->clear_flag(star::TwoPLPashaMetadataShared::valid_flag_index);
  row->is_migrated = 0;
  row->migrated_smeta_off = kNullOffset;
  smeta->unlock();
  if (!shared_tree_->remove(fixed_key)) {
    smeta->lock();
    smeta->set_flag(star::TwoPLPashaMetadataShared::valid_flag_index);
    row->is_migrated = 1;
    row->migrated_smeta_off = smeta_offset;
    smeta->clear_write_locked();
    smeta->unlock();
    UnlockRow(row);
    return false;
  }
  smeta->lock();
  smeta->clear_write_locked();
  smeta->unlock();
  ebr_.add_retired_object(smeta, sizeof(star::TwoPLPashaMetadataShared),
                          star::CXLMemory::METADATA_FREE, owner_shard_);
  ebr_.add_retired_object(payload, regions_.layout().fixed_value_size,
                          star::CXLMemory::DATA_FREE, owner_shard_);
  UnlockRow(row);
  PersistRoots();
  mem_access::HwccAtomicRmw(&directory_.migration_out_seq);
  directory_.migration_out_seq.fetch_add(1, std::memory_order_relaxed);
  star::num_data_move_out.fetch_add(1, std::memory_order_relaxed);
  KvMigrationRuntime::SyncHwCcUsage(*this);
  return true;
}

std::string KVPartition::KeyString(const FixedKey &key) const {
  const auto bytes = regions_.layout().fixed_key_size;
  size_t length = bytes;
  while (length != 0 && key.bytes[length - 1] == '\0') --length;
  return std::string(key.bytes, length);
}

bool KVPartition::ScanOwned(
    std::string_view start_key, uint64_t limit,
    std::vector<std::pair<std::string, std::string>> *items,
    const std::function<void()> *progress) const {
  EnterEbr();
  if (items == nullptr) throw std::invalid_argument("null partition scan output");
  // Original local TwoPLPasha scan walks the owner table, not the CXL index.
  // Our private tree retains one locator for every migrated row, so following
  // is_migrated under that row's lock gives one authority without a dual-tree
  // merge or migration-sequence retry.
  FixedKey high{};
  std::memset(high.bytes, 0xff, sizeof(high.bytes));
  items->clear();
  FixedKey low = MakeKey(start_key);
  bool left_inclusive = true;
  for (;;) {
    if (limit != 0 && items->size() >= limit) break;
    const uint64_t remaining =
        limit == 0 ? 0 : static_cast<uint64_t>(limit - items->size());
    const uint32_t fetch =
        remaining > std::numeric_limits<uint32_t>::max()
            ? 0
            : static_cast<uint32_t>(remaining);
    std::vector<PrivateTree::KeyValuePair> rows;
    private_tree_->scan(low, high, left_inclusive, true, fetch, rows);
    if (rows.empty()) break;
    for (const auto &entry : rows) {
      auto *row = RowFromOffset(entry.second);
      LockRow(row);
      if (!row->is_tombstone) {
        std::string value;
        if (!row->is_migrated) {
          value.assign(row->kv + row->key_len, row->value_len);
        } else {
          auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
              regions_.hwcc().FromOffset(row->migrated_smeta_off));
          value.resize(regions_.layout().fixed_value_size);
          uint32_t value_len = 0;
          if (!star::TwoPLPashaHelper::kv_shared_read_value(
                  smeta, owner_shard_, value.data(), value.size(),
                  &value_len)) {
            UnlockRow(row);
            return false;
          }
          value.resize(value_len);
          mem_access::SharedPayloadRead(smeta->get_scc_data()->data,
                                        value_len);
        }
        items->emplace_back(KeyString(entry.first), std::move(value));
      }
      UnlockRow(row);
      if (limit != 0 && items->size() >= limit) break;
    }
    if (progress != nullptr) (*progress)();
    if (limit == 0 || (limit != 0 && items->size() >= limit) ||
        rows.size() < fetch)
      break;
    low = rows.back().first;
    left_inclusive = false;
  }
  return true;
}

StatusCode KVPartition::PrepareSharedScan(
    std::string_view start_key, uint64_t limit, uint32_t host_id,
    std::vector<star::TwoPLPashaMetadataShared *> *pinned,
    const std::function<void()> *progress) {
  if (pinned == nullptr) return StatusCode::kInvalidArgument;
  std::vector<std::pair<std::string, std::string>> rows;
  if (!ScanOwned(start_key, limit, &rows, progress)) {
    for (auto *entry : *pinned)
      star::TwoPLPashaHelper::kv_unpin_shared_ref(entry);
    pinned->clear();
    return StatusCode::kCorruption;
  }
  for (const auto &row : rows) {
    star::TwoPLPashaMetadataShared *smeta = nullptr;
    PromotePrivate(row.first, host_id, &smeta);
    if (smeta == nullptr) {
      for (auto *entry : *pinned)
        star::TwoPLPashaHelper::kv_unpin_shared_ref(entry);
      pinned->clear();
      return StatusCode::kOutOfMemory;
    }
    pinned->push_back(smeta);
  }
  return StatusCode::kOk;
}

bool KVPartition::ScanSharedPinned(
    std::string_view start_key, uint64_t limit,
    std::vector<std::pair<std::string, std::string>> *items) const {
  EnterEbr();
  if (items == nullptr) throw std::invalid_argument("null shared scan output");
  FixedKey high{};
  std::memset(high.bytes, 0xff, sizeof(high.bytes));
  std::vector<SharedTree::KeyValuePair> rows;
  const uint32_t fetch =
      limit > std::numeric_limits<uint32_t>::max()
          ? 0
          : static_cast<uint32_t>(limit);
  shared_tree_->scan(MakeKey(start_key), high, true, true, fetch, rows);
  items->clear();
  items->reserve(rows.size());
  bool complete = true;
  for (const auto &entry : rows) {
    auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
        regions_.hwcc().FromOffset(entry.second));
    std::string value(regions_.layout().fixed_value_size, '\0');
    uint32_t value_len = 0;
    bool read = false;
    for (uint32_t attempt = 0; attempt < 64 && !read; ++attempt) {
      read = star::TwoPLPashaHelper::kv_shared_read_value(
          smeta, owner_shard_, value.data(), value.size(), &value_len);
      if (!read) std::this_thread::yield();
    }
    // PrepareSharedScan owns one pin for every row in this exact prefix.
    star::TwoPLPashaHelper::kv_unpin_shared_ref(smeta);
    if (!read) {
      complete = false;
      continue;
    }
    value.resize(value_len);
    mem_access::SharedPayloadRead(smeta->get_scc_data()->data, value_len);
    items->emplace_back(KeyString(entry.first), std::move(value));
  }
  return complete;
}

bool KVPartition::DeletePrivate(std::string_view key) {
  EnterEbr();
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
    RegionOffset indexed = kNullOffset;
    if (smeta_offset == kNullOffset || !shared_tree_->lookup(fixed_key, indexed) ||
        indexed != smeta_offset) {
      row->is_tombstone = 0;
      UnlockRow(row);
      return false;
    }
    auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
        regions_.hwcc().FromOffset(smeta_offset));
    smeta->lock();
    auto *payload = smeta->get_scc_data();
    if (smeta->get_ref_cnt() != 0 || smeta->get_reader_count() != 0 ||
        smeta->is_write_locked()) {
      smeta->unlock();
      row->is_tombstone = 0;
      UnlockRow(row);
      return false;
    }
    smeta->set_write_locked();
    smeta->clear_flag(star::TwoPLPashaMetadataShared::valid_flag_index);
    if (!shared_tree_->remove(fixed_key)) {
      smeta->set_flag(star::TwoPLPashaMetadataShared::valid_flag_index);
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
    ebr_.add_retired_object(payload, regions_.layout().fixed_value_size,
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
  return true;
}

void KVPartition::NoteSharedAccess(star::TwoPLPashaMetadataShared *smeta) const {
  if (smeta == nullptr) return;
  if (star::migration_manager != nullptr)
    star::migration_manager->access_row(&smeta->migration_policy_meta, partition_id_);
  // Mirror Clock heat into the reserved HWCC atomic bit (bit 37).
  // set_bit is CAS-safe vs latch; still take the latch so heat updates
  // serialize with move-out / finish_write_bits on the same word.
  smeta->lock();
  smeta->set_bit(star::TwoPLPashaMetadataShared::second_chance_bit_index);
  smeta->unlock();
}

bool KVPartition::TryPinShared(const FixedKey &key,
                               star::TwoPLPashaMetadataShared **smeta,
                               RegionOffset *smeta_offset) const {
  if (smeta == nullptr || smeta_offset == nullptr) return false;
  *smeta = nullptr;
  *smeta_offset = kNullOffset;
  RegionOffset offset = kNullOffset;
  if (!shared_tree_->lookup(key, offset) || offset == kNullOffset) return false;
  auto *candidate = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(offset));
  // Move-out may race between lookup and pin; refuse invalid / saturated rows.
  if (!star::TwoPLPashaHelper::kv_pin_shared_ref(candidate)) return false;
  RegionOffset again = kNullOffset;
  if (!shared_tree_->lookup(key, again) || again != offset) {
    star::TwoPLPashaHelper::kv_unpin_shared_ref(candidate);
    return false;
  }
  *smeta = candidate;
  *smeta_offset = offset;
  return true;
}

void KVPartition::RebuildClockTracker() {
  EnterEbr();
  auto *clock = KvMigrationRuntime::Instance().clock();
  auto *table = KvMigrationRuntime::Instance().TableFor(partition_id_);
  if (clock == nullptr || table == nullptr) return;
  FixedKey low{};
  FixedKey high{};
  std::memset(high.bytes, 0xff, sizeof(high.bytes));
  std::vector<SharedTree::KeyValuePair> entries;
  shared_tree_->scan(low, high, true, true, 0, entries);
  for (const auto &entry : entries) {
    auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
        regions_.hwcc().FromOffset(entry.second));
    auto *payload = smeta->get_scc_data();
    std::tuple<std::atomic<uint64_t> *, void *> row{nullptr, nullptr};
    clock->track_already_migrated(table, entry.first.bytes, row,
                                  &smeta->migration_policy_meta);
  }
}

bool KVPartition::MoveOutClockVictim(uint32_t host_id) {
  (void)host_id;
  EnterEbr();
  if (star::migration_manager == nullptr) return false;
  // Caller (EnforceMigrationBudget / tests) syncs CXLMemory::TOTAL_HW_CC_USAGE
  // before invoking so PolicyClock's original budget gate sees the intended
  // over-budget condition.
  return star::migration_manager->move_row_out(partition_id_);
}

uint64_t KVPartition::shared_payload_used_bytes() const {
  return regions_.layout().domains[static_cast<size_t>(
      AllocationDomain::kSharedPayloadSwcc)].used_bytes.load(std::memory_order_relaxed);
}

uint64_t KVPartition::shared_payload_capacity_bytes() const {
  return regions_.SharedPayloadCapacityBytes();
}

uint64_t KVPartition::hwcc_used_bytes() const {
  const auto &counter = regions_.layout().owner_migration_hwcc[owner_shard_];
  mem_access::HwccAtomicLoad(&counter.used_bytes);
  return counter.used_bytes.load(std::memory_order_relaxed);
}

uint64_t KVPartition::migrated_key_count() const {
  FixedKey low{};
  FixedKey high{};
  std::memset(high.bytes, 0xff, sizeof(high.bytes));
  std::vector<SharedTree::KeyValuePair> entries;
  shared_tree_->scan(low, high, true, true, 0, entries);
  return entries.size();
}


void KVPartition::PersistRoots() {
  directory_.private_root = regions_.swcc().ToOffset(
      private_tree_->root_for_persistence());
  // Shared live root is published on every store_root via the HWCC atomic slot;
  // keep the slot coherent after owner-local mirror updates.
  mem_access::HwccAtomicStore(&directory_.shared_root);
  directory_.shared_root.store(
      regions_.hwcc().ToOffset(shared_tree_->root_for_persistence()),
      std::memory_order_release);
}

}  // namespace tigonkv::engine
