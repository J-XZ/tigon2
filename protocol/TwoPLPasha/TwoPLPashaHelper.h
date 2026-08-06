//
// Created by Yi Lu on 9/11/18.
//

#pragma once

#include <atomic>
#include <limits>
#include <list>
#include <tuple>
#include <utility>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/CCSet.h"
#include "common/CCHashTable.h"
#include "common/CXLMemory.h"
#include "common/CXL_EBR.h"
#include "core/Context.h"
#include "core/CXLTable.h"
#include "core/Table.h"
#include "kv/engine/mem_access.h"
#include "glog/logging.h"

#include "protocol/Pasha/MigrationManager.h"
#include "protocol/Pasha/SCCManager.h"

namespace star
{

// Preserve the original SCC row-image layout.  `tid` and `valid` are part of
// the non-coherent row image and are made visible only by WriteThrough SCC;
// the two legacy fields below retain the original header size/cacheline span
// but carry no state.  Cross-node ref counting and Clock's second chance are
// in HWCC smeta instead.
struct TwoPLPashaSharedDataSCC {
        TwoPLPashaSharedDataSCC() : tid(0), flags(0) {}

        static constexpr int valid_flag_index = 0;

        bool get_flag(int flag_index) const {
                return (flags & (1u << flag_index)) != 0;
        }

        void set_flag(int flag_index) {
                flags = static_cast<uint8_t>(flags | (1u << flag_index));
        }

        void clear_flag(int flag_index) {
                flags = static_cast<uint8_t>(flags & ~(1u << flag_index));
        }

        uint64_t tid{0};
        uint8_t flags{0};
        uint8_t legacy_ref_cnt_padding{0};
        char legacy_policy_padding[MigrationManager::migration_policy_meta_size]{};
        char data[];
};

enum class RowOutcome : uint8_t { kDone = 0, kMissing = 1, kBusy = 2 };
static_assert(offsetof(TwoPLPashaSharedDataSCC, data) == 34);
static_assert(sizeof(TwoPLPashaSharedDataSCC) == 40);

using TwoPLPashaMetadataLocal =
    tigonkv::engine::TwoPLPashaMetadataLocalStorage<
        char *, TwoPLPashaSharedDataSCC *>;
using TwoPLPashaMetadataLocalOffset =
    tigonkv::engine::TwoPLPashaMetadataLocalStorage<
        tigonkv::engine::RegionOffset, tigonkv::engine::RegionOffset>;

static_assert(offsetof(TwoPLPashaMetadataLocal, latch) ==
              offsetof(TwoPLPashaMetadataLocalOffset, latch));
static_assert(offsetof(TwoPLPashaMetadataLocal, tid) ==
              offsetof(TwoPLPashaMetadataLocalOffset, tid));
static_assert(offsetof(TwoPLPashaMetadataLocal, is_valid) ==
              offsetof(TwoPLPashaMetadataLocalOffset, is_valid));
static_assert(offsetof(TwoPLPashaMetadataLocal, is_migrated) ==
              offsetof(TwoPLPashaMetadataLocalOffset, is_migrated));
static_assert(offsetof(TwoPLPashaMetadataLocal,
                       is_data_modified_since_moved_out) ==
              offsetof(TwoPLPashaMetadataLocalOffset,
                       is_data_modified_since_moved_out));
static_assert(offsetof(TwoPLPashaMetadataLocal, migrated_row) ==
              offsetof(TwoPLPashaMetadataLocalOffset, migrated_row));
static_assert(offsetof(TwoPLPashaMetadataLocal, scc_data) ==
              offsetof(TwoPLPashaMetadataLocalOffset, scc_data));
static_assert(sizeof(TwoPLPashaMetadataLocal) ==
              sizeof(TwoPLPashaMetadataLocalOffset));

// Zero-state access adapter for owner-local metadata critical sections.
// Legacy DRAM `TwoPLPashaMetadataLocal` is a no-op; offset-backed owner-private
// SWCC records latch/state ranges without performing the lock or mutating
// fields.  Call order is fixed: Lock → pthread lock → Read → … → optional
// Write → Unlock → pthread unlock.
template <typename LocalMetadata>
struct LocalMetadataAccess {
        static void Lock(LocalMetadata &) {}
        static void Unlock(LocalMetadata &) {}
        static void Read(const LocalMetadata &) {}
        static void Write(LocalMetadata &) {}
};

template <>
struct LocalMetadataAccess<TwoPLPashaMetadataLocalOffset> {
        static constexpr size_t kStateOffset =
            offsetof(TwoPLPashaMetadataLocalOffset, tid);
        static constexpr size_t kStateBytes =
            sizeof(TwoPLPashaMetadataLocalOffset) - kStateOffset;

        static void Lock(TwoPLPashaMetadataLocalOffset &lmeta) {
                // pthread_spinlock_t is an opaque lock object, not a
                // std::atomic operation that this wrapper can execute.  The
                // real lock operation remains pthread_spin_lock below.
                (void)lmeta;
        }
        static void Unlock(TwoPLPashaMetadataLocalOffset &lmeta) {
                (void)lmeta;
        }
        static void Read(const TwoPLPashaMetadataLocalOffset &lmeta) {
                tigonkv::engine::mem_access::PrivateRead(
                    reinterpret_cast<const char *>(&lmeta) + kStateOffset,
                    kStateBytes);
        }
        static void Write(TwoPLPashaMetadataLocalOffset &lmeta) {
                tigonkv::engine::mem_access::PrivateWrite(
                    reinterpret_cast<char *>(&lmeta) + kStateOffset,
                    kStateBytes);
        }
};

struct TwoPLPashaMetadataShared {
        uint64_t load_atomic_word(std::memory_order order = std::memory_order_seq_cst)
        {
                return tigonkv::engine::mem_access::HwccAtomicLoad(
                    atomic_word, order);
        }

        void store_atomic_word(uint64_t value,
                               std::memory_order order = std::memory_order_seq_cst)
        {
                tigonkv::engine::mem_access::HwccAtomicStore(
                    atomic_word, value, order);
        }

        bool compare_exchange_word_strong(
            uint64_t &expected, uint64_t desired,
            std::memory_order success = std::memory_order_seq_cst,
            std::memory_order failure = std::memory_order_seq_cst)
        {
                return tigonkv::engine::mem_access::HwccAtomicCompareExchangeStrong(
                    atomic_word, expected, desired, success, failure);
        }

        TwoPLPashaMetadataShared(TwoPLPashaSharedDataSCC *scc_data)
        {
                uint64_t scc_data_cxl_offset = 0;

                scc_data_cxl_offset = CXLMemory::shared_payload_pointer_to_offset(scc_data);
                DCHECK(scc_data_cxl_offset < (1ull << 37));

                store_atomic_word(scc_data_cxl_offset << SCC_DATA_OFFSET,
                                  std::memory_order_release);
                tigonkv::engine::mem_access::HwccWrite(&ref_cnt, sizeof(ref_cnt));
        }

	void lock()
	{
retry:
			uint64_t v_before_lock = load_atomic_word(std::memory_order_acquire);
                uint64_t v_after_lock = (v_before_lock | (LATCH_BIT_MASK << LATCH_BIT_OFFSET));

		if ((v_before_lock & (LATCH_BIT_MASK << LATCH_BIT_OFFSET)) == 0) {
				if (compare_exchange_word_strong(v_before_lock, v_after_lock)) {
					return;
                        } else {
			        goto retry;
                        }
		} else {
			goto retry;
		}
	}

	void unlock()
	{
		uint64_t v_before_unlock = load_atomic_word(std::memory_order_acquire);
                uint64_t v_after_unlock =
                    v_before_unlock & ~(LATCH_BIT_MASK << LATCH_BIT_OFFSET);
                DCHECK(((v_before_unlock & (LATCH_BIT_MASK << LATCH_BIT_OFFSET)) != 0) == true);
                store_atomic_word(v_after_unlock, std::memory_order_release);
        }

        TwoPLPashaSharedDataSCC *get_scc_data()
        {
                uint64_t scc_data_cxl_offset =
                    load_atomic_word(std::memory_order_acquire) &
                    (SCC_DATA_MASK << SCC_DATA_OFFSET);
                void *scc_data_ptr = CXLMemory::shared_payload_offset_to_pointer(
                    scc_data_cxl_offset, sizeof(TwoPLPashaSharedDataSCC));

                return reinterpret_cast<TwoPLPashaSharedDataSCC *>(scc_data_ptr);
        }

        // next-key information
        bool get_next_key_real_bit()
        {
                return is_bit_set(is_next_key_real_bit_index);
        }

        void set_next_key_real_bit()
        {
                set_bit(is_next_key_real_bit_index);
        }

        void clear_next_key_real_bit()
        {
                clear_bit(is_next_key_real_bit_index);
        }

        bool get_prev_key_real_bit()
        {
                return is_bit_set(is_prev_key_real_bit_index);
        }

        void set_prev_key_real_bit()
        {
                set_bit(is_prev_key_real_bit_index);
        }

        void clear_prev_key_real_bit()
        {
                clear_bit(is_prev_key_real_bit_index);
        }

        // The original shared metadata word reserves bit 37 for Clock.  The
        // caller holds this smeta latch, as it does for adjacency and dirty.
        bool get_second_chance_bit()
        {
                return is_bit_set(second_chance_bit_index);
        }

        void set_second_chance_bit()
        {
                set_bit(second_chance_bit_index);
        }

        void clear_second_chance_bit()
        {
                clear_bit(second_chance_bit_index);
        }

        // Caller holds the original smeta latch for every mutable bit update.
        void set_bit(uint64_t bit_index)
        {
                uint64_t orig_atomic_word = load_atomic_word(std::memory_order_acquire);
                store_atomic_word(orig_atomic_word | (1ull << bit_index),
                                  std::memory_order_release);
        }

        // Function to clear a bit at a given position in the bitmap
        void clear_bit(uint64_t bit_index)
        {
                uint64_t orig_atomic_word = load_atomic_word(std::memory_order_acquire);
                store_atomic_word(orig_atomic_word & ~(1ull << bit_index),
                                  std::memory_order_release);
        }

        // Function to check if a bit is set (returns true if set, false if clear)
        bool is_bit_set(uint64_t bit_index)
        {
                return (load_atomic_word(std::memory_order_acquire) &
                        (1ull << bit_index)) != 0;
        }

        // read lock
        uint64_t get_reader_count()
        {
                return (load_atomic_word(std::memory_order_acquire) >>
                        READ_LOCK_BITS_OFFSET) & READ_LOCK_BITS_MASK;
        }

        void set_reader_count(uint64_t reader_count)
        {
                uint64_t orig_atomic_word = load_atomic_word(std::memory_order_acquire);
                orig_atomic_word &= ~(READ_LOCK_BITS_MASK << READ_LOCK_BITS_OFFSET);
                orig_atomic_word |= (reader_count << READ_LOCK_BITS_OFFSET);
                store_atomic_word(orig_atomic_word, std::memory_order_release);
        }

        void increase_reader_count()
        {
                uint64_t orig_atomic_word = load_atomic_word(std::memory_order_acquire);
                store_atomic_word(orig_atomic_word +
                                      (1ull << READ_LOCK_BITS_OFFSET),
                                  std::memory_order_release);
        }

        void decrease_reader_count()
        {
                uint64_t orig_atomic_word = load_atomic_word(std::memory_order_acquire);
                store_atomic_word(orig_atomic_word -
                                      (1ull << READ_LOCK_BITS_OFFSET),
                                  std::memory_order_release);
        }

        uint64_t get_reader_count_max()
        {
                return READ_LOCK_BITS_MASK;
        }

        // write lock
        bool is_write_locked()
        {
                return is_bit_set(WRITE_LOCK_BIT_OFFSET);
        }

        void set_write_locked()
        {
                set_bit(WRITE_LOCK_BIT_OFFSET);
        }

        void clear_write_locked()
        {
                clear_bit(WRITE_LOCK_BIT_OFFSET);
        }

        // True when the requester still holds the write-lock bit (Pending
        // state of the remote-delete protocol).  Only meaningful under the
        // smeta latch.  The owner's shared delete step re-checks this bit
        // under the same latch and clears it as the single linearization
        // point; the requester's Pending -> Cancelled rollback CASes this bit
        // away, so exactly one of owner-delete or requester-cancel wins.
        //
        // Remote-delete mailbox state machine (single HWCC atomic word bit
        // WRITE_LOCK_BIT_OFFSET plus the SWCC SCC valid flag and ref count):
        //
        //   State     | writer   | CAS pre-state      | row visible | requester
        //              |          |                    |             | may rollback
        //   ----------+----------+--------------------+-------------+------------
        //   Empty     | -        | -                  | valid=1     | n/a
        //   Pending   | requester| Empty (set bit,    | valid=0     | YES (only
        //              |          | clear valid, +ref)|             | this state)
        //   Cancelled | requester| Pending (restore   | valid=1     | NO (already
        //              |          | valid, clear bit, |             | terminal)
        //              |          | -ref)             |             |
        //   Executing | owner    | Pending (shared    | deleting/   | NO (hard
        //   /Deleted  |          | delete step checks | deleted     | fail)
        //              |          | bit under latch,  |             |
        //              |          | clears it as the  |             |
        //              |          | linearization)    |             |
        //
        // Slot reuse: the requester's request carries a monotonically
        // increasing sequence that the response echoes; a stale response for
        // an older sequence is dropped (kv_engine.cpp ConsumeTransportResponse)
        // and never consumes the next request's slot.
        //
        // The two races this table resolves:
        //  * cancel CAS wins before owner claim: the owner's shared delete
        //    step re-checks the bit under the same latch and sees it cleared,
        //    so it refuses to delete (Busy/terminal) and the row stays in the
        //    requester's restored state.
        //  * owner claim wins before the requester deadline: the bit is
        //    cleared by the owner's linearized delete, so the requester's
        //    rollback hard fails instead of resurrecting a deleted row, and
        //    any late success response carries the old sequence and is
        //    dropped.
        bool requester_holds_write_lock()
        {
                return (load_atomic_word(std::memory_order_acquire) &
                        (1ull << WRITE_LOCK_BIT_OFFSET)) != 0;
        }

        bool is_data_modified_since_moved_in()
        {
                return is_bit_set(is_data_modified_since_moved_in_bit_index);
        }

        void set_is_data_modified_since_moved_in()
        {
                set_bit(is_data_modified_since_moved_in_bit_index);
        }

        void clear_is_data_modified_since_moved_in()
        {
                clear_bit(is_data_modified_since_moved_in_bit_index);
        }

        // SCC
        void clear_all_scc_bits()
        {
                uint64_t orig_atomic_word = load_atomic_word(std::memory_order_acquire);
                store_atomic_word(orig_atomic_word &
                                      ~(SCC_BITS_MASK << SCC_BITS_OFFSET),
                                  std::memory_order_release);
        }

        void set_scc_bit(uint64_t host_id)
        {
                set_bit(host_id + SCC_BITS_OFFSET);
        }

	static constexpr int LATCH_BIT_OFFSET = 63;
	static constexpr uint64_t LATCH_BIT_MASK = 0x1ull;

        static constexpr int SCC_DATA_OFFSET = 0;
	static constexpr uint64_t SCC_DATA_MASK = 0x1fffffffffull;

        static constexpr int SCC_BITS_OFFSET = 47;
	static constexpr uint64_t SCC_BITS_MASK = 0xffffull;

        static constexpr int READ_LOCK_BITS_OFFSET = 42;
	static constexpr uint64_t READ_LOCK_BITS_MASK = 0x1full;

        static constexpr int WRITE_LOCK_BIT_OFFSET = 41;
	static constexpr uint64_t WRITE_LOCK_BIT_MASK = 0x1ull;

        static constexpr int is_data_modified_since_moved_in_bit_index = 40;

        static constexpr int scc_bits_base_index = 47;
        static constexpr int scc_bits_num = 8;

        static constexpr int is_next_key_real_bit_index = 39;
        static constexpr int is_prev_key_real_bit_index = 38;
        static constexpr int second_chance_bit_index = 37;

        // Compatibility accessors retain the original helper call shape.  All
        // production callers hold smeta and first prepare the SCC row image;
        // §3.15's focused tests cover that release sequence.
        static constexpr int valid_flag_index =
            TwoPLPashaSharedDataSCC::valid_flag_index;

        bool get_flag(int flag_index) const {
                return const_cast<TwoPLPashaMetadataShared *>(this)
                    ->get_scc_data()->get_flag(flag_index);
        }

        void set_flag(int flag_index) {
                get_scc_data()->set_flag(flag_index);
        }

        void clear_flag(int flag_index) {
                get_scc_data()->clear_flag(flag_index);
        }

        uint8_t get_ref_cnt() const {
                tigonkv::engine::mem_access::HwccRead(&ref_cnt, sizeof(ref_cnt));
                return ref_cnt;
        }

        void increment_ref_cnt() {
                tigonkv::engine::mem_access::HwccRead(&ref_cnt, sizeof(ref_cnt));
                if (ref_cnt == std::numeric_limits<uint8_t>::max()) {
                        LOG(FATAL) << "TwoPLPasha shared ref count overflow smeta="
                                   << this;
                }
                tigonkv::engine::mem_access::HwccWrite(&ref_cnt, sizeof(ref_cnt));
                ++ref_cnt;
        }

        void decrement_ref_cnt() {
                tigonkv::engine::mem_access::HwccRead(&ref_cnt, sizeof(ref_cnt));
                if (ref_cnt == 0) {
                        LOG(FATAL) << "TwoPLPasha shared ref count underflow smeta="
                                   << this;
                }
                tigonkv::engine::mem_access::HwccWrite(&ref_cnt, sizeof(ref_cnt));
                --ref_cnt;
        }

        // bit 63: latch bit
        // bit 62 - 47: software cache-coherence metadata
        // bit 46 - 42: read lock bits
        // bit 41 - 41: write lock bit
        // bit 40 - 40: is_data_modified_since_moved_in
        // bit 39 - 38: is_next_key_real, is_prev_key_real
        // bit 37 - 37: Clock second chance
        // bit 36 - 0: scc_data - enough for referencing 128 GB shared CXL memory
        std::atomic<uint64_t> atomic_word{ 0 };

        // multi-host accessors pin this; move-out requires ref_cnt == 0
        uint8_t ref_cnt{ 0 };
};
static_assert(offsetof(TwoPLPashaMetadataShared, ref_cnt) == 8);
static_assert(sizeof(TwoPLPashaMetadataShared) == 16);

uint64_t TwoPLPashaMetadataLocalInit(bool is_tuple_valid);

class TwoPLPashaHelper {
    public:
	using MetaDataType = std::atomic<uint64_t>;

        static constexpr std::size_t scc_data_bytes(std::size_t value_bytes)
        {
                return sizeof(TwoPLPashaSharedDataSCC) + value_bytes;
        }

        // The transaction layer owns a max_tid per worker.  The KV facade
        // supplies its bound foreground-worker slot explicitly and retains
        // the original TID bit layout.
        static uint64_t kv_next_commit_tid(uint64_t observed_tid,
                                           uint64_t &worker_max_tid)
        {
                const uint64_t observed = remove_lock_bit(observed_tid);
                if (observed >= worker_max_tid) worker_max_tid = observed + 1;
                else ++worker_max_tid;
                if (worker_max_tid >= (uint64_t{1} << READ_LOCK_BIT_OFFSET))
                        throw std::overflow_error("TwoPLPasha KV TID overflow");
                return worker_max_tid;
        }

        // The remote-row lock transition is shared by the original executor
        // and the single-operation KV facade.  The facade differs only in
        // how it obtains the row (offset lookup rather than Transaction), so
        // keep the SCC/read-lock/write-lock state machine here instead of
        // maintaining a second facade-only implementation.
        static uint64_t remote_take_read_lock_and_read(
            TwoPLPashaMetadataShared *smeta, std::size_t host_id, void *dest,
            std::size_t size, bool inc_ref_cnt, bool &success,
            RowOutcome *result = nullptr)
        {
                if (result != nullptr) *result = RowOutcome::kBusy;
                success = false;
                if (smeta == nullptr || scc_manager == nullptr) return 0;
                smeta->lock();
                auto *scc_data = smeta->get_scc_data();
                scc_manager->prepare_read(smeta, host_id, scc_data,
                                          scc_data_bytes(size));
                if (!scc_data->get_flag(TwoPLPashaSharedDataSCC::valid_flag_index)) {
                        smeta->unlock();
                        if (result != nullptr) *result = RowOutcome::kMissing;
                        return 0;
                }
                const uint64_t tid = remove_lock_bit(scc_data->tid);
                const bool write_locked = smeta->is_write_locked();
                const uint64_t reader_count = smeta->get_reader_count();
                const uint8_t ref_count = smeta->get_ref_cnt();
                if (write_locked ||
                    reader_count == smeta->get_reader_count_max() ||
                    (inc_ref_cnt && ref_count ==
                        std::numeric_limits<uint8_t>::max())) {
                        smeta->unlock();
                        return tid;
                }
                smeta->increase_reader_count();
                if (inc_ref_cnt) smeta->increment_ref_cnt();
                if (dest != nullptr && size != 0) {
                        tigonkv::engine::mem_access::SharedPayloadRead(
                            scc_data->data, size);
                        scc_manager->do_read(nullptr, host_id, dest,
                                             scc_data->data, size);
                }
                success = true;
                smeta->unlock();
                if (result != nullptr) *result = RowOutcome::kDone;
                return tid;
        }

        static uint64_t remote_take_write_lock_and_read(
            TwoPLPashaMetadataShared *smeta, std::size_t host_id, void *dest,
            std::size_t size, bool inc_ref_cnt, bool &success,
            RowOutcome *result = nullptr, bool allow_invalid = false)
        {
                if (result != nullptr) *result = RowOutcome::kBusy;
                success = false;
                if (smeta == nullptr || scc_manager == nullptr) return 0;
                smeta->lock();
                auto *scc_data = smeta->get_scc_data();
                scc_manager->prepare_read(smeta, host_id, scc_data,
                                          scc_data_bytes(size));
                if (!allow_invalid &&
                    !scc_data->get_flag(TwoPLPashaSharedDataSCC::valid_flag_index)) {
                        smeta->unlock();
                        if (result != nullptr) *result = RowOutcome::kMissing;
                        return 0;
                }
                const uint64_t tid = remove_lock_bit(scc_data->tid);
                if (smeta->is_write_locked() || smeta->get_reader_count() != 0 ||
                    (inc_ref_cnt && smeta->get_ref_cnt() ==
                        std::numeric_limits<uint8_t>::max())) {
                        smeta->unlock();
                        return tid;
                }
                smeta->set_write_locked();
                if (inc_ref_cnt) smeta->increment_ref_cnt();
                if (dest != nullptr && size != 0) {
                        tigonkv::engine::mem_access::SharedPayloadRead(
                            scc_data->data, size);
                        scc_manager->do_read(nullptr, host_id, dest,
                                             scc_data->data, size);
                }
                success = true;
                smeta->unlock();
                if (result != nullptr) *result = RowOutcome::kDone;
                return tid;
        }

        static void remote_read_lock_release(TwoPLPashaMetadataShared *smeta,
                                             bool dec_ref_cnt = false)
        {
                DCHECK(smeta != nullptr);
                smeta->lock();
                DCHECK(smeta->get_reader_count() > 0);
                DCHECK(!smeta->is_write_locked());
                if (dec_ref_cnt) {
                        smeta->decrement_ref_cnt();
                }
                smeta->decrease_reader_count();
                smeta->unlock();
        }

        static void remote_write_lock_abort(TwoPLPashaMetadataShared *smeta,
                                            bool dec_ref_cnt = false)
        {
                DCHECK(smeta != nullptr);
                smeta->lock();
                DCHECK(smeta->get_reader_count() == 0);
                DCHECK(smeta->is_write_locked());
                if (dec_ref_cnt) {
                        smeta->decrement_ref_cnt();
                }
                smeta->clear_write_locked();
                smeta->unlock();
        }

        static void remote_write_lock_release(
            TwoPLPashaMetadataShared *smeta, std::size_t host_id,
            std::size_t scc_bytes, uint64_t new_tid, bool dec_ref_cnt = false)
        {
                DCHECK(smeta != nullptr);
                smeta->lock();
                auto *scc_data = smeta->get_scc_data();
                DCHECK(scc_data->get_flag(TwoPLPashaSharedDataSCC::valid_flag_index));
                DCHECK(smeta->get_reader_count() == 0);
                DCHECK(smeta->is_write_locked());
                if (dec_ref_cnt) {
                        smeta->decrement_ref_cnt();
                }
                smeta->clear_write_locked();
                tigonkv::engine::mem_access::SharedPayloadWrite(
                    &scc_data->tid, sizeof(scc_data->tid));
                scc_data->tid = new_tid;
                scc_manager->finish_write(smeta, host_id, scc_data, scc_bytes);
                smeta->unlock();
        }

        // A KV write is one SCC publication primitive: refresh the row image,
        // modify payload/valid/tid, then finish_write before releasing this
        // same smeta latch.  Keeping the update and release together avoids
        // a second bare SCC access between two latch acquisitions.
        static bool remote_write_lock_update_and_release(
            TwoPLPashaMetadataShared *smeta, std::size_t host_id,
            const void *src, std::size_t size, uint64_t new_tid,
            bool dec_ref_cnt = false, bool allow_invalid = false)
        {
                DCHECK(smeta != nullptr);
                smeta->lock();
                auto *scc_data = smeta->get_scc_data();
                scc_manager->prepare_read(smeta, host_id, scc_data,
                                          scc_data_bytes(size));
                if ((!allow_invalid &&
                     !scc_data->get_flag(TwoPLPashaSharedDataSCC::valid_flag_index)) ||
                    !smeta->is_write_locked()) {
                        smeta->unlock();
                        return false;
                }
                tigonkv::engine::mem_access::SharedPayloadWrite(scc_data->data,
                                                                  size);
                scc_manager->do_write(smeta, host_id, scc_data->data, src, size);
                scc_data->set_flag(TwoPLPashaSharedDataSCC::valid_flag_index);
                smeta->set_is_data_modified_since_moved_in();
                if (dec_ref_cnt) {
                        smeta->decrement_ref_cnt();
                }
                smeta->clear_write_locked();
                tigonkv::engine::mem_access::SharedPayloadWrite(
                    &scc_data->tid, sizeof(scc_data->tid));
                scc_data->tid = new_tid;
                scc_manager->finish_write(smeta, host_id, scc_data,
                                          scc_data_bytes(size));
                smeta->unlock();
                return true;
        }

        // Shared-resolver forms of the original local-row branches.  Legacy
        // metadata supplies a migrated_row pointer while the KV layout
        // supplies a RegionOffset through ValueStruct::meta; both enter this
        // one latch/SCC state machine.
        template <typename LocalMetadata>
        static uint64_t take_read_lock_and_read(
            LocalMetadata &lmeta, const void *src, void *dest, std::size_t size,
            bool &success, bool *migrated = nullptr)
        {
                using Access = LocalMetadataAccess<LocalMetadata>;
                uint64_t old_value = 0;
                bool wrote = false;
                Access::Lock(lmeta);
                lmeta.lock();
                Access::Read(lmeta);
                if (migrated != nullptr) *migrated = lmeta.is_migrated;
                if (!lmeta.is_valid || lmeta.is_migrated) {
                        success = false;
                } else {
                        old_value = lmeta.tid;
                        if (is_write_locked(old_value) ||
                            read_lock_num(old_value) == read_lock_max()) {
                                success = false;
                        } else {
                                lmeta.tid = old_value +
                                    (uint64_t{1} << READ_LOCK_BIT_OFFSET);
                                std::memcpy(dest, src, size);
                                success = true;
                                wrote = true;
                        }
                }
                if (wrote) Access::Write(lmeta);
                Access::Unlock(lmeta);
                lmeta.unlock();
                return remove_lock_bit(old_value);
        }

        // This is the migrated half of the original tuple overload above.
        // The resolver is the only layout-specific part: owner-private KV
        // metadata persists an HWCC RegionOffset instead of master's VA.
        template <typename LocalMetadata, typename ResolveShared>
        static uint64_t take_read_lock_and_read(
            LocalMetadata &lmeta, void *local_data, void *dest,
            std::size_t size, bool &success, ResolveShared &&resolve_shared,
            std::size_t host_id, TwoPLPashaMetadataShared **shared_locked,
            bool *migrated = nullptr, bool *local_refreshed = nullptr)
        {
                using Access = LocalMetadataAccess<LocalMetadata>;
                uint64_t old_value = 0;
                success = false;
                if (shared_locked != nullptr) *shared_locked = nullptr;
                if (local_refreshed != nullptr) *local_refreshed = false;
                Access::Lock(lmeta);
                lmeta.lock();
                Access::Read(lmeta);
                const bool row_migrated = lmeta.is_migrated;
                if (migrated != nullptr) *migrated = row_migrated;
                if (!row_migrated) {
                        if (!lmeta.is_valid) {
                                Access::Unlock(lmeta);
                                lmeta.unlock();
                                return 0;
                        }
                        old_value = lmeta.tid;
                        if (is_write_locked(old_value) ||
                            read_lock_num(old_value) == read_lock_max()) {
                                Access::Unlock(lmeta);
                                lmeta.unlock();
                                return remove_lock_bit(old_value);
                        }
                        lmeta.tid = old_value +
                            (uint64_t{1} << READ_LOCK_BIT_OFFSET);
                        std::memcpy(dest, local_data, size);
                        success = true;
                        Access::Write(lmeta);
                        Access::Unlock(lmeta);
                        lmeta.unlock();
                        return remove_lock_bit(old_value);
                }

                auto *smeta = resolve_shared(lmeta);
                if (smeta == nullptr || scc_manager == nullptr) {
                        Access::Unlock(lmeta);
                        lmeta.unlock();
                        return 0;
                }
                smeta->lock();
                auto *scc_data = smeta->get_scc_data();
                scc_manager->prepare_read(smeta, host_id, scc_data,
                                          scc_data_bytes(size));
                bool wrote = false;
                if (smeta->is_data_modified_since_moved_in()) {
                        if (!scc_data->get_flag(
                                TwoPLPashaSharedDataSCC::valid_flag_index)) {
                                smeta->unlock();
                                Access::Unlock(lmeta);
                                lmeta.unlock();
                                return 0;
                        }
                        old_value = scc_data->tid;
                        lmeta.is_valid = true;
                        lmeta.tid = scc_data->tid;
                        tigonkv::engine::mem_access::SharedPayloadRead(
                            scc_data->data, size);
                        tigonkv::engine::mem_access::PrivateWrite(local_data,
                                                                    size);
                        scc_manager->do_read(nullptr, host_id, local_data,
                                             scc_data->data, size);
                        smeta->clear_is_data_modified_since_moved_in();
                        wrote = true;
                        if (local_refreshed != nullptr)
                                *local_refreshed = true;
                } else {
                        if (!lmeta.is_valid) {
                                smeta->unlock();
                                Access::Unlock(lmeta);
                                lmeta.unlock();
                                return 0;
                        }
                        old_value = lmeta.tid;
                }
                if (smeta->is_write_locked() ||
                    smeta->get_reader_count() ==
                        smeta->get_reader_count_max()) {
                        smeta->unlock();
                        if (wrote) Access::Write(lmeta);
                        Access::Unlock(lmeta);
                        lmeta.unlock();
                        return remove_lock_bit(old_value);
                }
                smeta->increase_reader_count();
                std::memcpy(dest, local_data, size);
                success = true;
                if (shared_locked != nullptr) *shared_locked = smeta;
                smeta->unlock();
                if (wrote) Access::Write(lmeta);
                Access::Unlock(lmeta);
                lmeta.unlock();
                return remove_lock_bit(old_value);
        }

        template <typename LocalMetadata>
        static uint64_t take_write_lock(LocalMetadata &lmeta,
                                                   bool &success,
                                                   bool *migrated = nullptr)
        {
                using Access = LocalMetadataAccess<LocalMetadata>;
                uint64_t old_value = 0;
                bool wrote = false;
                Access::Lock(lmeta);
                lmeta.lock();
                Access::Read(lmeta);
                if (migrated != nullptr) *migrated = lmeta.is_migrated;
                if (!lmeta.is_valid || lmeta.is_migrated) {
                        success = false;
                } else {
                        old_value = lmeta.tid;
                        if (is_read_locked(old_value) || is_write_locked(old_value)) {
                                success = false;
                        } else {
                                lmeta.tid = old_value |
                                    (WRITE_LOCK_BIT_MASK << WRITE_LOCK_BIT_OFFSET);
                                success = true;
                                wrote = true;
                        }
                }
                if (wrote) Access::Write(lmeta);
                Access::Unlock(lmeta);
                lmeta.unlock();
                return remove_lock_bit(old_value);
        }

        // Offset-adapted complete owner write-lock acquisition.  This is the
        // migrated continuation of the original tuple-based write_lock: it
        // refreshes the owner mirror under lmeta→smeta, then transfers the
        // write lock to smeta.  `local_refreshed` remains for legacy
        // local_cxl_access accounting only; owner-private SWCC state writes
        // are recorded by LocalMetadataAccess inside this critical section.
        template <typename LocalMetadata, typename ResolveShared>
        static uint64_t take_write_lock(
            LocalMetadata &lmeta, void *local_data, std::size_t size,
            bool &success, ResolveShared &&resolve_shared, std::size_t host_id,
            TwoPLPashaMetadataShared **shared_locked,
            bool *migrated = nullptr, bool *local_refreshed = nullptr)
        {
                using Access = LocalMetadataAccess<LocalMetadata>;
                uint64_t old_value = 0;
                success = false;
                if (shared_locked != nullptr) *shared_locked = nullptr;
                if (local_refreshed != nullptr) *local_refreshed = false;
                Access::Lock(lmeta);
                lmeta.lock();
                Access::Read(lmeta);
                try {
                        const bool row_migrated = lmeta.is_migrated;
                        if (migrated != nullptr) *migrated = row_migrated;
                        if (!row_migrated) {
                                if (!lmeta.is_valid) {
                                        Access::Unlock(lmeta);
                                        lmeta.unlock();
                                        return 0;
                                }
                                old_value = lmeta.tid;
                                if (is_read_locked(old_value) ||
                                    is_write_locked(old_value)) {
                                        Access::Unlock(lmeta);
                                        lmeta.unlock();
                                        return remove_lock_bit(old_value);
                                }
                                lmeta.tid = old_value |
                                    (WRITE_LOCK_BIT_MASK << WRITE_LOCK_BIT_OFFSET);
                                success = true;
                                Access::Write(lmeta);
                                Access::Unlock(lmeta);
                                lmeta.unlock();
                                return remove_lock_bit(old_value);
                        }

                        auto *smeta = resolve_shared(lmeta);
                        if (smeta == nullptr || scc_manager == nullptr) {
                                Access::Unlock(lmeta);
                                lmeta.unlock();
                                return 0;
                        }
                        smeta->lock();
                        auto *scc_data = smeta->get_scc_data();
                        scc_manager->prepare_read(smeta, host_id, scc_data,
                                                  scc_data_bytes(size));
                        const bool shared_dirty =
                            smeta->is_data_modified_since_moved_in();
                        bool wrote = false;
                        if (shared_dirty) {
                                if (!scc_data->get_flag(
                                        TwoPLPashaSharedDataSCC::valid_flag_index)) {
                                        smeta->unlock();
                                        Access::Unlock(lmeta);
                                        lmeta.unlock();
                                        return 0;
                                }
                                lmeta.is_valid = true;
                                lmeta.tid = scc_data->tid;
                                tigonkv::engine::mem_access::SharedPayloadRead(
                                    scc_data->data, size);
                                tigonkv::engine::mem_access::PrivateWrite(local_data,
                                                                            size);
                                scc_manager->do_read(nullptr, host_id, local_data,
                                                     scc_data->data, size);
                                smeta->clear_is_data_modified_since_moved_in();
                                wrote = true;
                                if (local_refreshed != nullptr)
                                        *local_refreshed = true;
                        } else if (!lmeta.is_valid) {
                                smeta->unlock();
                                Access::Unlock(lmeta);
                                lmeta.unlock();
                                return 0;
                        }
                        // Preserve the original owner-mirror rule: only a
                        // dirty shared row refreshes the local TID.  Otherwise
                        // the local mirror remains the authority for this
                        // owner-side lock acquisition.
                        old_value = shared_dirty ? scc_data->tid : lmeta.tid;
                        if (smeta->get_reader_count() != 0 ||
                            smeta->is_write_locked()) {
                                smeta->unlock();
                                if (wrote) Access::Write(lmeta);
                                Access::Unlock(lmeta);
                                lmeta.unlock();
                                return remove_lock_bit(old_value);
                        }
                        smeta->set_write_locked();
                        if (shared_locked != nullptr) *shared_locked = smeta;
                        success = true;
                        smeta->unlock();
                        if (wrote) Access::Write(lmeta);
                        Access::Unlock(lmeta);
                        lmeta.unlock();
                        return remove_lock_bit(old_value);
                } catch (...) {
                        Access::Unlock(lmeta);
                        lmeta.unlock();
                        throw;
                }
        }

        // Offset-backed owner rows retain the original lock-and-read contract
        // without recreating the old pointer-based MetaDataType tuple.  A
        // migrated row is deliberately reported to its existing SCC path.
        template <typename LocalMetadata>
        static uint64_t take_write_lock_and_read(
            LocalMetadata &lmeta, const void *src, void *dest,
            std::size_t size, bool &success, bool *migrated = nullptr)
        {
                const uint64_t tid = take_write_lock(lmeta, success, migrated);
                if (success) std::memcpy(dest, src, size);
                return tid;
        }

        template <typename LocalMetadata, typename ResolveShared>
        static void read_lock_release(LocalMetadata &lmeta,
                                      ResolveShared &&resolve_shared)
        {
                using Access = LocalMetadataAccess<LocalMetadata>;
                Access::Lock(lmeta);
                lmeta.lock();
                Access::Read(lmeta);
                bool wrote = false;
                if (!lmeta.is_migrated) {
                        const uint64_t old_value = lmeta.tid;
                        DCHECK(is_read_locked(old_value));
                        DCHECK(!is_write_locked(old_value));
                        lmeta.tid = old_value -
                            (uint64_t{1} << READ_LOCK_BIT_OFFSET);
                        wrote = true;
                } else {
                        auto *smeta = resolve_shared(lmeta);
                        if (smeta == nullptr) {
                                Access::Unlock(lmeta);
                                lmeta.unlock();
                                throw std::runtime_error(
                                    "migrated local read release without smeta");
                        }
                        smeta->lock();
                        DCHECK(smeta->get_reader_count() > 0);
                        DCHECK(!smeta->is_write_locked());
                        smeta->decrease_reader_count();
                        smeta->unlock();
                }
                if (wrote) Access::Write(lmeta);
                Access::Unlock(lmeta);
                lmeta.unlock();
        }

        template <typename LocalMetadata, typename ResolveShared>
        static void write_lock_release(LocalMetadata &lmeta,
                                       uint64_t new_tid, std::size_t size,
                                       std::size_t host_id,
                                       ResolveShared &&resolve_shared)
        {
                using Access = LocalMetadataAccess<LocalMetadata>;
                Access::Lock(lmeta);
                lmeta.lock();
                Access::Read(lmeta);
                bool wrote = false;
                if (!lmeta.is_migrated) {
                        DCHECK(is_write_locked(lmeta.tid));
                        DCHECK(!is_read_locked(lmeta.tid));
                        DCHECK(!is_write_locked(new_tid));
                        DCHECK(!is_read_locked(new_tid));
                        lmeta.tid = new_tid;
                        wrote = true;
                } else {
                        auto *smeta = resolve_shared(lmeta);
                        if (smeta == nullptr) {
                                Access::Unlock(lmeta);
                                lmeta.unlock();
                                throw std::runtime_error(
                                    "migrated local write release without smeta");
                        }
                        auto *scc_data = smeta->get_scc_data();
                        smeta->lock();
                        DCHECK(smeta->get_reader_count() == 0);
                        DCHECK(smeta->is_write_locked());
                        DCHECK(!is_write_locked(new_tid));
                        DCHECK(!is_read_locked(new_tid));
                        smeta->clear_write_locked();
                        tigonkv::engine::mem_access::SharedPayloadWrite(
                            &scc_data->tid, sizeof(scc_data->tid));
                        scc_data->tid = new_tid;
                        scc_manager->finish_write(smeta, host_id, scc_data,
                                                  scc_data_bytes(size));
                        smeta->unlock();
                }
                if (wrote) Access::Write(lmeta);
                Access::Unlock(lmeta);
                lmeta.unlock();
        }

        TwoPLPashaHelper(std::size_t coordinator_id, Context context, std::vector<std::vector<CXLTableBase *> > &cxl_tbl_vecs)
                : coordinator_id(coordinator_id)
                , context(context)
                , cxl_tbl_vecs(cxl_tbl_vecs)
        {
        }

        // Pure per-row adjacency predicate from TwoPLPashaExecutor.h:294-310.
        // Caller holds smeta latch. Returns true when bits satisfy the scan rule
        // (so migration is NOT required). Branch order is intentional:
        //   key == min  → need next_real only
        //   size==limit → need prev_real only
        //   otherwise   → need prev_real && next_real
        // Does not lock, touch transport, B+Tree, or KV strings (§4.6.2).
        static bool scan_row_adjacency_ok(bool key_equals_min,
                                          bool is_limit_boundary,
                                          bool prev_real, bool next_real)
        {
                if (key_equals_min) return next_real;
                if (is_limit_boundary) return prev_real;
                return prev_real && next_real;
        }

        // The two scan drivers are the original Executor callback bodies with
        // only the row/lock operations parameterized.  They consume the
        // caller's existing result vector directly; no fragment or mirrored
        // result-count state is kept here.
        template <typename Row, typename Acquire, typename RowKey>
        static void scan_local_fragment(
            ITable &table, const void *min_key, const void *max_key,
            uint64_t limit, std::vector<Row> &scan_results, Row *next_row,
            bool &has_next_row, bool &scan_success, Acquire &&acquire,
            RowKey &&row_key, bool lock_terminal_tuple = true,
            bool retain_right_boundary = true)
        {
                if (next_row == nullptr)
                        throw std::invalid_argument("null local scan boundary");
                has_next_row = false;
                scan_success = true;
                table.scan(min_key, [&](const void *key,
                                        ITable::MetaDataType *meta,
                                        void *data, bool is_last_tuple) -> bool {
                        DCHECK(key != nullptr);
                        DCHECK(meta != nullptr);
                        DCHECK(data != nullptr);
                        const bool locking_next_tuple =
                            (lock_terminal_tuple && is_last_tuple) ||
                            (limit != 0 && scan_results.size() == limit) ||
                            table.compare_key(key, max_key) > 0;
                        if (table.compare_key(key, min_key) < 0) return false;
                        const void *previous_key = scan_results.empty()
                            ? nullptr : row_key(scan_results.back());
                        if (previous_key != nullptr &&
                            table.compare_key(key, previous_key) <= 0)
                                return false;
                        Row row{};
                        if (!acquire(key, meta, data, locking_next_tuple, &row)) {
                                scan_success = false;
                                return true;
                        }
                        if (locking_next_tuple) {
                                if (retain_right_boundary) {
                                        *next_row = std::move(row);
                                        has_next_row = true;
                                }
                                return true;
                        }
                        scan_results.emplace_back(std::move(row));
                        return false;
                });
        }

        template <typename SharedTable, typename Row, typename CompareKey,
                  typename AdjacencyOk, typename Acquire, typename RowKey>
        static void scan_remote_fragment(
            CompareKey &&compare_key, SharedTable &shared_table,
            const void *min_key, const void *max_key, uint64_t limit,
            std::vector<Row> &scan_results, Row *next_row,
            bool &has_next_row, bool &scan_success, bool &migration_required,
            bool &busy, AdjacencyOk &&adjacency_ok, Acquire &&acquire,
            RowKey &&row_key)
        {
                if (next_row == nullptr)
                        throw std::invalid_argument("null remote scan boundary");
                has_next_row = false;
                scan_success = false;
                migration_required = false;
                busy = false;
                shared_table.scan(min_key, [&](const void *key, void *cxl_row,
                                               bool is_last_tuple) -> bool {
                        DCHECK(key != nullptr);
                        DCHECK(cxl_row != nullptr);
                        const bool locking_next_tuple =
                            is_last_tuple ||
                            (limit != 0 && scan_results.size() == limit) ||
                            compare_key(key, max_key) > 0;
                        if (compare_key(key, min_key) < 0) return false;
                        const void *previous_key = scan_results.empty()
                            ? nullptr : row_key(scan_results.back());
                        if (previous_key != nullptr &&
                            compare_key(key, previous_key) <= 0)
                                return false;
                        if (!adjacency_ok(key, cxl_row, is_last_tuple,
                                          scan_results.size(),
                                          locking_next_tuple)) {
                                migration_required = true;
                                return true;
                        }
                        Row row{};
                        if (!acquire(key, cxl_row, locking_next_tuple, &row)) {
                                busy = true;
                                return true;
                        }
                        if (locking_next_tuple) {
                                *next_row = std::move(row);
                                has_next_row = true;
                                scan_success = true;
                                return true;
                        }
                        scan_results.emplace_back(std::move(row));
                        return false;
                });
                if (scan_results.empty() && !scan_success && !busy &&
                    !migration_required)
                        migration_required = true;
        }

        // Offset-backed form of the outer traversal in master's
        // delete_and_update_next_key_info().  The table keeps ownership of
        // its B+Tree callback/latch lifetime; the caller supplies only the
        // RegionOffset-specific row body.  This avoids a second KV-only
        // adjacent-remove traversal while preserving the original callback
        // arguments and return contract.
        template <typename DeleteProcessor>
        static bool delete_and_update_next_key_info(
            ITable *table, const void *key, DeleteProcessor &&processor)
        {
                if (table == nullptr)
                        throw std::invalid_argument("null delete table");
                return table->remove_and_process_adjacent_tuples(
                    key, std::forward<DeleteProcessor>(processor));
        }

        // Keep B+Tree's original adjacent-tuple callback/latch lifetime in
        // one template.  Legacy and fixed-KV differ only in the callback's
        // row representation and optional successor-lock adapter.
        template <typename InsertProcessor>
        static bool insert_and_update_next_key_info(
            ITable *table, const void *key, const void *value,
            InsertProcessor &&processor)
        {
                if (table == nullptr)
                        throw std::invalid_argument("null insert table");
                return table->insert_and_process_adjacent_tuples(
                    key, value, std::forward<InsertProcessor>(processor), true);
        }

        // The original insert/delete/move-out callback updates predecessor
        // then successor while each local latch is held.  Both the legacy
        // pointer form and the offset-backed KV form use this one body; their
        // resolver is the sole representation adapter.
        template <typename LocalMetadata, typename ResolveShared>
        static void update_adjacent_migrated_rows(
            LocalMetadata *prev, LocalMetadata *next, bool set_bits,
            ResolveShared &&resolve_shared)
        {
                const auto update = [&](LocalMetadata *local, bool next_bit) {
                        if (local == nullptr) return;
                        using Access = LocalMetadataAccess<LocalMetadata>;
                        Access::Lock(*local);
                        local->lock();
                        Access::Read(*local);
                        try {
                                if (local->is_migrated) {
                                        auto *smeta = resolve_shared(*local);
                                        if (smeta == nullptr)
                                                throw std::runtime_error(
                                                    "migrated local metadata has no shared metadata");
                                        smeta->lock();
                                        if (set_bits) {
                                                if (next_bit)
                                                        smeta->set_next_key_real_bit();
                                                else
                                                        smeta->set_prev_key_real_bit();
                                        } else {
                                                if (next_bit)
                                                        smeta->clear_next_key_real_bit();
                                                else
                                                        smeta->clear_prev_key_real_bit();
                                        }
                                        smeta->unlock();
                                }
                                Access::Unlock(*local);
                                local->unlock();
                        } catch (...) {
                                Access::Unlock(*local);
                                local->unlock();
                                throw;
                        }
                };
                update(prev, true);
                update(next, false);
        }

        template <typename LocalMetadata, typename ResolveShared>
        static void clear_adjacent_migrated_rows(
            LocalMetadata *prev, LocalMetadata *next,
            ResolveShared &&resolve_shared)
        {
                update_adjacent_migrated_rows(
                    prev, next, false,
                    std::forward<ResolveShared>(resolve_shared));
        }

        template <typename LocalMetadata, typename ResolveShared>
        static void set_adjacent_migrated_rows(
            LocalMetadata *prev, LocalMetadata *next,
            ResolveShared &&resolve_shared)
        {
                update_adjacent_migrated_rows(
                    prev, next, true,
                    std::forward<ResolveShared>(resolve_shared));
        }

        // Shared half of master's delete_and_update_next_key_info().  The
        // caller owns local metadata/EBR; this primitive owns exactly the
        // smeta/SCC invalidation and shared-index rollback sequence.
        template <typename RemoveShared>
        static RowOutcome delete_and_update_next_key_info(
            TwoPLPashaMetadataShared *smeta, std::size_t host_id,
            std::size_t scc_bytes, bool writer_prelocked,
            bool requester_prelocked, bool is_local_delete,
            RemoveShared &&remove_shared)
        {
                if (smeta == nullptr || scc_manager == nullptr)
                        throw std::invalid_argument("null delete shared row");
                smeta->lock();
                auto *payload = smeta->get_scc_data();
                if (smeta->get_reader_count() != 0 ||
                    (requester_prelocked
                         ? (smeta->get_ref_cnt() != 1 || !smeta->is_write_locked())
                         : (smeta->get_ref_cnt() != 0 ||
                            (!writer_prelocked && smeta->is_write_locked())))) {
                        smeta->unlock();
                        return RowOutcome::kBusy;
                }
                if (!writer_prelocked) smeta->set_write_locked();
                scc_manager->prepare_read(smeta, host_id, payload, scc_bytes);
                if (is_local_delete) {
                        DCHECK(payload->get_flag(
                            TwoPLPashaSharedDataSCC::valid_flag_index));
                        payload->clear_flag(
                            TwoPLPashaSharedDataSCC::valid_flag_index);
                } else {
                        DCHECK(!payload->get_flag(
                            TwoPLPashaSharedDataSCC::valid_flag_index));
                }
                scc_manager->finish_write(smeta, host_id, payload, scc_bytes);
                if (!remove_shared(smeta)) {
                        smeta->unlock();
                        throw std::runtime_error(
                            "shared tree remove failed during delete");
                }
                if (requester_prelocked) {
                        if (smeta->get_ref_cnt() == 0) {
                                smeta->unlock();
                                throw std::runtime_error(
                                    "shared delete reference count underflow");
                        }
                        smeta->decrement_ref_cnt();
                }
                // The delete callback owns the final shared write lock.  It
                // must consume it before the smeta is retired; PolicyClock
                // must not touch a callback result after returning.
                smeta->clear_write_locked();
                smeta->unlock();
                return RowOutcome::kDone;
        }

        template <typename LocalMetadata, typename RemoveShared,
                  typename RetireShared>
        static RowOutcome move_from_btree_to_partition(
            LocalMetadata &local, void *local_data,
            TwoPLPashaMetadataShared *smeta, std::size_t host_id,
            std::size_t value_bytes, RemoveShared &&remove_shared,
            RetireShared &&retire_shared)
        {
                if (smeta == nullptr || local_data == nullptr ||
                    scc_manager == nullptr)
                        throw std::invalid_argument("null move-out row");
                DCHECK(local.is_valid == true);
                smeta->lock();
                auto *payload = smeta->get_scc_data();
                if (smeta->get_ref_cnt() != 0) {
                        smeta->unlock();
                        return RowOutcome::kBusy;
                }
                scc_manager->prepare_read(smeta, host_id, payload,
                                          scc_data_bytes(value_bytes));
                local.is_valid = payload->get_flag(
                    TwoPLPashaSharedDataSCC::valid_flag_index);
                local.tid = payload->tid;
                set_read_lock_num(local.tid, smeta->get_reader_count());
                if (smeta->is_write_locked())
                        set_write_lock_bit(local.tid);
                else
                        clear_write_lock_bit(local.tid);
                DCHECK(read_lock_num(local.tid) == smeta->get_reader_count());
                DCHECK(is_write_locked(local.tid) == smeta->is_write_locked());
                if (smeta->is_data_modified_since_moved_in()) {
                        tigonkv::engine::mem_access::SharedPayloadRead(
                            payload->data, value_bytes);
                        tigonkv::engine::mem_access::PrivateWrite(local_data,
                                                                   value_bytes);
                        scc_manager->do_read(smeta, host_id, local_data,
                                             payload->data, value_bytes);
                        smeta->clear_is_data_modified_since_moved_in();
                }
                local.is_data_modified_since_moved_out = false;
                payload->clear_flag(TwoPLPashaSharedDataSCC::valid_flag_index);
                local.is_migrated = false;
                local.migrated_row = decltype(local.migrated_row){};
                scc_manager->finish_write(smeta, host_id, payload,
                                          scc_data_bytes(value_bytes));
                retire_shared(payload, smeta);
                // Master releases the shared metadata latch before removing
                // the corresponding shared-index entry.  The outer local
                // metadata latch remains held by the B+Tree callback.
                smeta->unlock();
                if (!remove_shared()) {
                        throw std::runtime_error(
                            "shared tree remove failed during move-out");
                }
                return RowOutcome::kDone;
        }

        // SCC/publication half of master's
        // move_from_btree_to_shared_region().  The caller retains allocator,
        // ITable insertion and RegionOffset ownership; this primitive retains
        // the original local-TID transfer, row-image publication and latch
        // order as one operation.
        template <typename LocalMetadata, typename PublishLocal>
        static migration_result move_from_btree_to_shared_region(
            LocalMetadata &local, const void *local_data,
            TwoPLPashaMetadataShared *smeta,
            TwoPLPashaSharedDataSCC *payload, std::size_t host_id,
            std::size_t value_bytes, bool copy_payload, bool inc_ref_cnt,
            bool prev_migrated, bool next_migrated,
            PublishLocal &&publish_local)
        {
                if (local_data == nullptr || smeta == nullptr || payload == nullptr ||
                    scc_manager == nullptr)
                        throw std::invalid_argument("null move-in row");

                smeta->lock();
                if (inc_ref_cnt &&
                    smeta->get_ref_cnt() == std::numeric_limits<uint8_t>::max()) {
                        smeta->unlock();
                        throw std::runtime_error("shared row reference count overflow");
                }
                const uint64_t local_tid = local.tid;
                smeta->set_reader_count(read_lock_num(local_tid));
                if (is_write_locked(local_tid))
                        smeta->set_write_locked();
                else
                        smeta->clear_write_locked();
                DCHECK(read_lock_num(local_tid) == smeta->get_reader_count());
                DCHECK(is_write_locked(local_tid) == smeta->is_write_locked());

                if (copy_payload) {
                        tigonkv::engine::mem_access::PrivateRead(local_data,
                                                                  value_bytes);
                        tigonkv::engine::mem_access::SharedPayloadWrite(
                            payload->data, value_bytes);
                        scc_manager->do_write(smeta, host_id, payload->data,
                                              local_data, value_bytes);
                }
                local.is_data_modified_since_moved_out = false;
                smeta->clear_is_data_modified_since_moved_in();
                if (local.is_valid)
                        payload->set_flag(
                            TwoPLPashaSharedDataSCC::valid_flag_index);
                else
                        payload->clear_flag(
                            TwoPLPashaSharedDataSCC::valid_flag_index);
                tigonkv::engine::mem_access::SharedPayloadWrite(
                    &payload->tid, sizeof(payload->tid));
                payload->tid = local_tid;

                if (inc_ref_cnt)
                        smeta->increment_ref_cnt();
                if (next_migrated)
                        smeta->set_next_key_real_bit();
                else
                        smeta->clear_next_key_real_bit();
                if (prev_migrated)
                        smeta->set_prev_key_real_bit();
                else
                        smeta->clear_prev_key_real_bit();

                publish_local();
                scc_manager->finish_write(smeta, host_id, payload,
                                          scc_data_bytes(value_bytes));
                smeta->unlock();
                return migration_result::SUCCESS;
        }

        // Requester half of master's remote_modify_tuple_valid_bit(...,
        // false).  The caller has already taken the original shared-row ref;
        // this helper owns only the smeta latch and the SCC-header valid-bit
        // publication, leaving index pin/Clock/RPC ownership in the adapter.
        static RowOutcome prepare_remote_delete(
            TwoPLPashaMetadataShared *smeta, std::size_t host_id,
            std::size_t scc_header_bytes)
        {
                if (smeta == nullptr || scc_manager == nullptr)
                        throw std::invalid_argument("null remote delete row");
                smeta->lock();
                auto *payload = smeta->get_scc_data();
                scc_manager->prepare_read(smeta, host_id, payload,
                                          scc_header_bytes);
                if (!payload->get_flag(
                        TwoPLPashaSharedDataSCC::valid_flag_index)) {
                        smeta->unlock();
                        return RowOutcome::kMissing;
                }
                if (smeta->is_write_locked() || smeta->get_reader_count() != 0) {
                        smeta->unlock();
                        return RowOutcome::kBusy;
                }
                smeta->set_write_locked();
                payload->clear_flag(TwoPLPashaSharedDataSCC::valid_flag_index);
                scc_manager->finish_write(smeta, host_id, payload,
                                          scc_header_bytes);
                smeta->unlock();
                return RowOutcome::kDone;
        }

        // Roll back the requester half above after a non-success delete ack.
        // A successful owner callback retires the row and consumes this pin,
        // so it must never call this release path.
        static void abort_remote_delete(TwoPLPashaMetadataShared *smeta,
                                        std::size_t host_id,
                                        std::size_t scc_header_bytes)
        {
                if (smeta == nullptr || scc_manager == nullptr)
                        throw std::invalid_argument("null remote delete rollback row");
                smeta->lock();
                // Only the Pending -> Cancelled transition may roll back.
                // Once the owner claimed (cleared the write-lock bit) or
                // published Deleted, rolling back would resurrect a row the
                // owner already deleted (double fact).  That is a hard
                // protocol error, never a silent restore.
                if (!smeta->requester_holds_write_lock()) {
                        smeta->unlock();
                        throw std::runtime_error(
                            "remote delete rollback raced owner claim: owner "
                            "already transitioned Pending -> Executing/Deleted");
                }
                auto *payload = smeta->get_scc_data();
                scc_manager->prepare_read(smeta, host_id, payload,
                                          scc_header_bytes);
                payload->set_flag(TwoPLPashaSharedDataSCC::valid_flag_index);
                // Pending -> Cancelled: publish the cancellation by clearing
                // the write-lock bit with the restored valid flag.
                smeta->clear_write_locked();
                scc_manager->finish_write(smeta, host_id, payload,
                                          scc_header_bytes);
                smeta->decrement_ref_cnt();
                smeta->unlock();
        }

        // Explicit migration-style pin used when a requester observes an
        // already-shared row (FAIL_ALREADY_IN_CXL) and must keep move-out
        // blocked until the request finishes.  Matches get_migrated_row /
        // release_migrated_row.
        static bool get_migrated_row(TwoPLPashaMetadataShared *smeta,
                                     std::size_t host_id,
                                     std::size_t value_bytes,
                                     bool allow_invalid = false)
        {
                if (smeta == nullptr || scc_manager == nullptr) return false;
                smeta->lock();
                auto *scc_data = smeta->get_scc_data();
                scc_manager->prepare_read(smeta, host_id, scc_data,
                                          scc_data_bytes(value_bytes));
                // Refuse move-out / invalid rows. write_locked is set by MoveOut
                // before tree remove; pinning through that window UAF'd smeta and
                // wedged Forward under YCSB-A.
                const bool valid = scc_data->get_flag(
                    TwoPLPashaSharedDataSCC::valid_flag_index);
                const bool write_locked = smeta->is_write_locked();
                const uint8_t ref_count = smeta->get_ref_cnt();
                if ((!allow_invalid && !valid) || write_locked ||
                    ref_count == std::numeric_limits<uint8_t>::max()) {
                        smeta->unlock();
                        return false;
                }
                smeta->increment_ref_cnt();
                smeta->unlock();
                return true;
        }

        static void release_migrated_row(TwoPLPashaMetadataShared *smeta)
        {
                if (smeta == nullptr) return;
                smeta->lock();
                if (smeta->get_ref_cnt() == 0) {
                        LOG(FATAL) << "TwoPLPasha shared ref underflow smeta="
                                   << smeta << " valid="
                                   << smeta->get_scc_data()->get_flag(
                                          TwoPLPashaSharedDataSCC::valid_flag_index)
                                   << " write_locked=" << smeta->is_write_locked();
                }
                smeta->decrement_ref_cnt();
                smeta->unlock();
        }

        static bool modify_tuple_valid_bit(TwoPLPashaMetadataShared *smeta,
                                           std::size_t coordinator_id,
                                           std::size_t scc_bytes,
                                           bool is_valid, bool is_insert,
                                           uint64_t tid = 0,
                                           bool update_tid = false)
        {
                if (smeta == nullptr || scc_manager == nullptr)
                        throw std::invalid_argument("null shared valid publisher");
                // The master ABI exposes is_insert.  Under non-coherent shared
                // SWCC every valid-bit mutation is a publication, so both
                // forms finish the same prepared SCC image.
                (void)is_insert;
                smeta->lock();
                auto *scc_data = smeta->get_scc_data();
                scc_manager->prepare_read(smeta, coordinator_id, scc_data,
                                          scc_bytes);
                if (scc_data->get_flag(
                        TwoPLPashaSharedDataSCC::valid_flag_index) != !is_valid) {
                        smeta->unlock();
                        throw std::runtime_error(
                            "shared valid publication violated row invariant");
                }
                if (is_valid)
                        scc_data->set_flag(
                            TwoPLPashaSharedDataSCC::valid_flag_index);
                else
                        scc_data->clear_flag(
                            TwoPLPashaSharedDataSCC::valid_flag_index);
                if (update_tid) {
                        tigonkv::engine::mem_access::SharedPayloadWrite(
                            &scc_data->tid, sizeof(scc_data->tid));
                        scc_data->tid = tid;
                }
                scc_manager->finish_write(smeta, coordinator_id, scc_data,
                                          scc_bytes);
                smeta->unlock();
                return true;
        }

        static bool remote_modify_tuple_valid_bit(
            TwoPLPashaMetadataShared *smeta, std::size_t coordinator_id,
            std::size_t scc_bytes, bool is_valid, bool is_insert,
            bool consume_ref, bool mark_shared_dirty)
        {
                if (smeta == nullptr || scc_manager == nullptr)
                        throw std::invalid_argument("null remote valid publisher");
                (void)is_insert;
                smeta->lock();
                auto *scc_data = smeta->get_scc_data();
                scc_manager->prepare_read(smeta, coordinator_id, scc_data,
                                          scc_bytes);
                if (smeta->get_ref_cnt() == 0) {
                        smeta->unlock();
                        throw std::runtime_error(
                            "remote valid publication reference count underflow");
                }
                if (scc_data->get_flag(
                        TwoPLPashaSharedDataSCC::valid_flag_index) != !is_valid) {
                        smeta->unlock();
                        throw std::runtime_error(
                            "remote valid publication violated row invariant");
                }
                if (is_valid)
                        scc_data->set_flag(
                            TwoPLPashaSharedDataSCC::valid_flag_index);
                else
                        scc_data->clear_flag(
                            TwoPLPashaSharedDataSCC::valid_flag_index);
                if (mark_shared_dirty)
                        smeta->set_is_data_modified_since_moved_in();
                scc_manager->finish_write(smeta, coordinator_id, scc_data,
                                          scc_bytes);
                if (consume_ref)
                        smeta->decrement_ref_cnt();
                smeta->unlock();
                return true;
        }

	uint64_t read(const std::tuple<MetaDataType *, void *> &row, void *dest, std::size_t size, std::atomic<uint64_t> &local_cxl_access)
	{
                MetaDataType &meta = *std::get<0>(row);
                TwoPLPashaMetadataLocal *lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(meta.load());
                uint64_t tid_ = 0;

                lmeta->lock();
                DCHECK(lmeta->is_valid == true);
                if (lmeta->is_migrated == false) {
		        void *src = std::get<1>(row);
		        std::memcpy(dest, src, size);
                        tid_ = lmeta->tid;
                } else {
                        // statistics
                        local_cxl_access.fetch_add(1);

                        TwoPLPashaMetadataShared *smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(lmeta->migrated_row);
                        TwoPLPashaSharedDataSCC *scc_data = smeta->get_scc_data();

                        void *src = scc_data->data;
                        smeta->lock();
                        scc_manager->prepare_read(nullptr, coordinator_id, scc_data,
                                                  scc_data_bytes(size));
                        DCHECK(smeta->get_flag(TwoPLPashaMetadataShared::valid_flag_index) == true);
                        scc_manager->do_read(nullptr, coordinator_id, dest, src, size);
                        tid_ = scc_data->tid;
                        smeta->unlock();
                }
                lmeta->unlock();

		return remove_lock_bit(tid_);
	}

        uint64_t remote_read(char *row, void *dest, std::size_t size)
	{
                // unused
		CHECK(0);
	}

        void update(const std::tuple<MetaDataType *, void *> &row, const void *value, std::size_t value_size)
	{
		MetaDataType &meta = *std::get<0>(row);
                TwoPLPashaMetadataLocal *lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(meta.load());

		lmeta->lock();
                DCHECK(lmeta->is_valid == true);
                if (lmeta->is_migrated == false) {
                        void *data_ptr = std::get<1>(row);
                        std::memcpy(data_ptr, value, value_size);
                        lmeta->is_data_modified_since_moved_out = true;
                } else {
                        TwoPLPashaMetadataShared *smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(lmeta->migrated_row);
                        TwoPLPashaSharedDataSCC *scc_data = smeta->get_scc_data();
                        void *data_ptr = scc_data->data;

                        smeta->lock();
                        DCHECK(smeta->get_flag(TwoPLPashaMetadataShared::valid_flag_index) == true);
                        scc_manager->do_write(nullptr, coordinator_id, data_ptr, value, value_size);
                        smeta->set_is_data_modified_since_moved_in();
                        smeta->unlock();
                }
		lmeta->unlock();
	}

        void remote_update(char *row, const void *value, std::size_t value_size)
	{
		TwoPLPashaMetadataShared *smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(row);
                TwoPLPashaSharedDataSCC *scc_data = smeta->get_scc_data();
                void *data_ptr = scc_data->data;

		smeta->lock();
                DCHECK(smeta->get_flag(TwoPLPashaMetadataShared::valid_flag_index) == true);
                scc_manager->do_write(nullptr, coordinator_id, data_ptr, value, value_size);
                smeta->set_is_data_modified_since_moved_in();
                smeta->unlock();
	}

	/**
	 * [write lock bit (1) |  read lock bit (9) -- 512 - 1 locks | seq id  (54) ]
	 *
	 */

	static bool is_read_locked(uint64_t value)
	{
		return value & (READ_LOCK_BIT_MASK << READ_LOCK_BIT_OFFSET);
	}

	static bool is_write_locked(uint64_t value)
	{
		return value & (WRITE_LOCK_BIT_MASK << WRITE_LOCK_BIT_OFFSET);
	}

	static uint64_t read_lock_num(uint64_t value)
	{
		return (value >> READ_LOCK_BIT_OFFSET) & READ_LOCK_BIT_MASK;
	}

        static void set_read_lock_num(uint64_t &value, uint64_t reader_count)
        {
                value &= ~(READ_LOCK_BIT_MASK << READ_LOCK_BIT_OFFSET);
                value += (reader_count << READ_LOCK_BIT_OFFSET);
        }

        static uint64_t read_lock_max()
	{
		return READ_LOCK_BIT_MASK;
	}

        static void set_write_lock_bit(uint64_t &value)
        {
                value |= (WRITE_LOCK_BIT_MASK << WRITE_LOCK_BIT_OFFSET);
        }

        static void clear_write_lock_bit(uint64_t &value)
        {
                value &= ~(WRITE_LOCK_BIT_MASK << WRITE_LOCK_BIT_OFFSET);
        }

	uint64_t read_lock(std::atomic<uint64_t> &meta, void* data_ptr, uint64_t size, bool &success)
	{
                TwoPLPashaMetadataLocal *lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(meta.load());
                uint64_t old_value = 0, new_value = 0;
                uint64_t tid = 0;

                lmeta->lock();
                if (lmeta->is_migrated == false) {
                        if (lmeta->is_valid == false) {
                                success = false;
                                goto out_unlock_lmeta;
                        }

                        old_value = lmeta->tid;
                        tid = remove_lock_bit(old_value);

                        // can we get the lock?
                        if (is_write_locked(old_value) || read_lock_num(old_value) == read_lock_max()) {
                                success = false;
                                goto out_unlock_lmeta;
                        }

                        // OK, we can get the lock
                        new_value = old_value + (1ull << READ_LOCK_BIT_OFFSET);
                        lmeta->tid = new_value;
                        success = true;
                } else {
                        TwoPLPashaMetadataShared *smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(lmeta->migrated_row);
                        TwoPLPashaSharedDataSCC *scc_data = smeta->get_scc_data();

                        smeta->lock();

                        // SCC prepare read
                        scc_manager->prepare_read(smeta, coordinator_id, scc_data,
                                                  scc_data_bytes(size));

                        if (smeta->is_data_modified_since_moved_in() == true) {
                                if (smeta->get_flag(TwoPLPashaMetadataShared::valid_flag_index) == false) {
                                        smeta->unlock();
                                        success = false;
                                        goto out_unlock_lmeta;
                                }

                                old_value = scc_data->tid;
                                tid = remove_lock_bit(old_value);

                                // we update our local cache
                                lmeta->is_valid = smeta->get_flag(TwoPLPashaMetadataShared::valid_flag_index);
                                lmeta->tid = scc_data->tid;
                                scc_manager->do_read(nullptr, coordinator_id, data_ptr, scc_data->data, size);

                                // unset the flag
                                smeta->clear_is_data_modified_since_moved_in();
                        } else {
                                if (lmeta->is_valid == false) {
                                        smeta->unlock();
                                        success = false;
                                        goto out_unlock_lmeta;
                                }

                                old_value = lmeta->tid;
                                tid = remove_lock_bit(old_value);
                        }

                        // can we get the lock?
                        if (smeta->is_write_locked() || smeta->get_reader_count() == smeta->get_reader_count_max()) {
                                success = false;
                                smeta->unlock();
                                goto out_unlock_lmeta;
                        }

                        // OK, we can get the lock
                        smeta->increase_reader_count();
                        success = true;

                        smeta->unlock();
                }

out_unlock_lmeta:
                lmeta->unlock();
		return tid;
	}

        uint64_t take_read_lock_and_read(
            const std::tuple<MetaDataType *, void *> &row, void *dest,
            std::size_t size, bool &success,
            std::atomic<uint64_t> &local_cxl_access)
        {
                MetaDataType &meta = *std::get<0>(row);
                auto *lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(
                    meta.load());
                bool local_refreshed = false;
                const uint64_t tid = TwoPLPashaHelper::take_read_lock_and_read(
                    *lmeta, std::get<1>(row), dest, size, success,
                    [](const TwoPLPashaMetadataLocal &local) {
                        return reinterpret_cast<TwoPLPashaMetadataShared *>(
                            local.migrated_row);
                    },
                    coordinator_id, nullptr, nullptr, &local_refreshed);
                if (local_refreshed) local_cxl_access.fetch_add(1);
                return tid;
        }

        uint64_t remote_take_read_lock_and_read(char *row, void *dest, std::size_t size, bool inc_ref_cnt, bool &success)
	{
		return remote_take_read_lock_and_read(
                        reinterpret_cast<TwoPLPashaMetadataShared *>(row),
                        coordinator_id, dest, size, inc_ref_cnt, success);
	}

        uint64_t remote_read_lock_and_inc_ref_cnt(char *row, uint64_t size, bool &success)
	{
		return remote_take_read_lock_and_read(
                        reinterpret_cast<TwoPLPashaMetadataShared *>(row),
                        coordinator_id, nullptr, size, true, success);
	}

	uint64_t write_lock(std::atomic<uint64_t> &meta, void* data_ptr,
                            uint64_t size, bool &success)
	{
                auto *lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(
                    meta.load());
                return TwoPLPashaHelper::take_write_lock(
                    *lmeta, data_ptr, size, success,
                    [](const TwoPLPashaMetadataLocal &local) {
                        return reinterpret_cast<TwoPLPashaMetadataShared *>(
                            local.migrated_row);
                    },
                    coordinator_id, nullptr);
	}

        uint64_t take_write_lock_and_read(
            const std::tuple<MetaDataType *, void *> &row, void *dest,
            std::size_t size, bool &success,
            std::atomic<uint64_t> &local_cxl_access)
	{
                MetaDataType &meta = *std::get<0>(row);
                auto *lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(
                    meta.load());
                bool local_refreshed = false;
                const uint64_t tid = TwoPLPashaHelper::take_write_lock(
                    *lmeta, std::get<1>(row), size, success,
                    [](const TwoPLPashaMetadataLocal &local) {
                        return reinterpret_cast<TwoPLPashaMetadataShared *>(
                            local.migrated_row);
                    },
                    coordinator_id, nullptr, nullptr, &local_refreshed);
                if (local_refreshed) local_cxl_access.fetch_add(1);
                if (success) std::memcpy(dest, std::get<1>(row), size);
                return tid;
	}

        uint64_t remote_take_write_lock_and_read(char *row, void *dest, std::size_t size, bool inc_ref_cnt, bool &success)
	{
		return remote_take_write_lock_and_read(
                        reinterpret_cast<TwoPLPashaMetadataShared *>(row),
                        coordinator_id, dest, size, inc_ref_cnt, success);
	}

        uint64_t remote_write_lock_and_inc_ref_cnt(char *row, uint64_t size, bool &success)
	{
		return remote_take_write_lock_and_read(
                        reinterpret_cast<TwoPLPashaMetadataShared *>(row),
                        coordinator_id, nullptr, size, true, success);
	}

	static void read_lock_release(std::atomic<uint64_t> &meta)
	{
                auto *lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(
                    meta.load());
                TwoPLPashaHelper::read_lock_release(
                    *lmeta, [](const TwoPLPashaMetadataLocal &local) {
                        return reinterpret_cast<TwoPLPashaMetadataShared *>(
                            local.migrated_row);
                    });
	}

        static void remote_read_lock_release(char *row)
	{
		remote_read_lock_release(
                        reinterpret_cast<TwoPLPashaMetadataShared *>(row));
	}

	static void write_lock_release(std::atomic<uint64_t> &meta)
	{
                TwoPLPashaMetadataLocal *lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(meta.load());
                uint64_t old_value = 0, new_value = 0;

                lmeta->lock();
                if (lmeta->is_migrated == false) {
                        DCHECK(lmeta->is_valid == true);
                        old_value = lmeta->tid;
                        DCHECK(!is_read_locked(old_value));
                        DCHECK(is_write_locked(old_value));
                        new_value = old_value - (1ull << WRITE_LOCK_BIT_OFFSET);
                        lmeta->tid = new_value;
                } else {
                        TwoPLPashaMetadataShared *smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(lmeta->migrated_row);

                        smeta->lock();
                        DCHECK(smeta->get_reader_count() == 0);
                        DCHECK(smeta->is_write_locked() == true);
                        smeta->clear_write_locked();
                        smeta->unlock();
                }
                lmeta->unlock();
	}

        static void remote_write_lock_release(char *row)
	{
		remote_write_lock_abort(
                        reinterpret_cast<TwoPLPashaMetadataShared *>(row));
	}

	void write_lock_release(std::atomic<uint64_t> &meta, uint64_t size,
                                uint64_t new_value)
	{
                auto *lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(
                    meta.load());
                TwoPLPashaHelper::write_lock_release(
                    *lmeta, new_value, size, coordinator_id,
                    [](const TwoPLPashaMetadataLocal &local) {
                        return reinterpret_cast<TwoPLPashaMetadataShared *>(
                            local.migrated_row);
                    });
	}

        void remote_write_lock_release(char *row, uint64_t size, uint64_t new_value)
	{
		remote_write_lock_release(
                        reinterpret_cast<TwoPLPashaMetadataShared *>(row),
                        coordinator_id, scc_data_bytes(size),
                        new_value);
	}

        void modify_tuple_valid_bit(std::atomic<uint64_t> &meta, bool is_valid, bool is_insert = false)
        {
                TwoPLPashaMetadataLocal *lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(meta.load());

                lmeta->lock();
                if (lmeta->is_migrated == true) {
                        TwoPLPashaMetadataShared *smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(lmeta->migrated_row);
                        modify_tuple_valid_bit(smeta, coordinator_id,
                                               scc_data_bytes(0), is_valid,
                                               is_insert);
                } else {
                        DCHECK(lmeta->is_valid == !is_valid);
                        lmeta->is_valid = is_valid;
                }
                lmeta->unlock();
        }

        void remote_modify_tuple_valid_bit(char *row, bool is_valid, bool is_insert = false)
        {
                TwoPLPashaMetadataShared *smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(row);
                remote_modify_tuple_valid_bit(
                    smeta, coordinator_id, scc_data_bytes(0), is_valid,
                    is_insert, /*consume_ref=*/false,
                    /*mark_shared_dirty=*/false);
        }

	static uint64_t remove_lock_bit(uint64_t value)
	{
		return value & ~(LOCK_BIT_MASK << LOCK_BIT_OFFSET);
	}

	static uint64_t remove_read_lock_bit(uint64_t value)
	{
		return value & ~(READ_LOCK_BIT_MASK << READ_LOCK_BIT_OFFSET);
	}

	static uint64_t remove_write_lock_bit(uint64_t value)
	{
		return value & ~(WRITE_LOCK_BIT_MASK << WRITE_LOCK_BIT_OFFSET);
	}

        void commit_pasha_metadata_init()
        {
                init_finished.store(1, std::memory_order_release);
        }

        void wait_for_pasha_metadata_init()
        {
                while (init_finished.load(std::memory_order_acquire) == 0);
        }

        CXLTableBase *get_cxl_table(std::size_t table_id, std::size_t partition_id)
        {
                if (table_id >= cxl_tbl_vecs.size() ||
                    partition_id >= cxl_tbl_vecs[table_id].size() ||
                    cxl_tbl_vecs[table_id][partition_id] == nullptr) {
                        LOG(FATAL) << "TwoPLPasha CXL table lookup out of range"
                                   << " table=" << table_id
                                   << " partition=" << partition_id
                                   << " tables=" << cxl_tbl_vecs.size();
                }
                return cxl_tbl_vecs[table_id][partition_id];
        }

        // used for modelling the overhead of always go through the CXL indexes for local search
        void model_cxl_search_overhead(const std::tuple<MetaDataType *, void *> &row, std::size_t table_id, std::size_t partition_id, const void *key)
        {
                MetaDataType &meta = *std::get<0>(row);
                TwoPLPashaMetadataLocal *lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(meta.load());

                lmeta->lock();
                if (lmeta->is_migrated == true) {
                        // the tuple is migrated, so we should search through the CXL index
                        CXLTableBase *target_cxl_table = cxl_tbl_vecs[table_id][partition_id];
                        char *migrated_row = reinterpret_cast<char *>(target_cxl_table->search(key));
                        DCHECK(migrated_row != nullptr);
                }
                lmeta->unlock();
        }

        // used for remote point queries
        RowOutcome get_migrated_row_result(
                std::size_t table_id, std::size_t partition_id,
                const void *key, bool inc_ref_cnt,
                std::size_t value_bytes,
                TwoPLPashaMetadataShared **result,
                bool record_clock_access = true)
        {
                if (result == nullptr)
                        throw std::invalid_argument("null migrated row result");
                *result = nullptr;
                CXLTableBase *target_cxl_table = get_cxl_table(table_id, partition_id);
                char *migrated_row = reinterpret_cast<char *>(target_cxl_table->search(key));
                if (migrated_row == nullptr)
                        return RowOutcome::kMissing;

                auto *smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(migrated_row);
                if (inc_ref_cnt) {
                        // Keep the original single lookup/ref/latch sequence;
                        // callers receive the classification from that same
                        // probe instead of searching the shared tree again.
                        if (!get_migrated_row(smeta, coordinator_id, value_bytes))
                                return RowOutcome::kBusy;
                } else {
                        smeta->lock();
                        const bool valid = smeta->get_flag(
                            TwoPLPashaMetadataShared::valid_flag_index);
                        smeta->unlock();
                        if (!valid)
                                return RowOutcome::kBusy;
                }
                if (record_clock_access && migration_manager != nullptr)
                        migration_manager->access_row(smeta, partition_id);
                *result = smeta;
                return RowOutcome::kDone;
        }

        // used for remote point queries
        char *get_migrated_row(std::size_t table_id, std::size_t partition_id,
                               const void *key, bool inc_ref_cnt,
                               std::size_t value_bytes = 0,
                               bool record_clock_access = true)
        {
                TwoPLPashaMetadataShared *result = nullptr;
                return get_migrated_row_result(
                           table_id, partition_id, key, inc_ref_cnt, value_bytes,
                           &result, record_clock_access) == RowOutcome::kDone
                           ? reinterpret_cast<char *>(result) : nullptr;
        }

        // used for remote point queries
        void release_migrated_row(std::size_t table_id, std::size_t partition_id, const void *key)
        {
                CXLTableBase *target_cxl_table = get_cxl_table(table_id, partition_id);
                char *migrated_row = reinterpret_cast<char *>(target_cxl_table->search(key));
                if (migrated_row == nullptr)
                        LOG(FATAL) << "release_migrated_row lost indexed row"
                                   << " table=" << table_id
                                   << " partition=" << partition_id;
                release_migrated_row(
                    reinterpret_cast<TwoPLPashaMetadataShared *>(migrated_row));
        }

        // used for remote scan
        static void decrease_reference_count_via_ptr(void *cxl_row)
        {
                TwoPLPashaMetadataShared *smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(cxl_row);
                TwoPLPashaSharedDataSCC *scc_data = smeta->get_scc_data();

                smeta->lock();
                smeta->decrement_ref_cnt();
                smeta->unlock();
        }

        // used for remote point queries
        bool remove_migrated_row(std::size_t table_id, std::size_t partition_id, const void *key)
        {
                CXLTableBase *target_cxl_table = cxl_tbl_vecs[table_id][partition_id];
                return target_cxl_table->remove(key, nullptr);
        }

        migration_result move_from_hashmap_to_shared_region(ITable *table, const void *key, const std::tuple<MetaDataType *, void *> &row, bool inc_ref_cnt, void *&migration_policy_meta)
	{
                MetaDataType &meta = *std::get<0>(row);
                TwoPLPashaMetadataLocal *lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(meta.load());
                void *local_data = std::get<1>(row);
                bool insert_ret = false;
                migration_result res = migration_result::FAIL_OOM;

		lmeta->lock();
                if (lmeta->is_migrated == false) {
                        TwoPLPashaMetadataShared *smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(cxl_memory.cxlalloc_malloc_wrapper(sizeof(TwoPLPashaMetadataShared), CXLMemory::METADATA_ALLOCATION));
                        if (smeta == nullptr) {
                                res = migration_result::FAIL_OOM;
                                lmeta->unlock();
                                return res;
                        }

                        TwoPLPashaSharedDataSCC *scc_data = nullptr;
                        if (lmeta->scc_data == nullptr || context.enable_scc == false) {
                                // there is no cached copy in CXL - allocate SCC data
                                scc_data = reinterpret_cast<TwoPLPashaSharedDataSCC *>(
                                    cxl_memory.cxlalloc_malloc_wrapper(
                                        scc_data_bytes(table->value_size()),
                                        CXLMemory::DATA_ALLOCATION));
                                if (scc_data == nullptr) {
                                        res = migration_result::FAIL_OOM;
                                        lmeta->unlock();
                                        return res;
                                }

                                lmeta->scc_data = scc_data;
                        } else {
                                // we have a cached copy in CXL - reuse it
                                scc_data = lmeta->scc_data;
                        }
                        new(smeta) TwoPLPashaMetadataShared(scc_data);

                        // init migration policy metadata
                        migration_manager->init_migration_policy_metadata(smeta, table, key, row, sizeof(TwoPLPashaMetadataShared));
                        migration_policy_meta = smeta;

                        // init software cache-coherence metadata
                        scc_manager->init_scc_metadata(smeta, coordinator_id);

                        // take the CXL latch
                        smeta->lock();

                        // copy metadata
                        if (lmeta->is_valid == true) {
                                smeta->set_flag(TwoPLPashaMetadataShared::valid_flag_index);
                        } else {
                                smeta->clear_flag(TwoPLPashaMetadataShared::valid_flag_index);
                        }
                        scc_data->tid = lmeta->tid;
                        smeta->set_reader_count(read_lock_num(lmeta->tid));
                        if (is_write_locked(lmeta->tid) == true) {
                                smeta->set_write_locked();
                        } else {
                                smeta->clear_write_locked();
                        }
                        DCHECK(read_lock_num(lmeta->tid) == smeta->get_reader_count());
                        DCHECK(is_write_locked(lmeta->tid) == smeta->is_write_locked());

                        // copy data
                        if (lmeta->is_data_modified_since_moved_out == true || context.enable_migration_optimization == false) {
                                scc_manager->do_write(nullptr, coordinator_id, scc_data->data, local_data, table->value_size());
                        }
                        lmeta->is_data_modified_since_moved_out = false;    // optimization to reduce memcpy when moving data in
                        smeta->clear_is_data_modified_since_moved_in();   // optimization to reduce memcpy when moving data out

                        // increase the reference count for the requesting host
                        if (inc_ref_cnt == true) {
                                smeta->increment_ref_cnt();
                        }

                        // insert into the corresponding CXL table
                        CXLTableBase *target_cxl_table = cxl_tbl_vecs[table->tableID()][table->partitionID()];
                        insert_ret = target_cxl_table->insert(key, smeta);
                        DCHECK(insert_ret == true);

                        // mark the local row as migrated
                        lmeta->migrated_row = reinterpret_cast<char *>(smeta);
                        lmeta->is_migrated = true;

                        scc_manager->finish_write(smeta, coordinator_id, scc_data,
                                                  scc_data_bytes(table->value_size()));

                        // release the CXL latch
                        smeta->unlock();


                        res = migration_result::SUCCESS;
                } else {
                        if (inc_ref_cnt == true) {
                                // increase the reference count for the requesting host, even if it is already migrated
                                TwoPLPashaMetadataShared *smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(lmeta->migrated_row);
                                TwoPLPashaSharedDataSCC *scc_data = smeta->get_scc_data();

                                smeta->lock();
                                smeta->increment_ref_cnt();
                                smeta->unlock();
                        }
                        res = migration_result::FAIL_ALREADY_IN_CXL;
                }
		lmeta->unlock();

		return res;
	}

        migration_result move_from_btree_to_shared_region(ITable *table, const void *key, const std::tuple<MetaDataType *, void *> &row, bool inc_ref_cnt, void *&migration_policy_meta)
	{
                bool insert_ret = false;
                bool update_next_key_ret = false;
                migration_result res = migration_result::FAIL_OOM;

                auto move_in_processor = [&](const void *prev_key, void *prev_meta, void *prev_data, const void *cur_key, void *cur_meta, void *cur_data, const void *next_key, void *next_meta, void *next_data) {
                        auto prev_lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(prev_meta);
                        auto cur_lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(cur_meta);
                        auto next_lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(next_meta);

                        bool is_next_key_migrated = false, next_key_exist = false;
                        bool is_prev_key_migrated = false, prev_key_exist = false;

                        // check if the previous tuple is migrated
                        // and update its next-key information
                        if (prev_lmeta != nullptr) {
                                prev_lmeta->lock();
                                if (prev_lmeta->is_migrated == true) {
                                        is_prev_key_migrated = true;
                                }
                                prev_lmeta->unlock();
                                prev_key_exist = true;
                        }

                        // check if the next tuple is migrated
                        // and update its prev-key information
                        if (next_lmeta != nullptr) {
                                next_lmeta->lock();
                                if (next_lmeta->is_migrated == true) {
                                        is_next_key_migrated = true;
                                }
                                next_lmeta->unlock();
                                next_key_exist = true;
                        }

                        // check if the current tuple is migrated
                        // if yes, do the migration and update the next-key information
                        cur_lmeta->lock();
                        if (cur_lmeta->is_migrated == false) {
                                // allocate the CXL row
                                TwoPLPashaMetadataShared *cur_smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(cxl_memory.cxlalloc_malloc_wrapper(sizeof(TwoPLPashaMetadataShared), CXLMemory::METADATA_ALLOCATION));
                                if (cur_smeta == nullptr) {
                                        res = migration_result::FAIL_OOM;
                                        cur_lmeta->unlock();
                                        return;
                                }

                                TwoPLPashaSharedDataSCC *cur_scc_data = nullptr;
                                if (cur_lmeta->scc_data == nullptr || context.enable_scc == false) {
                                        // there is no cached copy in CXL - allocate SCC data
                                        cur_scc_data = reinterpret_cast<TwoPLPashaSharedDataSCC *>(
                                            cxl_memory.cxlalloc_malloc_wrapper(
                                                scc_data_bytes(table->value_size()),
                                                CXLMemory::DATA_ALLOCATION));
                                        if (cur_scc_data == nullptr) {
                                                res = migration_result::FAIL_OOM;
                                                cur_lmeta->unlock();
                                                return;
                                        }

                                        cur_lmeta->scc_data = cur_scc_data;
                                } else {
                                        // we have a cached copy in CXL - reuse it
                                        cur_scc_data = cur_lmeta->scc_data;
                                }
                                new(cur_smeta) TwoPLPashaMetadataShared(cur_scc_data);

                                // init migration policy metadata
                                migration_manager->init_migration_policy_metadata(cur_smeta, table, key, row, sizeof(TwoPLPashaMetadataShared));
                                migration_policy_meta = cur_smeta;

                                // init software cache-coherence metadata
                                scc_manager->init_scc_metadata(cur_smeta, coordinator_id);

                                CXLTableBase *target_cxl_table = cxl_tbl_vecs[table->tableID()][table->partitionID()];
                                const auto move_result = move_from_btree_to_shared_region(
                                    *cur_lmeta, cur_data, cur_smeta, cur_scc_data,
                                    coordinator_id, table->value_size(),
                                    cur_lmeta->is_data_modified_since_moved_out ||
                                        context.enable_migration_optimization == false,
                                    inc_ref_cnt, is_prev_key_migrated,
                                    is_next_key_migrated, [&]() {
                                        insert_ret = target_cxl_table->insert(key, cur_smeta);
                                        if (!insert_ret)
                                                throw std::runtime_error(
                                                    "shared tree insert failed during move-in");
                                        cur_lmeta->migrated_row =
                                            reinterpret_cast<char *>(cur_smeta);
                                        cur_lmeta->is_migrated = true;
                                    });
                                if (move_result != migration_result::SUCCESS) {
                                        cur_lmeta->unlock();
                                        return;
                                }

                                // lazily update the next-key information for the previous key
                                if (prev_lmeta != nullptr) {
                                        prev_lmeta->lock();
                                        if (prev_lmeta->is_migrated == true) {
                                                auto prev_smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(prev_lmeta->migrated_row);

                                                prev_smeta->lock();
                                                prev_smeta->set_next_key_real_bit();
                                                prev_smeta->unlock();
                                        }
                                        prev_lmeta->unlock();
                                }

                                // lazily update the next-key information for the previous key
                                if (next_lmeta != nullptr) {
                                        next_lmeta->lock();
                                        if (next_lmeta->is_migrated == true) {
                                                auto next_smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(next_lmeta->migrated_row);

                                                next_smeta->lock();
                                                next_smeta->set_prev_key_real_bit();
                                                next_smeta->unlock();
                                        }
                                        next_lmeta->unlock();
                                }

                                res = migration_result::SUCCESS;
                        } else {
                                // lazily update the next-key information for the previous key
                                if (prev_lmeta != nullptr) {
                                        prev_lmeta->lock();
                                        if (prev_lmeta->is_migrated == true) {
                                                auto prev_smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(prev_lmeta->migrated_row);

                                                prev_smeta->lock();
                                                prev_smeta->set_next_key_real_bit();
                                                prev_smeta->unlock();
                                        }
                                        prev_lmeta->unlock();
                                }

                                // lazily update the next-key information for the previous key
                                if (next_lmeta != nullptr) {
                                        next_lmeta->lock();
                                        if (next_lmeta->is_migrated == true) {
                                                auto next_smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(next_lmeta->migrated_row);

                                                next_smeta->lock();
                                                next_smeta->set_prev_key_real_bit();
                                                next_smeta->unlock();
                                        }
                                        next_lmeta->unlock();
                                }

                                if (inc_ref_cnt == true) {
                                        // increase the reference count for the requesting host, even if it is already migrated
                                        TwoPLPashaMetadataShared *cur_smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(cur_lmeta->migrated_row);
                                        auto cur_scc_data = reinterpret_cast<TwoPLPashaSharedDataSCC *>(cur_smeta->get_scc_data());

                                        cur_smeta->lock();
                                        cur_smeta->increment_ref_cnt();
                                        cur_smeta->unlock();
                                }
                                res = migration_result::FAIL_ALREADY_IN_CXL;

                                // the next-key information should already been updated
                        }
                        cur_lmeta->unlock();
		};

                // update next-key information
                update_next_key_ret = table->search_and_update_next_key_info(key, move_in_processor);
                DCHECK(update_next_key_ret == true);

		return res;
	}

        migration_result move_from_partition_to_shared_region(ITable *table, const void *key, const std::tuple<MetaDataType *, void *> &row, bool inc_ref_cnt, void *&migration_policy_meta)
	{
                migration_result res = migration_result::FAIL_OOM;

                if (this->context.enable_phantom_detection == true) {
                        if (table->tableType() == ITable::HASHMAP) {
                                res = move_from_hashmap_to_shared_region(table, key, row, inc_ref_cnt, migration_policy_meta);
                        } else if (table->tableType() == ITable::BTREE) {
                                res = move_from_btree_to_shared_region(table, key, row, inc_ref_cnt, migration_policy_meta);
                        } else {
                                DCHECK(0);
                        }
                } else {
                        res = move_from_hashmap_to_shared_region(table, key, row, inc_ref_cnt, migration_policy_meta);
                }

                // statistics
                if (res == migration_result::SUCCESS) {
                        num_data_move_in.fetch_add(1);
                }

		return res;
	}

        bool move_from_hashmap_to_partition(ITable *table, const void *key, const std::tuple<MetaDataType *, void *> &row)
	{
                MetaDataType &meta = *std::get<0>(row);
		TwoPLPashaMetadataLocal *lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(meta.load());
                void *local_data = std::get<1>(row);
                bool ret = false;

                lmeta->lock();
                if (lmeta->is_migrated == true) {
                        TwoPLPashaMetadataShared *smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(lmeta->migrated_row);
                        TwoPLPashaSharedDataSCC *scc_data = smeta->get_scc_data();

                        // take the CXL latch
                        smeta->lock();

                        // reference count > 0, cannot move out the tuple
                        if (smeta->get_ref_cnt() > 0) {
                                smeta->unlock();
                                lmeta->unlock();
                                return false;
                        }

                        scc_manager->prepare_read(smeta, coordinator_id, scc_data,
                                                  scc_data_bytes(table->value_size()));

                        // copy metadata back
                        lmeta->is_valid = smeta->get_flag(TwoPLPashaMetadataShared::valid_flag_index);
                        lmeta->tid = scc_data->tid;
                        set_read_lock_num(lmeta->tid, smeta->get_reader_count());
                        if (smeta->is_write_locked() == true) {
                                set_write_lock_bit(lmeta->tid);
                        } else {
                                clear_write_lock_bit(lmeta->tid);
                        }
                        DCHECK(read_lock_num(lmeta->tid) == smeta->get_reader_count());
                        DCHECK(is_write_locked(lmeta->tid) == smeta->is_write_locked());

                        // copy data back
                        if (smeta->is_data_modified_since_moved_in() == true || context.enable_migration_optimization == false) {
                                scc_manager->do_read(nullptr, coordinator_id, local_data, scc_data->data, table->value_size());
                        }
                        lmeta->is_data_modified_since_moved_out = false;
                        smeta->clear_is_data_modified_since_moved_in();

                        // set the migrated row as invalid
                        smeta->clear_flag(TwoPLPashaMetadataShared::valid_flag_index);

                        // remove from CXL index
                        CXLTableBase *target_cxl_table = cxl_tbl_vecs[table->tableID()][table->partitionID()];
                        ret = target_cxl_table->remove(key, lmeta->migrated_row);
                        DCHECK(ret == true);

                        // mark the local row as not migrated
                        lmeta->migrated_row = nullptr;
                        lmeta->is_migrated = false;

                        // free the CXL row
                        if (context.enable_scc == false) {
                                cxl_memory.cxlalloc_free_wrapper(
                                    smeta->get_scc_data(),
                                    scc_data_bytes(table->value_size()),
                                    CXLMemory::DATA_FREE);
                                global_ebr_meta->add_retired_object(
                                    smeta->get_scc_data(),
                                    scc_data_bytes(table->value_size()),
                                    CXLMemory::DATA_FREE);
                        }
                        cxl_memory.cxlalloc_free_wrapper(smeta, sizeof(TwoPLPashaMetadataShared), CXLMemory::METADATA_FREE);
                        global_ebr_meta->add_retired_object(smeta, sizeof(TwoPLPashaMetadataShared), CXLMemory::METADATA_FREE);

                        // release the CXL latch
                        smeta->unlock();
                } else {
                        DCHECK(0);
                }
                lmeta->unlock();

                return true;
	}

        bool move_from_btree_to_partition(ITable *table, const void *key, const std::tuple<MetaDataType *, void *> &row)
	{
                bool move_out_success = false;
                bool ret = false;

                auto move_out_processor = [&](const void *prev_key, void *prev_meta, void *prev_data, const void *cur_key, void *cur_meta, void *cur_data, const void *next_key, void *next_meta, void *next_data) {
                        auto prev_lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(prev_meta);
                        auto cur_lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(cur_meta);
                        auto next_lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(next_meta);

                        // eagerly update the next-key information for the previous tuple
                        if (prev_lmeta != nullptr) {
                                prev_lmeta->lock();
                                if (prev_lmeta->is_migrated == true) {
                                        auto prev_smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(prev_lmeta->migrated_row);

                                        prev_smeta->lock();
                                        prev_smeta->clear_next_key_real_bit();
                                        prev_smeta->unlock();
                                }
                                prev_lmeta->unlock();
                        }

                        // eagerly update the prev-key information for the next tuple
                        if (next_lmeta != nullptr) {
                                next_lmeta->lock();
                                if (next_lmeta->is_migrated == true) {
                                        auto next_smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(next_lmeta->migrated_row);

                                        next_smeta->lock();
                                        next_smeta->clear_prev_key_real_bit();
                                        next_smeta->unlock();
                                }
                                next_lmeta->unlock();
                        }

                        cur_lmeta->lock();
                        if (cur_lmeta->is_migrated == true) {
                                TwoPLPashaMetadataShared *cur_smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(cur_lmeta->migrated_row);
                                CXLTableBase *target_cxl_table = cxl_tbl_vecs[table->tableID()][table->partitionID()];
                                const auto move_result = move_from_btree_to_partition(
                                    *cur_lmeta, cur_data, cur_smeta, coordinator_id,
                                    table->value_size(),
                                    [&]() {
                                        return target_cxl_table->remove(key, nullptr);
                                    },
                                    [&](TwoPLPashaSharedDataSCC *cur_scc_data,
                                        TwoPLPashaMetadataShared *retired_smeta) {
                                        if (context.enable_scc == false) {
                                                cxl_memory.cxlalloc_free_wrapper(
                                                    cur_scc_data,
                                                    scc_data_bytes(table->value_size()),
                                                    CXLMemory::DATA_FREE);
                                                global_ebr_meta->add_retired_object(
                                                    cur_scc_data,
                                                    scc_data_bytes(table->value_size()),
                                                    CXLMemory::DATA_FREE);
                                        }
                                        cxl_memory.cxlalloc_free_wrapper(
                                            retired_smeta,
                                            sizeof(TwoPLPashaMetadataShared),
                                            CXLMemory::METADATA_FREE);
                                        global_ebr_meta->add_retired_object(
                                            retired_smeta,
                                            sizeof(TwoPLPashaMetadataShared),
                                            CXLMemory::METADATA_FREE);
                                    });
                                if (move_result == RowOutcome::kBusy) {
                                        cur_lmeta->unlock();
                                        move_out_success = false;
                                        return;
                                }
                        } else {
                                DCHECK(0);
                        }
                        cur_lmeta->unlock();

                        move_out_success = true;
		};

                // update next-key information
                ret = table->search_and_update_next_key_info(key, move_out_processor);
                DCHECK(ret == true);

		return move_out_success;
	}

        bool move_from_shared_region_to_partition(ITable *table, const void *key, const std::tuple<MetaDataType *, void *> &row)
	{
                bool move_out_success = false;

                if (this->context.enable_phantom_detection == true) {
                        if (table->tableType() == ITable::HASHMAP) {
                                move_out_success = move_from_hashmap_to_partition(table, key, row);
                        } else if (table->tableType() == ITable::BTREE) {
                                move_out_success = move_from_btree_to_partition(table, key, row);
                        } else {
                                DCHECK(0);
                        }
                } else {
                        move_out_success = move_from_hashmap_to_partition(table, key, row);
                }

                // statistics
                if (move_out_success == true) {
                        num_data_move_out.fetch_add(1);
                }

		return move_out_success;
	}

        bool insert_and_update_next_key_info(ITable *table, const void *key, const void *value, bool require_lock_next_key, ITable::row_entity &next_row_entity)
        {
                auto adjacent_tuples_processor = [&](const void *prev_key, MetaDataType *prev_meta, void *prev_data, const void *next_key, MetaDataType *next_meta, void *next_data) -> bool {
                        TwoPLPashaMetadataLocal *prev_lmeta = nullptr, *next_lmeta = nullptr;
                        if (prev_meta != nullptr) {
                                prev_lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(prev_meta->load());
                        }
                        if (next_meta != nullptr) {
                                next_lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(next_meta->load());
                        }

                        if (require_lock_next_key == true) {
                                // try to acquire the write lock of the next key
                                DCHECK(next_meta != nullptr);
                                std::atomic<uint64_t> &meta = *reinterpret_cast<std::atomic<uint64_t> *>(next_meta);
                                bool lock_success = false;
                                write_lock(meta, next_data, table->value_size(), lock_success);
                                if (lock_success == true) {
                                        ITable::row_entity next_row(next_key, table->key_size(), &meta, next_data, table->value_size());
                                        next_row_entity = next_row;
                                        clear_adjacent_migrated_rows(
                                            prev_lmeta, next_lmeta,
                                            [](const TwoPLPashaMetadataLocal &local) {
                                                return reinterpret_cast<TwoPLPashaMetadataShared *>(
                                                    local.migrated_row);
                                            });
                                }
                                return lock_success;
                        } else {
                                clear_adjacent_migrated_rows(
                                    prev_lmeta, next_lmeta,
                                    [](const TwoPLPashaMetadataLocal &local) {
                                        return reinterpret_cast<TwoPLPashaMetadataShared *>(
                                            local.migrated_row);
                                    });
                                return true;
                        }
                };

                // insert a placeholder and update adjacent tuple information if necessary
                return TwoPLPashaHelper::insert_and_update_next_key_info(
                    table, key, value, adjacent_tuples_processor);
        }

        bool delete_and_update_next_key_info(ITable *table, const void *key, bool is_local_delete, bool &need_move_out_from_migration_tracker, void *&migration_policy_meta)
	{
                 auto adjacent_tuples_processor = [&](const void *prev_key, void *prev_meta, void *prev_data, const void *cur_key, void *cur_meta, void *cur_data, const void *next_key, void *next_meta, void *next_data) -> bool {
                        auto prev_lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(prev_meta);
                        auto cur_lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(cur_meta);
                        auto next_lmeta = reinterpret_cast<TwoPLPashaMetadataLocal *>(next_meta);

                        bool is_next_key_migrated = false;
                        bool is_prev_key_migrated = false;
                        bool need_remove_from_cxl_index = false;

                        // take the latches of the previous and the next keys
                        if (prev_lmeta != nullptr) {
                                prev_lmeta->lock();
                        }
                        if (next_lmeta != nullptr) {
                                next_lmeta->lock();
                        }

                        // update the next-key information for the previous key
                        if (prev_lmeta != nullptr) {
                                if (prev_lmeta->is_migrated == true) {
                                        auto prev_smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(prev_lmeta->migrated_row);

                                        prev_smeta->lock();
                                        if (next_lmeta != nullptr && next_lmeta->is_migrated == false) {
                                                prev_smeta->clear_next_key_real_bit();
                                        } else if (next_lmeta != nullptr && next_lmeta->is_migrated == true) {
                                                // We should treat migrated tuples as equal no matter they are valid or not.
                                                // Since we are removing the migrated tuple but keeping the local tuple, we should mark the next key as false.
                                                prev_smeta->clear_next_key_real_bit();
                                        } else {
                                                prev_smeta->clear_next_key_real_bit();
                                        }
                                        prev_smeta->unlock();
                                }
                        }

                        // update the prev-key information for next key
                        if (next_lmeta != nullptr) {
                                if (next_lmeta->is_migrated == true) {
                                        auto next_smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(next_lmeta->migrated_row);

                                        next_smeta->lock();
                                        if (prev_lmeta != nullptr && prev_lmeta->is_migrated == false) {
                                                next_smeta->clear_prev_key_real_bit();
                                        } else if (prev_lmeta != nullptr && prev_lmeta->is_migrated == true) {
                                                // We should treat migrated tuples as equal no matter they are valid or not.
                                                // Since we are removing the migrated tuple but keeping the local tuple, we should mark the previous key as false.
                                                next_smeta->clear_prev_key_real_bit();
                                        } else {
                                                next_smeta->clear_prev_key_real_bit();
                                        }
                                        next_smeta->unlock();
                                }
                        }

                        // release the latches of the previous and the next keys
                        if (prev_lmeta != nullptr) {
                                prev_lmeta->unlock();
                        }
                        if (next_lmeta != nullptr) {
                                next_lmeta->unlock();
                        }

                        // mark both the local and the migrated tuples as invalid
                        DCHECK(cur_lmeta != nullptr);
                        cur_lmeta->lock();
                        if (cur_lmeta->is_migrated == true) {
                                auto cur_smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(cur_lmeta->migrated_row);
                                auto cur_scc_data = cur_smeta->get_scc_data();

                                cur_smeta->lock();
                                if (is_local_delete == true) {
                                        DCHECK(cur_smeta->get_flag(TwoPLPashaMetadataShared::valid_flag_index) == true);
                                        cur_smeta->clear_flag(TwoPLPashaMetadataShared::valid_flag_index);
                                } else {
                                        DCHECK(cur_smeta->get_flag(TwoPLPashaMetadataShared::valid_flag_index) == false);
                                }
                                migration_policy_meta = cur_smeta;
                                
                                // free the CXL row
                                if (context.enable_scc == false) {
                                        cxl_memory.cxlalloc_free_wrapper(
                                            cur_scc_data,
                                            scc_data_bytes(table->value_size()),
                                            CXLMemory::DATA_FREE);
                                        global_ebr_meta->add_retired_object(
                                            cur_scc_data,
                                            scc_data_bytes(table->value_size()),
                                            CXLMemory::DATA_FREE);
                                }
                                cxl_memory.cxlalloc_free_wrapper(cur_smeta, sizeof(TwoPLPashaMetadataShared), CXLMemory::METADATA_FREE);
                                global_ebr_meta->add_retired_object(cur_smeta, sizeof(TwoPLPashaMetadataShared), CXLMemory::METADATA_FREE);
                                
                                cur_smeta->unlock();

                                need_remove_from_cxl_index = true;
                                need_move_out_from_migration_tracker = true;

                                cur_lmeta->migrated_row = nullptr;
                                cur_lmeta->is_migrated = false;
                        }

                        // local tuple might be invalid here because it can be moved back after being marked as invalid by a remote host
                        cur_lmeta->is_valid = false;
                        cur_lmeta->unlock();

                        // remove the migrated row from the CXL index
                        if (need_remove_from_cxl_index) {
                                bool remove_success = remove_migrated_row(table->tableID(), table->partitionID(), key);
                                DCHECK(remove_success == true);
                        }

                        return true;
                };

                // update next key info, mark both the local and the migrated tuples as invalid, and remove the migrated tuple
                bool success = TwoPLPashaHelper::delete_and_update_next_key_info(
                    table, key, adjacent_tuples_processor);
                DCHECK(success == true);

                return true;
	}

    public:
	static constexpr int LOCK_BIT_OFFSET = 58;
	static constexpr uint64_t LOCK_BIT_MASK = 0x3full;

	static constexpr int READ_LOCK_BIT_OFFSET = 58;
	static constexpr uint64_t READ_LOCK_BIT_MASK = 0x1full;

	static constexpr int WRITE_LOCK_BIT_OFFSET = 63;
	static constexpr uint64_t WRITE_LOCK_BIT_MASK = 0x1ull;

    private:
        std::size_t coordinator_id;

        Context context;

        std::vector<std::vector<CXLTableBase *> > &cxl_tbl_vecs;

        std::atomic<uint64_t> init_finished;
};

extern TwoPLPashaHelper *twopl_pasha_global_helper;

} // namespace star
