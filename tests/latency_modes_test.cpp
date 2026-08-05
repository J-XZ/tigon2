#include <latency_sim/access.h>
#include <latency_sim/atomic_access.h>
#include <latency_sim/config.h>
#include <latency_sim/domain.h>
#include <latency_sim/scope.h>
#include <latency_sim/simulator.h>
#include "kv/engine/mem_access.h"

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
  config.enabled = true;
  config.cache_line_bytes = 64;
  config.swcc_fixed_ns_per_line = swcc;
  config.hwcc_fixed_ns_per_line = hwcc;
  config.foreground_enabled = true;
  config.background_enabled = true;
  return config;
}

struct TestBuffers {
  explicit TestBuffers(latency_sim::LatencySimulator *simulator) {
    swcc = mmap(nullptr, kPage, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    hwcc = mmap(nullptr, kPage, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(swcc != MAP_FAILED && hwcc != MAP_FAILED);
    simulator->RegisterPool(latency_sim::MemoryDomain::kSwcc, swcc, kPage);
    simulator->RegisterPool(latency_sim::MemoryDomain::kHwcc, hwcc, kPage);
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

}  // namespace

int main() {
  auto &simulator = latency_sim::GlobalLatencySimulator();
#if defined(LATENCY_SIM_COMPILE_OFF)
  // Compile-off: the runtime simulator is unreachable; wrappers compile to the
  // raw operations and nothing can charge or create TLS.
  simulator.Configure(latency_sim::FixedLatencyConfig{});
  const size_t tls_before = simulator.ThreadStateCountForTest();
  latency_sim::FixedLatencyConfig enabled = Fixed(9, 9);
  simulator.Configure(enabled);
  assert(!latency_sim::FixedLatencyEnabledFast());
  {
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kForeground);
    assert(simulator.PendingDelayNsForTest() == 0);
  }
  assert(simulator.ThreadStateCountForTest() == tls_before);
  simulator.Configure(latency_sim::FixedLatencyConfig{});
  return 0;
#else
  TestBuffers buffers(&simulator);

  // Disabled mode does not enter a scope, create TLS, or accumulate delay.
  simulator.Configure(latency_sim::FixedLatencyConfig{});
  assert(!latency_sim::FixedLatencyEnabledFast());
  const size_t tls_before = simulator.ThreadStateCountForTest();
  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  simulator.ChargeRange(latency_sim::MemoryDomain::kHwcc,
                        latency_sim::AccessKind::kRead, buffers.hwcc, 256);
  assert(simulator.PendingDelayNsForTest() == 0);
  assert(!simulator.HasActiveScopeForCurrentThread());
  assert(simulator.ThreadStateCountForTest() == tls_before);

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

  // Foreground and background workers have independent enable switches.
  auto background_only = Fixed(5, 9);
  background_only.foreground_enabled = false;
  simulator.Configure(background_only);
  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                        latency_sim::AccessKind::kRead, buffers.swcc, 64);
  assert(simulator.PendingDelayNsForTest() == 0);
  simulator.EndScopeAndDelay();
  simulator.BeginScope(latency_sim::ExecutionClass::kBackground);
  simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                        latency_sim::AccessKind::kRead, buffers.swcc, 64);
  assert(simulator.PendingDelayNsForTest() == 5);
  simulator.EndScopeAndDelay();

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
  // addition overflow hard fail instead of saturating.
  simulator.Configure(Fixed(1.0e16, 0.0));
  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                        latency_sim::AccessKind::kRead, buffers.swcc, 64);
  fprintf(stderr, "max_pending=%llu per_line=%llu\n", (unsigned long long)simulator.PendingDelayPsForTest(), (unsigned long long)simulator.PendingDelayNsForTest());
  assert(simulator.PendingDelayPsForTest() == 10000000000000000000ull);
  simulator.Configure(latency_sim::FixedLatencyConfig{});
  simulator.EndScopeAndDelay();
  simulator.Configure(Fixed(1.0e16, 0.0));
  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  // Two lines in one access overflow the fixed-point multiply.
  if (fork() == 0) {
    simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                          latency_sim::AccessKind::kRead, buffers.swcc, 128);
    _exit(0);
  }
  int status = 0;
  assert(waitpid(-1, &status, 0) > 0);
  assert(WIFSIGNALED(status));
  simulator.Configure(latency_sim::FixedLatencyConfig{});
  simulator.EndScopeAndDelay();
  simulator.Configure(Fixed(1.0e16, 0.0));
  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
  simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                        latency_sim::AccessKind::kRead, buffers.swcc, 64);
  if (fork() == 0) {
    simulator.ChargeRange(latency_sim::MemoryDomain::kSwcc,
                          latency_sim::AccessKind::kRead, buffers.swcc, 64);
    _exit(0);
  }
  assert(waitpid(-1, &status, 0) > 0);
  assert(WIFSIGNALED(status));
  simulator.Configure(latency_sim::FixedLatencyConfig{});
  simulator.EndScopeAndDelay();

  // Enabled access outside an explicit scope hard fails.
  simulator.Configure(Fixed(3, 11));
  if (fork() == 0) {
    simulator.ChargeRange(latency_sim::MemoryDomain::kHwcc,
                          latency_sim::AccessKind::kRead, buffers.hwcc, 64);
    _exit(0);
  }
  assert(waitpid(-1, &status, 0) > 0);
  assert(WIFSIGNALED(status));

  // Wrong-domain and out-of-range accesses hard fail.
  simulator.BeginScope(latency_sim::ExecutionClass::kForeground);
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
  simulator.EndScopeAndDelay();
  simulator.Configure(latency_sim::FixedLatencyConfig{});

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
  simulator.Configure(latency_sim::FixedLatencyConfig{});

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
  simulator.Configure(latency_sim::FixedLatencyConfig{});

  // Sequential open/close of two different mappings in one process.  The
  // re-enable boundary (KVEngine::Open) disables the gate and clears stale
  // registrations before registering the new mapping, so the old mapping's
  // addresses are rejected and the new mapping charges normally.
  auto& global = latency_sim::GlobalLatencySimulator();
  global.Configure(latency_sim::FixedLatencyConfig{});
  global.ClearPoolRegistrations();
  void* gen1 = mmap(nullptr, kPage, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  void* gen2 = mmap(nullptr, kPage, PROT_READ | PROT_WRITE,
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
  if (fork() == 0) {
    global.ChargeRange(latency_sim::MemoryDomain::kHwcc,
                       latency_sim::AccessKind::kRead, gen1, 64);
    _exit(0);
  }
  assert(waitpid(-1, &status, 0) > 0);
  assert(WIFSIGNALED(status));
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
