//
// Epoch-based memory reclaimation across a CXL pod
//
#include "common/CXL_EBR.h"

namespace star
{

CXL_EBR *global_ebr_meta = nullptr;

// Process-local DualRegionAllocator binding for free/retire.  Must not live
// inside the CXL-resident CXL_EBR object (raw VAs are not portable across VMs).
static tigonkv::engine::DualRegionAllocator *g_ebr_regions = nullptr;

void CXL_EBR::bind_dual_region_allocator(tigonkv::engine::DualRegionAllocator *regions)
{
        g_ebr_regions = regions;
}

tigonkv::engine::DualRegionAllocator *CXL_EBR::bound_regions()
{
        return g_ebr_regions;
}

// Percentile sampling gate used by CXL_EBR::enter_critical_section stats.
// Original Coordinator.h defines the same symbol for the benchmark binary;
// tigonkv links this translation unit instead of Coordinator.
bool warmed_up = false;

} // namespace star
