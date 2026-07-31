#include "kv/engine/kv_partition.h"

#include "kv/engine/kv_migration.h"
#include "kv/engine/fixed_value.h"
#include "kv/engine/kv_migration.h"
#include "kv/engine/kv_worker_context.h"
#include "kv/engine/mem_access.h"
#include "protocol/Pasha/PolicyClock.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
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

uint32_t ReadHwccConfigField(const uint32_t *field) {
  mem_access::HwccRead(field, sizeof(*field));
  return *field;
}

bool IsInternalMaxSentinel(const FixedKey &key, uint32_t fixed_key_size) {
  return key.Compare(FixedKey::InternalMax(fixed_key_size)) == 0;
}

constexpr uint64_t SharedSccBytes(uint64_t value_bytes) {
  return sizeof(star::TwoPLPashaSharedDataSCC) + value_bytes;
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
      private_binding_{&regions, AllocationDomain::kOwnerPrivateSwcc, owner_shard, &ebr,
                       partition_id},
      shared_binding_{&regions, AllocationDomain::kHwccIndex, owner_shard, &ebr} {
  // Process-level CXLMemory owner binding is set once in KVEngine::Open to
  // config.node_id. Do not rebind per partition (§11.3).
  if (partition_id >=
      ReadHwccConfigField(&regions.layout().partition_count))
    throw std::invalid_argument("partition id outside persistent layout");
  if (materialize_private) {
    constexpr uint64_t header_bytes =
        (sizeof(OwnerPrivateArenaHeader) + RegionAllocator::kAlignment - 1) /
        RegionAllocator::kAlignment * RegionAllocator::kAlignment;
    auto *after_header = static_cast<std::byte *>(regions.ResolveOwnerPrivate(
        regions.OwnerPrivateArenaOffset(partition_id) + header_bytes, 1,
        partition_id, owner_shard));
    private_arena_ = reinterpret_cast<OwnerPrivateArenaHeader *>(
        after_header - header_bytes);
  }
  if (attach) {
    RegionOffset private_root = kNullOffset;
    if (materialize_private) {
      mem_access::PrivateRead(&private_arena_->private_root,
                              sizeof(private_arena_->private_root));
      private_root = private_arena_->private_root.load(std::memory_order_acquire);
    }
    mem_access::HwccAtomicLoad(&directory_.shared_root);
    if ((materialize_private && private_root == kNullOffset) ||
        directory_.shared_root.load(std::memory_order_acquire) == kNullOffset)
      throw std::runtime_error("partition attach missing tree root");
    if (materialize_private) {
      private_table_ = std::make_unique<KvPartitionTable>(
          this, fixed_key_size_, fixed_value_size_, private_binding_,
          regions_.ResolveOwnerPrivate(private_root, btreeolc_cxl::kPageSize,
                                       partition_id_,
                                       owner_shard_),
          /*create_max_sentinel=*/false);
      private_table_->BindPublishedRoot(&private_arena_->private_root);
    }
    mem_access::HwccAtomicLoad(&directory_.shared_root);
    shared_tree_ = new SharedTree(
        shared_binding_,
        regions_.ResolveDynamicHwcc(
            directory_.shared_root.load(std::memory_order_acquire),
            btreeolc_cxl::kPageSize, owner_shard_));
    shared_tree_->bind_published_root(&directory_.shared_root);
    shared_table_ = new SharedTable(
        shared_tree_, kSingleTableId, partition_id_,
        RegionOffsetSharedRowReference{&regions_, owner_shard_});
  } else {
    if (!materialize_private)
      throw std::logic_error("reset must materialize every private partition root");
    private_table_ = std::make_unique<KvPartitionTable>(
        this, fixed_key_size_, fixed_value_size_, private_binding_, nullptr,
        /*create_max_sentinel=*/true);
    shared_tree_ = new SharedTree(shared_binding_);
    shared_tree_->bind_published_root(&directory_.shared_root);
    shared_table_ = new SharedTable(
        shared_tree_, kSingleTableId, partition_id_,
        RegionOffsetSharedRowReference{&regions_, owner_shard_});
    private_table_->BindPublishedRoot(&private_arena_->private_root);
  }
  // Clock tracker rebuild runs after KvMigrationRuntime::Install so the
  // process-local PolicyClock exists (PLAN §4.5 attach rebuild).
}

KVPartition::~KVPartition() {
  delete shared_table_;
  delete shared_tree_;
}

FixedKey KVPartition::MakeKey(std::string_view key) const {
  return FixedKey::From(key, fixed_key_size_);
}

bool KVPartition::LookupPrivateOffset(const FixedKey &key,
                                      RegionOffset *offset) const {
  if (offset == nullptr) throw std::invalid_argument("null private offset");
  if (private_table_ == nullptr)
    throw std::runtime_error("owner-private TableBTreeOLC is unavailable");
  return private_table_->LookupOffset(key, offset);
}

bool KVPartition::LookupSharedReference(const FixedKey &key,
                                        RegionOffset *offset) const {
  if (offset == nullptr) throw std::invalid_argument("null shared offset");
  if (shared_table_ == nullptr)
    throw std::runtime_error("shared CXL table is unavailable");
  return shared_table_->lookup_reference(&key, offset);
}

PrivateValueStruct *KVPartition::ValueFromOffset(RegionOffset offset) const {
  if (offset == kNullOffset) return nullptr;
  return static_cast<PrivateValueStruct *>(regions_.ResolveOwnerPrivate(
      offset, sizeof(PrivateValueStruct) + fixed_value_size_, partition_id_,
      owner_shard_));
}

PrivateMetadataLocal *KVPartition::MetadataFromValue(
    PrivateValueStruct *value) const {
  if (value == nullptr) return nullptr;
  mem_access::PrivateAtomicLoad(&value->meta);
  const RegionOffset offset = value->meta.load(std::memory_order_acquire);
  if (offset == kNullOffset)
    throw std::runtime_error("private ValueStruct has no local metadata");
  return static_cast<PrivateMetadataLocal *>(regions_.ResolveOwnerPrivate(
      offset, sizeof(PrivateMetadataLocal), partition_id_, owner_shard_));
}

star::TwoPLPashaMetadataShared *KVPartition::SharedMetadataFromOffset(
    RegionOffset offset) const {
  if (offset == kNullOffset) {
    throw std::runtime_error("migrated row has null HWCC smeta offset");
  }
  return static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.ResolveDynamicHwcc(offset,
                                  sizeof(star::TwoPLPashaMetadataShared),
                                  owner_shard_));
}

void KVPartition::LockRow(PrivateMetadataLocal *metadata) {
  using Access = star::LocalMetadataAccess<PrivateMetadataLocal>;
  Access::Lock(*metadata);
  metadata->lock();
  Access::Read(*metadata);
}

void KVPartition::UnlockRow(PrivateMetadataLocal *metadata) {
  using Access = star::LocalMetadataAccess<PrivateMetadataLocal>;
  Access::Write(*metadata);
  Access::Unlock(*metadata);
  metadata->unlock();
}

bool KVPartition::AcquireOwnerNextRowWriteLock(
    PrivateValueStruct *value, OwnerNextRowLock *locked_row) {
  if (value == nullptr || locked_row == nullptr)
    throw std::invalid_argument("null owner insert next-row lock");
  auto *metadata = MetadataFromValue(value);
  locked_row->value = value;
  locked_row->metadata = metadata;
  locked_row->shared = false;
  // This is the original successor write-lock acquisition.  Do not duplicate
  // its private or migrated tid/read-count/write-bit transition in the KV
  // adapter: the offset overload only resolves ValueStruct::meta and the
  // owner-private RegionOffset locator before entering that primitive.
  bool locked = false;
  star::TwoPLPashaMetadataShared *shared_locked = nullptr;
  locked_row->observed_tid = star::TwoPLPashaHelper::take_write_lock(
      *metadata, value->data, fixed_value_size_, locked,
      [this](const PrivateMetadataLocal &local) {
        return SharedMetadataFromOffset(local.migrated_smeta_off);
      },
      owner_shard_, &shared_locked, nullptr, nullptr);
  if (!locked) return false;
  locked_row->shared = shared_locked != nullptr;
  return true;
}

void KVPartition::ReleaseOwnerNextRowWriteLock(
    const OwnerNextRowLock &locked_row, uint64_t new_tid, bool commit) {
  if (locked_row.metadata == nullptr) return;
  auto *metadata = locked_row.metadata;
  if (!locked_row.shared) {
    star::TwoPLPashaHelper::write_lock_release(
        *metadata, commit ? new_tid : locked_row.observed_tid,
        fixed_value_size_, owner_shard_,
        [this](const PrivateMetadataLocal &local) {
          return SharedMetadataFromOffset(local.migrated_smeta_off);
        });
    return;
  }

  LockRow(metadata);
  const RegionOffset smeta_offset = metadata->migrated_smeta_off;
  if (smeta_offset == kNullOffset) {
    UnlockRow(metadata);
    throw std::runtime_error("owner insert next-row shared locator vanished");
  }
  auto *smeta = SharedMetadataFromOffset(smeta_offset);
  // Preserve the owner-local-metadata → shared-row order, but let the
  // original remote write release own the SCC publication and write-bit
  // transition.  The abort case is likewise the original single-row abort;
  // it must not emit a synthetic SCC write.
  if (commit) {
    star::TwoPLPashaHelper::remote_write_lock_release(
        smeta, owner_shard_, SharedSccBytes(fixed_value_size_), new_tid);
  } else {
    // remote_write_lock_abort settles an active latency scope before taking
    // smeta.  Its shared write bit still prevents move-out, so drop the
    // owner-private locator lock first rather than settling delay while a row
    // lock is held (§ latency audit safety point).
    UnlockRow(metadata);
    star::TwoPLPashaHelper::remote_write_lock_abort(smeta);
    return;
  }
  UnlockRow(metadata);
}

bool KVPartition::InsertOwnerPlaceholderWithNextLock(
    const FixedKey &key, std::string_view value, OwnerNextRowLock *locked_row) {
  if (locked_row == nullptr)
    throw std::invalid_argument("null owner insert next-row output");
  *locked_row = {};
  auto *table = private_table_.get();
  if (table == nullptr) throw std::runtime_error("owner table is unavailable");
  // Keep the original TwoPLPashaHelper::insert_and_update_next_key_info
  // lifecycle: its one adjacent-tuple callback locks the successor and clears
  // both CXL adjacency bits before the placeholder becomes visible.  The
  // offset-backed ValueStruct keeps its meta slot first, so this is the direct
  // replacement for the original local-metadata pointer dereference.
  return star::TwoPLPashaHelper::insert_and_update_next_key_info(
      table, &key, value.data(),
      [&](const void *, std::atomic<uint64_t> *prev_meta, void *, const void *,
          std::atomic<uint64_t> *next_meta, void *) {
        // The permanent maximum-key tuple guarantees a successor for every
        // user-key insertion, matching the original helper's DCHECK.
        if (next_meta == nullptr)
          throw std::runtime_error("owner insert has no successor tuple");
        auto *next_value = reinterpret_cast<PrivateValueStruct *>(next_meta);
        if (!AcquireOwnerNextRowWriteLock(next_value, locked_row)) return false;
        try {
          auto *prev = prev_meta == nullptr
              ? nullptr
              : MetadataFromValue(
                    reinterpret_cast<PrivateValueStruct *>(prev_meta));
          auto *next = MetadataFromValue(next_value);
          star::TwoPLPashaHelper::clear_adjacent_migrated_rows(
              prev, next,
              [this](const PrivateMetadataLocal &local) {
                return SharedMetadataFromOffset(local.migrated_smeta_off);
              });
        } catch (...) {
          ReleaseOwnerNextRowWriteLock(*locked_row, 0, false);
          *locked_row = {};
          throw;
        }
        return true;
      });
}

bool KVPartition::PublishOwnerPlaceholder(const FixedKey &key,
                                          uint64_t commit_tid) {
  auto *table = private_table_.get();
  if (table == nullptr) throw std::runtime_error("owner table is unavailable");
  // `insert_and_update_next_key_info` already clears the adjacent CXL bits
  // while it holds the successor write lock.  The original commit path only
  // publishes the just-inserted placeholder; a second adjacent lookup here
  // needlessly extends that lock's critical section.
  auto [meta_slot, ignored_data] = table->search(&key);
  (void)ignored_data;
  if (meta_slot == nullptr) return false;
  auto *value = reinterpret_cast<PrivateValueStruct *>(meta_slot);
  auto *metadata = MetadataFromValue(value);
  LockRow(metadata);
  if (metadata->is_valid) {
    UnlockRow(metadata);
    return false;
  }
  metadata->tid = commit_tid;
  metadata->is_valid = true;
  metadata->is_data_modified_since_moved_out = true;
  if (metadata->is_migrated) {
    if (metadata->migrated_smeta_off == kNullOffset) {
      UnlockRow(metadata);
      throw std::runtime_error("owner insert migrated shared locator vanished");
    }
    auto *smeta = SharedMetadataFromOffset(metadata->migrated_smeta_off);
    // Preserve master modify_tuple_valid_bit(..., true, true): a concurrent
    // move-in may expose the owner placeholder before this commit.  Publish
    // that SCC row under the original local-metadata-then-smeta lock order.
    if (!star::TwoPLPashaHelper::modify_tuple_valid_bit(
            smeta, owner_shard_, SharedSccBytes(fixed_value_size_),
            /*is_valid=*/true, /*is_insert=*/true, commit_tid,
            /*update_tid=*/true)) {
      UnlockRow(metadata);
      throw std::runtime_error("owner insert shared placeholder publication failed");
    }
  }
  UnlockRow(metadata);
  return true;
}

bool KVPartition::CreatePrivateWithOwnerInsert(const FixedKey &key,
                                               std::string_view value) {
  OwnerNextRowLock next_row;
  if (!InsertOwnerPlaceholderWithNextLock(key, value, &next_row)) return false;
  const uint64_t commit_tid = NextCommitTid(next_row.observed_tid);
  if (!PublishOwnerPlaceholder(key, commit_tid)) {
    ReleaseOwnerNextRowWriteLock(next_row, 0, false);
    throw std::runtime_error("owner insert placeholder publication failed");
  }
  ReleaseOwnerNextRowWriteLock(next_row, commit_tid, true);
  return true;
}

void KVPartition::FreeUnpublishedPrivateValue(PrivateValueStruct *value) {
  if (value == nullptr) return;
  auto *metadata = MetadataFromValue(value);
  const uint64_t value_bytes = sizeof(PrivateValueStruct) + fixed_value_size_;
  const uint64_t metadata_bytes = sizeof(PrivateMetadataLocal);
  regions_.FreeOwnerPrivate(value, value_bytes, partition_id_, owner_shard_);
  regions_.FreeOwnerPrivate(metadata, metadata_bytes, partition_id_, owner_shard_);
}

uint64_t KVPartition::NextCommitTid(uint64_t observed_tid) const {
  return star::TwoPLPashaHelper::kv_next_commit_tid(
      observed_tid, CurrentKvWorkerMaxTid());
}

star::RowOutcome KVPartition::PutPrivate(std::string_view key,
                                         std::string_view value) {
  if (value.size() != fixed_value_size_)
    throw std::invalid_argument("private value must match fixed value size");
  const FixedKey fixed_key = MakeKey(key);
  RegionOffset row_offset = kNullOffset;
  if (LookupPrivateOffset(fixed_key, &row_offset)) {
    auto *private_value = ValueFromOffset(row_offset);
    auto *metadata = MetadataFromValue(private_value);
    // Reuse the offset-adapted original local write-lock primitive for the
    // private branch.  A migrated row is deliberately reported separately,
    // just as the helper's PrivateRowView contract requires its SCC path.
    bool write_locked = false;
    bool migrated = false;
    const uint64_t observed_tid =
        star::TwoPLPashaHelper::take_write_lock(*metadata, write_locked,
                                                &migrated);
    if (write_locked) {
      const uint64_t new_tid = NextCommitTid(observed_tid);
      std::memset(private_value->data, 0, fixed_value_size_);
      std::memcpy(private_value->data, value.data(), value.size());
      mem_access::PrivateWrite(private_value->data, fixed_value_size_);
      metadata->is_data_modified_since_moved_out = true;
      star::TwoPLPashaHelper::write_lock_release(
          *metadata, new_tid, fixed_value_size_, owner_shard_,
          [this](const PrivateMetadataLocal &local) {
            return SharedMetadataFromOffset(local.migrated_smeta_off);
          });
      return star::RowOutcome::kDone;
    }

    if (!migrated) {
      LockRow(metadata);
      const bool valid = metadata->is_valid;
      UnlockRow(metadata);
      // An invalid existing leaf is the owner-create placeholder, not a
      // stable absence. Preserve the former primitive's Busy outcome so the
      // only facade retry boundary reruns the whole operation.
      if (!valid) return star::RowOutcome::kBusy;
      return star::RowOutcome::kBusy;
    }

    // This is the original owner migrated-row branch: acquire the local
    // metadata latch and the shared write lock as one lock sequence.  Do not
    // first report `is_migrated` and then reacquire the locator: Clock
    // move-out may otherwise clear that locator between the two attempts.
    OwnerNextRowLock locked_row;
    if (!AcquireOwnerNextRowWriteLock(private_value, &locked_row))
      return star::RowOutcome::kBusy;
    const uint64_t new_tid = NextCommitTid(locked_row.observed_tid);

    LockRow(metadata);
    const RegionOffset smeta_offset = metadata->migrated_smeta_off;
    if (smeta_offset == kNullOffset) {
      UnlockRow(metadata);
      ReleaseOwnerNextRowWriteLock(locked_row, 0, false);
      throw std::runtime_error("owner migrated write locator vanished");
    }
    auto *smeta = SharedMetadataFromOffset(smeta_offset);
    UnlockRow(metadata);
    if (!star::TwoPLPashaHelper::remote_write_lock_update_and_release(
            smeta, owner_shard_, value.data(), fixed_value_size_, new_tid)) {
      star::TwoPLPashaHelper::remote_write_lock_abort(smeta);
      return star::RowOutcome::kBusy;
    }
    LockRow(metadata);
    metadata->is_data_modified_since_moved_out = true;
    UnlockRow(metadata);
    return star::RowOutcome::kDone;
  }
    if (!CreatePrivateWithOwnerInsert(fixed_key, value)) {
      // The original insert helper leaves no placeholder when it cannot lock
      // the successor or loses the tree create race.  This primitive does not
      // own a second operation retry policy: surface Busy and let the single
      // KVStore facade boundary retry the complete operation.
      return star::RowOutcome::kBusy;
    }
    return star::RowOutcome::kDone;
}

StatusCode KVPartition::InsertRemotePlaceholder(std::string_view key,
                                                std::string_view value,
                                                uint32_t requester_id) {
  if (value.size() != fixed_value_size_)
    throw std::invalid_argument("remote insert value must match fixed value size");
  const FixedKey fixed_key = MakeKey(key);
  auto *table = private_table_.get();
  if (table == nullptr) return StatusCode::kCorruption;

  // This is the require_lock_next_key=false branch of the original
  // insert_and_update_next_key_info.  The ITable callback retains the exact
  // adjacent leaves while these migrated-neighbour bits are cleared.
  const bool inserted = star::TwoPLPashaHelper::insert_and_update_next_key_info(
      table, &fixed_key, value.data(),
      [&](const void *, std::atomic<uint64_t> *prev_meta, void *, const void *,
          std::atomic<uint64_t> *next_meta, void *) {
        // ITable supplies ValueStruct::meta rather than resolved local
        // metadata; converting those two transient callback arguments is the
        // only adapter before the original adjacency primitive.
        auto *prev = prev_meta == nullptr
            ? nullptr
            : MetadataFromValue(
                  reinterpret_cast<PrivateValueStruct *>(prev_meta));
        auto *next = next_meta == nullptr
            ? nullptr
            : MetadataFromValue(
                  reinterpret_cast<PrivateValueStruct *>(next_meta));
        star::TwoPLPashaHelper::clear_adjacent_migrated_rows(
            prev, next,
            [this](const PrivateMetadataLocal &local) {
              return SharedMetadataFromOffset(local.migrated_smeta_off);
            });
        return true;
      });
  if (!inserted) {
    RegionOffset existing_offset = kNullOffset;
    if (LookupPrivateOffset(fixed_key, &existing_offset) &&
        existing_offset != kNullOffset)
      return StatusCode::kAlreadyExists;
    return StatusCode::kBusy;
  }

  RegionOffset placeholder_offset = kNullOffset;
  if (!LookupPrivateOffset(fixed_key, &placeholder_offset) ||
      placeholder_offset == kNullOffset)
    throw std::runtime_error("remote insert placeholder was not indexed");

  // The original handler owns this placeholder until move_row_in(..., true)
  // has produced the requester reference carried by its response.  Keep that
  // ownership local: allocator failure before that point must not strand an
  // invalid owner row in the private tree.
  const auto rollback_unpublished_placeholder = [&] {
    const bool removed = star::TwoPLPashaHelper::delete_and_update_next_key_info(
        table, &fixed_key,
        [&](const void *prev_key, void *prev_meta, void *prev_data,
            const void *cur_key, void *cur_meta, void *cur_data,
            const void *next_key, void *next_meta, void *next_data) {
          (void)prev_key;
          (void)prev_data;
          (void)next_key;
          (void)next_data;
          if (cur_key == nullptr || cur_meta == nullptr || cur_data == nullptr)
            throw std::runtime_error("remote insert rollback lost placeholder");
          const auto &current_key = *static_cast<const FixedKey *>(cur_key);
          if (current_key.Compare(fixed_key) != 0)
            throw std::runtime_error("remote insert rollback key mismatch");
          auto *current_value = reinterpret_cast<PrivateValueStruct *>(
              static_cast<char *>(cur_data) - sizeof(PrivateValueStruct));
          if (regions_.ToOwnerPrivateOffset(current_value, partition_id_) !=
              placeholder_offset)
            throw std::runtime_error("remote insert rollback row mismatch");
          auto *metadata = static_cast<PrivateMetadataLocal *>(cur_meta);
          LockRow(metadata);
          const bool valid = metadata->is_valid;
          const bool migrated = metadata->is_migrated;
          UnlockRow(metadata);
          if (valid || migrated)
            throw std::runtime_error(
                "remote insert rollback found a published placeholder");
          star::TwoPLPashaHelper::set_adjacent_migrated_rows(
              static_cast<PrivateMetadataLocal *>(prev_meta),
              static_cast<PrivateMetadataLocal *>(next_meta),
              [this](const PrivateMetadataLocal &local) {
                return SharedMetadataFromOffset(local.migrated_smeta_off);
              });
          return true;
        });
    if (!removed)
      throw std::runtime_error("remote insert placeholder rollback failed");
  };

  // The original remote-insert owner moves the invalid placeholder in with a
  // requester ref.  That ref spans this response and is consumed only by the
  // requester's remote_modify_tuple_valid_bit analogue below.
  star::TwoPLPashaMetadataShared *pinned = nullptr;
  // The original handler deliberately ignores move_row_in's already-shared
  // result.  A concurrent DATA_MIGRATION_REQUEST may have published this
  // invalid placeholder first; PromotePrivate then returns false but hands us
  // the requester ref that remote publication must consume.
  star::migration_result result = star::migration_result::FAIL_OOM;
  try {
    result = PromotePrivate(key, requester_id, &pinned);
  } catch (...) {
    rollback_unpublished_placeholder();
    throw;
  }

  if (result == star::migration_result::FAIL_ALREADY_IN_CXL) {
    // Master ignores move_row_in's already-shared result after creating this
    // invalid placeholder: a concurrent DATA_MIGRATION_REQUEST (e.g. Scan
    // range move-in) may have published the same row first. PromotePrivate
    // with inc_ref must still hand back the requester pin that publication
    // consumes. Already-shared without that pin is the only invariant break.
    if (pinned == nullptr)
      throw std::runtime_error(
          "remote insert already-shared without requester pin");
    return StatusCode::kOk;
  }
  if (result == star::migration_result::SUCCESS)
    return StatusCode::kOk;
  if (pinned != nullptr)
    throw std::runtime_error("remote insert move-in returned an unexplained pin");

  // move_row_in failed before a remote-visible placeholder was acknowledged.
  rollback_unpublished_placeholder();
  if (result == star::migration_result::FAIL_OOM)
    return StatusCode::kOutOfMemory;
  throw std::runtime_error("remote insert move-in returned unknown result");
}

bool KVPartition::PublishRemotePlaceholder(std::string_view key,
                                           uint32_t requester_id) {
  // The original CXLTable leaf remains valid while the SCC valid bit is
  // pending remote-insert publication, so its normal CXLTable search is the
  // correct lookup here as well.
  const FixedKey fixed_key = MakeKey(key);
  RegionOffset smeta_offset = kNullOffset;
  if (!LookupSharedReference(fixed_key, &smeta_offset)) return false;
  auto *smeta = SharedMetadataFromOffset(smeta_offset);
  return star::TwoPLPashaHelper::remote_modify_tuple_valid_bit(
      smeta, requester_id, SharedSccBytes(0), /*is_valid=*/true,
      /*is_insert=*/true, /*consume_ref=*/true,
      /*mark_shared_dirty=*/true);
}

star::RowOutcome KVPartition::GetPrivate(std::string_view key,
                                         std::string *value) const {
  RegionOffset row_offset = kNullOffset;
  if (!LookupPrivateOffset(MakeKey(key), &row_offset))
    return star::RowOutcome::kMissing;
  auto *private_value = ValueFromOffset(row_offset);
  auto *metadata = MetadataFromValue(private_value);
  // The non-migrated branch is the original take_read_lock_and_read / release
  // sequence with ValueStruct::meta resolved from an owner-private offset.
  std::string local(fixed_value_size_, '\0');
  bool local_success = false;
  bool migrated = false;
  star::TwoPLPashaMetadataShared *shared_locked = nullptr;
  star::TwoPLPashaHelper::take_read_lock_and_read(
      *metadata, private_value->data, local.data(), local.size(), local_success,
      [this](const PrivateMetadataLocal &local) {
        return SharedMetadataFromOffset(local.migrated_smeta_off);
      },
      owner_shard_, &shared_locked, &migrated);
  if (local_success) {
    mem_access::PrivateRead(private_value->data, fixed_value_size_);
    if (shared_locked != nullptr) {
      // The original migrated branch owns one shared reader pin.  Keep Clock
      // access inside that pin's lifetime, then release at the latency-safe
      // remote primitive boundary.
      star::TwoPLPashaHelper::remote_read_lock_release(
          shared_locked, /*dec_ref_cnt=*/false);
    } else {
      star::TwoPLPashaHelper::read_lock_release(
          *metadata,
          [this](const PrivateMetadataLocal &local) {
            return SharedMetadataFromOffset(local.migrated_smeta_off);
          });
    }
    *value = std::move(local);
    return star::RowOutcome::kDone;
  }
  if (!migrated) {
    // take_read_lock_and_read deliberately has one failure return
    // for an invalid row and for ordinary reader/writer contention.  Recheck
    // under the owner-private latch before mapping it to the KV API: a live
    // non-migrated row can only be Busy, never NotFound.
    LockRow(metadata);
    const bool valid = metadata->is_valid;
    const bool now_migrated = metadata->is_migrated;
    UnlockRow(metadata);
    if (!valid) return star::RowOutcome::kMissing;
    if (!now_migrated)
      return star::RowOutcome::kBusy;
  }
  // Contention under latch: surface Busy rather than NotFound (§10.1/§10.2).
  return star::RowOutcome::kBusy;
}

SharedAccessState KVPartition::GetShared(std::string_view key, uint32_t host_id,
                                           std::string *value,
                                           bool record_clock_access) const {
  if (value == nullptr) throw std::invalid_argument("null shared GET output");
  const FixedKey fixed_key = MakeKey(key);
  star::TwoPLPashaMetadataShared *smeta = nullptr;
  const SharedAccessState pin =
      TryPinShared(fixed_key, &smeta, record_clock_access);
  if (pin != SharedAccessState::kDone) return pin;
  std::string shared(fixed_value_size_, '\0');
  star::RowOutcome read_result = star::RowOutcome::kBusy;
  bool read = false;
  // TryPinShared is the get_migrated_row(ref=true) equivalent, so the
  // original reader primitive must not take a second ref here.
  star::TwoPLPashaHelper::remote_take_read_lock_and_read(
      smeta, host_id, shared.data(), shared.size(), /*inc_ref_cnt=*/false,
      read, &read_result);
  if (read)
    star::TwoPLPashaHelper::remote_read_lock_release(
        smeta, /*dec_ref_cnt=*/false);
  star::TwoPLPashaHelper::release_migrated_row(smeta);
  if (!read) {
    if (read_result == star::RowOutcome::kMissing)
      return SharedAccessState::kMissing;
    return SharedAccessState::kRetry;
  }
  *value = std::move(shared);
  return SharedAccessState::kDone;
}

SharedAccessState KVPartition::PutShared(std::string_view key, uint32_t host_id,
                                         std::string_view value,
                                         bool record_clock_access) {
  if (value.size() != fixed_value_size_)
    throw std::invalid_argument("shared value must match fixed value size");
  const FixedKey fixed_key = MakeKey(key);
  star::TwoPLPashaMetadataShared *smeta = nullptr;
  const SharedAccessState pin =
      TryPinShared(fixed_key, &smeta, record_clock_access);
  if (pin != SharedAccessState::kDone) return pin;
  bool locked = false;
  const uint64_t observed_tid =
      star::TwoPLPashaHelper::remote_take_write_lock_and_read(
          smeta, host_id, nullptr, fixed_value_size_,
          /*inc_ref_cnt=*/false, locked, nullptr,
          /*allow_invalid=*/true);
  bool written = false;
  if (locked) {
    written = star::TwoPLPashaHelper::remote_write_lock_update_and_release(
        smeta, host_id, value.data(), fixed_value_size_,
        NextCommitTid(observed_tid),
        /*dec_ref_cnt=*/false, /*allow_invalid=*/true);
    if (!written)
      star::TwoPLPashaHelper::remote_write_lock_abort(
          smeta, /*dec_ref_cnt=*/false);
  }
  star::TwoPLPashaHelper::release_migrated_row(smeta);
  return written ? SharedAccessState::kDone : SharedAccessState::kRetry;
}

SharedAccessState KVPartition::PrepareRemoteDelete(
    std::string_view key, uint32_t host_id,
    star::TwoPLPashaMetadataShared **locked_row,
    bool record_clock_access) {
  if (locked_row == nullptr)
    throw std::invalid_argument("null remote delete lock output");
  *locked_row = nullptr;
  star::TwoPLPashaMetadataShared *smeta = nullptr;
  const SharedAccessState pin =
      TryPinShared(MakeKey(key), &smeta, record_clock_access);
  if (pin != SharedAccessState::kDone) return pin;
  const auto prepared = star::TwoPLPashaHelper::prepare_remote_delete(
      smeta, host_id, SharedSccBytes(0));
  if (prepared != star::RowOutcome::kDone) {
    star::TwoPLPashaHelper::release_migrated_row(smeta);
    return prepared == star::RowOutcome::kMissing
               ? SharedAccessState::kMissing
               : SharedAccessState::kRetry;
  }
  *locked_row = smeta;
  return SharedAccessState::kDone;
}

void KVPartition::AbortRemoteDelete(
    star::TwoPLPashaMetadataShared *locked_row, uint32_t host_id) {
  if (locked_row == nullptr) return;
  star::TwoPLPashaHelper::abort_remote_delete(
      locked_row, host_id, SharedSccBytes(0));
}

SharedAccessState KVPartition::CompareExchangeShared(
    std::string_view key, uint32_t host_id, std::string_view expected,
    std::string_view desired, bool *exchanged, bool record_clock_access) {
  if (exchanged == nullptr) throw std::invalid_argument("null shared CAS result");
  if (desired.size() != fixed_value_size_ ||
      (!expected.empty() && expected.size() != fixed_value_size_))
    throw std::invalid_argument("shared CAS values must match fixed value size");
  *exchanged = false;
  const FixedKey fixed_key = MakeKey(key);
  star::TwoPLPashaMetadataShared *smeta = nullptr;
  const SharedAccessState pin =
      TryPinShared(fixed_key, &smeta, record_clock_access);
  if (pin != SharedAccessState::kDone) return pin;
  std::string current(fixed_value_size_, '\0');
  bool write_locked = false;
  const uint64_t observed_tid =
      star::TwoPLPashaHelper::remote_take_write_lock_and_read(
          smeta, host_id, current.data(), current.size(),
          /*inc_ref_cnt=*/false, write_locked);
  if (!write_locked) {
    star::TwoPLPashaHelper::release_migrated_row(smeta);
    return SharedAccessState::kRetry;
  }
  bool changed = false;
  if (current == expected) {
    changed = true;
    if (!star::TwoPLPashaHelper::remote_write_lock_update_and_release(
            smeta, host_id, desired.data(), desired.size(),
            NextCommitTid(observed_tid),
            /*dec_ref_cnt=*/false)) {
      star::TwoPLPashaHelper::remote_write_lock_abort(
          smeta, /*dec_ref_cnt=*/false);
      star::TwoPLPashaHelper::release_migrated_row(smeta);
      return SharedAccessState::kRetry;
    }
  } else {
    star::TwoPLPashaHelper::remote_write_lock_abort(
        smeta, /*dec_ref_cnt=*/false);
  }
  if (!changed) {
    star::TwoPLPashaHelper::release_migrated_row(smeta);
    *exchanged = false;
    return SharedAccessState::kDone;
  }
  *exchanged = changed;
  star::TwoPLPashaHelper::release_migrated_row(smeta);
  return SharedAccessState::kDone;
}

SharedAccessState KVPartition::IncrementShared(std::string_view key,
                                               uint32_t host_id, int64_t delta,
                                               int64_t *value,
                                               bool record_clock_access) {
  if (value == nullptr) throw std::invalid_argument("null shared increment output");
  const FixedKey fixed_key = MakeKey(key);
  star::TwoPLPashaMetadataShared *smeta = nullptr;
  const SharedAccessState pin =
      TryPinShared(fixed_key, &smeta, record_clock_access);
  if (pin != SharedAccessState::kDone) return pin;
  std::string current(fixed_value_size_, '\0');
  bool write_locked = false;
  const uint64_t observed_tid =
      star::TwoPLPashaHelper::remote_take_write_lock_and_read(
          smeta, host_id, current.data(), current.size(),
          /*inc_ref_cnt=*/false, write_locked);
  if (!write_locked) {
    star::TwoPLPashaHelper::release_migrated_row(smeta);
    return SharedAccessState::kRetry;
  }
  int64_t next = 0;
  int64_t previous = 0;
  if (!DecodeCanonicalFixedDecimal(current, &previous) ||
      (delta > 0 && previous > std::numeric_limits<int64_t>::max() - delta) ||
      (delta < 0 && previous < std::numeric_limits<int64_t>::min() - delta)) {
    star::TwoPLPashaHelper::remote_write_lock_abort(
        smeta, /*dec_ref_cnt=*/false);
    star::TwoPLPashaHelper::release_migrated_row(smeta);
    throw std::invalid_argument(
        "increment requires a non-overflowing int64 value");
  }
  next = previous + delta;
  std::string replacement;
  if (!EncodeCanonicalFixedDecimal(next, fixed_value_size_, &replacement)) {
    star::TwoPLPashaHelper::remote_write_lock_abort(
        smeta, /*dec_ref_cnt=*/false);
    star::TwoPLPashaHelper::release_migrated_row(smeta);
    throw std::invalid_argument("increment value exceeds fixed value size");
  }
  if (!star::TwoPLPashaHelper::remote_write_lock_update_and_release(
          smeta, host_id, replacement.data(), replacement.size(),
          NextCommitTid(observed_tid),
          /*dec_ref_cnt=*/false)) {
    star::TwoPLPashaHelper::remote_write_lock_abort(
        smeta, /*dec_ref_cnt=*/false);
    star::TwoPLPashaHelper::release_migrated_row(smeta);
    return SharedAccessState::kRetry;
  }
  *value = next;
  star::TwoPLPashaHelper::release_migrated_row(smeta);
  return SharedAccessState::kDone;
}

star::RowOutcome KVPartition::CompareExchangePrivate(
    std::string_view key, std::string_view expected, std::string_view desired,
    bool *exchanged, bool *inserted) {
  if (exchanged == nullptr) throw std::invalid_argument("null CAS result");
  if (desired.size() != fixed_value_size_ ||
      (!expected.empty() && expected.size() != fixed_value_size_))
    throw std::invalid_argument("CAS values must match fixed value size");
  *exchanged = false;
  if (inserted != nullptr) *inserted = false;
  RegionOffset row_offset = kNullOffset;
  const FixedKey fixed_key = MakeKey(key);
  if (!LookupPrivateOffset(fixed_key, &row_offset)) {
    if (!expected.empty()) return star::RowOutcome::kMissing;
    if (CreatePrivateWithOwnerInsert(fixed_key, desired)) {
      *exchanged = true;
      if (inserted != nullptr) *inserted = true;
      return star::RowOutcome::kDone;
    }
    // Loser of create race: re-resolve the winner and compare expected.
    if (!LookupPrivateOffset(fixed_key, &row_offset))
      return star::RowOutcome::kBusy;
  }
  auto *private_value = ValueFromOffset(row_offset);
  auto *metadata = MetadataFromValue(private_value);
  bool write_locked = false;
  star::TwoPLPashaMetadataShared *shared_locked = nullptr;
  std::string current(fixed_value_size_, '\0');
  const uint64_t observed_tid =
      star::TwoPLPashaHelper::take_write_lock(
          *metadata, private_value->data, fixed_value_size_, write_locked,
          [this](const PrivateMetadataLocal &local) {
            return SharedMetadataFromOffset(local.migrated_smeta_off);
          }, owner_shard_, &shared_locked);
  if (!write_locked) {
    // The original lmeta→smeta primitive already classified this attempt by
    // the lock it could not acquire. Do not re-read the locator after it
    // returns; the facade retries the complete operation at its boundary.
    return star::RowOutcome::kBusy;
  }
  if (shared_locked == nullptr) {
    mem_access::PrivateRead(private_value->data, fixed_value_size_);
    std::memcpy(current.data(), private_value->data, fixed_value_size_);
    if (current == expected) {
      std::memcpy(private_value->data, desired.data(), desired.size());
      mem_access::PrivateWrite(private_value->data, fixed_value_size_);
      metadata->is_data_modified_since_moved_out = true;
      *exchanged = true;
    }
    star::TwoPLPashaHelper::write_lock_release(
        *metadata, *exchanged
            ? NextCommitTid(observed_tid)
            : observed_tid, fixed_value_size_, owner_shard_,
        [this](const PrivateMetadataLocal &local) {
          return SharedMetadataFromOffset(local.migrated_smeta_off);
        });
    return star::RowOutcome::kDone;
  }
  mem_access::PrivateRead(private_value->data, fixed_value_size_);
  std::memcpy(current.data(), private_value->data, fixed_value_size_);
  const bool changed = current == expected;
  if (changed) {
    if (!star::TwoPLPashaHelper::remote_write_lock_update_and_release(
            shared_locked, owner_shard_, desired.data(), desired.size(),
            NextCommitTid(observed_tid),
            /*dec_ref_cnt=*/false)) {
      star::TwoPLPashaHelper::remote_write_lock_abort(
          shared_locked, /*dec_ref_cnt=*/false);
      return star::RowOutcome::kBusy;
    }
  } else {
    star::TwoPLPashaHelper::remote_write_lock_abort(
        shared_locked, /*dec_ref_cnt=*/false);
    return star::RowOutcome::kDone;
  }
  LockRow(metadata);
  metadata->is_data_modified_since_moved_out = true;
  *exchanged = true;
  UnlockRow(metadata);
  return star::RowOutcome::kDone;
}

star::RowOutcome KVPartition::IncrementPrivate(std::string_view key,
                                               int64_t delta, int64_t *value,
                                               bool *inserted) {
  if (value == nullptr) throw std::invalid_argument("null increment result");
  if (inserted != nullptr) *inserted = false;
  RegionOffset row_offset = kNullOffset;
  const FixedKey fixed_key = MakeKey(key);
  if (!LookupPrivateOffset(fixed_key, &row_offset)) {
    std::string encoded;
    if (!EncodeCanonicalFixedDecimal(delta, fixed_value_size_, &encoded))
      throw std::invalid_argument("increment value exceeds fixed value size");
    if (CreatePrivateWithOwnerInsert(fixed_key, encoded)) {
      *value = delta;
      if (inserted != nullptr) *inserted = true;
      return star::RowOutcome::kDone;
    }
    // Loser of create race: apply delta on the published row (§10.9).
    if (!LookupPrivateOffset(fixed_key, &row_offset))
      return star::RowOutcome::kBusy;
  }
  auto *private_value = ValueFromOffset(row_offset);
  auto *metadata = MetadataFromValue(private_value);
  bool write_locked = false;
  star::TwoPLPashaMetadataShared *shared_locked = nullptr;
  std::string current(fixed_value_size_, '\0');
  const uint64_t observed_tid =
      star::TwoPLPashaHelper::take_write_lock(
          *metadata, private_value->data, fixed_value_size_, write_locked,
          [this](const PrivateMetadataLocal &local) {
            return SharedMetadataFromOffset(local.migrated_smeta_off);
          }, owner_shard_, &shared_locked);
  if (!write_locked) {
    return star::RowOutcome::kBusy;
  }
  mem_access::PrivateRead(private_value->data, fixed_value_size_);
  std::memcpy(current.data(), private_value->data, fixed_value_size_);
  if (shared_locked != nullptr) {
    int64_t next = 0;
    int64_t previous = 0;
    if (!DecodeCanonicalFixedDecimal(current, &previous) ||
        (delta > 0 && previous > std::numeric_limits<int64_t>::max() - delta) ||
        (delta < 0 && previous < std::numeric_limits<int64_t>::min() - delta)) {
      star::TwoPLPashaHelper::remote_write_lock_abort(
          shared_locked, /*dec_ref_cnt=*/false);
      throw std::invalid_argument("increment requires a non-overflowing int64 value");
    }
    next = previous + delta;
    std::string encoded;
    if (!EncodeCanonicalFixedDecimal(next, fixed_value_size_, &encoded)) {
      star::TwoPLPashaHelper::remote_write_lock_abort(
          shared_locked, /*dec_ref_cnt=*/false);
      throw std::invalid_argument("increment value exceeds fixed value size");
    }
    if (!star::TwoPLPashaHelper::remote_write_lock_update_and_release(
            shared_locked, owner_shard_, encoded.data(), encoded.size(),
            NextCommitTid(observed_tid),
            /*dec_ref_cnt=*/false)) {
      star::TwoPLPashaHelper::remote_write_lock_abort(
          shared_locked, /*dec_ref_cnt=*/false);
      return star::RowOutcome::kBusy;
    }
    LockRow(metadata);
    metadata->is_data_modified_since_moved_out = true;
    *value = next;
    UnlockRow(metadata);
    return star::RowOutcome::kDone;
  } else {
    int64_t previous = 0;
    if (!DecodeCanonicalFixedDecimal(current, &previous) ||
        (delta > 0 && previous > std::numeric_limits<int64_t>::max() - delta) ||
        (delta < 0 && previous < std::numeric_limits<int64_t>::min() - delta)) {
      star::TwoPLPashaHelper::write_lock_release(
          *metadata, observed_tid, fixed_value_size_, owner_shard_,
          [this](const PrivateMetadataLocal &local) {
            return SharedMetadataFromOffset(local.migrated_smeta_off);
          });
      throw std::invalid_argument(
          "increment requires a non-overflowing int64 value");
    }
    const int64_t next = previous + delta;
    std::string encoded;
    if (!EncodeCanonicalFixedDecimal(next, fixed_value_size_, &encoded)) {
      star::TwoPLPashaHelper::write_lock_release(
          *metadata, observed_tid, fixed_value_size_, owner_shard_,
          [this](const PrivateMetadataLocal &local) {
            return SharedMetadataFromOffset(local.migrated_smeta_off);
          });
      throw std::invalid_argument("increment value exceeds fixed value size");
    }
    std::memcpy(private_value->data, encoded.data(), encoded.size());
    mem_access::PrivateWrite(private_value->data, fixed_value_size_);
    metadata->is_data_modified_since_moved_out = true;
    *value = next;
    star::TwoPLPashaHelper::write_lock_release(
        *metadata, NextCommitTid(observed_tid),
        fixed_value_size_, owner_shard_,
        [this](const PrivateMetadataLocal &local) {
          return SharedMetadataFromOffset(local.migrated_smeta_off);
        });
    return star::RowOutcome::kDone;
  }
}

StatusCode KVPartition::EnsureInShared(std::string_view key, uint32_t host_id,
                                       bool *moved_in) {
  (void)host_id;
  if (moved_in != nullptr) *moved_in = false;
  if (star::scc_manager == nullptr) return StatusCode::kOutOfMemory;
  const FixedKey fixed_key = MakeKey(key);
  star::migration_result result = star::migration_result::FAIL_OOM;
  if (star::migration_manager != nullptr) {
    auto *table = private_table_.get();
    if (table == nullptr) return StatusCode::kOutOfMemory;
    // Preserve the original PolicyClock contract: its move-in callback owns
    // one already-resolved local row, which is then recorded by the tracker.
    // A stable miss is classified before the callback; no post-publication
    // lookup is needed to synthesize tracker state.
    const auto row = table->search(&fixed_key);
    if (std::get<0>(row) == nullptr || std::get<1>(row) == nullptr)
      return StatusCode::kNotFound;
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
  if (absent) return StatusCode::kNotFound;
  // A concurrent owner create can publish the local row after the original
  // B+Tree lookup miss. This is ordinary create/migrate contention, not OOM;
  // the single facade Busy retry owns the next complete attempt.
  return StatusCode::kBusy;
}

star::migration_result KVPartition::PromotePrivate(std::string_view key,
                                                    uint32_t host_id) {
  return PromotePrivate(key, host_id, nullptr);
}

star::migration_result KVPartition::PromotePrivate(
    std::string_view key, uint32_t host_id,
    star::TwoPLPashaMetadataShared **pinned_existing) {
  (void)host_id;
  if (pinned_existing != nullptr) *pinned_existing = nullptr;
  if (star::scc_manager == nullptr)
    return star::migration_result::FAIL_OOM;
  const FixedKey fixed_key = MakeKey(key);
  const bool inc_ref = pinned_existing != nullptr;
  star::migration_result result = star::migration_result::FAIL_OOM;
  if (star::migration_manager != nullptr) {
    auto *table = private_table_.get();
    if (table == nullptr) return star::migration_result::FAIL_OOM;
    // PolicyClock is the original move_row_in caller and records this exact
    // ITable row in its owner-private tracker after the helper publishes it.
    // Passing a synthetic null row lets publication succeed but makes track()
    // fail afterwards, leaving remote-insert rollback to see a published
    // placeholder. Reuse the real TableBTreeOLC result as EnsureInShared does.
    const auto row = table->search(&fixed_key);
    if (std::get<0>(row) == nullptr || std::get<1>(row) == nullptr)
      return star::migration_result::FAIL_OOM;
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
        *pinned_existing = SharedMetadataFromOffset(metadata->migrated_smeta_off);
      }
      UnlockRow(metadata);
    }
    // Move-in already pinned; if the private locator raced away, recover the
    // smeta from the shared tree so the caller can unpin (§10.8).
    if (*pinned_existing == nullptr) {
      RegionOffset shared_offset = kNullOffset;
      if (LookupSharedReference(fixed_key, &shared_offset) &&
          shared_offset != kNullOffset) {
        *pinned_existing = SharedMetadataFromOffset(shared_offset);
      }
    }
    if (*pinned_existing == nullptr) {
      // Pin was taken but no resolvable smeta remains — should not happen
      // while ref_cnt blocks move-out; treat as hard failure rather than leak.
      throw std::runtime_error("move-in pin without resolvable smeta");
    }
  }
  return result;
}

star::migration_result KVPartition::MoveInForMigrationManager(
    const void *key, bool inc_ref_cnt, void *&migration_policy_meta) {
  migration_policy_meta = nullptr;
  if (star::scc_manager == nullptr) return star::migration_result::FAIL_OOM;
  const FixedKey fixed_key = MakeKey(std::string_view(
      static_cast<const char *>(key), fixed_key_size_));
  const bool terminal_sentinel =
      IsInternalMaxSentinel(fixed_key, fixed_key_size_);
  auto *table = private_table_.get();
  if (table == nullptr) return star::migration_result::FAIL_OOM;
  star::migration_result result = star::migration_result::FAIL_OOM;
  const bool found = table->search_and_update_next_key_info(
      &fixed_key,
      [&](const void *prev_key, void *prev_meta, void *prev_data,
          const void *cur_key, void *cur_meta, void *cur_data,
          const void *next_key, void *next_meta, void *next_data) {
        (void)prev_key;
        (void)next_key;
        (void)prev_data;
        (void)next_data;
        if (cur_key == nullptr || cur_meta == nullptr || cur_data == nullptr) {
          result = star::migration_result::FAIL_OOM;
          return;
        }
        auto *prev_lmeta = static_cast<PrivateMetadataLocal *>(prev_meta);
        auto *metadata = static_cast<PrivateMetadataLocal *>(cur_meta);
        auto *next_lmeta = static_cast<PrivateMetadataLocal *>(next_meta);
        auto *private_value = reinterpret_cast<PrivateValueStruct *>(
            static_cast<char *>(cur_data) - sizeof(PrivateValueStruct));
        bool prev_migrated = false;
        bool next_migrated = false;
        if (prev_lmeta != nullptr) {
          LockRow(prev_lmeta);
          prev_migrated = prev_lmeta->is_migrated;
          UnlockRow(prev_lmeta);
        }
        if (next_lmeta != nullptr) {
          LockRow(next_lmeta);
          next_migrated = next_lmeta->is_migrated;
          UnlockRow(next_lmeta);
        }
        const auto update_neighbors = [&] {
          star::TwoPLPashaHelper::set_adjacent_migrated_rows(
              prev_lmeta, next_lmeta,
              [this](const PrivateMetadataLocal &local) {
                return SharedMetadataFromOffset(local.migrated_smeta_off);
              });
        };
        LockRow(metadata);
  if (metadata->is_migrated) {
    // Match the original already-migrated branch: it repairs adjacency even
    // when a remote-insert owner mirror has not yet consumed smeta validity.
    // Only a new move-in may reject an invalid private placeholder.
    if (metadata->migrated_smeta_off == kNullOffset) {
      UnlockRow(metadata);
      throw std::runtime_error("already-migrated row has null HWCC smeta offset");
    }
    auto *smeta = SharedMetadataFromOffset(metadata->migrated_smeta_off);
    if (inc_ref_cnt) {
      // Caller (PromotePrivate with pinned_existing) will unpin exactly once.
      // Only report FAIL_ALREADY_IN_CXL when the pin is actually held; a failed
      // pin must not hand back smeta or RelWithDebInfo uint8_t ref_cnt wraps
      // 0→255 and saturates every later shared read/write (YCSB Forward stall).
      // A concurrent DATA_MIGRATION_REQUEST may have moved this remote-insert
      // placeholder before its owner handler reaches move_row_in(true).  The
      // original already-migrated branch increments the requester ref even
      // while the placeholder is invalid; its later publish consumes that ref.
      if (!star::TwoPLPashaHelper::get_migrated_row(
              smeta, owner_shard_, fixed_value_size_,
              /*allow_invalid=*/!metadata->is_valid)) {
        UnlockRow(metadata);
        result = star::migration_result::FAIL_OOM;
        return;
      }
      migration_policy_meta = smeta;
    }
    // Master assumes current next/prev bits are already correct and only
    // lazily updates neighbors. Insert's clear_adjacent can leave this row
    // with next_real=0 while its private successor is already migrated; a
    // scan range re-move-in must restore current bits from the observed
    // neighbor migration state or the CXL probe Busy-livelocks on that hole.
    smeta->lock();
    if (prev_migrated)
      smeta->set_prev_key_real_bit();
    else
      smeta->clear_prev_key_real_bit();
    if (next_migrated || terminal_sentinel)
      smeta->set_next_key_real_bit();
    else
      smeta->clear_next_key_real_bit();
    smeta->unlock();
    UnlockRow(metadata);
    update_neighbors();
    result = star::migration_result::FAIL_ALREADY_IN_CXL;
    return;
  }
  // Preserve the original B+Tree move-in path for an owner-private placeholder:
  // it may publish an invalid shared row while the remote creator has not yet
  // set the shared valid bit.  A concurrent reader then observes the existing
  // shared-row validity/retry protocol instead of receiving a fabricated OOM.
  const uint64_t payload_bytes = SharedSccBytes(fixed_value_size_);
  void *payload_mem = nullptr;
  void *smeta_mem = nullptr;
  // Preserve the original TwoPLPasha migration optimisation: a row owns one
  // SCC payload allocation across ordinary move-out/move-in cycles.  Only the
  // transient HWCC metadata is retired on move-out; Delete releases the
  // cached payload together with the owner-private row.
  const bool reuse_cached_payload = metadata->scc_data_off != kNullOffset;
  try {
    if (reuse_cached_payload) {
      payload_mem = regions_.ResolveSharedPayload(metadata->scc_data_off,
                                                  payload_bytes);
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
    UnlockRow(metadata);
    // Do not overload the original helper's FAIL_OOM sentinel: a true
    // allocator failure must reach the RPC boundary as StatusCode::kOutOfMemory.
    throw;
  }
  auto *payload = new (payload_mem) star::TwoPLPashaSharedDataSCC;
  auto *smeta = new (smeta_mem) star::TwoPLPashaMetadataShared(payload);
  // Keep the original migration lifecycle: policy metadata is initialized
  // before SCC setup and before the caller holds the shared-row latch.
  if (star::migration_manager != nullptr) {
    star::migration_manager->init_migration_policy_metadata(
        smeta, nullptr, key,
        std::tuple<std::atomic<uint64_t> *, void *>{nullptr, nullptr},
        sizeof(star::TwoPLPashaMetadataShared));
  }
  star::scc_manager->init_scc_metadata(smeta, owner_shard_);
  const RegionOffset smeta_offset =
      regions_.ToDynamicHwccOffset(smeta, owner_shard_);
  const auto publish_result =
      star::TwoPLPashaHelper::move_from_btree_to_shared_region(
          *metadata, private_value->data, smeta, payload, owner_shard_,
          fixed_value_size_,
          !reuse_cached_payload || metadata->is_data_modified_since_moved_out,
          inc_ref_cnt, prev_migrated, next_migrated || terminal_sentinel,
          [&] {
            if (!shared_table_->insert(&fixed_key, smeta)) {
              // The original helper DCHECKs this insertion: while the local
              // metadata latch says !is_migrated, a second index entry is
              // corruption rather than allocator exhaustion.
              RegionOffset existing_offset = kNullOffset;
              const bool existing = LookupSharedReference(fixed_key, &existing_offset);
              std::ostringstream detail;
              detail << "move-in shared-index insertion failed partition="
                     << partition_id_ << " owner=" << owner_shard_
                     << " local_migrated=" << metadata->is_migrated
                     << " local_smeta=" << metadata->migrated_smeta_off
                     << " new_smeta=" << smeta_offset << " indexed="
                     << existing << " indexed_smeta=" << existing_offset;
              throw std::runtime_error(detail.str());
            }
            metadata->migrated_smeta_off = smeta_offset;
            if (!reuse_cached_payload)
              metadata->scc_data_off = regions_.EncodeSharedPayloadOffset(
                  payload, owner_shard_);
            metadata->is_migrated = true;
          });
  if (publish_result == star::migration_result::FAIL_OOM) {
    UnlockRow(metadata);
    regions_.Free(smeta_mem, sizeof(star::TwoPLPashaMetadataShared),
                  AllocationDomain::kHwccMetadata, owner_shard_, owner_shard_);
    if (!reuse_cached_payload)
      regions_.Free(payload_mem, payload_bytes,
                    AllocationDomain::kSharedPayloadSwcc, owner_shard_,
                    owner_shard_);
    result = star::migration_result::FAIL_OOM;
    return;
  }
  migration_policy_meta = smeta;
  update_neighbors();
  UnlockRow(metadata);
  star::num_data_move_in.fetch_add(1, std::memory_order_relaxed);
  result = star::migration_result::SUCCESS;
  return;
      });
  return found ? result : star::migration_result::FAIL_OOM;
}

bool KVPartition::MoveOutForMigrationManager(const void *key) {
  return MoveOutPrivateRaw(
      std::string_view(static_cast<const char *>(key), fixed_key_size_),
      owner_shard_);
}

bool KVPartition::MoveOutPrivateRaw(std::string_view key, uint32_t host_id) {
  if (star::scc_manager == nullptr) return false;
  const FixedKey fixed_key = MakeKey(key);
  auto *table = private_table_.get();
  if (table == nullptr) return false;
  bool result = false;
  const bool found = table->search_and_update_next_key_info(
      &fixed_key,
      [&](const void *prev_key, void *prev_meta, void *prev_data,
          const void *cur_key, void *cur_meta, void *cur_data,
          const void *next_key, void *next_meta, void *next_data) {
        (void)prev_key;
        (void)next_key;
        (void)prev_data;
        (void)next_data;
        if (cur_key == nullptr || cur_meta == nullptr || cur_data == nullptr)
          return;
        auto *prev_lmeta = static_cast<PrivateMetadataLocal *>(prev_meta);
        auto *metadata = static_cast<PrivateMetadataLocal *>(cur_meta);
        auto *next_lmeta = static_cast<PrivateMetadataLocal *>(next_meta);
        auto *private_value = reinterpret_cast<PrivateValueStruct *>(
            static_cast<char *>(cur_data) - sizeof(PrivateValueStruct));
        star::TwoPLPashaHelper::clear_adjacent_migrated_rows(
            prev_lmeta, next_lmeta,
            [this](const PrivateMetadataLocal &local) {
              return SharedMetadataFromOffset(local.migrated_smeta_off);
            });
        LockRow(metadata);
  if (!metadata->is_migrated || metadata->migrated_smeta_off == kNullOffset) {
    UnlockRow(metadata);
    std::ostringstream detail;
    detail << "Clock move-out victim lost migrated locator partition="
           << partition_id_ << " owner=" << owner_shard_ << " migrated="
           << metadata->is_migrated << " smeta=" << metadata->migrated_smeta_off;
    throw std::runtime_error(detail.str());
  }
  const RegionOffset smeta_offset = metadata->migrated_smeta_off;
  RegionOffset indexed = kNullOffset;
  if (!LookupSharedReference(fixed_key, &indexed) || indexed != smeta_offset) {
    UnlockRow(metadata);
    std::ostringstream detail;
    detail << "move-out shared locator mismatch partition=" << partition_id_
           << " owner=" << owner_shard_ << " local_smeta=" << smeta_offset
           << " indexed_smeta=" << indexed;
    throw std::runtime_error(detail.str());
  }
  star::TwoPLPashaMetadataShared *smeta = nullptr;
  try {
    smeta = SharedMetadataFromOffset(smeta_offset);
  } catch (...) {
    UnlockRow(metadata);
    throw;
  }
  const auto move_result =
      star::TwoPLPashaHelper::move_from_btree_to_partition(
          *metadata, private_value->data, smeta, host_id, fixed_value_size_,
          [&]() {
            return shared_table_->remove(&fixed_key, nullptr);
          },
          [this](star::TwoPLPashaSharedDataSCC *,
                 star::TwoPLPashaMetadataShared *retired_smeta) {
            ebr_.add_retired_object(retired_smeta,
                                    sizeof(star::TwoPLPashaMetadataShared),
                                    star::CXLMemory::METADATA_FREE,
                                    owner_shard_);
          });
  if (move_result != star::RowOutcome::kDone) {
    UnlockRow(metadata);
    return;
  }
  UnlockRow(metadata);
  star::num_data_move_out.fetch_add(1, std::memory_order_relaxed);
  result = true;
  return;
      });
  return found && result;
}

std::string KVPartition::KeyString(const FixedKey &key) const {
  return std::string(key.bytes, fixed_key_size_);
}

bool KVPartition::ScanLocalPartition(
    std::string_view start_key, uint64_t limit,
    std::vector<std::pair<std::string, std::string>> *items,
    std::string_view inclusive_max) const {
  if (items == nullptr) throw std::invalid_argument("null partition scan output");
  auto *table = private_table_.get();
  if (table == nullptr)
    throw std::runtime_error("owner scan table is unavailable");
  const FixedKey min_key = MakeKey(start_key);
  FixedKey max_key{};
  if (inclusive_max.empty()) max_key = FixedKey::InternalMax(fixed_key_size_);
  else max_key = MakeKey(inclusive_max);

  // One scan fragment retains each original read lock through the right
  // boundary, then copies values and releases together.  The only adaptation
  // is resolving owner-private offsets back into transient row views.
  struct HeldRow {
    FixedKey key{};
    PrivateMetadataLocal *private_metadata = nullptr;
    star::TwoPLPashaMetadataShared *shared_metadata = nullptr;
    std::string private_value;
    bool output = false;
  };
  items->clear();
  const auto acquire =
      [&](const void *raw_key, std::atomic<uint64_t> * /*meta_slot*/,
          void *data, bool boundary, HeldRow *row) -> bool {
        const auto &key = *static_cast<const FixedKey *>(raw_key);
        auto *private_value = reinterpret_cast<PrivateValueStruct *>(
            static_cast<char *>(data) - sizeof(PrivateValueStruct));
        auto *metadata = MetadataFromValue(private_value);
        row->key = key;
        row->output = !boundary &&
                      !IsInternalMaxSentinel(key, fixed_key_size_);
        bool local_read = false;
        star::TwoPLPashaMetadataShared *shared_locked = nullptr;
        row->private_value.resize(fixed_value_size_);
        // This is the original local scan callback's read_lock path.  Its
        // migrated continuation refreshes the owner mirror and takes a shared
        // reader lock, but deliberately does not take the remote scan ref.
        // The latter belongs only to ScanSharedPartition's CXL callback.
        star::TwoPLPashaHelper::take_read_lock_and_read(
            *metadata, private_value->data, row->private_value.data(),
            row->private_value.size(), local_read,
            [this](const PrivateMetadataLocal &local) {
              return SharedMetadataFromOffset(local.migrated_smeta_off);
            },
            owner_shard_, &shared_locked, nullptr, nullptr);
        if (!local_read) {
          // A deleted row or lock conflict is a failed original scan
          // primitive.  The single facade Busy retry decides whether to retry.
          return false;
        }
        if (shared_locked == nullptr) {
          mem_access::PrivateRead(private_value->data, fixed_value_size_);
          row->private_metadata = metadata;
        } else {
          row->shared_metadata = shared_locked;
        }
        return true;
      };
  bool has_next_row = false;
  bool scan_success = false;
  HeldRow next_row;
  std::vector<HeldRow> rows;
  star::TwoPLPashaHelper::scan_local_fragment(
      *table, &min_key, &max_key, limit, rows,
      &next_row, has_next_row, scan_success, acquire,
      [](const HeldRow &row) -> const void * { return &row.key; });
  const auto release_row = [this](HeldRow *row) {
    if (row->private_metadata != nullptr)
      star::TwoPLPashaHelper::read_lock_release(
          *row->private_metadata,
          [this](const PrivateMetadataLocal &local) {
            return SharedMetadataFromOffset(local.migrated_smeta_off);
          });
    if (row->shared_metadata != nullptr)
      star::TwoPLPashaHelper::remote_read_lock_release(
          row->shared_metadata, /*dec_ref_cnt=*/false);
  };
  const auto release_fragment = [&] {
    if (has_next_row) release_row(&next_row);
    for (auto it = rows.rbegin(); it != rows.rend(); ++it)
      release_row(&*it);
    rows.clear();
    has_next_row = false;
  };
  if (!scan_success) {
    release_fragment();
    return false;
  }
  for (auto &row : rows) {
    if (!row.output) continue;
    std::string value = std::move(row.private_value);
    items->emplace_back(KeyString(row.key), std::move(value));
  }
  release_fragment();
  return true;
}

KVPartition::SharedScanResult KVPartition::ScanSharedPartition(
    uint32_t host_id, std::string_view start_key, uint64_t output_limit,
    std::string_view inclusive_max,
    bool allow_lower_bound_left_boundary) const {
  SharedScanResult result;
  const FixedKey min_key = MakeKey(start_key);
  FixedKey max_key{};
  if (inclusive_max.empty())
    max_key = FixedKey::InternalMax(fixed_key_size_);
  else
    max_key = MakeKey(inclusive_max);
  struct Pinned {
    FixedKey key{};
    star::TwoPLPashaMetadataShared *smeta = nullptr;
    star::TwoPLPashaSharedDataSCC *scc_data = nullptr;
    bool result_row = false;
  };
  bool corruption = false;
  const auto adjacency_ok =
      [&](const void *raw_key, void *raw_smeta, bool /*is_last_tuple*/,
          size_t result_count, bool /*locking_next_tuple*/) -> bool {
    const auto &key = *static_cast<const FixedKey *>(raw_key);
    auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(raw_smeta);
    if (!regions_.IsHwccAddress(smeta)) {
      corruption = true;
      return false;
    }
    // K1: after owner forward-only move-in, the first lower-bound row may lack
    // a migrated predecessor. Apply that exemption only when the caller opts
    // in (post-move-in re-probe). The first probe keeps master's exact-min
    // rule so an island beyond the move-in window cannot be accepted while
    // [min, island) is still private-only.
    const bool lower_bound_left_boundary =
        allow_lower_bound_left_boundary && result_count == 0 &&
        key.Compare(min_key) >= 0;
    smeta->lock();
    const bool adj_ok = star::TwoPLPashaHelper::scan_row_adjacency_ok(
        key.Compare(min_key) == 0 || lower_bound_left_boundary,
        result_count == output_limit,
        smeta->get_prev_key_real_bit(),
        smeta->get_next_key_real_bit());
    smeta->unlock();
    return adj_ok;
  };
  const auto acquire =
      [&](const void *raw_key, void *raw_smeta, bool locking_next_tuple,
          Pinned *row) -> bool {
    const auto &key = *static_cast<const FixedKey *>(raw_key);
    auto *smeta = static_cast<star::TwoPLPashaMetadataShared *>(raw_smeta);
    if (!regions_.IsHwccAddress(smeta)) {
      corruption = true;
      return false;
    }
    star::TwoPLPashaSharedDataSCC *scc_data = nullptr;
    bool shared_locked = false;
    star::TwoPLPashaHelper::remote_take_read_lock_and_read(
        smeta, host_id, nullptr, fixed_value_size_, /*inc_ref_cnt=*/true,
        shared_locked);
    if (shared_locked) scc_data = smeta->get_scc_data();
    if (!shared_locked) {
      return false;
    }
    *row = {key, smeta, scc_data, !locking_next_tuple};
    return true;
  };
  std::vector<Pinned> rows;
  Pinned next_row;
  bool has_next_row = false;
  bool scan_success = false;
  bool migration_required = false;
  bool scan_busy = false;
  star::TwoPLPashaHelper::scan_remote_fragment(
      [](const void *left, const void *right) {
        return FixedKeyComparator()(*static_cast<const FixedKey *>(left),
                                    *static_cast<const FixedKey *>(right));
      }, *shared_table_, &min_key, &max_key, output_limit, rows, &next_row,
      has_next_row, scan_success, migration_required, scan_busy, adjacency_ok,
      acquire, [](const Pinned &row) -> const void * { return &row.key; });
  const auto unpin_row = [](Pinned *row) {
    star::TwoPLPashaHelper::remote_read_lock_release(
        row->smeta, /*dec_ref_cnt=*/true);
  };
  const auto unpin_all = [&] {
    if (has_next_row) unpin_row(&next_row);
    for (auto it = rows.rbegin(); it != rows.rend(); ++it)
      unpin_row(&*it);
    rows.clear();
    has_next_row = false;
  };

  if (scan_busy) {
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
  if (migration_required) {
    unpin_all();
    result.migration_required = true;
    return result;
  }
  if (rows.empty() && !has_next_row) {
    result.migration_required = true;
    return result;
  }

  for (auto &row : rows) {
    if (!row.result_row) continue;
    std::string value(fixed_value_size_, '\0');
    mem_access::SharedPayloadRead(row.scc_data->data, value.size());
    star::scc_manager->do_read(row.smeta, host_id, value.data(),
                               row.scc_data->data, value.size());
    result.items.emplace_back(KeyString(row.key), std::move(value));
  }
  unpin_all();
  result.scan_success = true;
  return result;
}

star::RowOutcome KVPartition::DeletePrivate(std::string_view key) {
  const FixedKey fixed_key = MakeKey(key);
  auto *table = private_table_.get();
  if (table == nullptr || star::migration_manager == nullptr)
    throw std::runtime_error("owner delete has no installed migration callback");
  // Keep the original owner read-and-delete entry: lookup through ITable,
  // take the row write lock while reading it, then let the Clock callback
  // consume that lock with the retired row.
  auto [meta_slot, data] = table->search(&fixed_key);
  if (meta_slot == nullptr || data == nullptr)
    return star::RowOutcome::kMissing;
  auto *value = reinterpret_cast<PrivateValueStruct *>(meta_slot);
  auto *metadata = MetadataFromValue(value);
  OwnerNextRowLock row_lock;
  bool write_locked = false;
  bool migrated = false;
  std::string prior_value(fixed_value_size_, '\0');
  const uint64_t observed_tid =
      star::TwoPLPashaHelper::take_write_lock_and_read(
          *metadata, data, prior_value.data(), prior_value.size(),
          write_locked, &migrated);
  if (write_locked) {
    row_lock.value = value;
    row_lock.metadata = metadata;
    row_lock.observed_tid = observed_tid;
    row_lock.shared = false;
  } else if (migrated) {
    // The offset-backed local helper intentionally leaves migrated rows to
    // the existing SCC acquisition path.
    if (!AcquireOwnerNextRowWriteLock(value, &row_lock))
      return star::RowOutcome::kBusy;
  } else {
    LockRow(metadata);
    const bool valid = metadata->is_valid;
    UnlockRow(metadata);
    if (!valid) return star::RowOutcome::kMissing;
    return star::RowOutcome::kBusy;
  }

  // Match the original read-and-delete order: take the write lock first,
  // then let the PolicyClock callback consume it with the deleted row.  The
  // successful callback retires the row, so it must not be released again.
  try {
    const bool deleted = star::migration_manager->delete_specific_row_and_move_out(
        table, &fixed_key, /*is_delete_local=*/true);
    if (!deleted)
      ReleaseOwnerNextRowWriteLock(row_lock, 0, false);
    return deleted ? star::RowOutcome::kDone : star::RowOutcome::kBusy;
  } catch (...) {
    ReleaseOwnerNextRowWriteLock(row_lock, 0, false);
    throw;
  }
}

bool KVPartition::DeletePrivateForMigrationManager(
    std::string_view key, bool *need_untrack,
    void **migration_policy_meta, bool writer_prelocked,
    bool requester_prelocked) {
  if (need_untrack == nullptr || migration_policy_meta == nullptr)
    throw std::invalid_argument("null Clock delete output");
  *need_untrack = false;
  *migration_policy_meta = nullptr;
  const FixedKey fixed_key = MakeKey(key);
  if (IsInternalMaxSentinel(fixed_key, fixed_key_size_))
    throw std::invalid_argument("internal max sentinel is reserved");
  star::TwoPLPashaMetadataShared *retired_smeta = nullptr;
  star::TwoPLPashaSharedDataSCC *retired_payload = nullptr;
  PrivateValueStruct *retired_value = nullptr;
  PrivateMetadataLocal *retired_metadata = nullptr;
  auto *table = private_table_.get();
  if (table == nullptr)
    throw std::runtime_error("owner delete has no installed table adapter");

  // Competition must be classified before entering the original adjacent-row
  // remove callback. Once that callback begins, master ordering changes
  // adjacency before invalidating/removing the current row and has no
  // rollback outcome. The owner write lock (or remote reference/write lock)
  // obtained by the caller keeps this exact row stable until callback.
  auto [preflight_meta_slot, preflight_data] = table->search(&fixed_key);
  if (preflight_meta_slot == nullptr || preflight_data == nullptr) return false;
  auto *preflight_value =
      reinterpret_cast<PrivateValueStruct *>(preflight_meta_slot);
  auto *preflight_metadata = MetadataFromValue(preflight_value);
  star::TwoPLPashaMetadataShared *smeta = nullptr;
  RegionOffset smeta_offset = kNullOffset;
  bool initially_migrated = false;
  bool reserved_shared_write = false;
  LockRow(preflight_metadata);
  if (!preflight_metadata->is_valid) {
    UnlockRow(preflight_metadata);
    return false;
  }
  initially_migrated = preflight_metadata->is_migrated;
  if (requester_prelocked && !initially_migrated) {
    UnlockRow(preflight_metadata);
    throw std::runtime_error("remote delete lost migrated owner locator");
  }
  if (preflight_metadata->scc_data_off != kNullOffset) {
    retired_payload = static_cast<star::TwoPLPashaSharedDataSCC *>(
        regions_.ResolveSharedPayload(preflight_metadata->scc_data_off,
                                      SharedSccBytes(fixed_value_size_)));
  }
  if (initially_migrated) {
    smeta_offset = preflight_metadata->migrated_smeta_off;
    if (smeta_offset == kNullOffset || retired_payload == nullptr) {
      UnlockRow(preflight_metadata);
      throw std::runtime_error("migrated delete has inconsistent shared locator");
    }
    smeta = SharedMetadataFromOffset(smeta_offset);
    smeta->lock();
    auto *payload = smeta->get_scc_data();
    if (retired_payload != payload) {
      smeta->unlock();
      UnlockRow(preflight_metadata);
      throw std::runtime_error("migrated row payload disagrees with local cache");
    }
    const bool busy =
        smeta->get_reader_count() != 0 ||
        (requester_prelocked
             ? (smeta->get_ref_cnt() != 1 || !smeta->is_write_locked())
             : (smeta->get_ref_cnt() != 0 ||
                (!writer_prelocked && smeta->is_write_locked())));
    if (busy) {
      smeta->unlock();
      UnlockRow(preflight_metadata);
      return false;
    }
    if (payload->get_flag(star::TwoPLPashaSharedDataSCC::valid_flag_index) !=
        writer_prelocked) {
      smeta->unlock();
      UnlockRow(preflight_metadata);
      throw std::runtime_error("migrated delete has inconsistent shared valid bit");
    }
    if (!writer_prelocked && !requester_prelocked) {
      smeta->set_write_locked();
      reserved_shared_write = true;
    }
    smeta->unlock();
  }
  UnlockRow(preflight_metadata);

  const auto release_reserved_shared_write = [&] {
    if (!reserved_shared_write || smeta == nullptr) return;
    smeta->lock();
    smeta->clear_write_locked();
    smeta->unlock();
    reserved_shared_write = false;
  };

  bool removed = false;
  try {
    removed = star::TwoPLPashaHelper::delete_and_update_next_key_info(
        table, &fixed_key,
      [&](const void *prev_key, void *prev_meta, void *prev_data,
          const void *cur_key, void *cur_meta, void *cur_data,
          const void *next_key, void *next_meta, void *next_data) {
        if (cur_key == nullptr || cur_meta == nullptr || cur_data == nullptr)
          throw std::runtime_error("owner delete callback returned null row");
        (void)prev_key;
        (void)next_key;
        (void)prev_data;
        (void)next_data;
        auto *prev_lmeta = static_cast<PrivateMetadataLocal *>(prev_meta);
        auto *metadata = static_cast<PrivateMetadataLocal *>(cur_meta);
        auto *next_lmeta = static_cast<PrivateMetadataLocal *>(next_meta);
        auto *private_value = reinterpret_cast<PrivateValueStruct *>(
            static_cast<char *>(cur_data) - sizeof(PrivateValueStruct));
        if (metadata != preflight_metadata || private_value != preflight_value ||
            cur_data != preflight_data)
          throw std::runtime_error(
              "owner delete callback row changed after preflight");

        // Keep master ordering: update predecessor and successor separately,
        // release both, then take the current-row latch for invalidation.
        try {
          star::TwoPLPashaHelper::clear_adjacent_migrated_rows(
              prev_lmeta, next_lmeta,
              [this](const PrivateMetadataLocal &local) {
                return SharedMetadataFromOffset(local.migrated_smeta_off);
              });
        } catch (...) {
          release_reserved_shared_write();
          throw;
        }

        LockRow(metadata);
        const auto unlock = [&] { UnlockRow(metadata); };
        if (!metadata->is_valid || metadata->is_migrated != initially_migrated ||
            (initially_migrated && metadata->migrated_smeta_off != smeta_offset)) {
          unlock();
          release_reserved_shared_write();
          throw std::runtime_error(
              "owner delete row changed after delete preflight");
        }
        metadata->is_valid = false;
        if (initially_migrated) {
          const auto shared_result =
              star::TwoPLPashaHelper::delete_and_update_next_key_info(
                  smeta, owner_shard_, SharedSccBytes(fixed_value_size_),
                  writer_prelocked || reserved_shared_write, requester_prelocked,
                  /*is_local_delete=*/writer_prelocked,
                  [this, &fixed_key](star::TwoPLPashaMetadataShared *row) {
                    return shared_table_->remove(&fixed_key, row);
                  });
          if (shared_result != star::RowOutcome::kDone) {
            unlock();
            throw std::runtime_error(
                "shared delete contention after adjacency update");
          }
          reserved_shared_write = false;
          metadata->is_migrated = false;
          metadata->migrated_smeta_off = kNullOffset;
          *need_untrack = true;
          *migration_policy_meta = smeta;
          retired_smeta = smeta;
        }
        metadata->scc_data_off = kNullOffset;
        retired_value = private_value;
        retired_metadata = metadata;
        unlock();
          return true;
      });
  } catch (...) {
    release_reserved_shared_write();
    throw;
  }
  if (!removed) {
    release_reserved_shared_write();
    return false;
  }
  CHECK(retired_value != nullptr && retired_metadata != nullptr);
  if (retired_smeta != nullptr) {
    ebr_.add_retired_object(
        retired_smeta, sizeof(star::TwoPLPashaMetadataShared),
        star::CXLMemory::METADATA_FREE, owner_shard_);
  }
  if (retired_payload != nullptr) {
    ebr_.add_retired_object(
        retired_payload, SharedSccBytes(fixed_value_size_),
        star::CXLMemory::DATA_FREE, owner_shard_);
  }
  return true;
}

SharedAccessState KVPartition::TryPinShared(
    const FixedKey &key, star::TwoPLPashaMetadataShared **smeta,
    bool record_clock_access) const {
  if (smeta == nullptr)
    throw std::invalid_argument("null TryPinShared output");
  *smeta = nullptr;
  auto *helper = KvMigrationRuntime::Instance().helper();
  if (helper == nullptr)
    throw std::runtime_error("TwoPLPasha shared lookup helper is unavailable");
  star::TwoPLPashaMetadataShared *candidate = nullptr;
  const star::RowOutcome result = helper->get_migrated_row_result(
      kSingleTableId, partition_id_, &key, /*inc_ref_cnt=*/true,
      fixed_value_size_, &candidate, record_clock_access);
  if (result == star::RowOutcome::kMissing)
    return SharedAccessState::kMissing;
  if (result == star::RowOutcome::kBusy)
    return SharedAccessState::kRetry;
  if (candidate == nullptr)
    throw std::runtime_error("shared lookup completed without smeta");
  *smeta = candidate;
  return SharedAccessState::kDone;
}

PrivateClockTrackerNode *KVPartition::ResolveClockTrackerNode(
    RegionOffset offset) const {
  if (offset == kNullOffset) return nullptr;
  return static_cast<PrivateClockTrackerNode *>(regions_.ResolveOwnerPrivate(
      offset, sizeof(PrivateClockTrackerNode), partition_id_, owner_shard_));
}

PrivateClockTrackerNode *KVPartition::AllocateClockTrackerNode() {
  return new (regions_.AllocateOwnerPrivate(sizeof(PrivateClockTrackerNode),
                                             partition_id_, owner_shard_))
      PrivateClockTrackerNode;
}

void KVPartition::FreeClockTrackerNode(PrivateClockTrackerNode *node) {
  if (node == nullptr) throw std::invalid_argument("null Clock tracker node");
  regions_.FreeOwnerPrivate(node, sizeof(PrivateClockTrackerNode), partition_id_,
                            owner_shard_);
}

RegionOffset KVPartition::ClockTrackerNodeOffset(
    const PrivateClockTrackerNode *node) const {
  if (node == nullptr) throw std::invalid_argument("null Clock tracker node");
  return regions_.ToOwnerPrivateOffset(node, partition_id_);
}

RegionOffset KVPartition::ClockTrackerLocalRowOffset(
    const std::tuple<std::atomic<uint64_t> *, void *> &row) const {
  auto *data = static_cast<char *>(std::get<1>(row));
  if (data == nullptr) throw std::runtime_error("Clock row has null data");
  auto *value = reinterpret_cast<PrivateValueStruct *>(
      data - sizeof(PrivateValueStruct));
  if (ValueFromOffset(regions_.ToOwnerPrivateOffset(value, partition_id_)) != value)
    throw std::runtime_error("Clock row is outside owner-private arena");
  return regions_.ToOwnerPrivateOffset(value, partition_id_);
}

RegionOffset KVPartition::ClockTrackerSharedRowOffset(void *smeta) const {
  if (smeta == nullptr) throw std::runtime_error("Clock row has null smeta");
  return regions_.ToDynamicHwccOffset(smeta, owner_shard_);
}

std::tuple<std::atomic<uint64_t> *, void *> KVPartition::ClockTrackerLocalRow(
    RegionOffset row_offset) const {
  auto *value = ValueFromOffset(row_offset);
  if (value == nullptr) throw std::runtime_error("Clock row offset is invalid");
  return std::make_tuple(&value->meta, value->data);
}

star::TwoPLPashaMetadataShared *KVPartition::ClockTrackerSharedRow(
    RegionOffset smeta_offset) const {
  if (smeta_offset == kNullOffset)
    throw std::runtime_error("Clock smeta offset is null");
  return static_cast<star::TwoPLPashaMetadataShared *>(
      regions_.ResolveDynamicHwcc(smeta_offset,
                                  sizeof(star::TwoPLPashaMetadataShared),
                                  owner_shard_));
}

bool KVPartition::ClockTrackerNodeMatches(
    const PrivateClockTrackerNode &node) const {
  if (node.value_off == kNullOffset || node.smeta_off == kNullOffset)
    return false;
  auto *value = ValueFromOffset(node.value_off);
  if (value == nullptr) return false;
  auto *metadata = MetadataFromValue(value);
  return metadata->is_migrated && metadata->migrated_smeta_off == node.smeta_off;
}

uint64_t KVPartition::shared_payload_used_bytes() const {
  return regions_.SharedPayloadUsedBytes(owner_shard_);
}

uint64_t KVPartition::shared_payload_capacity_bytes() const {
  return regions_.SharedPayloadCapacityBytes(owner_shard_);
}


}  // namespace tigonkv::engine
