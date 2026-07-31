//
// Created by Yibo Huang on 8/8/24.
//

#pragma once

#include <atomic>
#include <immintrin.h>
#include <stdexcept>

#include "core/Context.h"
#include "kv/engine/mem_access.h"
#include "kv/engine/region_allocator.h"

#include <glog/logging.h>

namespace star
{

class CXLMemory {
    public:
        // statistics
        enum {
                TOTAL_USAGE,
                TOTAL_HW_CC_USAGE,
                INDEX_USAGE,
                METADATA_USAGE,
                DATA_USAGE,
                TRANSPORT_USAGE,
                MISC_USAGE,
                INDEX_ALLOCATION,
                METADATA_ALLOCATION,
                DATA_ALLOCATION,
                TRANSPORT_ALLOCATION,
                MISC_ALLOCATION,
                INDEX_FREE,
                METADATA_FREE,
                DATA_FREE,
                TRANSPORT_FREE,
                MISC_FREE
        };

        static constexpr uint64_t default_cxl_mem_size = (((1024 * 1024 * 1024) + 64 * 1024) * (uint64_t)31);

        static constexpr uint64_t cxl_transport_root_index = 0;
        static constexpr uint64_t cxl_data_migration_root_index = 1;
        static constexpr uint64_t cxl_lru_trackers_root_index = 2;
        static constexpr uint64_t cxl_global_epoch_root_index = 3;
        static constexpr uint64_t cxl_global_ebr_meta_root_index = 4;

        void init(Context context)
        {
                this->context = context;
        }

        // tigonkv: the allocator is process-local state over a persistent
        // mapped pool.  It is deliberately an explicit binding rather than a
        // cxlalloc compatibility fallback.
        static void bind_dual_region_allocator(tigonkv::engine::DualRegionAllocator *regions,
                                                uint32_t owner_shard)
        {
                if (regions == nullptr) throw std::invalid_argument("null dual-region allocator");
                dual_regions_ = regions;
                owner_shard_ = owner_shard;
        }

        // Read-only process binding used by tests / Open invariants (§11.3).
        static uint32_t bound_owner_shard() { return owner_shard_; }
        static bool dual_region_allocator_bound() { return dual_regions_ != nullptr; }

        static tigonkv::engine::RegionOffset transport_pointer_to_offset(
                const void *pointer)
        {
                if (dual_regions_ == nullptr)
                        throw std::runtime_error("tigonkv: dual-region allocator is not bound");
                return dual_regions_->ToTransportOffset(pointer);
        }

        static void *transport_offset_to_pointer(
                tigonkv::engine::RegionOffset offset, uint64_t bytes)
        {
                if (dual_regions_ == nullptr)
                        throw std::runtime_error("tigonkv: dual-region allocator is not bound");
                return dual_regions_->ResolveTransport(offset, bytes);
        }

        static uint64_t shared_payload_pointer_to_offset(const void *pointer)
        {
                if (dual_regions_ == nullptr)
                        throw std::runtime_error("tigonkv: dual-region allocator is not bound");
                return dual_regions_->EncodeSharedPayloadOffset(pointer, owner_shard_);
        }

        static void *shared_payload_offset_to_pointer(uint64_t offset, uint64_t bytes)
        {
                if (dual_regions_ == nullptr)
                        throw std::runtime_error("tigonkv: dual-region allocator is not bound");
                return dual_regions_->ResolveSharedPayload(offset, bytes);
        }

        void init_cxlalloc_for_given_thread(uint64_t threads_num_per_host, uint64_t thread_id, uint64_t hosts_num, uint64_t host_id)
        {
                // Legacy Tigon thread init. TigonKV workers must not call this: the
                // process owner shard is bound once to node_id in KVEngine::Open
                // (§11.3). Host_id may still be used by source-only benchmarks.
                (void)threads_num_per_host; (void)thread_id; (void)hosts_num;
                if (dual_regions_ == nullptr)
                        throw std::runtime_error("tigonkv: dual-region allocator is not bound");
                owner_shard_ = static_cast<uint32_t>(host_id);
        }

        // backward compatibility
        void *cxlalloc_malloc_wrapper(uint64_t size, int category, uint64_t metadata_size, uint64_t data_size)
        {
                // collect statistics
                switch (category) {
                case INDEX_ALLOCATION:
                        size_index_usage.fetch_add(size);
                        break;
                case DATA_ALLOCATION:
                        size_metadata_usage.fetch_add(metadata_size);
                        size_data_usage.fetch_add(data_size);
                        break;
                case TRANSPORT_ALLOCATION:
                        size_transport_usage.fetch_add(size);
                        break;
                case MISC_ALLOCATION:
                        size_misc_usage.fetch_add(size);
                        break;
                default:
                        CHECK(0);
                }

                return Allocate(size, category);
        }

        void cxlalloc_free_wrapper(void *ptr, uint64_t size, int category, uint64_t metadata_size, uint64_t data_size)
        {
                const auto checked_sub = [](std::atomic<uint64_t> &counter,
                                            uint64_t bytes, const char *detail) {
                        const uint64_t before = counter.load(std::memory_order_relaxed);
                        if (before < bytes) LOG(FATAL) << detail;
                        counter.fetch_sub(bytes, std::memory_order_relaxed);
                };
                // collect statistics
                switch (category) {
                case INDEX_FREE:
                        checked_sub(size_index_usage, size, "CXL index accounting underflow");
                        break;
                case DATA_FREE:
                        checked_sub(size_metadata_usage, metadata_size,
                                    "CXL metadata accounting underflow");
                        checked_sub(size_data_usage, data_size,
                                    "CXL data accounting underflow");
                        break;
                case TRANSPORT_FREE:
                        checked_sub(size_transport_usage, size,
                                    "CXL transport accounting underflow");
                        break;
                case MISC_FREE:
                        checked_sub(size_misc_usage, size,
                                    "CXL misc accounting underflow");
                        break;
                default:
                        CHECK(0);
                }
        }

        // new APIs
        void *cxlalloc_malloc_wrapper(uint64_t size, int category)
        {
                // collect statistics
                switch (category) {
                case INDEX_ALLOCATION:
                        size_index_usage.fetch_add(size);
                        break;
                case METADATA_ALLOCATION:
                        size_metadata_usage.fetch_add(size);
                        break;
                case DATA_ALLOCATION:
                        size_data_usage.fetch_add(size);
                        break;
                case TRANSPORT_ALLOCATION:
                        size_transport_usage.fetch_add(size);
                        break;
                case MISC_ALLOCATION:
                        size_misc_usage.fetch_add(size);
                        break;
                default:
                        CHECK(0);
                }

                return Allocate(size, category);
        }

        void cxlalloc_free_wrapper(void *ptr, uint64_t size, int category)
        {
                const auto checked_sub = [](std::atomic<uint64_t> &counter,
                                            uint64_t bytes, const char *detail) {
                        const uint64_t before = counter.load(std::memory_order_relaxed);
                        if (before < bytes) LOG(FATAL) << detail;
                        counter.fetch_sub(bytes, std::memory_order_relaxed);
                };
                // collect statistics
                switch (category) {
                case INDEX_FREE:
                        checked_sub(size_index_usage, size, "CXL index accounting underflow");
                        break;
                case METADATA_FREE:
                        checked_sub(size_metadata_usage, size,
                                    "CXL metadata accounting underflow");
                        break;
                case DATA_FREE:
                        checked_sub(size_data_usage, size, "CXL data accounting underflow");
                        break;
                case TRANSPORT_FREE:
                        checked_sub(size_transport_usage, size,
                                    "CXL transport accounting underflow");
                        break;
                case MISC_FREE:
                        checked_sub(size_misc_usage, size, "CXL misc accounting underflow");
                        break;
                default:
                        CHECK(0);
                }
        }

        static void commit_shared_data_initialization(uint64_t root_index, void *shared_data)
        {
                if (dual_regions_ == nullptr || root_index >= tigonkv::engine::kRootSlotCount)
                        throw std::runtime_error("tigonkv: dual-region root table is unavailable");
                if (!dual_regions_->IsHwccAddress(shared_data))
                        throw std::invalid_argument("tigonkv: root must be in HWCC");
                tigonkv::engine::mem_access::HwccAtomicStore(
                        &dual_regions_->layout().roots[root_index]);
                dual_regions_->layout().roots[root_index].store(
                        dual_regions_->hwcc().ToOffset(shared_data), std::memory_order_release);
        }

        static void wait_and_retrieve_cxl_shared_data(uint64_t root_index, void **shared_data)
        {
                if (shared_data == nullptr || dual_regions_ == nullptr ||
                    root_index >= tigonkv::engine::kRootSlotCount)
                        throw std::runtime_error("tigonkv: dual-region root table is unavailable");
                // Preserve Tigon's startup wait: an attaching coordinator may
                // observe the static layout before VM0 has published every
                // root.  This waits only for that original root publication;
                // the higher-level Ready barrier still gates request service.
                tigonkv::engine::RegionOffset offset = tigonkv::engine::kNullOffset;
                do {
                        tigonkv::engine::mem_access::HwccAtomicLoad(
                                &dual_regions_->layout().roots[root_index]);
                        offset = dual_regions_->layout().roots[root_index].load(
                                std::memory_order_acquire);
                        if (offset == tigonkv::engine::kNullOffset) _mm_pause();
                } while (offset == tigonkv::engine::kNullOffset);
                if (root_index == cxl_transport_root_index)
                        *shared_data = dual_regions_->ResolveTransport(offset, 1);
                else if (root_index == cxl_global_ebr_meta_root_index)
                        *shared_data = dual_regions_->ResolveEbr(offset, 1);
                else
                        throw std::runtime_error(
                            "tigonkv: unknown static HWCC root index");
        }

        uint64_t get_stats(int category)
        {
                switch (category) {
                case INDEX_USAGE:
                        return size_index_usage;
                case METADATA_USAGE:
                        return size_metadata_usage;
                case DATA_USAGE:
                        return size_data_usage;
                case TRANSPORT_USAGE:
                        return size_transport_usage;
                case MISC_USAGE:
                        return size_misc_usage;
                case TOTAL_HW_CC_USAGE:
                        if (dual_regions_ == nullptr)
                                throw std::runtime_error("Clock policy counter is unbound");
                        return dual_regions_->PolicyHwccUsedBytes(owner_shard_);
                case TOTAL_USAGE:
                        return size_index_usage + size_metadata_usage + size_data_usage + size_transport_usage + size_misc_usage;      // does not need to be consistent
                default:
                        CHECK(0);
                }
        }

        void print_stats()
        {
                LOG(INFO) << "local CXL memory usage:"
                          << " size_index_usage: " << get_stats(INDEX_USAGE)
                          << " size_metadata_usage: " << get_stats(METADATA_USAGE)
                          << " size_data_usage: " << get_stats(DATA_USAGE)
                          << " size_transport_usage: " << get_stats(TRANSPORT_USAGE)
                          << " size_misc_usage: " << get_stats(MISC_USAGE)
                          << " total_size_hw_cc_usage: " << get_stats(TOTAL_HW_CC_USAGE)
                          << " total_usage: " << get_stats(TOTAL_USAGE);
        }

    private:
        static tigonkv::engine::AllocationDomain DomainForAllocation(int category)
        {
                switch (category) {
                case INDEX_ALLOCATION: return tigonkv::engine::AllocationDomain::kHwccIndex;
                case METADATA_ALLOCATION: return tigonkv::engine::AllocationDomain::kHwccMetadata;
                case DATA_ALLOCATION: return tigonkv::engine::AllocationDomain::kSharedPayloadSwcc;
                case TRANSPORT_ALLOCATION: return tigonkv::engine::AllocationDomain::kTransport;
                // Original Tigon places CXL_EBR (and other misc HWCC meta) via
                // MISC_ALLOCATION into the coherent CXL heap.  Map to kHwccEbr
                // so dual-region accounting matches PLAN's EBR domain.
                case MISC_ALLOCATION: return tigonkv::engine::AllocationDomain::kHwccEbr;
                default: throw std::invalid_argument("invalid CXL allocation category");
                }
        }

        static void *Allocate(uint64_t size, int category)
        {
                if (dual_regions_ == nullptr)
                        throw std::runtime_error("tigonkv: dual-region allocator is not bound");
                return dual_regions_->Allocate(size, DomainForAllocation(category), owner_shard_);
        }

        inline static tigonkv::engine::DualRegionAllocator *dual_regions_ = nullptr;
        inline static uint32_t owner_shard_ = 0;
        Context context;

        std::atomic<uint64_t> size_index_usage{ 0 };
        std::atomic<uint64_t> size_metadata_usage{ 0 };
        std::atomic<uint64_t> size_data_usage{ 0 };
        std::atomic<uint64_t> size_transport_usage{ 0 };
        std::atomic<uint64_t> size_misc_usage{ 0 };

};

extern CXLMemory cxl_memory;

}
