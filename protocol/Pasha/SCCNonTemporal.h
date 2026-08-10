//
// Software Cache-Coherence Manager
//

#pragma once

#include <cstring>
#include "protocol/Pasha/SCCManager.h"

namespace star
{

class SCCNonTemporal : public SCCManager {
    public:
        void init_scc_metadata(void *scc_meta, std::size_t cur_host_id)
        {}

        void do_read(void *scc_meta, std::size_t cur_host_id, void *dst,
                     const void *src, uint64_t size,
                     ReadDestination destination = ReadDestination::kLocal)
        {
                clflush(src, size);
                copy_read(dst, src, size, destination);
        }

        void do_write(void *scc_meta, std::size_t cur_host_id, void *dst,
                      const void *src, uint64_t size,
                      WriteSource source = WriteSource::kLocal)
        {
                copy_write(dst, src, size, source);
                clwb(dst, size);
        }
};

} // namespace star
