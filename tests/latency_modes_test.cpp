#include <latency_sim/access.h>
#include <latency_sim/atomic_access.h>
#include <latency_sim/config.h>
#include <latency_sim/domain.h>
#include <latency_sim/scope.h>
#include <latency_sim/simulator.h>
#include <latency_sim/testing.h>
#include "kv/engine/mem_access.h"
#include "tests/latency_test_support.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>

namespace {

constexpr size_t kPage = 4096;

latency_sim::FixedLatencyConfig Fixed(double swcc, double hwcc) {
  latency_sim::FixedLatencyConfig config;
  config.cache_line_bytes = 64;
  config.swcc_fixed_ns_per_line = swcc;
  config.hwcc_fixed_ns_per_line = hwcc;
  return config;
}

struct TestBuffers {
  TestBuffers() {
    swcc = mmap(nullptr, kPage, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    hwcc = mmap(nullptr, kPage, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(swcc != MAP_FAILED && hwcc != MAP_FAILED);
  }
  ~TestBuffers() {
    munmap(swcc, kPage);
    munmap(hwcc, kPage);
  }
  void *swcc = nullptr;
  void *hwcc = nullptr;
};

void ThrowsOnEarlyExit(TestBuffers *buffers) {
  tigonkv::engine::mem_access::LatencyScope scope(
      latency_sim::ExecutionClass::kForeground);
  tigonkv::engine::mem_access::Record(
      latency_sim::MemoryDomain::kHwcc, latency_sim::AccessKind::kWrite,
      buffers->hwcc, 128);
  throw std::runtime_error("scope cleanup");
}

#if !defined(LATENCY_SIM_COMPILE_OFF)
// Deterministic delay backend: makes scope-exit settlement a no-op so the
// accounting assertions below never actually busy-wait for ~1e16 ns.
void NoOpDelaySpin(std::uint64_t) {}
#endif

}  // namespace

int main() {
#if defined(LATENCY_SIM_COMPILE_OFF)
  // Compile-off: there is no simulator.  Wrappers compile to the raw
  // operations and scopes are no-ops, so the wrapped accesses below behave
  // exactly like the raw operations on the same buffers.
  TestBuffers buffers;
  {
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kForeground);
    auto *value = new (buffers.hwcc) std::atomic<uint64_t>{7};
    assert(tigonkv::engine::mem_access::HwccAtomicFetchAdd(
               *value, uint64_t{3}, std::memory_order_relaxed) == 7);
    assert(value->load(std::memory_order_relaxed) == 10);
    tigonkv::engine::mem_access::HwccRead(buffers.hwcc, 64);
    tigonkv::engine::mem_access::HwccWrite(buffers.hwcc, 64);
    tigonkv::engine::mem_access::TransportRead(buffers.hwcc, 64);
    tigonkv::engine::mem_access::SharedPayloadRead(buffers.swcc, 64);
    tigonkv::engine::mem_access::PrivateWrite(buffers.swcc, 64);
  }
  return 0;
#else
  TestBuffers buffers;
  auto &simulator = latency_sim::GlobalLatencySimulator();
  // The simulator's range-geometry hard fails (wrong domain / out of range)
  // are compiled into Debug builds only; NDEBUG skips them by design.  Those
  // fork assertions below are therefore Debug-only.
#if defined(TIGONKV_CMAKE_BUILD_TYPE)
  const bool debug_range_checks =
      std::string_view(TIGONKV_CMAKE_BUILD_TYPE) == "Debug";
#else
  const bool debug_range_checks = true;
#endif
  // Compile-on: the simulator is always active once configured.  Register one
  // SWCC + one HWCC range and apply the zero-nanosecond model; the pools stay
  // registered for the whole test body.
  tigonkv::test::ScopedLatencyPools pools(buffers.swcc, kPage, buffers.hwcc,
                                          kPage);

  // Zero-ns config: the simulator stays active (compile-on), so an explicit
  // scope is required and bookkeeping still runs; the pending delay is zero.
  simulator.Configure(latency_sim::FixedLatencyConfig{});
  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  simulator.ChargeRange(latency_sim::MemoryDomain::kHwcc,
                        latency_sim::AccessKind::kRead, buffers.hwcc, 256);
  assert(simulator.PendingDelayNsForTest() == 0);
  assert(simulator.HasActiveScopeForCurrentThread());
  simulator.EndScopeAndDelay();
  assert(!simulator.HasActiveScopeForCurrentThread());
  assert(simulator.PendingDelayNsForTest() == 0);

  // Real range coverage: zero bytes are free, unaligned and repeated accesses
  // are charged by covered line on every call.
  simulator.Configure(Fixed(7, 13));
  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                        latency_sim::AccessKind::kRead, buffers.swcc, 0);
  simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                        latency_sim::AccessKind::kRead,
                        static_cast<std::byte *>(buffers.swcc) + 1, 1);
  simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                        latency_sim::AccessKind::kRead,
                        static_cast<std::byte *>(buffers.swcc) + 63, 2);
  simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                        latency_sim::AccessKind::kRead,
                        static_cast<std::byte *>(buffers.swcc) + 1, 1);
  simulator.ChargeRange(latency_sim::MemoryDomain::kHwcc,
                        latency_sim::AccessKind::kWrite,
                        static_cast<std::byte *>(buffers.hwcc) + 127, 2);
  assert(simulator.PendingDelayNsForTest() == 54);  // 4*7 + 2*13

  // Nested scopes share one worker-local pending delay and settle only at the
  // outer safe point.
  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  simulator.ChargeRange(latency_sim::MemoryDomain::kHwcc,
                        latency_sim::AccessKind::kRead, buffers.hwcc, 1);
  assert(simulator.PendingDelayNsForTest() == 67);
  simulator.EndScopeAndDelay();
  assert(simulator.HasActiveScopeForCurrentThread());
  assert(simulator.PendingDelayNsForTest() == 67);
  simulator.EndScopeAndDelay();
  assert(!simulator.HasActiveScopeForCurrentThread());
  assert(simulator.PendingDelayNsForTest() == 0);

  // Atomic wrappers preserve result/expected semantics and charge the actual
  // atomic operation exactly once, regardless of CAS success.
  simulator.Configure(Fixed(3, 11));
  auto *value = new (buffers.hwcc) std::atomic<uint64_t>{7};
  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  assert(latency_sim::FixedLatencyAtomicLoad(
             *value, std::memory_order_acquire,
             latency_sim::MemoryDomain::kHwcc) == 7);
  latency_sim::FixedLatencyAtomicStore(
      *value, uint64_t{8}, std::memory_order_release,
      latency_sim::MemoryDomain::kHwcc);
  assert(latency_sim::FixedLatencyAtomicExchange(
             *value, uint64_t{9}, std::memory_order_acq_rel,
             latency_sim::MemoryDomain::kHwcc) == 8);
  assert(latency_sim::FixedLatencyAtomicFetchAdd(
             *value, uint64_t{1}, std::memory_order_relaxed,
             latency_sim::MemoryDomain::kHwcc) == 9);
  assert(latency_sim::FixedLatencyAtomicFetchXor(
             *value, uint64_t{3}, std::memory_order_relaxed,
             latency_sim::MemoryDomain::kHwcc) == 10);
  uint64_t expected = 100;
  assert(!latency_sim::FixedLatencyAtomicCompareExchangeStrong(
      *value, expected, uint64_t{20}, std::memory_order_acq_rel,
      std::memory_order_acquire, latency_sim::MemoryDomain::kHwcc));
  assert(expected == 9);
  expected = 9;
  assert(latency_sim::FixedLatencyAtomicCompareExchangeStrong(
      *value, expected, uint64_t{20}, std::memory_order_acq_rel,
      std::memory_order_acquire, latency_sim::MemoryDomain::kHwcc));
  assert(value->load(std::memory_order_relaxed) == 20);
  assert(simulator.PendingDelayNsForTest() == 77);  // seven HWCC operations
  simulator.EndScopeAndDelay();

  // A background worker gets its own scope and pending delay; it cannot leak
  // into the foreground thread.
  simulator.Configure(Fixed(17, 19));
  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  simulator.ChargeRange(latency_sim::MemoryDomain::kHwcc,
                        latency_sim::AccessKind::kRead, buffers.hwcc, 64);
  std::thread worker([&] {
    simulator.BeginScope(latency_sim::ExecutionClass::kBackground);
    simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                          latency_sim::AccessKind::kRead, buffers.swcc, 64);
    assert(simulator.PendingDelayNsForTest() == 17);
    simulator.EndScopeAndDelay();
  });
  worker.join();
  assert(simulator.PendingDelayNsForTest() == 19);
  simulator.EndScopeAndDelay();

  // RAII settles pending delay on an exception and leaves no active scope.
  simulator.Configure(Fixed(1, 1));
  try {
    ThrowsOnEarlyExit(&buffers);
  } catch (const std::runtime_error &) {
  }
  assert(!simulator.HasActiveScopeForCurrentThread());
  assert(simulator.PendingDelayNsForTest() == 0);

  // Fractional per-line delays accumulate exactly in fixed point and round
  // once at scope exit: 0.4ns x 5 lines = 2ns, 1.25ns x 4 lines = 5ns, and
  // 0.4 + 0.4 + 0.2 = 1ns while each access alone would round to 0ns.
  simulator.Configure(Fixed(0.4, 0.2));
  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                        latency_sim::AccessKind::kRead, buffers.swcc, 64 * 5);
  assert(simulator.PendingDelayPsForTest() == 2000);
  assert(simulator.PendingDelayNsForTest() == 2);
  simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                        latency_sim::AccessKind::kRead,
                        static_cast<std::byte *>(buffers.swcc) + 64, 64);
  simulator.ChargeRange(latency_sim::MemoryDomain::kHwcc,
                        latency_sim::AccessKind::kRead, buffers.hwcc, 64);
  simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                        latency_sim::AccessKind::kRead,
                        static_cast<std::byte *>(buffers.swcc) + 128, 64);
  assert(simulator.PendingDelayPsForTest() == 3000);
  assert(simulator.PendingDelayNsForTest() == 3);
  simulator.EndScopeAndDelay();

  simulator.Configure(Fixed(1.25, 0.0));
  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                        latency_sim::AccessKind::kRead, buffers.swcc, 64 * 4);
  assert(simulator.PendingDelayPsForTest() == 5000);
  assert(simulator.PendingDelayNsForTest() == 5);
  simulator.EndScopeAndDelay();

  // Flush/invalidate labels are audit-only and never add a second delay.
  simulator.Configure(Fixed(7, 13));
  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                        latency_sim::AccessKind::kRead, buffers.swcc, 64);
  assert(simulator.PendingDelayNsForTest() == 7);
  simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                        latency_sim::AccessKind::kFlush, buffers.swcc, 64);
  simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                        latency_sim::AccessKind::kInvalidate, buffers.swcc, 64);
  assert(simulator.PendingDelayNsForTest() == 7);
  simulator.EndScopeAndDelay();

  // Maximum legal value accumulates without overflow; multiplication and
  // addition overflow hard fail instead of saturating.  A deterministic fake
  // delay backend replaces the real TSC busy-wait so the accounting
  // assertions never actually spin for the ~1e16 ns budgets below.
  latency_sim::detail::SetDelaySpinBackendForTest(&NoOpDelaySpin);
  simulator.Configure(Fixed(1.0e16, 0.0));
  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                        latency_sim::AccessKind::kRead, buffers.swcc, 64);
  assert(simulator.PendingDelayPsForTest() == 10000000000000000000ull);
  simulator.EndScopeAndDelay();
  assert(simulator.PendingDelayNsForTest() == 0);

  // Two lines in one access overflow the fixed-point multiply at the scope
  // exit settlement, not at the charge site.
  simulator.Configure(Fixed(1.0e16, 0.0));
  if (fork() == 0) {
    {
      latency_sim::ScopeGuard scope(latency_sim::ExecutionClass::kForeground);
      simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                            latency_sim::AccessKind::kRead, buffers.swcc, 128);
    }  // scope exit settles: 2 x 1e19 ps overflows -> abort before _exit
    _exit(0);
  }
  int status = 0;
  assert(waitpid(-1, &status, 0) > 0);
  assert(WIFSIGNALED(status));

  // One SWCC line plus one HWCC line overflow the fixed-point add at the
  // scope exit settlement.
  simulator.Configure(Fixed(1.0e16, 1.0e16));
  if (fork() == 0) {
    {
      latency_sim::ScopeGuard scope(latency_sim::ExecutionClass::kForeground);
      simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                            latency_sim::AccessKind::kRead, buffers.swcc, 64);
      simulator.ChargeRange(latency_sim::MemoryDomain::kHwcc,
                            latency_sim::AccessKind::kRead, buffers.hwcc, 64);
    }  // scope exit settles: 1e19 + 1e19 ps overflows -> abort before _exit
    _exit(0);
  }
  assert(waitpid(-1, &status, 0) > 0);
  assert(WIFSIGNALED(status));
  latency_sim::detail::SetDelaySpinBackendForTest(nullptr);

  // Always-active access outside an explicit scope hard fails.
  simulator.Configure(Fixed(3, 11));
  if (fork() == 0) {
    simulator.ChargeRange(latency_sim::MemoryDomain::kHwcc,
                          latency_sim::AccessKind::kRead, buffers.hwcc, 64);
    _exit(0);
  }
  assert(waitpid(-1, &status, 0) > 0);
  assert(WIFSIGNALED(status));

  // Wrong-domain and out-of-range accesses hard fail (Debug range geometry
  // only; NDEBUG compiles the boundary checks out, so this is skipped there).
  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  if (debug_range_checks) {
    if (fork() == 0) {
      simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                            latency_sim::AccessKind::kRead, buffers.hwcc, 64);
      _exit(0);
    }
    assert(waitpid(-1, &status, 0) > 0);
    assert(WIFSIGNALED(status));
    if (fork() == 0) {
      simulator.ChargeRange(
          latency_sim::MemoryDomain::kHwcc, latency_sim::AccessKind::kRead,
          static_cast<std::byte *>(buffers.hwcc) + kPage - 1, 2);
      _exit(0);
    }
    assert(waitpid(-1, &status, 0) > 0);
    assert(WIFSIGNALED(status));
  }
  simulator.EndScopeAndDelay();

  // Suspension defers the pending delay to the outer scope exit instead of
  // busy-waiting inside a still-alive guard; resume restores the scope.  The
  // latency_sim contract allows at most one active suspension: a second
  // SuspendScopeAndDelayLater before the resume returns false, so the
  // temporary scope settles its own budget at its own exit (never inside a
  // still-alive guard) and no deferred segment is lost or duplicated.
  simulator.Configure(Fixed(9, 9));
  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  simulator.ChargeRange(latency_sim::MemoryDomain::kHwcc,
                        latency_sim::AccessKind::kRead, buffers.hwcc, 64);
  assert(simulator.PendingDelayNsForTest() == 9);
  assert(simulator.SuspendScopeAndDelayLater());
  assert(!simulator.HasActiveScopeForCurrentThread());
  assert(simulator.PendingDelayNsForTest() == 0);
  simulator.BeginScope(latency_sim::ExecutionClass::kBackground);
  simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                        latency_sim::AccessKind::kRead, buffers.swcc, 64);
  assert(simulator.PendingDelayNsForTest() == 9);
  assert(!simulator.SuspendScopeAndDelayLater());  // one suspension at a time
  assert(simulator.HasActiveScopeForCurrentThread());
  simulator.EndScopeAndDelay();  // temporary scope settles its own 9 (swcc)
  assert(!simulator.HasActiveScopeForCurrentThread());
  assert(simulator.PendingDelayNsForTest() == 0);
  simulator.ResumeScope(latency_sim::ExecutionClass::kForeground);
  assert(simulator.HasActiveScopeForCurrentThread());
  simulator.EndScopeAndDelay();  // settles the deferred 9 (hwcc) exactly once
  assert(!simulator.HasActiveScopeForCurrentThread());
  assert(simulator.PendingDelayNsForTest() == 0);

  // A ForegroundScopeSuspension inside an already nested scope must not eat a
  // nesting level (which would settle the outer budget while guards are still
  // alive) and must not busy-wait early.
  simulator.Configure(Fixed(3, 3));
  simulator.BeginScope(latency_sim::ExecutionClass::kBackground);
  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  simulator.ChargeRange(latency_sim::MemoryDomain::kHwcc,
                        latency_sim::AccessKind::kRead, buffers.hwcc, 64);
  assert(simulator.PendingDelayNsForTest() == 3);
  {
    tigonkv::engine::mem_access::ForegroundScopeSuspension suspend;
    assert(simulator.HasActiveScopeForCurrentThread());
  }
  assert(simulator.HasActiveScopeForCurrentThread());
  assert(simulator.PendingDelayNsForTest() == 3);
  simulator.EndScopeAndDelay();  // nested exit: depth stays 1, no settlement
  assert(simulator.HasActiveScopeForCurrentThread());
  assert(simulator.PendingDelayNsForTest() == 3);
  simulator.EndScopeAndDelay();  // outermost exit settles exactly once
  assert(!simulator.HasActiveScopeForCurrentThread());
  assert(simulator.PendingDelayNsForTest() == 0);

  // Sequential open/close of two different mappings in one process.  The
  // reopen boundary (KVEngine::Open) clears stale registrations before
  // registering the new mapping, so the old mapping's addresses are rejected
  // and the new mapping charges normally.
  auto &global = latency_sim::GlobalLatencySimulator();
  global.Configure(latency_sim::FixedLatencyConfig{});
  global.ClearPoolRegistrations();
  void *gen1 = mmap(nullptr, kPage, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  void *gen2 = mmap(nullptr, kPage, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(gen1 != MAP_FAILED && gen2 != MAP_FAILED);
  global.RegisterPool(latency_sim::MemoryDomain::kHwcc, gen1, kPage);
  global.Configure(Fixed(1, 1));
  global.BeginScope(latency_sim::ExecutionClass::kForeground);
  global.ChargeRange(latency_sim::MemoryDomain::kHwcc,
                     latency_sim::AccessKind::kRead, gen1, 64);
  assert(global.PendingDelayNsForTest() == 1);
  global.EndScopeAndDelay();

  global.Configure(latency_sim::FixedLatencyConfig{});
  global.ClearPoolRegistrations();
  global.RegisterPool(latency_sim::MemoryDomain::kHwcc, gen2, kPage);
  global.Configure(Fixed(1, 1));
  global.BeginScope(latency_sim::ExecutionClass::kForeground);
  if (debug_range_checks) {
    if (fork() == 0) {
      global.ChargeRange(latency_sim::MemoryDomain::kHwcc,
                         latency_sim::AccessKind::kRead, gen1, 64);
      _exit(0);
    }
    assert(waitpid(-1, &status, 0) > 0);
    assert(WIFSIGNALED(status));
  }
  global.ChargeRange(latency_sim::MemoryDomain::kHwcc,
                     latency_sim::AccessKind::kRead, gen2, 64);
  assert(global.PendingDelayNsForTest() == 1);
  global.EndScopeAndDelay();
  global.Configure(latency_sim::FixedLatencyConfig{});
  global.ClearPoolRegistrations();
  munmap(gen1, kPage);
  munmap(gen2, kPage);
  return 0;
#endif
}
