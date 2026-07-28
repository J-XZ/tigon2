#include "kv/engine/kv_partition.h"
#include "kv/engine/fixed_value.h"
#include "kv/engine/kv_migration.h"
#include "kv/engine/mem_access.h"
#include "protocol/Pasha/PolicyClock.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace tigonkv::engine {
namespace {

constexpr size_t kPrivateMetadataStateOffset =
    offsetof(PrivateMetadataLocal, is_valid);
constexpr size_t kPrivateMetadataStateBytes =
    sizeof(PrivateMetadataLocal) - kPrivateMetadataStateOffset;

void RecordPrivateMetadataRead(const PrivateMetadataLocal *metadata) {
  mem_access::PrivateRead(
      reinterpret_cast<const char *>(metadata) + kPrivateMetadataStateOffset,
      kPrivateMetadataStateBytes);
}

void RecordPrivateMetadataWrite(const PrivateMetadataLocal *metadata) {
  mem_access::PrivateWrite(
      reinterpret_cast<const char *>(metadata) + kPrivateMetadataStateOffset,
      kPrivateMetadataStateBytes);
}

std::string PadFixedValue(std::string_view value, uint32_t fixed_value_size) {
  std::string fixed(fixed_value_size, '\0');
  std::memcpy(fixed.data(), value.data(), value.size());
  return fixed;
}

uint32_t ReadHwccConfigField(const uint32_t *field) {
  mem_access::HwccRead(field, sizeof(*field));
  return *field;
}

bool IsInternalMaxSentinel(const FixedKey &key) {
  return std::all_of(key.bytes, key.bytes + sizeof(key.bytes),
                     [](char byte) {
                       return static_cast<unsigned char>(byte) == 0xff;
                     });
}

}  // namespace

KVPartition::KVPartition(DualRegionAllocator &regions, star::CXL_EBR &ebr,
                         uint32_t partition_id, uint32_t owner_shard,
                         bool attach, bool materialize_private)
    : regions_(regions), ebr_(ebr), partition_id_(partition_id),
      owner_shard_(owner_shard),
      fixed_key_size_(ReadHwccConfigField(&regions.layout().fixed_key_size)),
      fixed_value_size_(ReadHwccConfigField(&regions.layout().fixed_value_size)),
      directory_(regions.layout().partitions.at(partition_id)),
      private_arena_(*static_cast<OwnerPrivateArenaHeader *>(
          regions.swcc().FromOffset(regions.OwnerPrivateArenaOffset(partition_id)))),
      private_binding_{&regions, AllocationDomain::kOwnerPrivateSwcc, owner_shard, &ebr,
                       partition_id},
      shared_binding_{&regions, AllocationDomain::kHwccIndex, owner_shard, &ebr} {
  // Process-level CXLMemory owner binding is set once in KVEngine::Open to
  // config.node_id. Do not rebind per partition (§11.3).
  if (partition_id >=
      ReadHwccConfigField(&regions.layout().partition_count))
    throw std::invalid_argument("partition id outside persistent layout");
  if (attach) {
    RegionOffset private_root = kNullOffset;
    if (materialize_private) {
      mem_access::PrivateRead(&private_arena_.private_root,
                              sizeof(private_arena_.private_root));
      private_root = private_arena_.private_root;
    }
    mem_access::HwccAtomicLoad(&directory_.shared_root);
    if ((materialize_private && private_root == kNullOffset) ||
        directory_.shared_root.load(std::memory_order_acquire) == kNullOffset)
      throw std::runtime_error("partition attach missing tree root");
    if (materialize_private) {
      private_tree_ = new PrivateTree(
          private_binding_, regions_.swcc().FromOffset(private_root));
      persisted_private_root_offset_ = private_root;
    }
    mem_access::HwccAtomicLoad(&directory_.shared_root);
    shared_tree_ = new SharedTree(
        shared_binding_,
        regions_.hwcc().FromOffset(
            directory_.shared_root.load(std::memory_order_acquire)));
    shared_tree_->bind_published_root(&directory_.shared_root);
  } else {
    if (!materialize_private)
      throw std::logic_error("reset must materialize every private partition root");
    private_tree_ = new PrivateTree(private_binding_);
    // Preserve the original per-partition maximum-key tuple. It is a normal
    // private ValueStruct/local-metadata row and supplies the mandatory right
    // neighbour for TableBTreeOLC adjacent callbacks.
    FixedKey sentinel{};
    std::memset(sentinel.bytes, 0xff, fixed_key_size_);
    const std::string zero_value(fixed_value_size_, '\0');
    auto *sentinel_value = AllocateValue(zero_value);
    // This is the one bootstrap insertion before a right neighbour exists.
    // Every later logical insert uses the original adjacent callback and sees
    // this tuple as its mandatory next key.
    if (!private_tree_->insert(
            sentinel, PrivateTreeValue{regions_.swcc().ToOffset(sentinel_value)})) {
      FreeUnpublishedPrivateValue(sentinel_value);
      throw std::runtime_error("private tree internal max sentinel duplicate");
    }
    shared_tree_ = new SharedTree(shared_binding_);
    shared_tree_->bind_published_root(&directory_.shared_root);
    PersistPrivateRootIfChanged();
  }
  pthread_spin_init(&clock_lock_, PTHREAD_PROCESS_PRIVATE);
  clock_lock_inited_ = true;
  // Clock tracker rebuild runs after KvMigrationRuntime::Install so the
  // process-local PolicyClock exists (PLAN §4.5 attach rebuild).
}

KVPartition::~KVPartition() {
  delete private_tree_;
  delete shared_tree_;
  if (clock_lock_inited_) pthread_spin_destroy(&clock_lock_);
}

FixedKey KVPartition::MakeKey(std::string_view key) const {
  return FixedKey::From(key, fixed_key_size_);
}

bool KVPartition::LookupPrivateOffset(const FixedKey &key,
                                      RegionOffset *offset) const {
  if (offset == nullptr) throw std::invalid_argument("null private offset");
  PrivateTreeValue value;
  if (!private_tree_->lookup(key, value)) return false;
  *offset = value.row;
  return value.row != kNullOffset;
}

bool KVPartition::LookupSharedOffset(const FixedKey &key,
                                     RegionOffset *offset) const {
  if (offset == nullptr) throw std::invalid_argument("null shared offset");
  SharedTreeValue value;
  if (!shared_tree_->lookup(key, value)) return false;
  if (!value.is_valid.load(std::memory_order_acquire)) return false;
  *offset = value.row;
  return value.row != kNullOffset;
}

PrivateValueStruct *KVPartition::ValueFromOffset(RegionOffset offset) const {
  if (offset == kNullOffset) return nullptr;
  return static_cast<PrivateValueStruct *>(regions_.swcc().FromOffset(offset));
}

PrivateMetadataLocal *KVPartition::MetadataFromValue(
    PrivateValueStruct *value) const {
  if (value == nullptr) return nullptr;
  mem_access::PrivateAtomicLoad(&value->meta);
  const RegionOffset offset = value->meta.load(std::memory_order_acquire);
  if (offset == kNullOffset)
    throw std::runtime_error("private ValueStruct has no local metadata");
  return static_cast<PrivateMetadataLocal *>(regions_.swcc().FromOffset(offset));
}

PrivateClockTrackerNode *KVPartition::ClockNodeFromOffset(
    RegionOffset offset) const {
  if (offset == kNullOffset) return nullptr;
  return static_cast<PrivateClockTrackerNode *>(
      regions_.swcc().FromOffset(offset));
}

void KVPartition::LockRow(PrivateMetadataLocal *metadata) {
  metadata->lock();
  RecordPrivateMetadataRead(metadata);
}

void KVPartition::UnlockRow(PrivateMetadataLocal *metadata) {
  metadata->unlock();
}

void KVPartition::UnlockAdjacentRows(AdjacentRows *rows) {
  if (rows == nullptr) return;
  if (rows->has_next) UnlockRow(rows->next.metadata);
  if (rows->has_current) UnlockRow(rows->current.metadata);
  if (rows->has_prev) UnlockRow(rows->prev.metadata);
  *rows = {};
}

void KVPartition::SetNextReal(const RowRef &row, bool real) {
  if (row.value == nullptr || !row.metadata->is_valid ||
      !row.metadata->is_migrated ||
      row.metadata->migrated_smeta_off == kNullOffset)
    return;
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(row.metadata->migrated_smeta_off));
  smeta->lock();
  if (real)
    smeta->set_next_key_real_bit();
  else
    smeta->clear_next_key_real_bit();
  smeta->unlock();
}

void KVPartition::SetPrevReal(const RowRef &row, bool real) {
  if (row.value == nullptr || !row.metadata->is_valid ||
      !row.metadata->is_migrated ||
      row.metadata->migrated_smeta_off == kNullOffset)
    return;
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(row.metadata->migrated_smeta_off));
  smeta->lock();
  if (real)
    smeta->set_prev_key_real_bit();
  else
    smeta->clear_prev_key_real_bit();
  smeta->unlock();
}

void KVPartition::ApplySharedAdjacency(const AdjacentRows &neighborhood) {
  const bool prev_migrated =
      neighborhood.has_prev && neighborhood.prev.metadata->is_valid &&
      neighborhood.prev.metadata->is_migrated;
  const bool current_migrated =
      neighborhood.has_current && neighborhood.current.metadata->is_valid &&
      neighborhood.current.metadata->is_migrated;
  const bool next_migrated =
      neighborhood.has_next && neighborhood.next.metadata->is_valid &&
      neighborhood.next.metadata->is_migrated;
  if (neighborhood.has_current) {
    if (neighborhood.has_prev)
      SetNextReal(neighborhood.prev, current_migrated);
    SetPrevReal(neighborhood.current, prev_migrated);
    SetNextReal(neighborhood.current, next_migrated);
    if (neighborhood.has_next)
      SetPrevReal(neighborhood.next, current_migrated);
  } else {
    if (neighborhood.has_prev)
      SetNextReal(neighborhood.prev, next_migrated);
    if (neighborhood.has_next)
      SetPrevReal(neighborhood.next, prev_migrated);
  }
}

void KVPartition::ClearSharedAdjacency(const AdjacentRows &neighborhood) {
  if (neighborhood.has_prev) SetNextReal(neighborhood.prev, false);
  if (neighborhood.has_current) {
    SetPrevReal(neighborhood.current, false);
    SetNextReal(neighborhood.current, false);
  }
  if (neighborhood.has_next) SetPrevReal(neighborhood.next, false);
}

bool KVPartition::InsertPrivateValue(const FixedKey &key, PrivateValueStruct *value) {
  auto *metadata = MetadataFromValue(value);
  const PrivateTreeValue offset{regions_.swcc().ToOffset(value)};
  // Direct mechanical use of the original B+Tree adjacent-insert callback.
  // The permanent maximum-key tuple supplies a right neighbour at tree EOF.
  return private_tree_->insert_and_process_adjacent_tuples(
      key, offset,
      [&](const FixedKey *prev_key, PrivateTreeValue *prev_off,
          const FixedKey *next_key, PrivateTreeValue *next_off) {
        (void)metadata;  // fully initialized before publication in the leaf.
        RowRef prev;
        RowRef next;
        if (prev_key != nullptr && prev_off != nullptr) {
          prev = {*prev_key, prev_off->row, ValueFromOffset(prev_off->row), nullptr};
          prev.metadata = MetadataFromValue(prev.value);
        }
        if (next_key != nullptr && next_off != nullptr) {
          next = {*next_key, next_off->row, ValueFromOffset(next_off->row), nullptr};
          next.metadata = MetadataFromValue(next.value);
        }
        if (prev.metadata != nullptr) LockRow(prev.metadata);
        if (next.metadata != nullptr) LockRow(next.metadata);
        if (prev.metadata != nullptr) SetNextReal(prev, false);
        if (next.metadata != nullptr) SetPrevReal(next, false);
        if (next.metadata != nullptr) UnlockRow(next.metadata);
        if (prev.metadata != nullptr) UnlockRow(prev.metadata);
        return true;
      });
}

void KVPartition::FreeUnpublishedPrivateValue(PrivateValueStruct *value) {
  if (value == nullptr) return;
  auto *metadata = MetadataFromValue(value);
  const uint64_t value_bytes = sizeof(PrivateValueStruct) + fixed_value_size_;
  const uint64_t metadata_bytes = sizeof(PrivateMetadataLocal);
  regions_.FreeOwnerPrivate(value, value_bytes, partition_id_, owner_shard_);
  regions_.FreeOwnerPrivate(metadata, metadata_bytes, partition_id_, owner_shard_);
}

PrivateMetadataLocal *KVPartition::AllocateMetadata() {
  return new (regions_.AllocateOwnerPrivate(sizeof(PrivateMetadataLocal),
                                            partition_id_, owner_shard_))
      PrivateMetadataLocal;
}

PrivateValueStruct *KVPartition::AllocateValue(std::string_view value) {
  const uint64_t value_bytes = sizeof(PrivateValueStruct) + fixed_value_size_;
  auto *metadata = AllocateMetadata();
  auto *private_value = new (
      regions_.AllocateOwnerPrivate(value_bytes, partition_id_, owner_shard_))
      PrivateValueStruct;
  mem_access::PrivateAtomicStore(&private_value->meta);
  private_value->meta.store(regions_.swcc().ToOffset(metadata),
                            std::memory_order_release);
  std::memset(private_value->data, 0, fixed_value_size_);
  std::memcpy(private_value->data, value.data(), value.size());
  mem_access::PrivateWrite(private_value->data, fixed_value_size_);
  metadata->is_valid = true;
  metadata->tid = star::TwoPLPashaHelper::kv_next_commit_tid(metadata->tid);
  RecordPrivateMetadataWrite(metadata);
  return private_value;
}

bool KVPartition::PutPrivate(std::string_view key, std::string_view value) {
  EnterEbr();
  if (value.size() > fixed_value_size_)
    throw std::invalid_argument("private value exceeds fixed value size");
  std::string padded_value;
  if (value.size() != fixed_value_size_) {
    padded_value = PadFixedValue(value, fixed_value_size_);
    value = padded_value;
  }
  // Iterative upsert: create-race losers free and retry without recursion.
  for (int attempt = 0;; ++attempt) {
    if (attempt > 1024)
      throw std::runtime_error("private put create-race busy");
    const FixedKey fixed_key = MakeKey(key);
    RegionOffset row_offset = kNullOffset;
    if (LookupPrivateOffset(fixed_key, &row_offset)) {
      auto *private_value = ValueFromOffset(row_offset);
      auto *metadata = MetadataFromValue(private_value);
      LockRow(metadata);
      if (!metadata->is_valid) {
        UnlockRow(metadata);
        // Concurrent delete/EBR: do not report Ok without a published write.
        throw std::runtime_error("tombstone put busy");
      }
      if (metadata->is_migrated) {
        const RegionOffset smeta_offset = metadata->migrated_smeta_off;
        if (smeta_offset == kNullOffset ||
            !regions_.IsHwccAddress(regions_.hwcc().FromOffset(smeta_offset))) {
          UnlockRow(metadata);
          throw std::runtime_error("migrated row locator busy");
        }
        auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
            regions_.hwcc().FromOffset(smeta_offset));
        // Follow the owner-private metadata offset under latch; one helper
        // attempt (§10.2).
        const bool written = star::TwoPLPashaHelper::kv_shared_write(
            smeta, owner_shard_, value.data(), fixed_value_size_);
        if (written) {
          metadata->is_data_modified_since_moved_out = true;
          RecordPrivateMetadataWrite(metadata);
          NoteSharedAccess(smeta);
          UnlockRow(metadata);
          return false;
        }
        UnlockRow(metadata);
        throw std::runtime_error("migrated row shared write busy");
      }
      std::memset(private_value->data, 0, fixed_value_size_);
      std::memcpy(private_value->data, value.data(), value.size());
      mem_access::PrivateWrite(private_value->data, fixed_value_size_);
      metadata->is_data_modified_since_moved_out = true;
      metadata->tid = star::TwoPLPashaHelper::kv_next_commit_tid(metadata->tid);
      RecordPrivateMetadataWrite(metadata);
      UnlockRow(metadata);
      return false;
    }
    auto *private_value = AllocateValue(value);
    if (!InsertPrivateValue(fixed_key, private_value)) {
      // Concurrent create race: another owner worker published first. Free the
      // unpublished loser and retry as an upsert (§10.9).
      FreeUnpublishedPrivateValue(private_value);
      continue;
    }
    PersistPrivateRootIfChanged();
    return true;
  }
}

StatusCode KVPartition::InsertRemotePlaceholder(std::string_view key,
                                                std::string_view value,
                                                uint32_t requester_id) {
  EnterEbr();
  if (value.size() > fixed_value_size_)
    throw std::invalid_argument("remote insert value exceeds fixed value size");
  std::string padded_value;
  if (value.size() != fixed_value_size_) {
    padded_value = PadFixedValue(value, fixed_value_size_);
    value = padded_value;
  }
  const FixedKey fixed_key = MakeKey(key);
  auto *table = KvMigrationRuntime::Instance().TableFor(partition_id_);
  if (table == nullptr) return StatusCode::kCorruption;

  // This is the require_lock_next_key=false branch of the original
  // insert_and_update_next_key_info.  The ITable callback retains the exact
  // adjacent leaves while these migrated-neighbour bits are cleared.
  const bool inserted = table->insert_and_process_adjacent_tuples(
      &fixed_key, value.data(),
      [&](const void *, std::atomic<uint64_t> *prev_meta, void *, const void *,
          std::atomic<uint64_t> *next_meta, void *) {
        const auto clear_adjacent = [&](std::atomic<uint64_t> *slot,
                                        bool clear_next) {
          if (slot == nullptr) return true;
          // insert_and_process_adjacent_tuples follows the original ITable
          // contract and supplies ValueStruct::meta, unlike the resolved
          // local-metadata arguments of search_and_update_next_key_info.
          auto *value = reinterpret_cast<PrivateValueStruct *>(slot);
          auto *metadata = MetadataFromValue(value);
          LockRow(metadata);
          bool ok = true;
          if (metadata->is_migrated) {
            if (metadata->migrated_smeta_off == kNullOffset) {
              ok = false;
            } else {
              auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
                  regions_.hwcc().FromOffset(metadata->migrated_smeta_off));
              smeta->lock();
              if (clear_next)
                smeta->clear_next_key_real_bit();
              else
                smeta->clear_prev_key_real_bit();
              smeta->unlock();
            }
          }
          UnlockRow(metadata);
          return ok;
        };
        return clear_adjacent(prev_meta, true) &&
               clear_adjacent(next_meta, false);
      },
      true);
  if (!inserted) return StatusCode::kBusy;

  // The original remote-insert owner moves the invalid placeholder in with a
  // requester ref.  That ref spans this response and is consumed only by the
  // requester's remote_modify_tuple_valid_bit analogue below.
  star::TwoPLPashaMetadataShared *pinned = nullptr;
  if (PromotePrivate(key, requester_id, &pinned)) return StatusCode::kOk;

  // move_row_in failed before a remote-visible placeholder was acknowledged.
  // Reuse the normal adjacent delete callback for rollback; it expects a
  // valid local row, so make the never-published placeholder locally visible
  // only for that synchronous cleanup.
  RegionOffset row_offset = kNullOffset;
  if (LookupPrivateOffset(fixed_key, &row_offset)) {
    auto *metadata = MetadataFromValue(ValueFromOffset(row_offset));
    LockRow(metadata);
    metadata->is_valid = true;
    RecordPrivateMetadataWrite(metadata);
    UnlockRow(metadata);
    (void)DeletePrivate(key);
  }
  return StatusCode::kBusy;
}

bool KVPartition::PublishRemotePlaceholder(std::string_view key,
                                           uint32_t requester_id) {
  EnterEbr();
  RegionOffset smeta_offset = kNullOffset;
  if (!LookupSharedOffset(MakeKey(key), &smeta_offset) ||
      smeta_offset == kNullOffset)
    return false;
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(smeta_offset));
  return star::TwoPLPashaHelper::kv_remote_publish_insert(smeta, requester_id);
}

bool KVPartition::GetPrivate(std::string_view key, std::string *value) const {
  EnterEbr();
  RegionOffset row_offset = kNullOffset;
  if (!LookupPrivateOffset(MakeKey(key), &row_offset)) return false;
  auto *private_value = ValueFromOffset(row_offset);
  auto *metadata = MetadataFromValue(private_value);
  // The non-migrated branch is the original take_read_lock_and_read / release
  // sequence with ValueStruct::meta resolved from an owner-private offset.
  std::string local(fixed_value_size_, '\0');
  bool local_success = false;
  bool migrated = false;
  RecordPrivateMetadataRead(metadata);
  star::TwoPLPashaHelper::kv_take_private_read_lock_and_read(
      *metadata, private_value->data, local.data(), local.size(), local_success,
      &migrated);
  if (local_success) {
    mem_access::PrivateRead(private_value->data, fixed_value_size_);
    star::TwoPLPashaHelper::kv_private_read_lock_release(*metadata);
    RecordPrivateMetadataWrite(metadata);
    *value = std::move(local);
    return true;
  }
  if (!migrated) return false;
  LockRow(metadata);
  // REMOTE_INSERT initially leaves the owner placeholder invalid.  Once the
  // requester publishes its pinned shared row, the owner observes that shared
  // authority here and restores the local validity view, matching the
  // original migrated-read branch.
  if (!metadata->is_migrated) {
    UnlockRow(metadata);
    return false;
  }
  const RegionOffset smeta_offset = metadata->migrated_smeta_off;
  if (smeta_offset == kNullOffset ||
      !regions_.IsHwccAddress(regions_.hwcc().FromOffset(smeta_offset))) {
    UnlockRow(metadata);
    return false;
  }
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(smeta_offset));
  std::string shared(fixed_value_size_, '\0');
  const bool read = star::TwoPLPashaHelper::kv_shared_read_value(
      smeta, owner_shard_, shared.data(), shared.size());
  if (read) {
    if (!metadata->is_valid) {
      metadata->is_valid = true;
      RecordPrivateMetadataWrite(metadata);
    }
    NoteSharedAccess(smeta);
    UnlockRow(metadata);
    *value = std::move(shared);
    return true;
  }
  UnlockRow(metadata);
  // Contention under latch: surface Busy rather than NotFound (§10.1/§10.2).
  throw std::runtime_error("migrated row shared read busy");
}

SharedAccessState KVPartition::GetShared(std::string_view key, uint32_t host_id,
                                           std::string *value) const {
  EnterEbr();
  if (value == nullptr) throw std::invalid_argument("null shared GET output");
  const FixedKey fixed_key = MakeKey(key);
  star::TwoPLPashaMetadataShared *smeta = nullptr;
  RegionOffset smeta_offset = kNullOffset;
  const SharedAccessState pin = TryPinShared(fixed_key, &smeta, &smeta_offset);
  if (pin != SharedAccessState::kDone) return pin;
  std::string shared(fixed_value_size_, '\0');
  star::TwoPLPashaHelper::KvSharedResult read_result =
      star::TwoPLPashaHelper::KvSharedResult::kBusy;
  const bool read = star::TwoPLPashaHelper::kv_shared_read_value(
      smeta, host_id, shared.data(), shared.size(), false, &read_result);
  if (read) {
    NoteSharedAccess(smeta);
  }
  star::TwoPLPashaHelper::kv_unpin_shared_ref(smeta);
  if (!read) {
    if (read_result == star::TwoPLPashaHelper::KvSharedResult::kMissing)
      return SharedAccessState::kMissing;
    return SharedAccessState::kRetry;
  }
  *value = std::move(shared);
  return SharedAccessState::kDone;
}

SharedAccessState KVPartition::PutShared(std::string_view key, uint32_t host_id,
                                         std::string_view value) {
  EnterEbr();
  if (value.size() > fixed_value_size_)
    throw std::invalid_argument("shared value exceeds fixed value size");
  std::string padded_value;
  if (value.size() != fixed_value_size_) {
    padded_value = PadFixedValue(value, fixed_value_size_);
    value = padded_value;
  }
  const FixedKey fixed_key = MakeKey(key);
  star::TwoPLPashaMetadataShared *smeta = nullptr;
  RegionOffset smeta_offset = kNullOffset;
  const SharedAccessState pin = TryPinShared(fixed_key, &smeta, &smeta_offset);
  if (pin != SharedAccessState::kDone) return pin;
  const bool written = star::TwoPLPashaHelper::kv_shared_write(
      smeta, host_id, value.data(), fixed_value_size_);
  if (written) NoteSharedAccess(smeta);
  star::TwoPLPashaHelper::kv_unpin_shared_ref(smeta);
  return written ? SharedAccessState::kDone : SharedAccessState::kRetry;
}

SharedAccessState KVPartition::CompareExchangeShared(
    std::string_view key, uint32_t host_id, std::string_view expected,
    std::string_view desired, bool *exchanged) {
  EnterEbr();
  if (exchanged == nullptr) throw std::invalid_argument("null shared CAS result");
  if (desired.size() > fixed_value_size_)
    throw std::invalid_argument("shared CAS desired value exceeds fixed value size");
  std::string padded_expected;
  if (!expected.empty() && expected.size() != fixed_value_size_) {
    padded_expected = PadFixedValue(expected, fixed_value_size_);
    expected = padded_expected;
  }
  std::string padded_desired;
  if (desired.size() != fixed_value_size_) {
    padded_desired = PadFixedValue(desired, fixed_value_size_);
    desired = padded_desired;
  }
  *exchanged = false;
  const FixedKey fixed_key = MakeKey(key);
  star::TwoPLPashaMetadataShared *smeta = nullptr;
  RegionOffset smeta_offset = kNullOffset;
  const SharedAccessState pin = TryPinShared(fixed_key, &smeta, &smeta_offset);
  if (pin != SharedAccessState::kDone) return pin;
  bool changed = false;
  const bool updated = star::TwoPLPashaHelper::kv_shared_update(
      smeta, host_id, fixed_value_size_,
      [&](const std::string &current, std::string *replacement) {
        if (current != expected) return false;
        replacement->assign(desired);
        return true;
      },
      &changed);
  if (!updated) {
    star::TwoPLPashaHelper::kv_unpin_shared_ref(smeta);
    return SharedAccessState::kRetry;
  }
  *exchanged = changed;
  NoteSharedAccess(smeta);
  star::TwoPLPashaHelper::kv_unpin_shared_ref(smeta);
  return SharedAccessState::kDone;
}

SharedAccessState KVPartition::IncrementShared(std::string_view key,
                                               uint32_t host_id, int64_t delta,
                                               int64_t *value) {
  EnterEbr();
  if (value == nullptr) throw std::invalid_argument("null shared increment output");
  const FixedKey fixed_key = MakeKey(key);
  star::TwoPLPashaMetadataShared *smeta = nullptr;
  RegionOffset smeta_offset = kNullOffset;
  const SharedAccessState pin = TryPinShared(fixed_key, &smeta, &smeta_offset);
  if (pin != SharedAccessState::kDone) return pin;
  int64_t next = 0;
  bool changed = false;
  bool invalid = false;
  const bool updated = star::TwoPLPashaHelper::kv_shared_update(
      smeta, host_id, fixed_value_size_,
      [&](const std::string &current, std::string *replacement) {
        int64_t previous = 0;
        if (!DecodeCanonicalFixedDecimal(current, &previous) ||
            (delta > 0 && previous > std::numeric_limits<int64_t>::max() - delta) ||
            (delta < 0 && previous < std::numeric_limits<int64_t>::min() - delta)) {
          invalid = true;
          return false;
        }
        next = previous + delta;
        if (!EncodeCanonicalFixedDecimal(next, fixed_value_size_, replacement)) {
          invalid = true;
          return false;
        }
        return true;
      },
      &changed);
  if (invalid) {
    star::TwoPLPashaHelper::kv_unpin_shared_ref(smeta);
    throw std::invalid_argument(
        "increment requires a non-overflowing int64 value");
  }
  if (!updated) {
    star::TwoPLPashaHelper::kv_unpin_shared_ref(smeta);
    return SharedAccessState::kRetry;
  }
  DCHECK(changed);
  *value = next;
  NoteSharedAccess(smeta);
  star::TwoPLPashaHelper::kv_unpin_shared_ref(smeta);
  return SharedAccessState::kDone;
}

bool KVPartition::CompareExchangePrivate(std::string_view key,
                                         std::string_view expected,
                                         std::string_view desired,
                                         bool *exchanged,
                                         bool *inserted) {
  EnterEbr();
  if (exchanged == nullptr) throw std::invalid_argument("null CAS result");
  if (desired.size() > fixed_value_size_)
    throw std::invalid_argument("CAS desired value exceeds fixed value size");
  std::string padded_expected;
  if (!expected.empty() && expected.size() != fixed_value_size_) {
    padded_expected = PadFixedValue(expected, fixed_value_size_);
    expected = padded_expected;
  }
  std::string padded_desired;
  if (desired.size() != fixed_value_size_) {
    padded_desired = PadFixedValue(desired, fixed_value_size_);
    desired = padded_desired;
  }
  *exchanged = false;
  if (inserted != nullptr) *inserted = false;
  RegionOffset row_offset = kNullOffset;
  const FixedKey fixed_key = MakeKey(key);
  if (!LookupPrivateOffset(fixed_key, &row_offset)) {
    if (!expected.empty()) return false;
    {
      auto *private_value = AllocateValue(desired);
      if (InsertPrivateValue(fixed_key, private_value)) {
        PersistPrivateRootIfChanged();
        *exchanged = true;
        if (inserted != nullptr) *inserted = true;
        return true;
      }
      FreeUnpublishedPrivateValue(private_value);
    }
    // Loser of create race: re-resolve the winner and compare expected.
    if (!LookupPrivateOffset(fixed_key, &row_offset)) return false;
  }
  auto *private_value = ValueFromOffset(row_offset);
  auto *metadata = MetadataFromValue(private_value);
  LockRow(metadata);
  if (!metadata->is_valid) {
    UnlockRow(metadata);
    return false;
  }
  if (!metadata->is_migrated) {
    const std::string_view current(private_value->data, fixed_value_size_);
    mem_access::PrivateRead(private_value->data, fixed_value_size_);
    if (current == expected) {
      std::memcpy(private_value->data, desired.data(), desired.size());
      mem_access::PrivateWrite(private_value->data, fixed_value_size_);
      metadata->is_data_modified_since_moved_out = true;
      metadata->tid = star::TwoPLPashaHelper::kv_next_commit_tid(metadata->tid);
      RecordPrivateMetadataWrite(metadata);
      *exchanged = true;
    }
    UnlockRow(metadata);
    return true;
  }
  const RegionOffset smeta_offset = metadata->migrated_smeta_off;
  if (smeta_offset == kNullOffset ||
      !regions_.IsHwccAddress(regions_.hwcc().FromOffset(smeta_offset))) {
    UnlockRow(metadata);
    return false;
  }
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(smeta_offset));
  bool changed = false;
  const bool read = star::TwoPLPashaHelper::kv_shared_update(
      smeta, owner_shard_, fixed_value_size_,
      [&](const std::string &current, std::string *replacement) {
        if (current != expected) return false;
        replacement->assign(desired);
        return true;
      },
      &changed);
  if (!read) {
    UnlockRow(metadata);
    throw std::runtime_error("migrated row shared CAS busy");
  }
  if (changed) {
    metadata->is_data_modified_since_moved_out = true;
    RecordPrivateMetadataWrite(metadata);
    NoteSharedAccess(smeta);
    *exchanged = true;
  }
  UnlockRow(metadata);
  return true;
}

bool KVPartition::IncrementPrivate(std::string_view key, int64_t delta,
                                   int64_t *value, bool *inserted) {
  EnterEbr();
  if (value == nullptr) throw std::invalid_argument("null increment result");
  if (inserted != nullptr) *inserted = false;
  RegionOffset row_offset = kNullOffset;
  const FixedKey fixed_key = MakeKey(key);
  if (!LookupPrivateOffset(fixed_key, &row_offset)) {
    std::string encoded;
    if (!EncodeCanonicalFixedDecimal(delta, fixed_value_size_, &encoded))
      throw std::invalid_argument("increment value exceeds fixed value size");
    {
      auto *private_value = AllocateValue(encoded);
      if (InsertPrivateValue(fixed_key, private_value)) {
        PersistPrivateRootIfChanged();
        *value = delta;
        if (inserted != nullptr) *inserted = true;
        return true;
      }
      FreeUnpublishedPrivateValue(private_value);
    }
    // Loser of create race: apply delta on the published row (§10.9).
    if (!LookupPrivateOffset(fixed_key, &row_offset)) return false;
  }
  auto *private_value = ValueFromOffset(row_offset);
  auto *metadata = MetadataFromValue(private_value);
  LockRow(metadata);
  if (!metadata->is_valid) {
    UnlockRow(metadata);
    return false;
  }
  std::string current;
  star::TwoPLPashaMetadataShared *smeta = nullptr;
  if (!metadata->is_migrated) {
    current.assign(private_value->data, fixed_value_size_);
    mem_access::PrivateRead(private_value->data, fixed_value_size_);
  } else {
    const RegionOffset smeta_offset = metadata->migrated_smeta_off;
    if (smeta_offset == kNullOffset ||
        !regions_.IsHwccAddress(regions_.hwcc().FromOffset(smeta_offset))) {
      UnlockRow(metadata);
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
            smeta, owner_shard_, fixed_value_size_,
            [&](const std::string &value_before, std::string *replacement) {
              int64_t previous = 0;
              if (!DecodeCanonicalFixedDecimal(value_before, &previous) ||
                  (delta > 0 &&
                   previous > std::numeric_limits<int64_t>::max() - delta) ||
                  (delta < 0 &&
                   previous < std::numeric_limits<int64_t>::min() - delta))
                throw std::invalid_argument(
                    "increment requires a non-overflowing int64 value");
              next = previous + delta;
              if (!EncodeCanonicalFixedDecimal(next, fixed_value_size_, replacement))
                throw std::invalid_argument(
                    "increment value exceeds fixed value size");
              return true;
            },
            &changed)) {
      UnlockRow(metadata);
      throw std::runtime_error("migrated increment shared write rejected");
    }
    DCHECK(changed);
    std::string encoded;
    if (!EncodeCanonicalFixedDecimal(next, fixed_value_size_, &encoded)) {
      UnlockRow(metadata);
      throw std::invalid_argument("increment value exceeds fixed value size");
    }
    metadata->is_data_modified_since_moved_out = true;
    RecordPrivateMetadataWrite(metadata);
    NoteSharedAccess(smeta);
    *value = next;
    UnlockRow(metadata);
    return true;
  } else {
    int64_t previous = 0;
    if (!DecodeCanonicalFixedDecimal(current, &previous) ||
        (delta > 0 && previous > std::numeric_limits<int64_t>::max() - delta) ||
        (delta < 0 && previous < std::numeric_limits<int64_t>::min() - delta)) {
      UnlockRow(metadata);
      throw std::invalid_argument(
          "increment requires a non-overflowing int64 value");
    }
    const int64_t next = previous + delta;
    std::string encoded;
    if (!EncodeCanonicalFixedDecimal(next, fixed_value_size_, &encoded)) {
      UnlockRow(metadata);
      throw std::invalid_argument("increment value exceeds fixed value size");
    }
    std::memcpy(private_value->data, encoded.data(), encoded.size());
    mem_access::PrivateWrite(private_value->data, fixed_value_size_);
    metadata->is_data_modified_since_moved_out = true;
    metadata->tid = star::TwoPLPashaHelper::kv_next_commit_tid(metadata->tid);
    RecordPrivateMetadataWrite(metadata);
    *value = next;
    UnlockRow(metadata);
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
  if (!LookupPrivateOffset(fixed_key, &row_offset)) return StatusCode::kNotFound;
  auto *private_value = ValueFromOffset(row_offset);
  auto *metadata = MetadataFromValue(private_value);
  LockRow(metadata);
  const bool absent = !metadata->is_valid;
  UnlockRow(metadata);
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
    if (LookupPrivateOffset(fixed_key, &row_offset)) {
      auto *private_value = ValueFromOffset(row_offset);
      auto *metadata = MetadataFromValue(private_value);
      LockRow(metadata);
      if (metadata->is_migrated && metadata->migrated_smeta_off != kNullOffset) {
        *pinned_existing = static_cast<star::TwoPLPashaMetadataShared *>(
            regions_.hwcc().FromOffset(metadata->migrated_smeta_off));
      }
      UnlockRow(metadata);
    }
    // Move-in already pinned; if the private locator raced away, recover the
    // smeta from the shared tree so the caller can unpin (§10.8).
    if (*pinned_existing == nullptr) {
      RegionOffset shared_offset = kNullOffset;
      if (LookupSharedOffset(fixed_key, &shared_offset) &&
          shared_offset != kNullOffset) {
        *pinned_existing = static_cast<star::TwoPLPashaMetadataShared *>(
            regions_.hwcc().FromOffset(shared_offset));
      }
    }
    if (*pinned_existing == nullptr) {
      // Pin was taken but no resolvable smeta remains — should not happen
      // while ref_cnt blocks move-out; treat as hard failure rather than leak.
      throw std::runtime_error("move-in pin without resolvable smeta");
    }
  }
  return result == star::migration_result::SUCCESS;
}

star::migration_result KVPartition::MoveInForMigrationManager(
    const void *key, bool inc_ref_cnt, void *&migration_policy_meta) {
  migration_policy_meta = nullptr;
  if (star::scc_manager == nullptr) return star::migration_result::FAIL_OOM;
  const FixedKey fixed_key = MakeKey(std::string_view(
      static_cast<const char *>(key), fixed_key_size_));
  auto *table = KvMigrationRuntime::Instance().TableFor(partition_id_);
  if (table == nullptr) return star::migration_result::FAIL_OOM;
  star::migration_result result = star::migration_result::FAIL_OOM;
  const bool found = table->search_and_update_next_key_info(
      &fixed_key,
      [&](const void *prev_key, void *prev_meta, void *prev_data,
          const void *cur_key, void *cur_meta, void *cur_data,
          const void *next_key, void *next_meta, void *next_data) {
        AdjacentRows neighborhood;
        const auto fill = [&](const void *row_key, void *row_meta,
                              void *row_data, bool *present, RowRef *row) {
          if (row_key == nullptr || row_meta == nullptr || row_data == nullptr)
            return;
          *present = true;
          row->key = *static_cast<const FixedKey *>(row_key);
          row->metadata = static_cast<PrivateMetadataLocal *>(row_meta);
          row->value = reinterpret_cast<PrivateValueStruct *>(
              static_cast<char *>(row_data) - sizeof(PrivateValueStruct));
          row->offset = regions_.swcc().ToOffset(row->value);
        };
        fill(prev_key, prev_meta, prev_data, &neighborhood.has_prev,
             &neighborhood.prev);
        fill(cur_key, cur_meta, cur_data, &neighborhood.has_current,
             &neighborhood.current);
        fill(next_key, next_meta, next_data, &neighborhood.has_next,
             &neighborhood.next);
        if (!neighborhood.has_current) return;
        if (neighborhood.has_prev) LockRow(neighborhood.prev.metadata);
        LockRow(neighborhood.current.metadata);
        if (neighborhood.has_next) LockRow(neighborhood.next.metadata);
        auto run = [&]() -> star::migration_result {
  auto *private_value = neighborhood.current.value;
  auto *metadata = neighborhood.current.metadata;
  // A remote insert follows the original placeholder path: its owner-private
  // row is invalid until the requester publishes the shared valid bit, but it
  // is moved in with an inc_ref pin first.  Ordinary migration never moves an
  // invalid row.
  if (!metadata->is_valid && !inc_ref_cnt) {
    UnlockAdjacentRows(&neighborhood);
    return star::migration_result::FAIL_OOM;
  }
  if (metadata->is_migrated) {
    ApplySharedAdjacency(neighborhood);
    if (inc_ref_cnt) {
      // Caller (PromotePrivate with pinned_existing) will unpin exactly once.
      // Only report FAIL_ALREADY_IN_CXL when the pin is actually held; a failed
      // pin must not hand back smeta or RelWithDebInfo uint8_t ref_cnt wraps
      // 0→255 and saturates every later shared read/write (YCSB Forward stall).
      if (metadata->migrated_smeta_off == kNullOffset) {
        UnlockAdjacentRows(&neighborhood);
        return star::migration_result::FAIL_OOM;
      }
      auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
          regions_.hwcc().FromOffset(metadata->migrated_smeta_off));
      if (!star::TwoPLPashaHelper::kv_pin_shared_ref(smeta)) {
        UnlockAdjacentRows(&neighborhood);
        return star::migration_result::FAIL_OOM;
      }
      migration_policy_meta = &smeta->migration_policy_meta;
    }
    UnlockAdjacentRows(&neighborhood);
    return star::migration_result::FAIL_ALREADY_IN_CXL;
  }
  const uint64_t payload_bytes = fixed_value_size_;
  void *payload_mem = nullptr;
  void *smeta_mem = nullptr;
  // Preserve the original TwoPLPasha migration optimisation: a row owns one
  // SCC payload allocation across ordinary move-out/move-in cycles.  Only the
  // transient HWCC metadata is retired on move-out; Delete releases the
  // cached payload together with the owner-private row.
  const bool reuse_cached_payload = metadata->scc_data_off != kNullOffset;
  try {
    if (reuse_cached_payload) {
      payload_mem = regions_.swcc().FromOffset(metadata->scc_data_off);
      if (!regions_.IsSwccAddress(payload_mem))
        throw std::runtime_error("cached SCC payload is outside SWCC");
    } else {
      payload_mem = regions_.Allocate(payload_bytes,
          AllocationDomain::kSharedPayloadSwcc, owner_shard_);
    }
    smeta_mem = regions_.Allocate(sizeof(star::TwoPLPashaMetadataShared),
        AllocationDomain::kHwccMetadata, owner_shard_);
  } catch (const std::bad_alloc &) {
    if (!reuse_cached_payload && payload_mem != nullptr) {
      regions_.Free(payload_mem, payload_bytes,
                    AllocationDomain::kSharedPayloadSwcc, owner_shard_,
                    owner_shard_);
    }
    UnlockAdjacentRows(&neighborhood);
    return star::migration_result::FAIL_OOM;
  }
  auto *payload = new (payload_mem) star::TwoPLPashaSharedDataSCC;
  auto *smeta = new (smeta_mem) star::TwoPLPashaMetadataShared(payload);
  star::scc_manager->init_scc_metadata(smeta, owner_shard_);
  smeta->lock();
  // The shared-tree entry may become visible before synthetic latency is
  // settled; write_locked keeps readers out without holding the HWCC latch.
  smeta->set_write_locked();
  if (star::migration_manager != nullptr) {
    star::migration_manager->init_migration_policy_metadata(
        &smeta->migration_policy_meta, nullptr, key,
        std::tuple<std::atomic<uint64_t> *, void *>{nullptr, nullptr},
        sizeof(star::TwoPLPashaMetadataShared));
  }
  if (!reuse_cached_payload || metadata->is_data_modified_since_moved_out) {
    mem_access::PrivateRead(private_value->data, fixed_value_size_);
    mem_access::SharedPayloadWrite(payload->data, fixed_value_size_);
    star::scc_manager->do_write(smeta, owner_shard_, payload->data,
                                private_value->data, fixed_value_size_);
  }
  if (metadata->is_valid)
    smeta->set_flag(star::TwoPLPashaMetadataShared::valid_flag_index);
  else
    smeta->clear_flag(star::TwoPLPashaMetadataShared::valid_flag_index);
  mem_access::HwccWrite(&smeta->tid, sizeof(smeta->tid));
  smeta->tid = metadata->tid;
  if (inc_ref_cnt) {
    if (smeta->get_ref_cnt() == std::numeric_limits<uint8_t>::max()) {
      smeta->clear_write_locked();
      smeta->unlock();
      UnlockAdjacentRows(&neighborhood);
      regions_.Free(smeta_mem, sizeof(star::TwoPLPashaMetadataShared),
                    AllocationDomain::kHwccMetadata, owner_shard_,
                    owner_shard_);
      if (!reuse_cached_payload)
        regions_.Free(payload_mem, payload_bytes,
                      AllocationDomain::kSharedPayloadSwcc, owner_shard_,
                      owner_shard_);
      return star::migration_result::FAIL_OOM;
    }
    smeta->increment_ref_cnt();
  }
  const RegionOffset smeta_offset = regions_.hwcc().ToOffset(smeta);
  SharedTreeValue shared_value;
  shared_value.row = smeta_offset;
  shared_value.is_valid.store(true, std::memory_order_relaxed);
  if (!shared_tree_->insert(fixed_key, shared_value)) {
    if (inc_ref_cnt) smeta->decrement_ref_cnt();
    smeta->clear_write_locked();
    smeta->unlock();
    UnlockAdjacentRows(&neighborhood);
    regions_.Free(smeta_mem, sizeof(star::TwoPLPashaMetadataShared),
                  AllocationDomain::kHwccMetadata, owner_shard_,
                  owner_shard_);
    if (!reuse_cached_payload)
      regions_.Free(payload_mem, payload_bytes,
                    AllocationDomain::kSharedPayloadSwcc, owner_shard_,
                    owner_shard_);
    return star::migration_result::FAIL_OOM;
  }
  metadata->migrated_smeta_off = smeta_offset;
  if (!reuse_cached_payload)
    metadata->scc_data_off = regions_.swcc().ToOffset(payload);
  metadata->is_migrated = true;
  metadata->is_data_modified_since_moved_out = false;
  RecordPrivateMetadataWrite(metadata);
  star::scc_manager->finish_write(smeta, owner_shard_, payload,
                                  fixed_value_size_);
  migration_policy_meta = &smeta->migration_policy_meta;
  smeta->unlock();
  ApplySharedAdjacency(neighborhood);
  UnlockAdjacentRows(&neighborhood);
  mem_access::DelayActiveScopeNow();
  smeta->lock();
  smeta->clear_write_locked();
  smeta->unlock();
  PersistPrivateRootIfChanged();
  mem_access::HwccAtomicRmw(&directory_.migration_in_seq);
  directory_.migration_in_seq.fetch_add(1, std::memory_order_relaxed);
  star::num_data_move_in.fetch_add(1, std::memory_order_relaxed);
  return star::migration_result::SUCCESS;
        };
        result = run();
      });
  return found ? result : star::migration_result::FAIL_OOM;
}

bool KVPartition::MoveOutForMigrationManager(const void *key) {
  return MoveOutPrivate(
      std::string_view(static_cast<const char *>(key), fixed_key_size_),
      owner_shard_);
}

bool KVPartition::MoveOutPrivate(std::string_view key, uint32_t host_id) {
  EnterEbr();
  if (star::scc_manager == nullptr) return false;
  const FixedKey fixed_key = MakeKey(key);
  auto *table = KvMigrationRuntime::Instance().TableFor(partition_id_);
  if (table == nullptr) return false;
  bool result = false;
  const bool found = table->search_and_update_next_key_info(
      &fixed_key,
      [&](const void *prev_key, void *prev_meta, void *prev_data,
          const void *cur_key, void *cur_meta, void *cur_data,
          const void *next_key, void *next_meta, void *next_data) {
        AdjacentRows neighborhood;
        const auto fill = [&](const void *row_key, void *row_meta,
                              void *row_data, bool *present, RowRef *row) {
          if (row_key == nullptr || row_meta == nullptr || row_data == nullptr)
            return;
          *present = true;
          row->key = *static_cast<const FixedKey *>(row_key);
          row->metadata = static_cast<PrivateMetadataLocal *>(row_meta);
          row->value = reinterpret_cast<PrivateValueStruct *>(
              static_cast<char *>(row_data) - sizeof(PrivateValueStruct));
          row->offset = regions_.swcc().ToOffset(row->value);
        };
        fill(prev_key, prev_meta, prev_data, &neighborhood.has_prev,
             &neighborhood.prev);
        fill(cur_key, cur_meta, cur_data, &neighborhood.has_current,
             &neighborhood.current);
        fill(next_key, next_meta, next_data, &neighborhood.has_next,
             &neighborhood.next);
        if (!neighborhood.has_current) return;
        if (neighborhood.has_prev) LockRow(neighborhood.prev.metadata);
        LockRow(neighborhood.current.metadata);
        if (neighborhood.has_next) LockRow(neighborhood.next.metadata);
        auto run = [&]() -> bool {
  auto *private_value = neighborhood.current.value;
  auto *metadata = neighborhood.current.metadata;
  if (!metadata->is_migrated || metadata->migrated_smeta_off == kNullOffset) {
    UnlockAdjacentRows(&neighborhood);
    return false;
  }
  const RegionOffset smeta_offset = metadata->migrated_smeta_off;
  RegionOffset indexed = kNullOffset;
  if (!LookupSharedOffset(fixed_key, &indexed) || indexed != smeta_offset) {
    UnlockAdjacentRows(&neighborhood);
    return false;
  }
  ClearSharedAdjacency(neighborhood);
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(smeta_offset));
  smeta->lock();
  auto *payload = smeta->get_scc_data();
  if (smeta->get_ref_cnt() != 0 || smeta->get_reader_count() != 0 ||
      smeta->is_write_locked()) {
    smeta->unlock();
    ApplySharedAdjacency(neighborhood);
    UnlockAdjacentRows(&neighborhood);
    return false;
  }
  smeta->set_write_locked();
  star::scc_manager->prepare_read(smeta, host_id, payload, fixed_value_size_);
  if (!smeta->get_flag(star::TwoPLPashaMetadataShared::valid_flag_index)) {
    smeta->clear_write_locked();
    smeta->unlock();
    ApplySharedAdjacency(neighborhood);
    UnlockAdjacentRows(&neighborhood);
    return false;
  }
  if (smeta->is_data_modified_since_moved_in()) {
    mem_access::SharedPayloadRead(payload->data, fixed_value_size_);
    mem_access::PrivateWrite(private_value->data, fixed_value_size_);
    star::scc_manager->do_read(smeta, host_id, private_value->data,
                               payload->data, fixed_value_size_);
    smeta->clear_is_data_modified_since_moved_in();
  }
  mem_access::HwccRead(&smeta->tid, sizeof(smeta->tid));
  metadata->tid = smeta->tid;
  metadata->is_data_modified_since_moved_out = false;
  RecordPrivateMetadataWrite(metadata);
  // Invalidate before tree remove so concurrent TryPinShared cannot pin a
  // row that is about to be EBR-retired. Drop the HWCC latch across the
  // shared-tree remove so GetShared/PutShared (tree then smeta) cannot
  // deadlock against MoveOut (smeta then tree).
  smeta->clear_flag(star::TwoPLPashaMetadataShared::valid_flag_index);
  metadata->is_migrated = false;
  metadata->migrated_smeta_off = kNullOffset;
  smeta->unlock();
  if (!shared_tree_->remove(fixed_key)) {
    smeta->lock();
    smeta->set_flag(star::TwoPLPashaMetadataShared::valid_flag_index);
    metadata->is_migrated = true;
    metadata->migrated_smeta_off = smeta_offset;
    RecordPrivateMetadataWrite(metadata);
    smeta->clear_write_locked();
    smeta->unlock();
    ApplySharedAdjacency(neighborhood);
    UnlockAdjacentRows(&neighborhood);
    return false;
  }
  const RegionOffset clock_row_off = neighborhood.current.offset;
  ApplySharedAdjacency(neighborhood);
  UnlockAdjacentRows(&neighborhood);
  ClockLock();
  ClockUntrackRowOffset(clock_row_off);
  ClockUnlock();
  mem_access::DelayActiveScopeNow();
  smeta->lock();
  smeta->clear_write_locked();
  smeta->unlock();
  ebr_.add_retired_object(smeta, sizeof(star::TwoPLPashaMetadataShared),
                          star::CXLMemory::METADATA_FREE, owner_shard_);
  PersistPrivateRootIfChanged();
  star::num_data_move_out.fetch_add(1, std::memory_order_relaxed);
  KvMigrationRuntime::SyncHwCcUsage(*this);
  return true;
        };
        result = run();
      });
  return found && result;
}

std::string KVPartition::KeyString(const FixedKey &key) const {
  const auto bytes = fixed_key_size_;
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
      if (IsInternalMaxSentinel(entry.first)) break;
      auto *private_value = ValueFromOffset(entry.second.row);
      auto *metadata = MetadataFromValue(private_value);
      {
        LockRow(metadata);
        if (!metadata->is_valid) {
          UnlockRow(metadata);
          continue;
        }
        std::string value;
        bool read = true;
        if (!metadata->is_migrated) {
          value.assign(private_value->data, fixed_value_size_);
          mem_access::PrivateRead(
              private_value->data, fixed_value_size_);
        } else {
          auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
              regions_.hwcc().FromOffset(metadata->migrated_smeta_off));
          value.resize(fixed_value_size_);
          read = star::TwoPLPashaHelper::kv_shared_read_value(
              smeta, owner_shard_, value.data(), value.size());
        }
        UnlockRow(metadata);
        if (read) {
          items->emplace_back(KeyString(entry.first), std::move(value));
        } else {
          // A shared-row lock conflict is ordinary operation contention.  Do
          // not add a Scan-local deadline/retry policy; the facade retries the
          // logical Scan as a whole.
          return false;
        }
      }
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

bool KVPartition::ScanOwnedKeys(
    std::string_view start_key, uint64_t limit,
    std::vector<std::string> *keys,
    const std::function<void()> *progress) const {
  EnterEbr();
  if (keys == nullptr) throw std::invalid_argument("null partition key scan output");
  FixedKey high{};
  std::memset(high.bytes, 0xff, sizeof(high.bytes));
  keys->clear();
  FixedKey low = MakeKey(start_key);
  bool left_inclusive = true;
  for (;;) {
    if (limit != 0 && keys->size() >= limit) break;
    const uint64_t remaining =
        limit == 0 ? 0 : static_cast<uint64_t>(limit - keys->size());
    const uint32_t fetch =
        remaining > std::numeric_limits<uint32_t>::max()
            ? 0
            : static_cast<uint32_t>(remaining);
    std::vector<PrivateTree::KeyValuePair> rows;
    private_tree_->scan(low, high, left_inclusive, true, fetch, rows);
    if (rows.empty()) break;
    for (const auto &entry : rows) {
      if (IsInternalMaxSentinel(entry.first)) break;
      auto *private_value = ValueFromOffset(entry.second.row);
      auto *metadata = MetadataFromValue(private_value);
      LockRow(metadata);
      if (metadata->is_valid) keys->push_back(KeyString(entry.first));
      UnlockRow(metadata);
      if (limit != 0 && keys->size() >= limit) break;
    }
    if (progress != nullptr) (*progress)();
    if (limit == 0 || keys->size() >= limit || rows.size() < fetch) break;
    low = rows.back().first;
    left_inclusive = false;
  }
  return true;
}

bool KVPartition::ScanShared(
    std::string_view start_key, uint64_t limit, uint32_t host_id,
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
        regions_.hwcc().FromOffset(entry.second.row));
    std::string value(fixed_value_size_, '\0');
    const bool read = star::TwoPLPashaHelper::kv_shared_read_value(
        smeta, host_id, value.data(), value.size());
    if (!read) {
      complete = false;
      continue;
    }
    items->emplace_back(KeyString(entry.first), std::move(value));
  }
  return complete;
}

void KVPartition::ScanSharedForUpdate(
    const FixedKey &min_key,
    const std::function<bool(const FixedKey &key, RegionOffset smeta_off,
                             bool is_last_tuple)> &processor) const {
  EnterEbr();
  if (!processor) throw std::invalid_argument("null ScanSharedForUpdate processor");
  // Passthrough only: no adjacency, pin, or migration decisions (§4.3).
  shared_tree_->scanForUpdate(
      min_key, [&](const FixedKey &key, SharedTreeValue &smeta,
                   bool is_last_tuple) -> bool {
        return processor(key, smeta.row, is_last_tuple);
      });
}

KVPartition::SharedScanProbeResult KVPartition::ProbeSharedScanPage(
    uint32_t host_id, std::string_view start_key, uint64_t output_limit,
    bool owner_exhausted_for_cursor, bool cursor_is_duplicate,
    bool owner_no_predecessor_for_cursor) const {
  EnterEbr();
  SharedScanProbeResult result;
  if (output_limit == 0) {
    result.status =
        Status::Error(StatusCode::kInvalidArgument, "probe requires non-zero limit");
    return result;
  }
  const FixedKey min_key = MakeKey(start_key);
  struct Pinned {
    FixedKey key{};
    star::TwoPLPashaMetadataShared *smeta = nullptr;
  };
  std::vector<Pinned> pinned;
  auto unpin_all = [&] {
    for (auto &row : pinned)
      star::TwoPLPashaHelper::kv_unpin_shared_ref(row.smeta);
    pinned.clear();
  };
  bool busy = false;
  bool corruption = false;
  bool stop = false;
  FixedKey last_emitted{};
  bool has_last_emitted = false;

  ScanSharedForUpdate(min_key, [&](const FixedKey &key, RegionOffset smeta_off,
                                   bool is_last_tuple) {
    if (stop) return true;
    if (key.Compare(min_key) < 0) return false;
    if (cursor_is_duplicate && key.Compare(min_key) == 0) return false;
    if (has_last_emitted && key.Compare(last_emitted) <= 0) return false;

    const bool is_limit_boundary =
        pinned.size() == output_limit;
    star::TwoPLPashaMetadataShared *smeta = nullptr;
    if (!TryPinSharedEntryUnderScan(smeta_off, &smeta)) {
      busy = true;
      stop = true;
      return true;
    }
    smeta->lock();
    const bool key_equals_min = key.Compare(min_key) == 0;
    bool adj_ok = star::TwoPLPashaHelper::scan_row_adjacency_ok(
        key_equals_min, is_limit_boundary, smeta->get_prev_key_real_bit(),
        smeta->get_next_key_real_bit());
    // Open left edge: first output row with key > min and owner reported no
    // private predecessor — exempt prev_real (legacy no_predecessor).
    if (!adj_ok && owner_no_predecessor_for_cursor && !has_last_emitted &&
        !is_limit_boundary && !key_equals_min) {
      adj_ok = star::TwoPLPashaHelper::scan_row_adjacency_ok(
          true, is_limit_boundary, smeta->get_prev_key_real_bit(),
          smeta->get_next_key_real_bit());
    }
    // §4.5: with a one-shot owner EOF hint, exempt next_real on the leaf's
    // last tuple (still require prev when not key==min / open-left).
    if (!adj_ok && owner_exhausted_for_cursor && is_last_tuple) {
      adj_ok = star::TwoPLPashaHelper::scan_row_adjacency_ok(
          key_equals_min || owner_no_predecessor_for_cursor, is_limit_boundary,
          smeta->get_prev_key_real_bit(), true);
    }
    smeta->unlock();
    if (!adj_ok) {
      star::TwoPLPashaHelper::kv_unpin_shared_ref(smeta);
      result.migration_required = true;
      stop = true;
      return true;
    }
    if (is_limit_boundary) {
      // Right boundary row: adjacency only; not part of the result page.
      star::TwoPLPashaHelper::kv_unpin_shared_ref(smeta);
      result.more = true;
      stop = true;
      return true;
    }
    pinned.push_back({key, smeta});
    last_emitted = key;
    has_last_emitted = true;
    if (is_last_tuple && owner_exhausted_for_cursor) {
      // Reached true EOF inside this page.
      stop = true;
      return true;
    }
    return false;
  });

  if (busy) {
    unpin_all();
    result.status = Status::Error(StatusCode::kBusy, "shared scan pin contention");
    return result;
  }
  if (corruption) {
    unpin_all();
    result.status =
        Status::Error(StatusCode::kCorruption, "shared scan protocol error");
    return result;
  }
  if (result.migration_required) {
    unpin_all();
    return result;
  }
  if (pinned.empty()) {
    if (owner_exhausted_for_cursor) {
      result.scan_success = true;
      result.more = false;
      return result;
    }
    result.migration_required = true;
    return result;
  }

  for (auto &row : pinned) {
    std::string value(fixed_value_size_, '\0');
    const bool read = star::TwoPLPashaHelper::kv_shared_read_value(
        row.smeta, host_id, value.data(), value.size(), true);
    if (!read) {
      unpin_all();
      result.status =
          Status::Error(StatusCode::kBusy, "shared scan value read contention");
      return result;
    }
    result.items.emplace_back(KeyString(row.key), std::move(value));
    NoteSharedAccess(row.smeta);
  }
  unpin_all();
  result.scan_success = true;
  return result;
}

bool KVPartition::PrivatePredecessorKey(
    std::string_view key, std::string *predecessor) const {
  EnterEbr();
  if (predecessor == nullptr)
    throw std::invalid_argument("null private predecessor output");
  PrivateTree::AdjacentResult adjacent;
  private_tree_->lookupAdjacent(MakeKey(key), adjacent);
  if (!adjacent.prev.has_value()) {
    predecessor->clear();
    return false;
  }
  *predecessor = KeyString(adjacent.prev->first);
  return true;
}

bool KVPartition::DeletePrivate(std::string_view key) {
  EnterEbr();
  // Call the partition delete path directly so Busy throws propagate to the
  // engine. PolicyClock::delete_specific_row_and_move_out is only a thin
  // trampoline that would otherwise mask failures as false (§10.2b).
  bool need_untrack = false;
  void *migration_policy_meta = nullptr;
  return DeletePrivateForMigrationManager(
      key, &need_untrack, &migration_policy_meta);
}

bool KVPartition::DeletePrivateForMigrationManager(
    std::string_view key, bool *need_untrack,
    void **migration_policy_meta) {
  EnterEbr();
  if (need_untrack == nullptr || migration_policy_meta == nullptr)
    throw std::invalid_argument("null Clock delete output");
  *need_untrack = false;
  *migration_policy_meta = nullptr;
  const FixedKey fixed_key = MakeKey(key);
  if (IsInternalMaxSentinel(fixed_key))
    throw std::invalid_argument("internal max sentinel is reserved");
  star::TwoPLPashaMetadataShared *retired_smeta = nullptr;
  star::TwoPLPashaSharedDataSCC *retired_payload = nullptr;
  PrivateValueStruct *retired_value = nullptr;
  PrivateMetadataLocal *retired_metadata = nullptr;
  enum class DeleteFailure { kNone, kBusy, kCorruption } failure =
      DeleteFailure::kNone;
  std::string failure_detail;
  const uint64_t value_bytes = sizeof(PrivateValueStruct) + fixed_value_size_;
  const uint64_t metadata_bytes = sizeof(PrivateMetadataLocal);
  const bool removed = private_tree_->remove_and_process_adjacent_keys(
      fixed_key,
      [&](const FixedKey *prev_key, PrivateTreeValue *prev_off,
          const FixedKey *cur_key, PrivateTreeValue *cur_off,
          const FixedKey *next_key, PrivateTreeValue *next_off) {
        if (cur_key == nullptr || cur_off == nullptr) return false;
        AdjacentRows neighborhood;
        const auto fill = [&](const FixedKey *row_key, PrivateTreeValue *row_off,
                              bool *present, RowRef *row) {
          if (row_key == nullptr || row_off == nullptr) return;
          *present = true;
          row->key = *row_key;
          row->offset = row_off->row;
          row->value = ValueFromOffset(row_off->row);
          row->metadata = MetadataFromValue(row->value);
        };
        fill(prev_key, prev_off, &neighborhood.has_prev, &neighborhood.prev);
        fill(cur_key, cur_off, &neighborhood.has_current, &neighborhood.current);
        fill(next_key, next_off, &neighborhood.has_next, &neighborhood.next);
        // Preserve the original callback's row-lock order while the B+Tree
        // holds the exact adjacent leaves; no lookupAdjacent revalidation.
        if (neighborhood.has_prev) LockRow(neighborhood.prev.metadata);
        if (neighborhood.has_current) LockRow(neighborhood.current.metadata);
        if (neighborhood.has_next) LockRow(neighborhood.next.metadata);
        const auto unlock = [&] { UnlockAdjacentRows(&neighborhood); };
        auto *private_value = neighborhood.current.value;
        auto *metadata = neighborhood.current.metadata;
        if (!metadata->is_valid) {
          unlock();
          return false;
        }
        ClearSharedAdjacency(neighborhood);
        metadata->is_valid = false;
        RecordPrivateMetadataWrite(metadata);
        if (metadata->scc_data_off != kNullOffset) {
          retired_payload = static_cast<star::TwoPLPashaSharedDataSCC *>(
              regions_.swcc().FromOffset(metadata->scc_data_off));
          if (!regions_.IsSwccAddress(retired_payload)) {
            metadata->is_valid = true;
            RecordPrivateMetadataWrite(metadata);
            ApplySharedAdjacency(neighborhood);
            failure = DeleteFailure::kCorruption;
            failure_detail = "delete has cached SCC payload outside SWCC";
            unlock();
            return false;
          }
        }
        if (metadata->is_migrated) {
          const RegionOffset smeta_offset = metadata->migrated_smeta_off;
          if (smeta_offset == kNullOffset ||
              !regions_.IsHwccAddress(regions_.hwcc().FromOffset(smeta_offset))) {
            metadata->is_valid = true;
            RecordPrivateMetadataWrite(metadata);
            ApplySharedAdjacency(neighborhood);
            failure = DeleteFailure::kCorruption;
            failure_detail = "migrated delete has inconsistent shared offset";
            unlock();
            return false;
          }
          auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
              regions_.hwcc().FromOffset(smeta_offset));
          smeta->lock();
          auto *payload = smeta->get_scc_data();
          if (retired_payload != payload) {
            smeta->unlock();
            metadata->is_valid = true;
            RecordPrivateMetadataWrite(metadata);
            ApplySharedAdjacency(neighborhood);
            failure = DeleteFailure::kCorruption;
            failure_detail = "migrated row payload disagrees with local cache";
            unlock();
            return false;
          }
          if (smeta->get_ref_cnt() != 0 || smeta->get_reader_count() != 0 ||
              smeta->is_write_locked()) {
            smeta->unlock();
            metadata->is_valid = true;
            RecordPrivateMetadataWrite(metadata);
            ApplySharedAdjacency(neighborhood);
            failure = DeleteFailure::kBusy;
            failure_detail = "delete shared-row busy";
            unlock();
            return false;
          }
          smeta->set_write_locked();
          smeta->clear_flag(star::TwoPLPashaMetadataShared::valid_flag_index);
          if (!shared_tree_->remove(fixed_key)) {
            smeta->set_flag(star::TwoPLPashaMetadataShared::valid_flag_index);
            smeta->clear_write_locked();
            smeta->unlock();
            metadata->is_valid = true;
            RecordPrivateMetadataWrite(metadata);
            ApplySharedAdjacency(neighborhood);
            failure = DeleteFailure::kCorruption;
            failure_detail = "shared tree remove failed during delete";
            unlock();
            return false;
          }
          smeta->unlock();
          metadata->is_migrated = false;
          metadata->migrated_smeta_off = kNullOffset;
          RecordPrivateMetadataWrite(metadata);
          ClockLock();
          ClockUntrackRowOffset(neighborhood.current.offset);
          ClockUnlock();
          *migration_policy_meta = &smeta->migration_policy_meta;
          retired_smeta = smeta;
        }
        metadata->scc_data_off = kNullOffset;
        RecordPrivateMetadataWrite(metadata);
        AdjacentRows remaining = neighborhood;
        remaining.has_current = false;
        ApplySharedAdjacency(remaining);
        retired_value = private_value;
        retired_metadata = metadata;
        unlock();
        return true;
      });
  if (!removed) {
    if (failure == DeleteFailure::kBusy)
      throw std::runtime_error(failure_detail);
    if (failure == DeleteFailure::kCorruption)
      throw std::runtime_error(failure_detail);
    return false;
  }
  ebr_.add_retired_object(retired_value, value_bytes, star::CXLMemory::MISC_FREE,
                          owner_shard_, partition_id_);
  ebr_.add_retired_object(retired_metadata, metadata_bytes,
                          star::CXLMemory::MISC_FREE, owner_shard_, partition_id_);
  PersistPrivateRootIfChanged();
  if (retired_smeta != nullptr) {
    mem_access::DelayActiveScopeNow();
    retired_smeta->lock();
    retired_smeta->clear_write_locked();
    retired_smeta->unlock();
    ebr_.add_retired_object(
        retired_smeta, sizeof(star::TwoPLPashaMetadataShared),
        star::CXLMemory::METADATA_FREE, owner_shard_);
  }
  if (retired_payload != nullptr) {
    ebr_.add_retired_object(
        retired_payload, fixed_value_size_,
        star::CXLMemory::DATA_FREE, owner_shard_);
  }
  return true;
}

void KVPartition::NoteSharedAccess(star::TwoPLPashaMetadataShared *smeta) const {
  if (smeta == nullptr) return;
  if (star::migration_manager != nullptr)
    star::migration_manager->access_row(&smeta->migration_policy_meta, partition_id_);
}

SharedAccessState KVPartition::TryPinShared(
    const FixedKey &key, star::TwoPLPashaMetadataShared **smeta,
    RegionOffset *smeta_offset) const {
  if (smeta == nullptr || smeta_offset == nullptr)
    throw std::invalid_argument("null TryPinShared output");
  *smeta = nullptr;
  *smeta_offset = kNullOffset;
  RegionOffset offset = kNullOffset;
  if (!LookupSharedOffset(key, &offset) || offset == kNullOffset)
    return SharedAccessState::kMissing;
  auto *candidate = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(offset));
  // Move-out may race between lookup and pin; refuse invalid / saturated rows.
  if (!star::TwoPLPashaHelper::kv_pin_shared_ref(candidate))
    return SharedAccessState::kRetry;
  RegionOffset again = kNullOffset;
  if (!LookupSharedOffset(key, &again) || again != offset) {
    star::TwoPLPashaHelper::kv_unpin_shared_ref(candidate);
    return SharedAccessState::kRetry;
  }
  *smeta = candidate;
  *smeta_offset = offset;
  return SharedAccessState::kDone;
}

bool KVPartition::TryPinSharedEntry(
    const FixedKey &key, RegionOffset expected_offset,
    star::TwoPLPashaMetadataShared **smeta) const {
  if (smeta == nullptr || expected_offset == kNullOffset) return false;
  *smeta = nullptr;
  auto *candidate = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(expected_offset));
  if (!star::TwoPLPashaHelper::kv_pin_shared_ref(candidate)) return false;
  RegionOffset current = kNullOffset;
  if (!LookupSharedOffset(key, &current) || current != expected_offset) {
    star::TwoPLPashaHelper::kv_unpin_shared_ref(candidate);
    return false;
  }
  *smeta = candidate;
  return true;
}

bool KVPartition::TryPinSharedEntryUnderScan(
    RegionOffset expected_offset,
    star::TwoPLPashaMetadataShared **smeta) const {
  // Under scanForUpdate the leaf is already write-locked; must not re-enter
  // the shared tree (lookup would deadlock on the same leaf).
  if (smeta == nullptr || expected_offset == kNullOffset) return false;
  *smeta = nullptr;
  auto *candidate = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(expected_offset));
  if (!star::TwoPLPashaHelper::kv_pin_shared_ref(candidate)) return false;
  *smeta = candidate;
  return true;
}

void KVPartition::ClockLock() {
  pthread_spin_lock(&clock_lock_);
}

void KVPartition::ClockUnlock() {
  pthread_spin_unlock(&clock_lock_);
}

void KVPartition::ClockTrackMigratedKey(const void *key_bytes) {
  const FixedKey fixed_key =
      FixedKey::From(std::string_view(static_cast<const char *>(key_bytes),
                                      fixed_key_size_),
                     fixed_key_size_);
  RegionOffset row_off = kNullOffset;
  if (!LookupPrivateOffset(fixed_key, &row_off) || row_off == kNullOffset)
    throw std::runtime_error("ClockTrackMigratedKey: private row missing");
  auto *private_value = ValueFromOffset(row_off);
  auto *metadata = MetadataFromValue(private_value);
  // Move-in unlocks the neighborhood before track; a concurrent move-out may
  // have already cleared is_migrated. Never link a non-migrated row.
  if (!metadata->is_migrated || metadata->migrated_smeta_off == kNullOffset)
    return;
  if (metadata->clock_node_off != kNullOffset) return;
  auto *node = new (regions_.AllocateOwnerPrivate(sizeof(PrivateClockTrackerNode),
                                                   partition_id_, owner_shard_))
      PrivateClockTrackerNode;
  node->value_off = row_off;
  node->key = fixed_key;
  const RegionOffset node_off = regions_.swcc().ToOffset(node);
  mem_access::PrivateWrite(&private_arena_.clock_head, sizeof(private_arena_.clock_head));
  mem_access::PrivateWrite(&private_arena_.clock_tail, sizeof(private_arena_.clock_tail));
  if (private_arena_.clock_head == kNullOffset &&
      private_arena_.clock_tail == kNullOffset) {
    private_arena_.clock_head = node_off;
    private_arena_.clock_tail = node_off;
  } else {
    auto *tail = ClockNodeFromOffset(private_arena_.clock_tail);
    mem_access::PrivateWrite(tail, sizeof(*tail));
    tail->next_off = node_off;
    node->prev_off = private_arena_.clock_tail;
    private_arena_.clock_tail = node_off;
  }
  metadata->clock_node_off = node_off;
  RecordPrivateMetadataWrite(metadata);
  mem_access::PrivateAtomicRmw(&private_arena_.migrated_key_count);
  private_arena_.migrated_key_count.fetch_add(1, std::memory_order_relaxed);
}

void KVPartition::ClockUntrackMigratedKey(const void *key_bytes) {
  const FixedKey fixed_key =
      FixedKey::From(std::string_view(static_cast<const char *>(key_bytes),
                                      fixed_key_size_),
                     fixed_key_size_);
  RegionOffset row_off = kNullOffset;
  if (!LookupPrivateOffset(fixed_key, &row_off) || row_off == kNullOffset)
    return;
  ClockUntrackRowOffset(row_off);
}

void KVPartition::ClockUntrackRowOffset(RegionOffset row_off) {
  if (row_off == kNullOffset) return;
  auto *private_value = ValueFromOffset(row_off);
  auto *metadata = MetadataFromValue(private_value);
  const RegionOffset node_off = metadata->clock_node_off;
  auto *node = ClockNodeFromOffset(node_off);
  if (node == nullptr) return;
  const bool was_linked =
      private_arena_.clock_head == node_off || private_arena_.clock_tail == node_off ||
      node->prev_off != kNullOffset || node->next_off != kNullOffset;
  if (!was_linked) return;
  mem_access::PrivateWrite(&private_arena_.clock_head, sizeof(private_arena_.clock_head));
  mem_access::PrivateWrite(&private_arena_.clock_tail, sizeof(private_arena_.clock_tail));
  mem_access::PrivateWrite(&private_arena_.clock_cursor,
                           sizeof(private_arena_.clock_cursor));
  if (private_arena_.clock_cursor == node_off)
    private_arena_.clock_cursor = node->prev_off;
  if (private_arena_.clock_head == private_arena_.clock_tail) {
    if (private_arena_.clock_head != node_off) return;
    private_arena_.clock_head = kNullOffset;
    private_arena_.clock_tail = kNullOffset;
  } else {
    if (node->prev_off != kNullOffset) {
      auto *prev = ClockNodeFromOffset(node->prev_off);
      mem_access::PrivateWrite(prev, sizeof(*prev));
      prev->next_off = node->next_off;
    }
    if (node->next_off != kNullOffset) {
      auto *next = ClockNodeFromOffset(node->next_off);
      mem_access::PrivateWrite(next, sizeof(*next));
      next->prev_off = node->prev_off;
    }
    if (private_arena_.clock_head == node_off)
      private_arena_.clock_head = node->next_off;
    if (private_arena_.clock_tail == node_off)
      private_arena_.clock_tail = node->prev_off;
  }
  metadata->clock_node_off = kNullOffset;
  RecordPrivateMetadataWrite(metadata);
  mem_access::PrivateAtomicRmw(&private_arena_.migrated_key_count);
  private_arena_.migrated_key_count.fetch_sub(1, std::memory_order_relaxed);
  regions_.FreeOwnerPrivate(node, sizeof(PrivateClockTrackerNode), partition_id_,
                            owner_shard_);
}

RegionOffset KVPartition::ClockAdvanceCursor() {
  mem_access::PrivateWrite(&private_arena_.clock_cursor,
                           sizeof(private_arena_.clock_cursor));
  if (private_arena_.clock_cursor == kNullOffset) {
    mem_access::PrivateRead(&private_arena_.clock_head, sizeof(private_arena_.clock_head));
    private_arena_.clock_cursor = private_arena_.clock_head;
  } else {
    auto *cur = ClockNodeFromOffset(private_arena_.clock_cursor);
    mem_access::PrivateRead(cur, sizeof(*cur));
    const RegionOffset next = cur->next_off;
    // Linear intrusive list: wrap to head so a lone node with a fresh
    // second_chance can be reconsidered in the same eviction pass.
    private_arena_.clock_cursor =
        (next == kNullOffset) ? private_arena_.clock_head : next;
  }
  return private_arena_.clock_cursor;
}

bool KVPartition::ClockMoveOutRow(RegionOffset row_off) {
  if (row_off == kNullOffset) return false;
  auto *node = ClockNodeFromOffset(row_off);
  return node != nullptr && MoveOutForMigrationManager(node->key.bytes);
}

bool KVPartition::ClockEvictUntilUnderBudget(uint64_t hw_cc_budget) {
  bool ret = false;
  ClockLock();
  if (star::cxl_memory.get_stats(star::CXLMemory::TOTAL_HW_CC_USAGE) <
      hw_cc_budget) {
    ClockUnlock();
    return false;
  }
  // Bound the scan so an empty/corrupt list cannot spin forever.
  for (uint32_t steps = 0; steps < 1024; ++steps) {
    const RegionOffset victim_off = ClockAdvanceCursor();
    if (victim_off == kNullOffset) break;
    auto *node = ClockNodeFromOffset(victim_off);
    if (node == nullptr) continue;
    auto *private_value = ValueFromOffset(node->value_off);
    auto *metadata = MetadataFromValue(private_value);
    if (!metadata->is_migrated || metadata->migrated_smeta_off == kNullOffset) {
      // Stale list node after concurrent move-out raced past track: drop it
      // so migrated_key_count and the intrusive list stay honest.
      ClockUntrackRowOffset(node->value_off);
      continue;
    }
    auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
        regions_.hwcc().FromOffset(metadata->migrated_smeta_off));
    auto *clock_meta =
        reinterpret_cast<star::PolicyClock::ClockMeta *>(&smeta->migration_policy_meta);
    mem_access::HwccAtomicRmw(&clock_meta->second_chance);
    if (clock_meta->second_chance.exchange(0, std::memory_order_relaxed) == 1)
      continue;
    const FixedKey victim_key = node->key;
    // Do not hold the Clock spinlock across move-out (§11.15).
    ClockUnlock();
    // MoveOutForMigrationManager untracks under its own brief ClockLock.
    const bool moved = MoveOutForMigrationManager(victim_key.bytes);
    ClockLock();
    if (moved) {
      if (star::cxl_memory.get_stats(star::CXLMemory::TOTAL_HW_CC_USAGE) <
          hw_cc_budget) {
        ret = true;
        break;
      }
    }
  }
  ClockUnlock();
  return ret;
}

bool KVPartition::MoveOutClockVictim(uint32_t host_id) {
  (void)host_id;
  EnterEbr();
  if (star::migration_manager == nullptr) return false;
  // Caller syncs CXLMemory::TOTAL_HW_CC_USAGE for the HWCC over-budget path.
  return star::migration_manager->move_row_out(partition_id_);
}

uint64_t KVPartition::shared_payload_used_bytes() const {
  return regions_.SharedPayloadUsedBytes(owner_shard_);
}

uint64_t KVPartition::shared_payload_capacity_bytes() const {
  return regions_.SharedPayloadCapacityBytes(owner_shard_);
}

uint64_t KVPartition::hwcc_used_bytes() const {
  return regions_.DynamicHwccUsedBytes(owner_shard_);
}

uint64_t KVPartition::migrated_key_count() const {
  mem_access::PrivateAtomicLoad(&private_arena_.migrated_key_count);
  return private_arena_.migrated_key_count.load(std::memory_order_relaxed);
}


void KVPartition::PersistPrivateRootIfChanged() {
  // Shared live root is published only by BPlusTree::store_root through
  // bind_published_root; do not rewrite directory_.shared_root here (§11.6).
  const RegionOffset private_root =
      regions_.swcc().ToOffset(private_tree_->root_for_persistence());
  if (private_root == persisted_private_root_offset_) return;
  mem_access::PrivateWrite(&private_arena_.private_root,
                           sizeof(private_arena_.private_root));
  private_arena_.private_root = private_root;
  persisted_private_root_offset_ = private_root;
  ++private_root_publishes_;
}

}  // namespace tigonkv::engine
