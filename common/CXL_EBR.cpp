//
// Epoch-based memory reclaimation across a CXL pod
//
#include "common/CXL_EBR.h"

namespace star
{

CXL_EBR *global_ebr_meta = nullptr;

// Percentile sampling gate used by CXL_EBR::enter_critical_section stats.
// Original Coordinator.h defines the same symbol for the benchmark binary;
// tigonkv links this translation unit instead of Coordinator.
bool warmed_up = false;

} // namespace star
