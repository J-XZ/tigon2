#include "kv/engine/kv_partition.h"
#include "kv/engine/kv_migration.h"
#include "kv/engine/mem_access.h"

#include <charconv>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
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
      SharedIndexValue indexed{};
      const bool shared_present = smeta_offset != kNullOffset &&
          shared_tree_->lookup(fixed_key, indexed) && indexed.smeta_offset == smeta_offset;
      auto *smeta = shared_present
          ? static_cast<star::TwoPLPashaMetadataShared *>(regions_.hwcc().FromOffset(smeta_offset))
          : nullptr;
      auto *payload = smeta != nullptr ? smeta->get_scc_data() : nullptr;
      // Keep the private latch through the shared operation so move-out cannot
      // remove the shared authority between lookup and the SCC write.
      if (payload != nullptr) mem_access::SharedPayloadWrite(payload->data, value.size());
      const bool written = star::TwoPLPashaHelper::kv_shared_write(
          smeta, owner_shard_, value.data(), value.size());
      if (written) {
        row->value_len = static_cast<uint32_t>(value.size());
        ++row->version;
        // Keep shared-index length authoritative for non-owner CXL lookups.
        if (indexed.value_len != row->value_len) {
          indexed.value_len = row->value_len;
          shared_tree_->remove(fixed_key);
          if (!shared_tree_->insert(fixed_key, indexed))
            throw std::runtime_error("shared index length update insert failed");
        }
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
  const uint32_t value_len = row->value_len;
  if (smeta_offset == kNullOffset) {
    UnlockRow(row);
    return false;
  }
  SharedIndexValue shared_ref{};
  if (!shared_tree_->lookup(MakeKey(key), shared_ref) ||
      shared_ref.smeta_offset != smeta_offset) {
    UnlockRow(row);
    return false;
  }
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(smeta_offset));
  auto *payload = smeta->get_scc_data();
  std::string shared(value_len, '\0');
  mem_access::SharedPayloadRead(payload->data, value_len);
  const bool read = star::TwoPLPashaHelper::kv_shared_read(
      smeta, owner_shard_, shared.data(), value_len);
  if (read) {
    NoteSharedAccess(smeta);
  }
  UnlockRow(row);
  if (!read)
    return false;
  *value = std::move(shared);
  return true;
}

bool KVPartition::GetShared(std::string_view key, uint32_t host_id,
                            std::string *value) const {
  EnterEbr();
  if (value == nullptr) throw std::invalid_argument("null shared GET output");
  // CXL-first: shared_tree_ only (original get_migrated_row). Never PrivateRow.
  SharedIndexValue ref{};
  const FixedKey fixed_key = MakeKey(key);
  if (!shared_tree_->lookup(fixed_key, ref) || ref.smeta_offset == kNullOffset)
    return false;
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(ref.smeta_offset));
  auto *payload = smeta->get_scc_data();
  std::string shared(ref.value_len, '\0');
  mem_access::SharedPayloadRead(payload->data, ref.value_len);
  const bool read = star::TwoPLPashaHelper::kv_shared_read(
      smeta, host_id, shared.data(), ref.value_len);
  if (read) NoteSharedAccess(smeta);
  if (!read) return false;
  *value = std::move(shared);
  return true;
}

bool KVPartition::PutShared(std::string_view key, uint32_t host_id,
                            std::string_view value) {
  EnterEbr();
  if (value.size() > regions_.layout().fixed_value_size)
    throw std::invalid_argument("shared value exceeds fixed value size");
  SharedIndexValue ref{};
  const FixedKey fixed_key = MakeKey(key);
  if (!shared_tree_->lookup(fixed_key, ref) || ref.smeta_offset == kNullOffset)
    return false;
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(ref.smeta_offset));
  auto *payload = smeta->get_scc_data();
  mem_access::SharedPayloadWrite(payload->data, value.size());
  const bool written = star::TwoPLPashaHelper::kv_shared_write(
      smeta, host_id, value.data(), value.size());
  if (!written) return false;
  if (ref.value_len != static_cast<uint32_t>(value.size())) {
    ref.value_len = static_cast<uint32_t>(value.size());
    shared_tree_->remove(fixed_key);
    if (!shared_tree_->insert(fixed_key, ref))
      throw std::runtime_error("shared index length update insert failed");
  }
  NoteSharedAccess(smeta);
  return true;
}

bool KVPartition::CompareExchangeShared(std::string_view key, uint32_t host_id,
                                        std::string_view expected,
                                        std::string_view desired, bool *exchanged) {
  EnterEbr();
  if (exchanged == nullptr) throw std::invalid_argument("null shared CAS result");
  if (desired.size() > regions_.layout().fixed_value_size)
    throw std::invalid_argument("shared CAS desired value exceeds fixed value size");
  *exchanged = false;
  SharedIndexValue ref{};
  const FixedKey fixed_key = MakeKey(key);
  if (!shared_tree_->lookup(fixed_key, ref) || ref.smeta_offset == kNullOffset)
    return false;
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(ref.smeta_offset));
  auto *payload = smeta->get_scc_data();
  std::string current(ref.value_len, '\0');
  mem_access::SharedPayloadRead(payload->data, ref.value_len);
  if (!star::TwoPLPashaHelper::kv_shared_read(smeta, host_id, current.data(),
                                              ref.value_len))
    return false;
  if (current != expected) {
    NoteSharedAccess(smeta);
    return true;
  }
  mem_access::SharedPayloadWrite(payload->data, desired.size());
  if (!star::TwoPLPashaHelper::kv_shared_write(smeta, host_id, desired.data(),
                                              desired.size()))
    return false;
  if (ref.value_len != static_cast<uint32_t>(desired.size())) {
    ref.value_len = static_cast<uint32_t>(desired.size());
    shared_tree_->remove(fixed_key);
    if (!shared_tree_->insert(fixed_key, ref))
      throw std::runtime_error("shared index CAS length update insert failed");
  }
  *exchanged = true;
  NoteSharedAccess(smeta);
  return true;
}

bool KVPartition::IncrementShared(std::string_view key, uint32_t host_id,
                                  int64_t delta, int64_t *value) {
  EnterEbr();
  if (value == nullptr) throw std::invalid_argument("null shared increment output");
  SharedIndexValue ref{};
  const FixedKey fixed_key = MakeKey(key);
  if (!shared_tree_->lookup(fixed_key, ref) || ref.smeta_offset == kNullOffset)
    return false;
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(ref.smeta_offset));
  auto *payload = smeta->get_scc_data();
  std::string current(ref.value_len, '\0');
  mem_access::SharedPayloadRead(payload->data, ref.value_len);
  if (!star::TwoPLPashaHelper::kv_shared_read(smeta, host_id, current.data(),
                                              ref.value_len))
    return false;
  int64_t numeric = 0;
  try {
    numeric = std::stoll(current);
  } catch (...) {
    NoteSharedAccess(smeta);
    return false;
  }
  numeric += delta;
  const std::string encoded = std::to_string(numeric);
  if (encoded.size() > regions_.layout().fixed_value_size) {
    NoteSharedAccess(smeta);
    return false;
  }
  mem_access::SharedPayloadWrite(payload->data, encoded.size());
  if (!star::TwoPLPashaHelper::kv_shared_write(smeta, host_id, encoded.data(),
                                              encoded.size()))
    return false;
  if (ref.value_len != static_cast<uint32_t>(encoded.size())) {
    ref.value_len = static_cast<uint32_t>(encoded.size());
    shared_tree_->remove(fixed_key);
    if (!shared_tree_->insert(fixed_key, ref))
      throw std::runtime_error("shared index incr length update insert failed");
  }
  *value = numeric;
  NoteSharedAccess(smeta);
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
  SharedIndexValue indexed{};
  if (smeta_offset == kNullOffset || !shared_tree_->lookup(fixed_key, indexed) ||
      indexed.smeta_offset != smeta_offset) {
    UnlockRow(row);
    return false;
  }
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(smeta_offset));
  auto *payload = smeta->get_scc_data();
  std::string current(row->value_len, '\0');
  mem_access::SharedPayloadRead(payload->data, current.size());
  const bool read = star::TwoPLPashaHelper::kv_shared_read(
      smeta, owner_shard_, current.data(), current.size());
  if (read && current == expected) {
    mem_access::SharedPayloadWrite(payload->data, desired.size());
    if (!star::TwoPLPashaHelper::kv_shared_write(
            smeta, owner_shard_, desired.data(), desired.size())) {
      UnlockRow(row);
      throw std::runtime_error("migrated CAS shared write rejected");
    }
    row->value_len = static_cast<uint32_t>(desired.size());
    ++row->version;
    if (indexed.value_len != row->value_len) {
      indexed.value_len = row->value_len;
      shared_tree_->remove(fixed_key);
      if (!shared_tree_->insert(fixed_key, indexed))
        throw std::runtime_error("shared index CAS length update insert failed");
    }
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
  SharedIndexValue indexed{};
  if (!row->is_migrated) {
    current.assign(row->kv + row->key_len, row->value_len);
    mem_access::PrivateRead(row->kv + row->key_len, row->value_len);
  } else {
    const RegionOffset smeta_offset = row->migrated_smeta_off;
    if (smeta_offset == kNullOffset || !shared_tree_->lookup(fixed_key, indexed) ||
        indexed.smeta_offset != smeta_offset) {
      UnlockRow(row);
      return false;
    }
    smeta = static_cast<star::TwoPLPashaMetadataShared *>(
        regions_.hwcc().FromOffset(smeta_offset));
    auto *payload = smeta->get_scc_data();
    current.resize(row->value_len);
    mem_access::SharedPayloadRead(payload->data, current.size());
    if (!star::TwoPLPashaHelper::kv_shared_read(
            smeta, owner_shard_, current.data(), current.size())) {
      UnlockRow(row);
      return false;
    }
  }
  int64_t previous = 0;
  const auto parsed = std::from_chars(current.data(), current.data() + current.size(), previous);
  if (parsed.ec != std::errc{} || parsed.ptr != current.data() + current.size() ||
      (delta > 0 && previous > std::numeric_limits<int64_t>::max() - delta) ||
      (delta < 0 && previous < std::numeric_limits<int64_t>::min() - delta)) {
    UnlockRow(row);
    throw std::invalid_argument("increment requires a non-overflowing int64 value");
  }
  const int64_t next = previous + delta;
  const std::string encoded = std::to_string(next);
  if (encoded.size() > regions_.layout().fixed_value_size) {
    UnlockRow(row);
    throw std::invalid_argument("increment encoded value exceeds fixed value size");
  }
  if (smeta != nullptr) {
    auto *payload_w = smeta->get_scc_data();
    mem_access::SharedPayloadWrite(payload_w->data, encoded.size());
    if (!star::TwoPLPashaHelper::kv_shared_write(
            smeta, owner_shard_, encoded.data(), encoded.size())) {
      UnlockRow(row);
      throw std::runtime_error("migrated increment shared write rejected");
    }
    row->value_len = static_cast<uint32_t>(encoded.size());
    if (indexed.value_len != row->value_len) {
      indexed.value_len = row->value_len;
      shared_tree_->remove(fixed_key);
      if (!shared_tree_->insert(fixed_key, indexed))
        throw std::runtime_error("shared index incr length update insert failed");
    }
    NoteSharedAccess(smeta);
  } else {
    std::memcpy(row->kv + row->key_len, encoded.data(), encoded.size());
    mem_access::PrivateWrite(row->kv + row->key_len, encoded.size());
    row->value_len = static_cast<uint32_t>(encoded.size());
  }
  ++row->version;
  *value = next;
  UnlockRow(row);
  return true;
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
    if (inc_ref_cnt && row->migrated_smeta_off != kNullOffset) {
      auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
          regions_.hwcc().FromOffset(row->migrated_smeta_off));
      auto *payload = smeta->get_scc_data();
      if (star::TwoPLPashaHelper::kv_pin_shared_ref(smeta))
        migration_policy_meta = &payload->migration_policy_meta;
    }
    UnlockRow(row);
    return star::migration_result::FAIL_ALREADY_IN_CXL;
  }
  const uint64_t payload_bytes = sizeof(star::TwoPLPashaSharedDataSCC) +
                                 regions_.layout().fixed_value_size;
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
        &payload->migration_policy_meta, nullptr, key,
        std::tuple<std::atomic<uint64_t> *, void *>{nullptr, nullptr},
        sizeof(star::TwoPLPashaMetadataShared));
  }
  mem_access::SharedPayloadWrite(payload->data, row->value_len);
  star::scc_manager->do_write(smeta, owner_shard_, payload->data, row->kv + row->key_len,
                               row->value_len);
  payload->set_flag(star::TwoPLPashaSharedDataSCC::valid_flag_index);
  if (inc_ref_cnt) {
    if (payload->ref_cnt == std::numeric_limits<uint8_t>::max()) {
      smeta->unlock();
      UnlockRow(row);
      return star::migration_result::FAIL_OOM;
    }
    payload->ref_cnt++;
  }
  const RegionOffset smeta_offset = regions_.hwcc().ToOffset(smeta);
  const SharedIndexValue shared_ref{smeta_offset, row->value_len, 0};
  if (!shared_tree_->insert(fixed_key, shared_ref)) {
    if (inc_ref_cnt) payload->ref_cnt--;
    smeta->unlock();
    UnlockRow(row);
    return star::migration_result::FAIL_OOM;
  }
  row->migrated_smeta_off = smeta_offset;
  row->is_migrated = 1;
  star::scc_manager->finish_write(smeta, owner_shard_, payload,
                                  sizeof(star::TwoPLPashaSharedDataSCC) + row->value_len);
  migration_policy_meta = &payload->migration_policy_meta;
  smeta->unlock();
  UnlockRow(row);
  PersistRoots();
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
  SharedIndexValue indexed{};
  if (!shared_tree_->lookup(fixed_key, indexed) || indexed.smeta_offset != smeta_offset) {
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
  mem_access::SharedPayloadRead(payload->data, row->value_len);
  star::scc_manager->do_read(smeta, host_id, row->kv + row->key_len, payload->data,
                             row->value_len);
  row->is_migrated = 0;
  row->migrated_smeta_off = kNullOffset;
  if (!shared_tree_->remove(fixed_key)) {
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
  // Reuse BPlusTree::scan's native limit (0 = unlimited), matching original
  // Tigon ScanProcessor early-stop and cxlkv Tree::Scan(limit).  Dual-tree
  // merge walks sorted batches with a resume cursor so a finite limit never
  // materializes the full range before truncate.
  FixedKey high{};
  std::memset(high.bytes, 0xff, sizeof(high.bytes));
  const FixedKeyComparator key_cmp;
  for (uint32_t attempt = 0; attempt < 8; ++attempt) {
    const uint64_t in_before = directory_.migration_in_seq.load(std::memory_order_acquire);
    const uint64_t out_before = directory_.migration_out_seq.load(std::memory_order_acquire);
    items->clear();
    FixedKey low = MakeKey(start_key);
    bool left_inclusive = true;
    uint32_t batches = 0;
    for (;;) {
      const bool unlimited = limit == 0;
      if (!unlimited && items->size() >= limit) break;
      if (++batches > 1048576u) break;  // safety against resume livelock
      const uint64_t remaining =
          unlimited ? 0
                    : static_cast<uint64_t>(limit - items->size());
      const uint32_t fetch =
          unlimited
              ? 0
              : (remaining > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())
                     ? 0
                     : static_cast<uint32_t>(remaining));
      std::vector<PrivateTree::KeyValuePair> private_rows;
      std::vector<SharedTree::KeyValuePair> shared_rows;
      private_tree_->scan(low, high, left_inclusive, true, fetch, private_rows);
      shared_tree_->scan(low, high, left_inclusive, true, fetch, shared_rows);
      if (private_rows.empty() && shared_rows.empty()) break;

      size_t pi = 0;
      size_t si = 0;
      FixedKey last_raw{};
      bool have_last_raw = false;
      auto note_raw = [&](const FixedKey &key) {
        last_raw = key;
        have_last_raw = true;
      };
      auto try_private = [&](const PrivateTree::KeyValuePair &entry) {
        note_raw(entry.first);
        auto *row = RowFromOffset(entry.second);
        LockRow(row);
        const bool live = !row->is_tombstone && !row->is_migrated;
        std::string value;
        if (live) value.assign(row->kv + row->key_len, row->value_len);
        UnlockRow(row);
        if (!live) return;
        items->emplace_back(KeyString(entry.first), std::move(value));
      };
      auto try_shared = [&](const SharedTree::KeyValuePair &entry) {
        note_raw(entry.first);
        // Owner ScanOwned: validate private migration authority, then read CXL.
        RegionOffset private_offset = kNullOffset;
        if (!private_tree_->lookup(entry.first, private_offset)) return;
        auto *row = RowFromOffset(private_offset);
        LockRow(row);
        const bool live = !row->is_tombstone && row->is_migrated &&
                          row->migrated_smeta_off == entry.second.smeta_offset;
        if (!live) {
          UnlockRow(row);
          return;
        }
        const uint32_t value_len = entry.second.value_len;
        auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
            regions_.hwcc().FromOffset(entry.second.smeta_offset));
        auto *payload = smeta->get_scc_data();
        std::string value(value_len, '\0');
        mem_access::SharedPayloadRead(payload->data, value.size());
        const bool read = star::TwoPLPashaHelper::kv_shared_read(
            smeta, owner_shard_, value.data(), value.size());
        if (read) {
          const std::string key = KeyString(entry.first);
          if (!items->empty() && items->back().first == key)
            items->back().second = std::move(value);
          else
            items->emplace_back(key, std::move(value));
        }
        UnlockRow(row);
      };

      while (pi < private_rows.size() || si < shared_rows.size()) {
        if (!unlimited && items->size() >= limit) break;
        const bool take_private =
            si >= shared_rows.size() ||
            (pi < private_rows.size() &&
             key_cmp(private_rows[pi].first, shared_rows[si].first) <= 0);
        const bool take_shared =
            pi >= private_rows.size() ||
            (si < shared_rows.size() &&
             key_cmp(shared_rows[si].first, private_rows[pi].first) <= 0);
        // When keys compare equal, apply private then shared (same authority
        // order as the previous map merge: private first, shared overlays).
        if (take_private && take_shared) {
          try_private(private_rows[pi++]);
          try_shared(shared_rows[si++]);
        } else if (take_private) {
          try_private(private_rows[pi++]);
        } else {
          try_shared(shared_rows[si++]);
        }
      }

      // Let the engine drain MPSC traffic between batches so concurrent Scan
      // coordinators/serves do not stall while this partition walks keys.
      if (progress != nullptr) (*progress)();

      // Unlimited tree scans already covered [low, high]; finite scans resume
      // past the last raw key when filters discarded candidates or the batch
      // was capped by fetch.
      if (unlimited) break;
      if (!unlimited && items->size() >= limit) break;
      if (private_rows.size() < fetch && shared_rows.size() < fetch) break;
      if (!have_last_raw) break;
      low = last_raw;
      left_inclusive = false;
    }
    const uint64_t in_after = directory_.migration_in_seq.load(std::memory_order_acquire);
    const uint64_t out_after = directory_.migration_out_seq.load(std::memory_order_acquire);
    if (in_before != in_after || out_before != out_after) continue;
    return true;
  }
  return false;
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
    SharedIndexValue indexed{};
    if (smeta_offset == kNullOffset || !shared_tree_->lookup(fixed_key, indexed) ||
        indexed.smeta_offset != smeta_offset) {
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
  return true;
}

void KVPartition::NoteSharedAccess(star::TwoPLPashaMetadataShared *smeta) const {
  if (smeta == nullptr) return;
  auto *payload = smeta->get_scc_data();
  if (star::migration_manager != nullptr)
    star::migration_manager->access_row(&payload->migration_policy_meta, partition_id_);
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
        regions_.hwcc().FromOffset(entry.second.smeta_offset));
    auto *payload = smeta->get_scc_data();
    std::tuple<std::atomic<uint64_t> *, void *> row{nullptr, nullptr};
    clock->track_already_migrated(table, entry.first.bytes, row,
                                  &payload->migration_policy_meta);
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
  uint64_t total = 0;
  for (size_t i = 0; i < static_cast<size_t>(AllocationDomain::kOwnerPrivateSwcc); ++i)
    total += regions_.layout().domains[i].used_bytes.load(std::memory_order_relaxed);
  return total;
}

uint64_t KVPartition::migrated_key_count() const {
  FixedKey low{};
  FixedKey high{};
  std::memset(high.bytes, 0xff, sizeof(high.bytes));
  std::vector<SharedTree::KeyValuePair> entries;
  shared_tree_->scan(low, high, true, true, 0, entries);
  return entries.size();
}


bool KVPartition::ScanSharedOnly(
    std::string_view start_key, uint64_t limit,
    std::vector<std::pair<std::string, std::string>> *items,
    uint32_t host_id) const {
  EnterEbr();
  if (items == nullptr) throw std::invalid_argument("null shared scan output");
  items->clear();
  FixedKey high{};
  std::memset(high.bytes, 0xff, sizeof(high.bytes));
  FixedKey low = MakeKey(start_key);
  bool left_inclusive = true;
  const bool unlimited = limit == 0;
  for (uint32_t batches = 0; batches < 1048576u; ++batches) {
    if (!unlimited && items->size() >= limit) break;
    const uint64_t remaining =
        unlimited ? 0 : static_cast<uint64_t>(limit - items->size());
    const uint32_t fetch =
        unlimited ? 0
                  : (remaining > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())
                         ? 0
                         : static_cast<uint32_t>(remaining));
    std::vector<SharedTree::KeyValuePair> shared_rows;
    shared_tree_->scan(low, high, left_inclusive, true, fetch, shared_rows);
    if (shared_rows.empty()) break;
    FixedKey last_raw{};
    bool have_last = false;
    for (const auto &entry : shared_rows) {
      if (!unlimited && items->size() >= limit) break;
      last_raw = entry.first;
      have_last = true;
      if (entry.second.smeta_offset == kNullOffset) continue;
      auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
          regions_.hwcc().FromOffset(entry.second.smeta_offset));
      auto *payload = smeta->get_scc_data();
      std::string value(entry.second.value_len, '\0');
      mem_access::SharedPayloadRead(payload->data, value.size());
      if (!star::TwoPLPashaHelper::kv_shared_read(smeta, host_id, value.data(),
                                                  value.size()))
        continue;
      NoteSharedAccess(smeta);
      items->emplace_back(KeyString(entry.first), std::move(value));
    }
    if (unlimited) break;
    if (!unlimited && items->size() >= limit) break;
    if (shared_rows.size() < fetch) break;
    if (!have_last) break;
    low = last_raw;
    left_inclusive = false;
  }
  return true;
}

void KVPartition::PersistRoots() {
  directory_.private_root = regions_.swcc().ToOffset(
      private_tree_->root_for_persistence());
  directory_.shared_root = regions_.hwcc().ToOffset(
      shared_tree_->root_for_persistence());
}

}  // namespace tigonkv::engine
