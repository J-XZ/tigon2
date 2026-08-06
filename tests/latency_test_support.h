#pragma once

// Test-only support for low-level component tests that call the fixed-latency
// wrappers directly on raw buffers without a full engine mapping.  In a
// compile-on build the simulator is always active once configured, so every
// wrapped access requires (1) the address inside a registered pool range and
// (2) an active scope on the executing thread.  This helper registers the
// given SWCC/HWCC buffers as the single process-wide pool ranges (exactly one
// per domain; Configure requires both, so a small legal standalone buffer is
// used for a null side) and applies the supplied configuration for the
// lifetime of the object; wrapped interactions must also run inside a
// mem_access::LatencyScope / latency_sim::ScopeGuard.  It is a no-op in a
// compile-off build.

#include <latency_sim/config.h>
#include <latency_sim/domain.h>
#include <latency_sim/simulator.h>

#include <cstddef>

namespace tigonkv::test {

#if defined(LATENCY_SIM_COMPILE_OFF)

// Compile-off build: the simulator does not exist, so this is a no-op.
class ScopedLatencyPools {
 public:
  ScopedLatencyPools(const void * /*swcc*/, std::size_t /*swcc_size*/,
                     const void * /*hwcc*/, std::size_t /*hwcc_size*/,
                     latency_sim::FixedLatencyConfig /*config*/ = {}) {}

  // Re-registers a fresh set of ranges and applies the configuration.  Only
  // meaningful in a compile-on build; here it is a no-op.
  void Reset(const void * /*swcc*/, std::size_t /*swcc_size*/,
             const void * /*hwcc*/, std::size_t /*hwcc_size*/,
             latency_sim::FixedLatencyConfig /*config*/ = {}) {}
};

#else

// Registers the given SWCC/HWCC buffers as the single process-wide pool
// ranges and applies the configuration for the lifetime of this object.
// Reset() clears the previous registrations first so sequential
// sub-measurements on different mappings stay within the one-range per-domain
// contract.  ClearPoolRegistrations is the only reopen/reset path (Configure
// is not a reset: it requires both ranges and an unconfigured state).  Callers
// must also run the wrapped interactions inside a scope.
class ScopedLatencyPools {
 public:
  ScopedLatencyPools(const void *swcc, std::size_t swcc_size, const void *hwcc,
                     std::size_t hwcc_size,
                     latency_sim::FixedLatencyConfig config = {}) {
    Reset(swcc, swcc_size, hwcc, hwcc_size, config);
  }

  void Reset(const void *swcc, std::size_t swcc_size, const void *hwcc,
             std::size_t hwcc_size, latency_sim::FixedLatencyConfig config = {}) {
    auto &sim = latency_sim::GlobalLatencySimulator();
    sim.ClearPoolRegistrations();
    sim.RegisterPool(latency_sim::MemoryDomain::kSwcc,
                     swcc != nullptr ? swcc : kFallbackSwcc,
                     swcc != nullptr ? swcc_size : sizeof(kFallbackSwcc));
    sim.RegisterPool(latency_sim::MemoryDomain::kHwcc,
                     hwcc != nullptr ? hwcc : kFallbackHwcc,
                     hwcc != nullptr ? hwcc_size : sizeof(kFallbackHwcc));
    sim.Configure(config);
  }

  ~ScopedLatencyPools() {
    latency_sim::GlobalLatencySimulator().ClearPoolRegistrations();
  }

  ScopedLatencyPools(const ScopedLatencyPools &) = delete;
  ScopedLatencyPools &operator=(const ScopedLatencyPools &) = delete;

 private:
  inline static std::byte kFallbackSwcc[64] __attribute__((aligned(64)));
  inline static std::byte kFallbackHwcc[64] __attribute__((aligned(64)));
};

#endif  // LATENCY_SIM_COMPILE_OFF

}  // namespace tigonkv::test
