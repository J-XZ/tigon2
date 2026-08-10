//
// Software Cache-Coherence Manager
//

#pragma once

#include <stdint.h>
#include <immintrin.h>
#include <xmmintrin.h>
#include <glog/logging.h>
#include "kv/engine/mem_access.h"

/*
 * memory ordering:
 * clflush follows the TSO order in x86.
 * clwb is ordered only by store-fencing operations and is implicitly ordered
 * with older stores executed by the logical processor to the same address.
 * 
 * fortunately, we do not need to deal with this as the ordering
 * will be ensured by the locking primitives
 */
namespace star
{

class SCCManager {
    public:
        static constexpr uint64_t max_metadata_size_in_bytes = 8;

        enum class ReadDestination : uint8_t {
                kLocal,
                kOwnerPrivateSwcc,
        };

        enum class WriteSource : uint8_t {
                kLocal,
                kOwnerPrivateSwcc,
        };

        virtual void init_scc_metadata(void *scc_meta, std::size_t cur_host_id) = 0;

        // assuming every function call is protected by a lock so that
        // we do not need to worry about memory ordering
        virtual void do_read(void *scc_meta, std::size_t cur_host_id,
                             void *dst, const void *src, uint64_t size,
                             ReadDestination destination = ReadDestination::kLocal) = 0;
        virtual void do_write(void *scc_meta, std::size_t cur_host_id,
                              void *dst, const void *src, uint64_t size,
                              WriteSource source = WriteSource::kLocal) = 0;
        virtual void prepare_read(void *scc_meta, std::size_t cur_host_id, void *scc_data, uint64_t size) {}
        virtual void finish_write(void *scc_meta, std::size_t cur_host_id, void *scc_data, uint64_t size) {}

    protected:
        static constexpr uint64_t cacheline_size = 64;

        inline void copy_read(void *dst, const void *src, uint64_t size,
                              ReadDestination destination)
        {
                if (destination == ReadDestination::kOwnerPrivateSwcc) {
                        latency_sim::FixedLatencyMemcpySharedToShared(
                            latency_sim::MemoryDomain::kOwnerPrivateSwcc,
                            dst, latency_sim::MemoryDomain::kSwcc, src, size);
                } else {
                        latency_sim::FixedLatencyCopySharedToLocal(
                            latency_sim::MemoryDomain::kSwcc, dst, src, size);
                }
        }

        inline void copy_write(void *dst, const void *src, uint64_t size,
                               WriteSource source)
        {
                if (source == WriteSource::kOwnerPrivateSwcc) {
                        latency_sim::FixedLatencyMemcpySharedToShared(
                            latency_sim::MemoryDomain::kSwcc,
                            dst,
                            latency_sim::MemoryDomain::kOwnerPrivateSwcc,
                            src, size);
                } else {
                        latency_sim::FixedLatencyCopyLocalToShared(
                            latency_sim::MemoryDomain::kSwcc, dst, src, size);
                }
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

};

extern SCCManager *scc_manager;

} // namespace star
