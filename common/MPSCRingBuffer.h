//
// Created by Yibo Huang on 8/8/24.
//

#pragma once

#include "common/Message.h"
#include "common/CXLMemory.h"
#include "kv/engine/mem_access.h"
#include <stddef.h>
#include <atomic>
#include <stdexcept>
#include <xmmintrin.h>
#include <glog/logging.h>

namespace star
{

class MPSCRingBuffer {
    public:
        struct Entry {
                uint32_t remaining_size;
                uint32_t dequeue_offset;
                std::atomic<uint8_t> is_ready;
                uint8_t data[];
        };

        MPSCRingBuffer(uint64_t entry_struct_size, uint64_t entry_num)
        {
                const uint64_t computed_entries_offset =
                    CXLMemory::transport_pointer_to_offset(
                    cxl_memory.cxlalloc_malloc_wrapper(
                        entry_struct_size * entry_num,
                        CXLMemory::TRANSPORT_ALLOCATION));
                // Construction writes the ring header (offset/length fields,
                // the head/tail/count atomics and the entries offset) and every
                // entry's ready atomic, offset/length metadata and payload
                // memset into HWCC.  Every real shared access goes through the
                // typed/atomic/bulk wrappers, so the charged lines correspond
                // one-to-one with actual accesses (no charge-then-raw-read
                // pattern, no whole-entry envelope approximation, no double
                // charge); the surrounding KVEngine::Open background scope is
                // the settlement boundary.  Wrappers are raw operations in a
                // compile-off build.
                tigonkv::engine::mem_access::HwccStore(
                    &this->entry_struct_size, entry_struct_size);
                tigonkv::engine::mem_access::HwccStore(
                    &this->entry_data_size, entry_struct_size - 9);
                tigonkv::engine::mem_access::HwccStore(
                    &this->entry_num, entry_num);
                tigonkv::engine::mem_access::HwccAtomicStore(
                    head, uint64_t{0}, std::memory_order_seq_cst);
                tigonkv::engine::mem_access::HwccAtomicStore(
                    tail, uint64_t{0}, std::memory_order_seq_cst);
                tigonkv::engine::mem_access::HwccAtomicStore(
                    count, uint64_t{0}, std::memory_order_seq_cst);
                tigonkv::engine::mem_access::HwccStore(
                    &entries_buffer_offset, computed_entries_offset);
                char *const entries_buffer =
                    static_cast<char *>(CXLMemory::transport_offset_to_pointer(
                        computed_entries_offset,
                        entry_struct_size * entry_num));
                for (int i = 0; i < entry_num; i++) {
                        Entry *entry = reinterpret_cast<Entry *>(
                            entries_buffer + i * entry_struct_size);
                        tigonkv::engine::mem_access::HwccAtomicStore(
                            entry->is_ready, uint8_t{0},
                            std::memory_order_seq_cst);
                        tigonkv::engine::mem_access::HwccMemsetShared(
                            &entry->remaining_size,
                            0, sizeof(entry->remaining_size) +
                                   sizeof(entry->dequeue_offset));
                        tigonkv::engine::mem_access::HwccMemsetShared(
                            entry->data, 0, entry_struct_size - 9);
                }
        }

        uint64_t get_entry_num()
        {
                return tigonkv::engine::mem_access::HwccLoad(&entry_num);
        }

        uint64_t get_entry_size()
        {
                return tigonkv::engine::mem_access::HwccLoad(&entry_data_size);
        }

        uint64_t size()
        {
                // size() reports one real wrapped load of head and tail; no
                // charge is made for a read that does not happen.
                const uint64_t cur_head =
                    tigonkv::engine::mem_access::HwccAtomicLoad(
                    head, std::memory_order_acquire);
                const uint64_t cur_tail =
                    tigonkv::engine::mem_access::HwccAtomicLoad(
                    tail, std::memory_order_acquire);

                return cur_tail - cur_head;
        }

        bool enqueue(char *data, uint64_t data_size)
        {
                // One real wrapped header snapshot; all subsequent geometry is
                // computed from the snapshot, never re-read from shared state.
                const HeaderSnapshot snap = LoadHeader();
                const uint64_t local_entry_struct_size = snap.entry_struct_size;
                const uint64_t local_entry_data_size = snap.entry_data_size;
                const uint64_t local_entry_num = snap.entry_num;

                // Prefer throw over glog FATAL: aborting one guest made the
                // remaining VMs look like a Forward/Await stall under YCSB-A.
                if (local_entry_num == 0 || local_entry_struct_size < 9 ||
                    local_entry_data_size != local_entry_struct_size - 9) {
                        LOG(ERROR) << "MPSCRingBuffer corrupt metadata: data_size="
                                   << data_size << " entry_data_size=" << local_entry_data_size
                                   << " entry_struct_size=" << local_entry_struct_size
                                   << " entry_num=" << local_entry_num;
                        throw std::runtime_error("corrupt MPSCRingBuffer metadata");
                }
                if (data_size > local_entry_data_size) {
                        LOG(ERROR) << "MPSCRingBuffer enqueue too large: data_size="
                                   << data_size << " entry_data_size=" << local_entry_data_size;
                        throw std::runtime_error("KV message exceeds transport entry");
                }

                /* try to gain access to the queue */
                const uint64_t cur_count =
                    tigonkv::engine::mem_access::HwccAtomicFetchAdd(
                    count, uint64_t{1}, std::memory_order_acquire);
                if(cur_count >= local_entry_num) {
                        /* back off since queue is full */
                        tigonkv::engine::mem_access::HwccAtomicFetchSub(
                            count, uint64_t{1}, std::memory_order_release);
                        return false;
                }

                /* gain exclusive access to the entry */
                const uint64_t cur_tail =
                    tigonkv::engine::mem_access::HwccAtomicFetchAdd(
                    tail, uint64_t{1}, std::memory_order_release);

                /* get the entry */
                Entry *const entry = reinterpret_cast<Entry *>(
                    EntriesFor(snap) +
                    (cur_tail % local_entry_num) * local_entry_struct_size);

                /* memcpy the data to the target endpoint's receive queue */
                tigonkv::engine::mem_access::HwccCopyLocalToShared(
                    entry->data, data, data_size);
                clwb(entry->data, data_size);

                tigonkv::engine::mem_access::HwccStore(
                    &entry->remaining_size,
                    static_cast<uint32_t>(data_size));
                tigonkv::engine::mem_access::HwccStore(
                    &entry->dequeue_offset, uint32_t{0});

                /* mark the entry as ready */
                tigonkv::engine::mem_access::HwccAtomicStore(
                    entry->is_ready, uint8_t{1}, std::memory_order_release);

                return true;
        }

        uint64_t dequeue(char *data_buffer, uint64_t buffer_size)
        {
                if (buffer_size == 0)
                        return 0;

                const HeaderSnapshot snap = LoadHeader();
                const uint64_t local_entry_data_size = snap.entry_data_size;
                const uint64_t cur_head =
                    tigonkv::engine::mem_access::HwccAtomicLoad(
                        head, std::memory_order_acquire);
                const uint64_t cur_tail =
                    tigonkv::engine::mem_access::HwccAtomicLoad(
                        tail, std::memory_order_acquire);
                if (cur_head == cur_tail)
                        return 0;

                /* get the entry */
                Entry *const entry = reinterpret_cast<Entry *>(
                    EntriesFor(snap) +
                    (cur_head % snap.entry_num) * snap.entry_struct_size);

                /* wait for the entry to be ready */
                uint8_t ready = tigonkv::engine::mem_access::HwccAtomicLoad(
                    entry->is_ready, std::memory_order_acquire);
                while (ready != 1) {
                        ready = tigonkv::engine::mem_access::HwccAtomicLoad(
                            entry->is_ready, std::memory_order_acquire);
                }

                /* Partial dequeue is not supported. Metadata or wire-size
                 * violations are protocol corruption, not an empty/full
                 * backpressure condition; let the caller hard-fail with node
                 * and request diagnostics instead of hiding this as a stall. */
                const uint32_t remaining_size =
                    tigonkv::engine::mem_access::HwccLoad(
                        &entry->remaining_size);
                const uint32_t dequeue_offset =
                    tigonkv::engine::mem_access::HwccLoad(
                        &entry->dequeue_offset);
                if (remaining_size == 0 ||
                    dequeue_offset > local_entry_data_size ||
                    remaining_size >
                        local_entry_data_size - dequeue_offset) {
                        LOG(ERROR) << "MPSCRingBuffer corrupt dequeue metadata: remaining_size="
                                   << remaining_size
                                   << " dequeue_offset=" << dequeue_offset
                                   << " entry_data_size=" << local_entry_data_size;
                        throw std::runtime_error("corrupt MPSCRingBuffer dequeue metadata");
                }
                if (buffer_size < remaining_size) {
                        LOG(ERROR) << "MPSCRingBuffer receive buffer too small: buffer_size="
                                   << buffer_size
                                   << " remaining_size=" << remaining_size;
                        throw std::runtime_error("MPSCRingBuffer receive buffer too small");
                }
                const uint64_t dequeue_size = remaining_size;

                /* memcpy the data to the user-provided buffer and update the metadata */
                clflush(entry->data, dequeue_size);
                tigonkv::engine::mem_access::HwccCopySharedToLocal(
                    data_buffer, entry->data, dequeue_size);
                tigonkv::engine::mem_access::HwccStore(
                    &entry->dequeue_offset,
                    static_cast<uint32_t>(dequeue_offset + dequeue_size));
                tigonkv::engine::mem_access::HwccStore(
                    &entry->remaining_size,
                    static_cast<uint32_t>(remaining_size - dequeue_size));

                if (remaining_size == dequeue_size) {
                        /* mark it as not ready */
                        tigonkv::engine::mem_access::HwccAtomicStore(
                            entry->is_ready, uint8_t{0},
                            std::memory_order_relaxed);
                        /* increase head by 1 */
                        tigonkv::engine::mem_access::HwccAtomicStore(
                            head, cur_head + 1, std::memory_order_release);
                        /* reduce count by 1 */
                        tigonkv::engine::mem_access::HwccAtomicFetchSub(
                            count, uint64_t{1}, std::memory_order_release);
                }

                return dequeue_size;
        }

        uint64_t send(char *data, uint64_t data_size)
        {
                while (enqueue(data, data_size) != true);
                return data_size;
        }

        uint64_t recv(char *buffer, uint64_t buffer_size)
        {
                uint64_t available_entries = size();
                uint64_t data_size = 0;
                int i = 0;

                if (available_entries == 0)
                        return 0;

                data_size = dequeue(buffer, buffer_size);
                return data_size;
        }

    private:
        static constexpr uint64_t cacheline_size = 64;

        struct HeaderSnapshot {
                uint64_t entry_struct_size;
                uint64_t entry_data_size;
                uint64_t entry_num;
                uint64_t entries_buffer_offset;
        };

        // Loads the ring header exactly once with real wrapped loads.  All
        // geometry is then computed from the snapshot; no helper re-reads
        // shared header fields behind the caller's back.
        HeaderSnapshot LoadHeader() const {
                HeaderSnapshot snap;
                snap.entry_struct_size =
                    tigonkv::engine::mem_access::HwccLoad(&entry_struct_size);
                snap.entry_data_size =
                    tigonkv::engine::mem_access::HwccLoad(&entry_data_size);
                snap.entry_num =
                    tigonkv::engine::mem_access::HwccLoad(&entry_num);
                snap.entries_buffer_offset =
                    tigonkv::engine::mem_access::HwccLoad(
                        &entries_buffer_offset);
                return snap;
        }

        static char *EntriesFor(const HeaderSnapshot &snap) {
                if (snap.entries_buffer_offset == 0)
                        throw std::runtime_error(
                            "MPSCRingBuffer has null entries offset");
                return static_cast<char *>(CXLMemory::transport_offset_to_pointer(
                    snap.entries_buffer_offset,
                    snap.entry_struct_size * snap.entry_num));
        }

        inline void clflush(const void *addr, uint64_t len)
        {
                /*
                 * Loop through cache-line-size (typically 64B) aligned chunks
                 * covering the given range.
                 */
                for (uint64_t ptr = (uint64_t)addr & ~(cacheline_size - 1); ptr < (uint64_t)addr + len; ptr += cacheline_size) {
                        _mm_clflushopt((void *)ptr);
                }

                // make sure clflush completes before memcpy
                _mm_sfence();
        }

        inline void clwb(const void *addr, uint64_t len)
        {
                /*
                 * Loop through cache-line-size (typically 64B) aligned chunks
                 * covering the given range.
                 */
                for (uint64_t ptr = (uint64_t)addr & ~(cacheline_size - 1); ptr < (uint64_t)addr + len; ptr += cacheline_size) {
                        _mm_clwb((void *)ptr);
                }

                // make sure clwb completes before memcpy
                _mm_sfence();
        }

        uint64_t entry_struct_size;
        uint64_t entry_data_size;
        uint64_t entry_num;

        std::atomic<uint64_t> head;
        std::atomic<uint64_t> tail;
        std::atomic<uint64_t> count;
        uint64_t entries_buffer_offset = 0;
};

}
