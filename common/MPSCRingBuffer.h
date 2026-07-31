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
                : entry_struct_size(entry_struct_size)
                , entry_data_size(entry_struct_size - 9)
                , entry_num(entry_num)
                , head(0)
                , tail(0)
                , count(0)
        {
                entries_buffer_offset = CXLMemory::transport_pointer_to_offset(
                    cxl_memory.cxlalloc_malloc_wrapper(
                        entry_struct_size * entry_num,
                        CXLMemory::TRANSPORT_ALLOCATION));
                for (int i = 0; i < entry_num; i++) {
                        Entry *entry = reinterpret_cast<Entry *>(entries() + i * entry_struct_size);
                        entry->is_ready = 0;
                        entry->remaining_size = 0;
                        entry->dequeue_offset = 0;
                        memset(entry->data, 0, entry_data_size);
                }
        }

        uint64_t get_entry_num()
        {
                tigonkv::engine::mem_access::TransportRead(
                    &entry_num, sizeof(entry_num));
                return entry_num;
        }

        uint64_t get_entry_size()
        {
                tigonkv::engine::mem_access::TransportRead(
                    &entry_data_size, sizeof(entry_data_size));
                return entry_data_size;
        }

        uint64_t size()
        {
                uint64_t cur_head = 0, cur_tail = 0;
                tigonkv::engine::mem_access::TransportRead(
                    &entry_num, sizeof(entry_num));

                tigonkv::engine::mem_access::HwccAtomicLoad(&head);
                cur_head = head.load(std::memory_order_acquire);
                tigonkv::engine::mem_access::HwccAtomicLoad(&tail);
                cur_tail = tail.load(std::memory_order_acquire);

                return cur_tail - cur_head;
        }

        bool enqueue(char *data, uint64_t data_size)
        {
                uint64_t cur_count = 0, cur_tail = 0;
                Entry *entry = nullptr;

                tigonkv::engine::mem_access::TransportRead(
                    &entry_struct_size,
                    sizeof(entry_struct_size) + sizeof(entry_data_size) +
                        sizeof(entry_num));
                tigonkv::engine::mem_access::TransportRead(
                    &entries_buffer_offset, sizeof(entries_buffer_offset));
                const uint64_t local_entry_struct_size = entry_struct_size;
                const uint64_t local_entry_data_size = entry_data_size;
                const uint64_t local_entry_num = entry_num;

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
                tigonkv::engine::mem_access::HwccAtomicRmw(&count);
                cur_count = std::atomic_fetch_add_explicit(&count, 1, std::memory_order_acquire);
                if(cur_count >= local_entry_num) {
                        /* back off since queue is full */
                        tigonkv::engine::mem_access::HwccAtomicRmw(&count);
                        std::atomic_fetch_sub_explicit(&count, 1, std::memory_order_release);
                        return false;
                }

                /* gain exclusive access to the entry */
                tigonkv::engine::mem_access::HwccAtomicRmw(&tail);
                cur_tail = std::atomic_fetch_add_explicit(&tail, 1, std::memory_order_release);
                cur_tail %= local_entry_num;

                /* get the entry */
                entry = reinterpret_cast<Entry *>(entries() + cur_tail * local_entry_struct_size);

                /* memcpy the data to the target endpoint's receive queue */
                tigonkv::engine::mem_access::TransportWrite(entry->data, data_size);
                memcpy(entry->data, data, data_size);
                clwb(entry->data, data_size);

                tigonkv::engine::mem_access::TransportWrite(
                    &entry->remaining_size,
                    sizeof(entry->remaining_size) + sizeof(entry->dequeue_offset));
                entry->remaining_size = data_size;
                entry->dequeue_offset = 0;

                /* mark the entry as ready */
                tigonkv::engine::mem_access::HwccAtomicStore(&entry->is_ready);
                entry->is_ready.store(1, std::memory_order_release);

                return true;
        }

        uint64_t dequeue(char *data_buffer, uint64_t buffer_size)
        {
                uint64_t cur_head = 0;
                uint64_t entry_index = 0;
                uint64_t dequeue_size = 0;
                Entry *entry = nullptr;

                if (buffer_size == 0)
                        return 0;

                tigonkv::engine::mem_access::TransportRead(
                    &entry_struct_size,
                    sizeof(entry_struct_size) + sizeof(entry_data_size) +
                        sizeof(entry_num));
                tigonkv::engine::mem_access::TransportRead(
                    &entries_buffer_offset, sizeof(entries_buffer_offset));
                const uint64_t local_entry_struct_size = entry_struct_size;
                const uint64_t local_entry_data_size = entry_data_size;
                const uint64_t local_entry_num = entry_num;

                if (size() == 0)
                        return 0;

                tigonkv::engine::mem_access::HwccAtomicLoad(&head);
                cur_head = head.load(std::memory_order_acquire);
                entry_index = cur_head % local_entry_num;

                /* get the entry */
                entry = reinterpret_cast<Entry *>(
                    entries() + entry_index * local_entry_struct_size);

                /* wait for the entry to be ready */
                uint8_t ready = 0;
                do {
                        tigonkv::engine::mem_access::HwccAtomicLoad(&entry->is_ready);
                        ready = entry->is_ready.load(std::memory_order_acquire);
                } while (ready != 1);

                /* Partial dequeue is not supported. Metadata or wire-size
                 * violations are protocol corruption, not an empty/full
                 * backpressure condition; let the caller hard-fail with node
                 * and request diagnostics instead of hiding this as a stall. */
                tigonkv::engine::mem_access::TransportRead(
                    &entry->remaining_size,
                    sizeof(entry->remaining_size) + sizeof(entry->dequeue_offset));
                if (entry->remaining_size == 0 ||
                    entry->dequeue_offset > local_entry_data_size ||
                    entry->remaining_size >
                        local_entry_data_size - entry->dequeue_offset) {
                        LOG(ERROR) << "MPSCRingBuffer corrupt dequeue metadata: remaining_size="
                                   << entry->remaining_size
                                   << " dequeue_offset=" << entry->dequeue_offset
                                   << " entry_data_size=" << local_entry_data_size;
                        throw std::runtime_error("corrupt MPSCRingBuffer dequeue metadata");
                }
                if (buffer_size < entry->remaining_size) {
                        LOG(ERROR) << "MPSCRingBuffer receive buffer too small: buffer_size="
                                   << buffer_size
                                   << " remaining_size=" << entry->remaining_size;
                        throw std::runtime_error("MPSCRingBuffer receive buffer too small");
                }
                dequeue_size = entry->remaining_size;

                /* memcpy the data to the user-provided buffer and update the metadata */
                clflush(entry->data, dequeue_size);
                tigonkv::engine::mem_access::TransportRead(entry->data, dequeue_size);
                memcpy(data_buffer, entry->data, dequeue_size);
                tigonkv::engine::mem_access::TransportWrite(
                    &entry->remaining_size,
                    sizeof(entry->remaining_size) + sizeof(entry->dequeue_offset));
                entry->dequeue_offset += dequeue_size;
                entry->remaining_size -= dequeue_size;

                if (entry->remaining_size == 0) {
                        /* reset metadata */
                        entry->dequeue_offset = 0;

                        /* mark it as not ready */
                        tigonkv::engine::mem_access::HwccAtomicStore(&entry->is_ready);
                        /* increase head by 1 */
                        tigonkv::engine::mem_access::HwccAtomicStore(&head);
                        /* reduce count by 1 */
                        tigonkv::engine::mem_access::HwccAtomicRmw(&count);
                        entry->is_ready.store(0, std::memory_order_relaxed);
                        head.store(cur_head + 1, std::memory_order_release);
                        std::atomic_fetch_sub_explicit(&count, 1, std::memory_order_release);
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

        char *entries() const {
                if (entries_buffer_offset == 0)
                        throw std::runtime_error("MPSCRingBuffer has null entries offset");
                return static_cast<char *>(CXLMemory::transport_offset_to_pointer(
                    entries_buffer_offset, entry_struct_size * entry_num));
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
