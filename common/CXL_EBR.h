//
// Epoch-based memory reclaimation across a CXL pod
//

#pragma once

#include "stdint.h"
#include <glog/logging.h>
#include <stdexcept>

#include "common/CXLMemory.h"
#include "kv/engine/mem_access.h"
#include "kv/engine/region_allocator.h"

namespace star
{

class CXL_EBR {
    public:
        static constexpr uint64_t max_ebr_retiring_memory = 1 * 1024 * 1024;    // 1MB

        // 0 - max_epoch
        static constexpr uint64_t max_epoch = 3;

        static constexpr uint64_t max_coordinator_num = 8;
        static constexpr uint64_t max_thread_num = 5;

        // try to advance global epoch when we have more than this number of garbage
        static constexpr uint64_t epoch_advance_threshold = 100;

        // Per-thread identity/statistics remain local, as in Tigon. Retire
        // records themselves are owner-private SWCC RegionOffset chains.
        struct EBRMetaLocal {
                uint64_t coordinator_id;
                uint64_t thread_id;
                uint64_t coordinator_count;
                uint64_t thread_count;
                uint64_t last_freed_epoch;

                // statistics
                Percentile<uint64_t> garbage_size;
                uint64_t max_garbage_size;
        };

        // per-thread EBR metadata in CXL
        struct EBRMetaCXL {
                std::atomic<uint64_t> local_epoch{ 0 }; // local epoch always <= global epoch
        };

        // Shared epoch state (this object) must live in HWCC via cxlalloc MISC /
        // kHwccEbr and root index cxl_global_ebr_meta_root_index — matching
        // original Tigon Coordinator::initCXLEBR.  DualRegionAllocator is
        // process-local: bind it after create/attach; never store its VA in CXL.
        CXL_EBR(uint64_t coordinator_num, uint64_t thread_num,
                tigonkv::engine::DualRegionAllocator *regions = nullptr)
                : coordinator_num(coordinator_num)
                , thread_num(thread_num)
        {
                CHECK(coordinator_num <= max_coordinator_num);
                CHECK(thread_num <= max_thread_num);
                if (regions != nullptr) bind_dual_region_allocator(regions);
        }

        static void bind_dual_region_allocator(tigonkv::engine::DualRegionAllocator *regions);
        static void clear_dual_region_allocator() noexcept;
        static tigonkv::engine::DualRegionAllocator *bound_regions();

        void thread_init_ebr_meta(uint64_t coordinator_id, uint64_t thread_id)
        {
                EBRMetaLocal &local_ebr_meta = get_local_ebr_meta();

                initialize_ebr_meta(local_ebr_meta, coordinator_id, thread_id);
        }

        // KVEngine owns one persistent-in-process meta object per foreground
        // worker.  Binding is only a TLS handle; it must never reset the
        // worker's last-freed epoch when a different OS thread takes over.
        void initialize_ebr_meta(EBRMetaLocal &local_ebr_meta,
                                 uint64_t coordinator_id, uint64_t thread_id)
        {

                const uint64_t coordinator_count =
                    latency_sim::FixedLatencyMemoryLoad(
                        latency_sim::MemoryDomain::kHwcc, &coordinator_num);
                const uint64_t thread_count =
                    latency_sim::FixedLatencyMemoryLoad(
                        latency_sim::MemoryDomain::kHwcc, &thread_num);
                local_ebr_meta.coordinator_count = coordinator_count;
                local_ebr_meta.thread_count = thread_count;
                CHECK(coordinator_id < local_ebr_meta.coordinator_count);
                CHECK(thread_id < local_ebr_meta.thread_count);
                local_ebr_meta.coordinator_id = coordinator_id;
                // The caller owns the worker identity.  A process-global
                // counter leaked identities across independent engines and
                // could eventually index beyond this EBR instance's grid.
                local_ebr_meta.thread_id = thread_id;

                local_ebr_meta.last_freed_epoch = 0;

                local_ebr_meta.garbage_size.clear();
                local_ebr_meta.max_garbage_size = 0;

        }

        void bind_external_ebr_meta(EBRMetaLocal *meta)
        {
                if (meta == nullptr)
                        throw std::invalid_argument("null external EBR meta");
                if (bound_ebr_meta_ != nullptr)
                        throw std::runtime_error("EBR external meta already bound");
                bound_ebr_meta_ = meta;
        }

        void unbind_external_ebr_meta()
        {
                if (bound_ebr_meta_ == nullptr)
                        throw std::runtime_error("EBR external meta is not bound");
                bound_ebr_meta_ = nullptr;
        }

        void add_retired_object(void *ptr, uint64_t size, uint64_t category,
                                uint32_t owner_shard = 0,
                                uint32_t private_partition = UINT32_MAX,
                                uint32_t queue_partition = UINT32_MAX)
        {
                EBRMetaLocal &local_ebr_meta = get_local_ebr_meta();
                uint64_t coordinator_id = local_ebr_meta.coordinator_id;
                uint64_t thread_id = local_ebr_meta.thread_id;

                EBRMetaCXL &cxl_ebr_meta = cxl_ebr_meta_vec[coordinator_id][thread_id];
                uint64_t cur_local_epoch =
                    tigonkv::engine::mem_access::HwccAtomicLoad(
                        cxl_ebr_meta.local_epoch, std::memory_order_acquire);

                auto *regions = bound_regions();
                CHECK(regions != nullptr) << "tigonkv: EBR requires dual-region allocator";
                if (queue_partition == UINT32_MAX)
                        queue_partition = private_partition == UINT32_MAX
                                              ? owner_shard : private_partition;
                regions->Retire(owner_shard, queue_partition, thread_id,
                                cur_local_epoch % max_epoch, ptr, size,
                                allocation_domain(category), private_partition);
        }

        void enter_critical_section()
        {
                EBRMetaLocal &local_ebr_meta = get_local_ebr_meta();
                uint64_t coordinator_id = local_ebr_meta.coordinator_id;
                uint64_t thread_id = local_ebr_meta.thread_id;

                EBRMetaCXL &cxl_ebr_meta = cxl_ebr_meta_vec[coordinator_id][thread_id];
                uint64_t cur_local_epoch =
                    tigonkv::engine::mem_access::HwccAtomicLoad(
                        cxl_ebr_meta.local_epoch, std::memory_order_acquire);

                // load global epoch
                uint64_t cur_global_epoch =
                    tigonkv::engine::mem_access::HwccAtomicLoad(
                        global_epoch, std::memory_order_acquire);

                if (cur_global_epoch == cur_local_epoch) {
                        auto *regions = bound_regions();
                        CHECK(regions != nullptr) << "tigonkv: EBR requires dual-region allocator";

                        // try to advance the global epoch
                        if (regions->RetireCount(coordinator_id, thread_id,
                                                 cur_local_epoch % max_epoch) >=
                            epoch_advance_threshold) {
                                bool advance_global_ebr = true;

                                // check if all other threads have entered the current epoch
                                for (uint64_t i = 0;
                                     i < local_ebr_meta.coordinator_count; i++) {
                                        for (uint64_t j = 0;
                                             j < local_ebr_meta.thread_count; j++) {
                                                uint64_t local_epoch =
                                                    tigonkv::engine::mem_access::HwccAtomicLoad(
                                                        cxl_ebr_meta_vec[i][j].local_epoch,
                                                        std::memory_order_acquire);
                                                if (local_epoch < cur_global_epoch) {   // local epoch might be larger than 'cur_global_epoch' because of race conditions
                                                        advance_global_ebr = false;
                                                        break;
                                                }
                                        }
                                }

                                // advance the global epoch
                                if (advance_global_ebr == true) {
                                        uint64_t new_global_epoch = cur_global_epoch + 1;
                                        tigonkv::engine::mem_access::HwccAtomicCompareExchangeStrong(
                                            global_epoch, cur_global_epoch,
                                            new_global_epoch,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire);
                                }
                        }
                } else {
                        CHECK(cur_global_epoch > cur_local_epoch);
                }

                // reload global epoch
                cur_global_epoch =
                    tigonkv::engine::mem_access::HwccAtomicLoad(
                        global_epoch, std::memory_order_acquire);

                // update local epoch if necessary
                if (cur_local_epoch < cur_global_epoch) {
                        CHECK(cur_local_epoch == cur_global_epoch - 1);
                        tigonkv::engine::mem_access::HwccAtomicStore(
                            cxl_ebr_meta.local_epoch, cur_global_epoch,
                            std::memory_order_release);
                }

                // now it is time to reclaim garbage in local_epoch - 2
                if (cur_global_epoch >= 2) {
                        uint64_t epoch_to_reclaim = cur_global_epoch - 2;
                        if (epoch_to_reclaim > local_ebr_meta.last_freed_epoch) {
                                // Preserve the original EBR progression invariant.
                                // A gap would skip a retire epoch and hide allocator
                                // queue corruption rather than reclaiming safely.
                                CHECK(epoch_to_reclaim ==
                                      local_ebr_meta.last_freed_epoch + 1);
                                uint64_t gc_size = 0;
                                auto *regions = bound_regions();
                                CHECK(regions != nullptr) << "tigonkv: EBR requires dual-region allocator";
                                const auto retired = regions->TakeRetired(
                                    coordinator_id, thread_id,
                                    epoch_to_reclaim % max_epoch);
                                for (const auto &object : retired) {
                                        void *ptr = object.private_partition != UINT32_MAX
                                            ? regions->ResolveOwnerPrivate(
                                                  object.object_offset, object.bytes,
                                                  object.private_partition,
                                                  coordinator_id)
                                            : (object.domain == tigonkv::engine::AllocationDomain::kSharedPayloadSwcc
                                                   ? regions->ResolveSharedPayload(
                                                         object.object_offset,
                                                         object.bytes)
                                                   : ((object.domain == tigonkv::engine::AllocationDomain::kHwccIndex ||
                                                       object.domain == tigonkv::engine::AllocationDomain::kHwccMetadata)
                                                          ? regions->ResolveDynamicHwcc(object.object_offset,
                                                                                         object.bytes,
                                                                                         coordinator_id)
                                                          : throw std::runtime_error(
                                                                "EBR retire record has unsupported allocation domain")));
                                        if (object.private_partition != UINT32_MAX)
                                                regions->FreeOwnerPrivate(ptr, object.bytes,
                                                                          object.private_partition,
                                                                          coordinator_id);
                                        else
                                                regions->Free(ptr, object.bytes, object.domain,
                                                              coordinator_id, coordinator_id);
                                        gc_size += object.bytes;
                                }
                                local_ebr_meta.garbage_size.add(gc_size);
                                if (gc_size > local_ebr_meta.max_garbage_size) {
                                        local_ebr_meta.max_garbage_size = gc_size;
                                }
                                local_ebr_meta.last_freed_epoch = epoch_to_reclaim;
                        }
                }
        }

        void print_statistics()
        {
                EBRMetaLocal &local_ebr_meta = get_local_ebr_meta();

                LOG(INFO) << "EBR Statistics for worker " << local_ebr_meta.thread_id
                        << ": GC size 100% = " << local_ebr_meta.garbage_size.nth(100)
                        << " 99% = " << local_ebr_meta.garbage_size.nth(99)
                        << " 50% = " << local_ebr_meta.garbage_size.nth(50)
                        << " avg = " << local_ebr_meta.garbage_size.avg()
                        << " max = " << local_ebr_meta.max_garbage_size;
        }

    private:
        static EBRMetaLocal &get_local_ebr_meta()
	{
		if (bound_ebr_meta_ != nullptr)
			return *bound_ebr_meta_;
		static thread_local EBRMetaLocal local_ebr_meta;
		return local_ebr_meta;
	}
		inline static thread_local EBRMetaLocal *bound_ebr_meta_ = nullptr;

        uint64_t coordinator_num{ 0 };
        uint64_t thread_num{ 0 };
        static tigonkv::engine::AllocationDomain allocation_domain(uint64_t category)
        {
                switch (category) {
                case CXLMemory::INDEX_FREE:
                        return tigonkv::engine::AllocationDomain::kHwccIndex;
                case CXLMemory::METADATA_FREE:
                        return tigonkv::engine::AllocationDomain::kHwccMetadata;
                case CXLMemory::DATA_FREE:
                        return tigonkv::engine::AllocationDomain::kSharedPayloadSwcc;
                case CXLMemory::TRANSPORT_FREE:
                        return tigonkv::engine::AllocationDomain::kTransport;
                case CXLMemory::MISC_FREE:
                        return tigonkv::engine::AllocationDomain::kHwccEbr;
                default:
                        LOG(FATAL) << "tigonkv: unknown EBR allocation category " << category;
                }
        }

        // CXL-resident shared state only (no process VAs).
        std::atomic<uint64_t> global_epoch{ 0 };

        EBRMetaCXL cxl_ebr_meta_vec[max_coordinator_num][max_thread_num];
};

extern CXL_EBR *global_ebr_meta;

} // namespace star
