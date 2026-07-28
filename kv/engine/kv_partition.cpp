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

constexpr size_t kPrivateRowStateOffset =
    offsetof(PrivateRow, is_migrated);
constexpr size_t kPrivateRowStateBytes =
    offsetof(PrivateRow, kv) - kPrivateRowStateOffset;

void RecordPrivateRowStateRead(const PrivateRow *row) {
  mem_access::PrivateRead(
      reinterpret_cast<const char *>(row) + kPrivateRowStateOffset,
      kPrivateRowStateBytes);
}

void RecordPrivateRowStateWrite(const PrivateRow *row) {
  mem_access::PrivateWrite(
      reinterpret_cast<const char *>(row) + kPrivateRowStateOffset,
      kPrivateRowStateBytes);
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

PrivateRow *KVPartition::RowFromOffset(RegionOffset offset) const {
  if (offset == kNullOffset) return nullptr;
  return static_cast<PrivateRow *>(regions_.swcc().FromOffset(offset));
}

void KVPartition::LockRow(PrivateRow *row) {
  uint32_t expected = 0;
  for (;;) {
    mem_access::PrivateAtomicRmw(&row->latch);
    if (row->latch.compare_exchange_weak(
            expected, 1, std::memory_order_acquire,
            std::memory_order_relaxed))
      break;
    expected = 0;
  }
  RecordPrivateRowStateRead(row);
}

bool KVPartition::TryLockRow(PrivateRow *row) {
  uint32_t expected = 0;
  mem_access::PrivateAtomicRmw(&row->latch);
  if (!row->latch.compare_exchange_strong(
          expected, 1, std::memory_order_acquire,
          std::memory_order_relaxed))
    return false;
  RecordPrivateRowStateRead(row);
  return true;
}

void KVPartition::UnlockRow(PrivateRow *row) {
  mem_access::PrivateAtomicStore(&row->latch);
  row->latch.store(0, std::memory_order_release);
}

void KVPartition::LockNeighborhood(
    const FixedKey &key, Neighborhood *neighborhood) const {
  if (neighborhood == nullptr)
    throw std::invalid_argument("null private-tree neighborhood");
  auto load = [&](Neighborhood *result) {
    PrivateTree::AdjacentResult adjacent;
    private_tree_->lookupAdjacent(key, adjacent);
    *result = {};
    auto assign = [&](const auto &entry, bool *present, RowRef *row) {
      if (!entry.has_value()) return;
      *present = true;
      row->key = entry->first;
      row->offset = entry->second;
      row->row = RowFromOffset(entry->second);
      if (row->row == nullptr)
        throw std::runtime_error("private-tree adjacency has null row");
    };
    assign(adjacent.prev, &result->has_prev, &result->prev);
    assign(adjacent.equal, &result->has_current, &result->current);
    assign(adjacent.next, &result->has_next, &result->next);
  };
  for (;;) {
    Neighborhood candidate;
    load(&candidate);
    std::array<RowRef *, 3> rows{{
        candidate.has_prev ? &candidate.prev : nullptr,
        candidate.has_current ? &candidate.current : nullptr,
        candidate.has_next ? &candidate.next : nullptr}};
    size_t locked = 0;
    for (; locked < rows.size(); ++locked) {
      if (rows[locked] == nullptr) continue;
      if (!TryLockRow(rows[locked]->row)) break;
    }
    if (locked != rows.size()) {
      while (locked != 0) {
        --locked;
        if (rows[locked] != nullptr) UnlockRow(rows[locked]->row);
      }
      std::this_thread::yield();
      continue;
    }
    Neighborhood confirmed;
    load(&confirmed);
    if (SameNeighborhood(candidate, confirmed)) {
      *neighborhood = candidate;
      return;
    }
    UnlockNeighborhood(&candidate);
    std::this_thread::yield();
  }
}

void KVPartition::UnlockNeighborhood(Neighborhood *neighborhood) {
  if (neighborhood == nullptr) return;
  if (neighborhood->has_next) UnlockRow(neighborhood->next.row);
  if (neighborhood->has_current) UnlockRow(neighborhood->current.row);
  if (neighborhood->has_prev) UnlockRow(neighborhood->prev.row);
  *neighborhood = {};
}

bool KVPartition::SameNeighborhood(
    const Neighborhood &left, const Neighborhood &right) const {
  auto same = [](bool left_present, const RowRef &left_row,
                 bool right_present, const RowRef &right_row) {
    return left_present == right_present &&
           (!left_present ||
            (left_row.offset == right_row.offset &&
             left_row.key.Compare(right_row.key) == 0));
  };
  return same(left.has_prev, left.prev, right.has_prev, right.prev) &&
         same(left.has_current, left.current,
              right.has_current, right.current) &&
         same(left.has_next, left.next, right.has_next, right.next);
}

void KVPartition::SetNextReal(const RowRef &row, bool real) {
  if (row.row == nullptr || row.row->is_tombstone || !row.row->is_migrated ||
      row.row->migrated_smeta_off == kNullOffset)
    return;
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(row.row->migrated_smeta_off));
  smeta->lock();
  if (real)
    smeta->set_next_key_real_bit();
  else
    smeta->clear_next_key_real_bit();
  smeta->unlock();
}

void KVPartition::SetPrevReal(const RowRef &row, bool real) {
  if (row.row == nullptr || row.row->is_tombstone || !row.row->is_migrated ||
      row.row->migrated_smeta_off == kNullOffset)
    return;
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(row.row->migrated_smeta_off));
  smeta->lock();
  if (real)
    smeta->set_prev_key_real_bit();
  else
    smeta->clear_prev_key_real_bit();
  smeta->unlock();
}

void KVPartition::RefreshAdjacencyLocked(
    const Neighborhood &neighborhood) {
  const bool prev_migrated =
      neighborhood.has_prev && !neighborhood.prev.row->is_tombstone &&
      neighborhood.prev.row->is_migrated;
  const bool current_migrated =
      neighborhood.has_current && !neighborhood.current.row->is_tombstone &&
      neighborhood.current.row->is_migrated;
  const bool next_migrated =
      neighborhood.has_next && !neighborhood.next.row->is_tombstone &&
      neighborhood.next.row->is_migrated;
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

void KVPartition::BreakAdjacencyLocked(
    const Neighborhood &neighborhood) {
  if (neighborhood.has_prev) SetNextReal(neighborhood.prev, false);
  if (neighborhood.has_current) {
    SetPrevReal(neighborhood.current, false);
    SetNextReal(neighborhood.current, false);
  }
  if (neighborhood.has_next) SetPrevReal(neighborhood.next, false);
}

bool KVPartition::InsertPrivateRow(const FixedKey &key, PrivateRow *row) {
  Neighborhood neighborhood;
  LockNeighborhood(key, &neighborhood);
  if (neighborhood.has_current) {
    UnlockNeighborhood(&neighborhood);
    return false;
  }
  if (!TryLockRow(row)) {
    UnlockNeighborhood(&neighborhood);
    throw std::runtime_error("new private row unexpectedly locked");
  }
  BreakAdjacencyLocked(neighborhood);
  const RegionOffset offset = regions_.swcc().ToOffset(row);
  if (!private_tree_->insert(key, offset)) {
    UnlockRow(row);
    UnlockNeighborhood(&neighborhood);
    throw std::runtime_error("private-tree insert failed with locked gap");
  }
  neighborhood.has_current = true;
  neighborhood.current = RowRef{key, offset, row};
  RefreshAdjacencyLocked(neighborhood);
  UnlockNeighborhood(&neighborhood);
  return true;
}

void KVPartition::FreeUnpublishedPrivateRow(PrivateRow *row) {
  if (row == nullptr) return;
  const uint64_t bytes =
      sizeof(PrivateRow) + fixed_key_size_ + fixed_value_size_;
  regions_.FreeOwnerPrivate(row, bytes, partition_id_, owner_shard_);
}

PrivateRow *KVPartition::AllocateRow(const FixedKey &key, std::string_view value) {
  const uint64_t bytes =
      sizeof(PrivateRow) + fixed_key_size_ + fixed_value_size_;
  auto *row = new (regions_.AllocateOwnerPrivate(bytes, partition_id_, owner_shard_))
      PrivateRow;
  row->key_len = fixed_key_size_;
  mem_access::PrivateWrite(row, offsetof(PrivateRow, kv));
  std::memcpy(row->kv, key.bytes, row->key_len);
  std::memset(row->kv + row->key_len, 0, fixed_value_size_);
  std::memcpy(row->kv + row->key_len, value.data(), value.size());
  mem_access::PrivateWrite(row->kv, row->key_len + fixed_value_size_);
  return row;
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
    if (private_tree_->lookup(fixed_key, row_offset)) {
      auto *row = RowFromOffset(row_offset);
      LockRow(row);
      if (row->is_tombstone) {
        UnlockRow(row);
        // Concurrent delete/EBR: do not report Ok without a published write.
        throw std::runtime_error("tombstone put busy");
      }
      if (row->is_migrated) {
        const RegionOffset smeta_offset = row->migrated_smeta_off;
        if (smeta_offset == kNullOffset ||
            !regions_.IsHwccAddress(regions_.hwcc().FromOffset(smeta_offset))) {
          UnlockRow(row);
          throw std::runtime_error("migrated row locator busy");
        }
        auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
            regions_.hwcc().FromOffset(smeta_offset));
        // Follow PrivateRow offset under latch (§10.3); one helper attempt (§10.2).
        const bool written = star::TwoPLPashaHelper::kv_shared_write(
            smeta, owner_shard_, value.data(), fixed_value_size_);
        if (written) {
          RecordPrivateRowStateWrite(row);
          ++row->version;
          NoteSharedAccess(smeta);
          UnlockRow(row);
          return false;
        }
        UnlockRow(row);
        throw std::runtime_error("migrated row shared write busy");
      }
      RecordPrivateRowStateWrite(row);
      std::memset(row->kv + row->key_len, 0, fixed_value_size_);
      std::memcpy(row->kv + row->key_len, value.data(), value.size());
      mem_access::PrivateWrite(row->kv + row->key_len, fixed_value_size_);
      ++row->version;
      UnlockRow(row);
      return false;
    }
    auto *row = AllocateRow(fixed_key, value);
    if (!InsertPrivateRow(fixed_key, row)) {
      // Concurrent create race: another owner worker published first. Free the
      // unpublished loser and retry as an upsert (§10.9).
      FreeUnpublishedPrivateRow(row);
      continue;
    }
    PersistPrivateRootIfChanged();
    return true;
  }
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
    value->assign(row->kv + row->key_len, fixed_value_size_);
    mem_access::PrivateRead(row->kv + row->key_len, fixed_value_size_);
    UnlockRow(row);
    return true;
  }
  const RegionOffset smeta_offset = row->migrated_smeta_off;
  if (smeta_offset == kNullOffset ||
      !regions_.IsHwccAddress(regions_.hwcc().FromOffset(smeta_offset))) {
    UnlockRow(row);
    return false;
  }
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(smeta_offset));
  std::string shared(fixed_value_size_, '\0');
  const bool read = star::TwoPLPashaHelper::kv_shared_read_value(
      smeta, owner_shard_, shared.data(), shared.size());
  if (read) {
    NoteSharedAccess(smeta);
    UnlockRow(row);
    *value = std::move(shared);
    return true;
  }
  UnlockRow(row);
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
  const bool read = star::TwoPLPashaHelper::kv_shared_read_value(
      smeta, host_id, shared.data(), shared.size());
  const bool still_valid =
      smeta->get_flag(star::TwoPLPashaMetadataShared::valid_flag_index);
  if (read) {
    NoteSharedAccess(smeta);
  }
  star::TwoPLPashaHelper::kv_unpin_shared_ref(smeta);
  if (!read) {
    if (!still_valid) return SharedAccessState::kMissing;
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
  if (!private_tree_->lookup(fixed_key, row_offset)) {
    if (!expected.empty()) return false;
    {
          auto *row = AllocateRow(fixed_key, desired);
      if (InsertPrivateRow(fixed_key, row)) {
        PersistPrivateRootIfChanged();
        *exchanged = true;
        if (inserted != nullptr) *inserted = true;
        return true;
      }
      FreeUnpublishedPrivateRow(row);
    }
    // Loser of create race: re-resolve the winner and compare expected.
    if (!private_tree_->lookup(fixed_key, row_offset)) return false;
  }
  auto *row = RowFromOffset(row_offset);
  LockRow(row);
  if (row->is_tombstone) {
    UnlockRow(row);
    return false;
  }
  if (!row->is_migrated) {
    const std::string_view current(row->kv + row->key_len, fixed_value_size_);
    mem_access::PrivateRead(row->kv + row->key_len, fixed_value_size_);
    if (current == expected) {
      RecordPrivateRowStateWrite(row);
      std::memcpy(row->kv + row->key_len, desired.data(), desired.size());
      mem_access::PrivateWrite(row->kv + row->key_len, fixed_value_size_);
      ++row->version;
      *exchanged = true;
    }
    UnlockRow(row);
    return true;
  }
  const RegionOffset smeta_offset = row->migrated_smeta_off;
  if (smeta_offset == kNullOffset ||
      !regions_.IsHwccAddress(regions_.hwcc().FromOffset(smeta_offset))) {
    UnlockRow(row);
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
    UnlockRow(row);
    throw std::runtime_error("migrated row shared CAS busy");
  }
  if (changed) {
    RecordPrivateRowStateWrite(row);
    ++row->version;
    NoteSharedAccess(smeta);
    *exchanged = true;
  }
  UnlockRow(row);
  return true;
}

bool KVPartition::IncrementPrivate(std::string_view key, int64_t delta,
                                   int64_t *value, bool *inserted) {
  EnterEbr();
  if (value == nullptr) throw std::invalid_argument("null increment result");
  if (inserted != nullptr) *inserted = false;
  RegionOffset row_offset = kNullOffset;
  const FixedKey fixed_key = MakeKey(key);
  if (!private_tree_->lookup(fixed_key, row_offset)) {
    std::string encoded;
    if (!EncodeCanonicalFixedDecimal(delta, fixed_value_size_, &encoded))
      throw std::invalid_argument("increment value exceeds fixed value size");
    {
          auto *row = AllocateRow(fixed_key, encoded);
      if (InsertPrivateRow(fixed_key, row)) {
        PersistPrivateRootIfChanged();
        *value = delta;
        if (inserted != nullptr) *inserted = true;
        return true;
      }
      FreeUnpublishedPrivateRow(row);
    }
    // Loser of create race: apply delta on the published row (§10.9).
    if (!private_tree_->lookup(fixed_key, row_offset)) return false;
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
    current.assign(row->kv + row->key_len, fixed_value_size_);
    mem_access::PrivateRead(row->kv + row->key_len, fixed_value_size_);
  } else {
    const RegionOffset smeta_offset = row->migrated_smeta_off;
    if (smeta_offset == kNullOffset ||
        !regions_.IsHwccAddress(regions_.hwcc().FromOffset(smeta_offset))) {
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
      UnlockRow(row);
      throw std::runtime_error("migrated increment shared write rejected");
    }
    DCHECK(changed);
    std::string encoded;
    if (!EncodeCanonicalFixedDecimal(next, fixed_value_size_, &encoded)) {
      UnlockRow(row);
      throw std::invalid_argument("increment value exceeds fixed value size");
    }
    RecordPrivateRowStateWrite(row);
    NoteSharedAccess(smeta);
    ++row->version;
    *value = next;
    UnlockRow(row);
    return true;
  } else {
    int64_t previous = 0;
    if (!DecodeCanonicalFixedDecimal(current, &previous) ||
        (delta > 0 && previous > std::numeric_limits<int64_t>::max() - delta) ||
        (delta < 0 && previous < std::numeric_limits<int64_t>::min() - delta)) {
      UnlockRow(row);
      throw std::invalid_argument(
          "increment requires a non-overflowing int64 value");
    }
    const int64_t next = previous + delta;
    std::string encoded;
    if (!EncodeCanonicalFixedDecimal(next, fixed_value_size_, &encoded)) {
      UnlockRow(row);
      throw std::invalid_argument("increment value exceeds fixed value size");
    }
    RecordPrivateRowStateWrite(row);
    std::memcpy(row->kv + row->key_len, encoded.data(), encoded.size());
    mem_access::PrivateWrite(row->kv + row->key_len, fixed_value_size_);
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
      LockRow(private_row);
      if (private_row->is_migrated && private_row->migrated_smeta_off != kNullOffset) {
        *pinned_existing = static_cast<star::TwoPLPashaMetadataShared *>(
            regions_.hwcc().FromOffset(private_row->migrated_smeta_off));
      }
      UnlockRow(private_row);
    }
    // Move-in already pinned; if the private locator raced away, recover the
    // smeta from the shared tree so the caller can unpin (§10.8).
    if (*pinned_existing == nullptr) {
      RegionOffset shared_offset = kNullOffset;
      if (shared_tree_->lookup(fixed_key, shared_offset) &&
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
  Neighborhood neighborhood;
  LockNeighborhood(fixed_key, &neighborhood);
  if (!neighborhood.has_current) {
    UnlockNeighborhood(&neighborhood);
    return star::migration_result::FAIL_OOM;
  }
  auto *row = neighborhood.current.row;
  if (row->is_tombstone) {
    UnlockNeighborhood(&neighborhood);
    return star::migration_result::FAIL_OOM;
  }
  if (row->is_migrated) {
    RefreshAdjacencyLocked(neighborhood);
    if (inc_ref_cnt) {
      // Caller (PromotePrivate with pinned_existing) will unpin exactly once.
      // Only report FAIL_ALREADY_IN_CXL when the pin is actually held; a failed
      // pin must not hand back smeta or RelWithDebInfo uint8_t ref_cnt wraps
      // 0→255 and saturates every later shared read/write (YCSB Forward stall).
      if (row->migrated_smeta_off == kNullOffset) {
        UnlockNeighborhood(&neighborhood);
        return star::migration_result::FAIL_OOM;
      }
      auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
          regions_.hwcc().FromOffset(row->migrated_smeta_off));
      if (!star::TwoPLPashaHelper::kv_pin_shared_ref(smeta)) {
        UnlockNeighborhood(&neighborhood);
        return star::migration_result::FAIL_OOM;
      }
      migration_policy_meta = &smeta->migration_policy_meta;
    }
    UnlockNeighborhood(&neighborhood);
    return star::migration_result::FAIL_ALREADY_IN_CXL;
  }
  const uint64_t payload_bytes = fixed_value_size_;
  void *payload_mem = nullptr;
  void *smeta_mem = nullptr;
  try {
    payload_mem = regions_.Allocate(payload_bytes,
        AllocationDomain::kSharedPayloadSwcc, owner_shard_);
    smeta_mem = regions_.Allocate(sizeof(star::TwoPLPashaMetadataShared),
        AllocationDomain::kHwccMetadata, owner_shard_);
  } catch (const std::bad_alloc &) {
    if (payload_mem != nullptr) {
      regions_.Free(payload_mem, payload_bytes,
                    AllocationDomain::kSharedPayloadSwcc, owner_shard_,
                    owner_shard_);
    }
    UnlockNeighborhood(&neighborhood);
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
  mem_access::PrivateRead(row->kv + row->key_len, fixed_value_size_);
  mem_access::SharedPayloadWrite(payload->data, fixed_value_size_);
  star::scc_manager->do_write(smeta, owner_shard_, payload->data, row->kv + row->key_len,
                               fixed_value_size_);
  smeta->set_flag(star::TwoPLPashaMetadataShared::valid_flag_index);
  if (inc_ref_cnt) {
    if (smeta->get_ref_cnt() == std::numeric_limits<uint8_t>::max()) {
      smeta->clear_write_locked();
      smeta->unlock();
      UnlockNeighborhood(&neighborhood);
      regions_.Free(smeta_mem, sizeof(star::TwoPLPashaMetadataShared),
                    AllocationDomain::kHwccMetadata, owner_shard_,
                    owner_shard_);
      regions_.Free(payload_mem, payload_bytes,
                    AllocationDomain::kSharedPayloadSwcc, owner_shard_,
                    owner_shard_);
      return star::migration_result::FAIL_OOM;
    }
    smeta->increment_ref_cnt();
  }
  const RegionOffset smeta_offset = regions_.hwcc().ToOffset(smeta);
  if (!shared_tree_->insert(fixed_key, smeta_offset)) {
    if (inc_ref_cnt) smeta->decrement_ref_cnt();
    smeta->clear_write_locked();
    smeta->unlock();
    UnlockNeighborhood(&neighborhood);
    regions_.Free(smeta_mem, sizeof(star::TwoPLPashaMetadataShared),
                  AllocationDomain::kHwccMetadata, owner_shard_,
                  owner_shard_);
    regions_.Free(payload_mem, payload_bytes,
                  AllocationDomain::kSharedPayloadSwcc, owner_shard_,
                  owner_shard_);
    return star::migration_result::FAIL_OOM;
  }
  RecordPrivateRowStateWrite(row);
  row->migrated_smeta_off = smeta_offset;
  row->is_migrated = 1;
  star::scc_manager->finish_write_bits(smeta, owner_shard_);
  migration_policy_meta = &smeta->migration_policy_meta;
  smeta->unlock();
  RefreshAdjacencyLocked(neighborhood);
  UnlockNeighborhood(&neighborhood);
  star::scc_manager->flush_scc_data(payload, fixed_value_size_);
  mem_access::DelayActiveScopeNow();
  smeta->lock();
  smeta->clear_write_locked();
  smeta->unlock_for_publication();
  PersistPrivateRootIfChanged();
  mem_access::HwccAtomicRmw(&directory_.migration_in_seq);
  directory_.migration_in_seq.fetch_add(1, std::memory_order_relaxed);
  star::num_data_move_in.fetch_add(1, std::memory_order_relaxed);
  return star::migration_result::SUCCESS;
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
  Neighborhood neighborhood;
  LockNeighborhood(fixed_key, &neighborhood);
  if (!neighborhood.has_current) {
    UnlockNeighborhood(&neighborhood);
    return false;
  }
  auto *row = neighborhood.current.row;
  if (!row->is_migrated || row->migrated_smeta_off == kNullOffset) {
    UnlockNeighborhood(&neighborhood);
    return false;
  }
  const RegionOffset smeta_offset = row->migrated_smeta_off;
  RegionOffset indexed = kNullOffset;
  if (!shared_tree_->lookup(fixed_key, indexed) || indexed != smeta_offset) {
    UnlockNeighborhood(&neighborhood);
    return false;
  }
  BreakAdjacencyLocked(neighborhood);
  auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(smeta_offset));
  smeta->lock();
  auto *payload = smeta->get_scc_data();
  if (smeta->get_ref_cnt() != 0 || smeta->get_reader_count() != 0 ||
      smeta->is_write_locked()) {
    smeta->unlock();
    RefreshAdjacencyLocked(neighborhood);
    UnlockNeighborhood(&neighborhood);
    return false;
  }
  smeta->set_write_locked();
  star::scc_manager->prepare_read(smeta, host_id, payload, fixed_value_size_);
  if (!smeta->get_flag(star::TwoPLPashaMetadataShared::valid_flag_index)) {
    smeta->clear_write_locked();
    smeta->unlock();
    RefreshAdjacencyLocked(neighborhood);
    UnlockNeighborhood(&neighborhood);
    return false;
  }
  mem_access::SharedPayloadRead(payload->data, fixed_value_size_);
  mem_access::PrivateWrite(row->kv + row->key_len, fixed_value_size_);
  star::scc_manager->do_read(smeta, host_id, row->kv + row->key_len, payload->data,
                             fixed_value_size_);
  RecordPrivateRowStateWrite(row);
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
    RecordPrivateRowStateWrite(row);
    row->is_migrated = 1;
    row->migrated_smeta_off = smeta_offset;
    smeta->clear_write_locked();
    smeta->unlock();
    RefreshAdjacencyLocked(neighborhood);
    UnlockNeighborhood(&neighborhood);
    return false;
  }
  const RegionOffset clock_row_off = neighborhood.current.offset;
  RefreshAdjacencyLocked(neighborhood);
  UnlockNeighborhood(&neighborhood);
  ClockLock();
  ClockUntrackRowOffset(clock_row_off);
  ClockUnlock();
  mem_access::DelayActiveScopeNow();
  smeta->lock();
  smeta->clear_write_locked();
  smeta->unlock_for_publication();
  ebr_.add_retired_object(smeta, sizeof(star::TwoPLPashaMetadataShared),
                          star::CXLMemory::METADATA_FREE, owner_shard_);
  ebr_.add_retired_object(payload, fixed_value_size_,
                          star::CXLMemory::DATA_FREE, owner_shard_);
  PersistPrivateRootIfChanged();
  star::num_data_move_out.fetch_add(1, std::memory_order_relaxed);
  KvMigrationRuntime::SyncHwCcUsage(*this);
  return true;
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
      auto *row = RowFromOffset(entry.second);
      {
        LockRow(row);
        if (row->is_tombstone) {
          UnlockRow(row);
          continue;
        }
        std::string value;
        bool read = true;
        if (!row->is_migrated) {
          value.assign(row->kv + row->key_len, fixed_value_size_);
          mem_access::PrivateRead(
              row->kv + row->key_len, fixed_value_size_);
        } else {
          auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
              regions_.hwcc().FromOffset(row->migrated_smeta_off));
          value.resize(fixed_value_size_);
          read = star::TwoPLPashaHelper::kv_shared_read_value(
              smeta, owner_shard_, value.data(), value.size());
        }
        UnlockRow(row);
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
      auto *row = RowFromOffset(entry.second);
      LockRow(row);
      RecordPrivateRowStateRead(row);
      if (!row->is_tombstone) keys->push_back(KeyString(entry.first));
      UnlockRow(row);
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
        regions_.hwcc().FromOffset(entry.second));
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
      min_key, [&](const FixedKey &key, RegionOffset &smeta_off,
                   bool is_last_tuple) -> bool {
        return processor(key, smeta_off, is_last_tuple);
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
  star::TwoPLPashaMetadataShared *retired_smeta = nullptr;
  star::TwoPLPashaSharedDataSCC *retired_payload = nullptr;
  Neighborhood neighborhood;
  LockNeighborhood(fixed_key, &neighborhood);
  if (!neighborhood.has_current) {
    UnlockNeighborhood(&neighborhood);
    return false;
  }
  auto *row = neighborhood.current.row;
  if (row->is_tombstone) {
    UnlockNeighborhood(&neighborhood);
    return false;
  }
  BreakAdjacencyLocked(neighborhood);
  RecordPrivateRowStateWrite(row);
  row->is_tombstone = 1;
  const uint64_t row_bytes =
      sizeof(PrivateRow) + fixed_key_size_ + fixed_value_size_;
  if (row->is_migrated) {
    const RegionOffset smeta_offset = row->migrated_smeta_off;
    if (smeta_offset == kNullOffset ||
        !regions_.IsHwccAddress(regions_.hwcc().FromOffset(smeta_offset))) {
      RecordPrivateRowStateWrite(row);
      row->is_tombstone = 0;
      RefreshAdjacencyLocked(neighborhood);
      UnlockNeighborhood(&neighborhood);
      throw std::runtime_error(
          "migrated delete has inconsistent shared offset");
    }
    auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
        regions_.hwcc().FromOffset(smeta_offset));
    smeta->lock();
    auto *payload = smeta->get_scc_data();
    if (smeta->get_ref_cnt() != 0 || smeta->get_reader_count() != 0 ||
        smeta->is_write_locked()) {
      smeta->unlock();
      RecordPrivateRowStateWrite(row);
      row->is_tombstone = 0;
      RefreshAdjacencyLocked(neighborhood);
      UnlockNeighborhood(&neighborhood);
      // Do not spin under Clock tracker lock (§10.2b); caller sees Busy.
      throw std::runtime_error("delete shared-row busy");
    }
    smeta->set_write_locked();
    smeta->clear_flag(star::TwoPLPashaMetadataShared::valid_flag_index);
    if (!shared_tree_->remove(fixed_key)) {
      smeta->set_flag(star::TwoPLPashaMetadataShared::valid_flag_index);
      smeta->clear_write_locked();
      smeta->unlock();
      RecordPrivateRowStateWrite(row);
      row->is_tombstone = 0;
      RefreshAdjacencyLocked(neighborhood);
      UnlockNeighborhood(&neighborhood);
      throw std::runtime_error(
          "shared tree remove failed during delete");
    }
    smeta->unlock();
    RecordPrivateRowStateWrite(row);
    row->is_migrated = 0;
    row->migrated_smeta_off = kNullOffset;
    // Unlink before the PrivateRow is EBR-retired (§11.14).
    ClockLock();
    ClockUntrackRowOffset(neighborhood.current.offset);
    ClockUnlock();
    *need_untrack = false;  // already untracked by offset
    *migration_policy_meta = &smeta->migration_policy_meta;
    retired_smeta = smeta;
    retired_payload = payload;
  }
  if (!private_tree_->remove(fixed_key)) {
    UnlockNeighborhood(&neighborhood);
    throw std::runtime_error(
        "private tree remove failed after shared delete");
  }
  Neighborhood remaining = neighborhood;
  remaining.has_current = false;
  RefreshAdjacencyLocked(remaining);
  ebr_.add_retired_object(row, row_bytes, star::CXLMemory::MISC_FREE,
                          owner_shard_, partition_id_);
  UnlockNeighborhood(&neighborhood);
  PersistPrivateRootIfChanged();
  if (retired_smeta != nullptr) {
    mem_access::DelayActiveScopeNow();
    retired_smeta->lock();
    retired_smeta->clear_write_locked();
    retired_smeta->unlock();
    ebr_.add_retired_object(
        retired_smeta, sizeof(star::TwoPLPashaMetadataShared),
        star::CXLMemory::METADATA_FREE, owner_shard_);
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
  if (!shared_tree_->lookup(key, offset) || offset == kNullOffset)
    return SharedAccessState::kMissing;
  auto *candidate = static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.hwcc().FromOffset(offset));
  // Move-out may race between lookup and pin; refuse invalid / saturated rows.
  if (!star::TwoPLPashaHelper::kv_pin_shared_ref(candidate))
    return SharedAccessState::kRetry;
  RegionOffset again = kNullOffset;
  if (!shared_tree_->lookup(key, again) || again != offset) {
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
  if (!shared_tree_->lookup(key, current) || current != expected_offset) {
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
  if (!private_tree_->lookup(fixed_key, row_off) || row_off == kNullOffset)
    throw std::runtime_error("ClockTrackMigratedKey: private row missing");
  auto *row = RowFromOffset(row_off);
  // Move-in unlocks the neighborhood before track; a concurrent move-out may
  // have already cleared is_migrated. Never link a non-migrated row.
  if (!row->is_migrated || row->migrated_smeta_off == kNullOffset) return;
  // Already linked?
  if (row->clock_prev_off != kNullOffset || row->clock_next_off != kNullOffset ||
      private_arena_.clock_head == row_off || private_arena_.clock_tail == row_off) {
    if (private_arena_.clock_head == row_off || private_arena_.clock_tail == row_off ||
        row->clock_prev_off != kNullOffset || row->clock_next_off != kNullOffset)
      return;  // idempotent re-track
  }
  RecordPrivateRowStateWrite(row);
  row->clock_prev_off = kNullOffset;
  row->clock_next_off = kNullOffset;
  mem_access::PrivateWrite(&private_arena_.clock_head, sizeof(private_arena_.clock_head));
  mem_access::PrivateWrite(&private_arena_.clock_tail, sizeof(private_arena_.clock_tail));
  if (private_arena_.clock_head == kNullOffset &&
      private_arena_.clock_tail == kNullOffset) {
    private_arena_.clock_head = row_off;
    private_arena_.clock_tail = row_off;
  } else {
    auto *tail = RowFromOffset(private_arena_.clock_tail);
    RecordPrivateRowStateWrite(tail);
    tail->clock_next_off = row_off;
    row->clock_prev_off = private_arena_.clock_tail;
    private_arena_.clock_tail = row_off;
  }
  mem_access::PrivateAtomicRmw(&private_arena_.migrated_key_count);
  private_arena_.migrated_key_count.fetch_add(1, std::memory_order_relaxed);
}

void KVPartition::ClockUntrackMigratedKey(const void *key_bytes) {
  const FixedKey fixed_key =
      FixedKey::From(std::string_view(static_cast<const char *>(key_bytes),
                                      fixed_key_size_),
                     fixed_key_size_);
  RegionOffset row_off = kNullOffset;
  if (!private_tree_->lookup(fixed_key, row_off) || row_off == kNullOffset)
    return;
  ClockUntrackRowOffset(row_off);
}

void KVPartition::ClockUntrackRowOffset(RegionOffset row_off) {
  if (row_off == kNullOffset) return;
  auto *row = RowFromOffset(row_off);
  const bool was_linked =
      private_arena_.clock_head == row_off || private_arena_.clock_tail == row_off ||
      row->clock_prev_off != kNullOffset || row->clock_next_off != kNullOffset;
  if (!was_linked) return;
  mem_access::PrivateWrite(&private_arena_.clock_head, sizeof(private_arena_.clock_head));
  mem_access::PrivateWrite(&private_arena_.clock_tail, sizeof(private_arena_.clock_tail));
  mem_access::PrivateWrite(&private_arena_.clock_cursor,
                           sizeof(private_arena_.clock_cursor));
  if (private_arena_.clock_cursor == row_off)
    private_arena_.clock_cursor = row->clock_prev_off;
  if (private_arena_.clock_head == private_arena_.clock_tail) {
    if (private_arena_.clock_head != row_off) return;
    private_arena_.clock_head = kNullOffset;
    private_arena_.clock_tail = kNullOffset;
  } else {
    if (row->clock_prev_off != kNullOffset) {
      auto *prev = RowFromOffset(row->clock_prev_off);
      RecordPrivateRowStateWrite(prev);
      prev->clock_next_off = row->clock_next_off;
    }
    if (row->clock_next_off != kNullOffset) {
      auto *next = RowFromOffset(row->clock_next_off);
      RecordPrivateRowStateWrite(next);
      next->clock_prev_off = row->clock_prev_off;
    }
    if (private_arena_.clock_head == row_off)
      private_arena_.clock_head = row->clock_next_off;
    if (private_arena_.clock_tail == row_off)
      private_arena_.clock_tail = row->clock_prev_off;
  }
  RecordPrivateRowStateWrite(row);
  row->clock_prev_off = kNullOffset;
  row->clock_next_off = kNullOffset;
  mem_access::PrivateAtomicRmw(&private_arena_.migrated_key_count);
  private_arena_.migrated_key_count.fetch_sub(1, std::memory_order_relaxed);
}

RegionOffset KVPartition::ClockAdvanceCursor() {
  mem_access::PrivateWrite(&private_arena_.clock_cursor,
                           sizeof(private_arena_.clock_cursor));
  if (private_arena_.clock_cursor == kNullOffset) {
    mem_access::PrivateRead(&private_arena_.clock_head, sizeof(private_arena_.clock_head));
    private_arena_.clock_cursor = private_arena_.clock_head;
  } else {
    auto *cur = RowFromOffset(private_arena_.clock_cursor);
    RecordPrivateRowStateRead(cur);
    const RegionOffset next = cur->clock_next_off;
    // Linear intrusive list: wrap to head so a lone node with a fresh
    // second_chance can be reconsidered in the same eviction pass.
    private_arena_.clock_cursor =
        (next == kNullOffset) ? private_arena_.clock_head : next;
  }
  return private_arena_.clock_cursor;
}

bool KVPartition::ClockMoveOutRow(RegionOffset row_off) {
  if (row_off == kNullOffset) return false;
  auto *row = RowFromOffset(row_off);
  return MoveOutForMigrationManager(row->kv);
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
    auto *row = RowFromOffset(victim_off);
    if (row == nullptr || !row->is_migrated ||
        row->migrated_smeta_off == kNullOffset) {
      // Stale list node after concurrent move-out raced past track: drop it
      // so migrated_key_count and the intrusive list stay honest.
      ClockUntrackRowOffset(victim_off);
      continue;
    }
    auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(
        regions_.hwcc().FromOffset(row->migrated_smeta_off));
    auto *clock_meta =
        reinterpret_cast<star::PolicyClock::ClockMeta *>(&smeta->migration_policy_meta);
    mem_access::HwccAtomicRmw(&clock_meta->second_chance);
    if (clock_meta->second_chance.exchange(0, std::memory_order_relaxed) == 1)
      continue;
    FixedKey victim_key{};
    std::memcpy(victim_key.bytes, row->kv, fixed_key_size_);
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
