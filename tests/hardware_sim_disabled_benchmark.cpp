// Fixed-latency overhead benchmark for the real TigonKV paths, run as two
// independent binaries selected by the build (no runtime mode argument):
//   compile-on + 0ns   (default build)   prints variant=compile_on_zero
//   compile-off        (LATENCY_SIM_COMPILE_OFF=ON)   prints variant=compile_off
//
// Every case drives the actual project call site on addresses inside the two
// registered pool ranges of a real in-process KVEngine mapping: typed/atomic,
// the B+Tree domain/atomic adapters, a live RegionAllocator, a REAL
// MPSCRingBuffer construction/enqueue/dequeue, the REAL TwoPLPasha SCC
// write-through bulk (64B/256B over the SWCC shared payload) and the real
// short KV Put/Get/Delete/Scan facade.  The primary comparison is the same
// wrapped code under a 0/0 fixed-latency model (compile-on) versus the raw
// compile-off build.  In a compile-on build the simulator is always active
// once configured, so the wrapped path is the only runtime variant.
#include <latency_sim/config.h>
#include <latency_sim/simulator.h>
#include <latency_sim/testing.h>
#include "common/CXLMemory.h"
#include "common/MPSCRingBuffer.h"
#include "kv/engine/kv_partition.h"
#include "kv/engine/mem_access.h"
#include "kv/engine/region_allocator.h"
#include "common/btree_olc_cxl/BTreeOLC_CXL.h"
#include "protocol/TwoPLPasha/TwoPLPashaHelper.h"
#include "tests/latency_test_support.h"

// The benchmark needs the engine's mapped pool base to re-register its ranges
// while the KV cases run (the component cases register the benchmark pool's
// ranges); the pool handle is private, so access is relaxed for this TU only.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "kv/engine/kv_engine.h"
#undef private
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sched.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint64_t kIterations = 200'000;
constexpr uint64_t kKvIterations = 2'000;
constexpr size_t kSamples = 9;
constexpr uint32_t kFixedKeySize = 32;
constexpr uint32_t kFixedValueSize = 32;
constexpr uint64_t kEntryStructSize = 64;
constexpr uint64_t kRingEntries = 8;

tigonkv::engine::DualRegionConfig MakePoolConfig(uint64_t bytes) {
  tigonkv::engine::DualRegionConfig config;
  config.total_pool_bytes = bytes;
  config.hwcc_size_bytes = 32 * 1024 * 1024;
  config.swcc_offset_bytes = config.hwcc_size_bytes;
  config.swcc_size_bytes = bytes - config.swcc_offset_bytes;
  config.config_hash = 0xabcd;
  config.vm_count = 1;
  config.partition_count = 8;
  config.fixed_key_size = kFixedKeySize;
  config.fixed_value_size = kFixedValueSize;
  return config;
}

tigonkv::Config MakeEngineConfig(const std::string &path) {
  tigonkv::Config config;
  config.shared_memory_path = path;
  config.size_mb = 64;
  config.hwcc_offset_mb = 0;
  config.hwcc_size_mb = 32;
  config.swcc_offset_mb = 32;
  config.swcc_size_mb = 32;
  config.hw_cc_budget_mb = 32;
  config.vm_count = 1;
  config.node_id = 0;
  config.partition_count = 8;
  config.fixed_key_size = kFixedKeySize;
  config.fixed_value_size = kFixedValueSize;
  config.foreground_worker_count_per_vm = 1;
  config.transport_ring_total_mb = 1;
  config.partition_ranges.clear();
  std::string lower;
  for (uint32_t partition = 0; partition < config.partition_count; ++partition) {
    std::string upper;
    if (partition + 1 != config.partition_count)
      upper.assign(1, static_cast<char>((partition + 1) * 256 /
                                        config.partition_count));
    config.partition_ranges.push_back({lower, upper});
    lower = std::move(upper);
  }
  config.Validate();
  return config;
}

std::string BenchKey(uint64_t i) {
  char text[32];
  const int count = std::snprintf(text, sizeof(text), "bk%08llu",
                                  static_cast<unsigned long long>(i));
  std::string fixed(text, static_cast<size_t>(count));
  fixed.resize(kFixedKeySize, ' ');
  return fixed;
}

std::string BenchValue(std::string_view text) {
  std::string value(text);
  value.resize(kFixedValueSize, ' ');
  return value;
}

// The real production allocation entry point used by the engine itself
// (TRANSPORT/METADATA/DATA map to kTransport/kHwccMetadata/kSharedPayloadSwcc).
void *BenchAlloc(uint64_t bytes, int category) {
  return star::cxl_memory.cxlalloc_malloc_wrapper(bytes, category);
}

// Two real mappings: a benchmark pool (whose static layout is never
// finalized, so TRANSPORT-domain allocations stay legal) that owns every
// component-case buffer, and a vm_count=1 KVEngine over a separate backing
// file that owns the short KV facade cases.  The engine's Open registers its
// own ranges and installs the real SCC manager.  Each case runs with exactly
// the ranges of the pool it touches registered (the latency_sim contract has
// one registered range per domain).
class BenchmarkEngine {
 public:
  BenchmarkEngine() {
    char bench_template[] = "/tmp/tigonkv-bench-pool-XXXXXX";
    int fd = mkstemp(bench_template);
    assert(fd >= 0);
    close(fd);
    bench_path_ = bench_template;
    char engine_template[] = "/tmp/tigonkv-bench-engine-XXXXXX";
    fd = mkstemp(engine_template);
    assert(fd >= 0);
    close(fd);
    engine_path_ = engine_template;

    // The benchmark pool Open registers its own ranges (0/0) and runs its
    // init inside its own scope.  Then bind its allocator and allocate every
    // component buffer (including the transport-domain ring) inside the
    // registered HWCC/SWCC ranges, under an explicit scope.
    bench_pool_ = std::make_unique<tigonkv::engine::DualRegionMappedPool>(
        tigonkv::engine::DualRegionMappedPool::Open(
            bench_path_, MakePoolConfig(64ull << 20), true));
    {
      tigonkv::engine::mem_access::LatencyScope scope(
          latency_sim::ExecutionClass::kBackground);
      star::CXLMemory::bind_dual_region_allocator(&bench_pool_->allocator(), 0);
      AllocateComponentBuffers();
      star::CXLMemory::clear_dual_region_allocator();
    }
    // The engine open clears and re-registers its own ranges, initializes
    // the owner's shared-payload (SWCC) allocator and installs the real SCC
    // manager.  The SCC smeta/payload buffers must come from THIS allocator
    // (the shared SWCC allocator is only initialized by the owner).
    engine_ = tigonkv::engine::KVEngine::Open(MakeEngineConfig(engine_path_),
                                              true);
    engine_->BindWorker(0);
    engine_pool_ = engine_->pool_.get();
    engine_allocator_ = &engine_pool_->allocator();
    {
      tigonkv::engine::mem_access::LatencyScope scope(
          latency_sim::ExecutionClass::kBackground);
      star::CXLMemory::bind_dual_region_allocator(engine_allocator_, 0);
      scc_payload_ = new (BenchAlloc(4096, star::CXLMemory::DATA_ALLOCATION))
          star::TwoPLPashaSharedDataSCC;
      scc_smeta_ = new (BenchAlloc(sizeof(star::TwoPLPashaMetadataShared),
                                   star::CXLMemory::METADATA_ALLOCATION))
          star::TwoPLPashaMetadataShared(scc_payload_);
      star::CXLMemory::clear_dual_region_allocator();
    }
  }

  ~BenchmarkEngine() {
    engine_->ReleaseWorker();
    engine_->Shutdown();
    unlink(bench_path_.c_str());
    unlink(engine_path_.c_str());
  }

  tigonkv::engine::KVEngine &engine() { return *engine_; }

  // Switch the registered ranges and the allocator binding to the pool the
  // next case touches.  Quiescent: no scope is active across the switch.  The
  // engine's inbound demuxer thread continuously charges the engine's ring,
  // so it is stopped while the component cases register the benchmark pool's
  // ranges and restarted when the engine pools are re-registered.
  void UseComponentPools() {
    engine_->StopInboundDemuxer();
#if !defined(LATENCY_SIM_COMPILE_OFF)
    auto &sim = latency_sim::GlobalLatencySimulator();
    sim.ClearPoolRegistrations();
    RegisterRanges(sim, bench_pool_->base(), bench_pool_config_);
#endif
    star::CXLMemory::bind_dual_region_allocator(&bench_pool_->allocator(), 0);
  }

  void UseEnginePools() {
#if !defined(LATENCY_SIM_COMPILE_OFF)
    auto &sim = latency_sim::GlobalLatencySimulator();
    sim.ClearPoolRegistrations();
    RegisterRanges(sim, engine_pool_->base(), engine_pool_config_);
#endif
    star::CXLMemory::bind_dual_region_allocator(engine_allocator_, 0);
    engine_->StartInboundDemuxer();
  }

  uint64_t *typed_values() const { return typed_values_; }
  void *atomic_storage() const { return atomic_storage_; }
  uint64_t *btree_nodes() const { return btree_nodes_; }
  void *btree_counter_storage() const { return btree_counter_storage_; }
  void *allocator_region() const { return allocator_region_; }
  tigonkv::engine::DomainCounter *allocator_counter() const {
    return allocator_counter_;
  }
  void *ring_storage() const { return ring_storage_; }
  void *ring_construct_storage() const { return ring_construct_storage_; }
  star::TwoPLPashaSharedDataSCC *scc_payload() const { return scc_payload_; }
  star::TwoPLPashaMetadataShared *scc_smeta() const { return scc_smeta_; }

 private:
#if !defined(LATENCY_SIM_COMPILE_OFF)
  static void RegisterRanges(
      latency_sim::LatencySimulator &sim, void *base,
      const tigonkv::engine::DualRegionConfig &config) {
    sim.RegisterPool(latency_sim::MemoryDomain::kHwcc,
                     static_cast<const std::byte *>(base) +
                         config.hwcc_offset_bytes,
                     config.hwcc_size_bytes);
    sim.RegisterPool(latency_sim::MemoryDomain::kSwcc,
                     static_cast<const std::byte *>(base) +
                         config.swcc_offset_bytes,
                     config.swcc_size_bytes);
    latency_sim::FixedLatencyConfig zero;
    zero.cache_line_bytes = 64;
    zero.swcc_fixed_ns_per_line = 0.0;
    zero.hwcc_fixed_ns_per_line = 0.0;
    sim.Configure(zero);
  }
#endif

  void AllocateComponentBuffers() {
    // All buffers below are allocated through the real allocator while the
    // benchmark pool's ranges are registered and an explicit scope is active.
    typed_values_ = static_cast<uint64_t *>(BenchAlloc(
        4096, star::CXLMemory::TRANSPORT_ALLOCATION));
    atomic_storage_ = BenchAlloc(64, star::CXLMemory::TRANSPORT_ALLOCATION);
    btree_nodes_ = static_cast<uint64_t *>(BenchAlloc(
        4096, star::CXLMemory::TRANSPORT_ALLOCATION));
    btree_counter_storage_ =
        BenchAlloc(64, star::CXLMemory::TRANSPORT_ALLOCATION);
    allocator_region_ = BenchAlloc(
        kIterations / 8 * 128, star::CXLMemory::TRANSPORT_ALLOCATION);
    allocator_counter_ = static_cast<tigonkv::engine::DomainCounter *>(
        BenchAlloc(sizeof(tigonkv::engine::DomainCounter),
                   star::CXLMemory::TRANSPORT_ALLOCATION));
    ring_storage_ = BenchAlloc(sizeof(star::MPSCRingBuffer),
                               star::CXLMemory::TRANSPORT_ALLOCATION);
    ring_construct_storage_ =
        BenchAlloc(sizeof(star::MPSCRingBuffer),
                   star::CXLMemory::TRANSPORT_ALLOCATION);
  }

  std::string bench_path_;
  std::string engine_path_;
  std::unique_ptr<tigonkv::engine::DualRegionMappedPool> bench_pool_;
  tigonkv::engine::DualRegionConfig bench_pool_config_ = MakePoolConfig(64ull << 20);
  tigonkv::engine::DualRegionConfig engine_pool_config_ = [] {
    tigonkv::engine::DualRegionConfig config;
    config.total_pool_bytes = 64ull << 20;
    config.hwcc_size_bytes = 32ull << 20;
    config.swcc_offset_bytes = 32ull << 20;
    config.swcc_size_bytes = 32ull << 20;
    config.config_hash = 0xabcd;
    config.vm_count = 1;
    config.partition_count = 8;
    config.fixed_key_size = kFixedKeySize;
    config.fixed_value_size = kFixedValueSize;
    return config;
  }();
  std::unique_ptr<tigonkv::engine::KVEngine> engine_;
  tigonkv::engine::DualRegionMappedPool *engine_pool_ = nullptr;
  tigonkv::engine::DualRegionAllocator *engine_allocator_ = nullptr;

  uint64_t *typed_values_ = nullptr;
  void *atomic_storage_ = nullptr;
  uint64_t *btree_nodes_ = nullptr;
  void *btree_counter_storage_ = nullptr;
  void *allocator_region_ = nullptr;
  tigonkv::engine::DomainCounter *allocator_counter_ = nullptr;
  void *ring_storage_ = nullptr;
  void *ring_construct_storage_ = nullptr;
  star::TwoPLPashaSharedDataSCC *scc_payload_ = nullptr;
  star::TwoPLPashaMetadataShared *scc_smeta_ = nullptr;
};

using Runner = uint64_t (*)(bool wrapped, BenchmarkEngine &bench);

// Ordinary HwccRead/HwccWrite on a real allocator-owned HWCC slot.
uint64_t RunOrdinary(bool wrapped, BenchmarkEngine &bench) {
  auto *values = bench.typed_values();
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    auto &value = values[i & 255u];
    if (wrapped) {
      latency_sim::testing::ChargeRangeForTest(
          latency_sim::MemoryDomain::kHwcc, latency_sim::AccessKind::kRead,
          &value, sizeof(value));
      latency_sim::testing::ChargeRangeForTest(
          latency_sim::MemoryDomain::kHwcc, latency_sim::AccessKind::kWrite,
          &value, sizeof(value));
    }
    value += i + 1;
    checksum ^= value;
  }
  return checksum ^ values[0];
}

// Atomic fetch/load on a real std::atomic inside the HWCC range.
uint64_t RunAtomic(bool wrapped, BenchmarkEngine &bench) {
  auto *value = new (bench.atomic_storage()) std::atomic<uint64_t>{0};
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    checksum += wrapped
        ? tigonkv::engine::mem_access::HwccAtomicFetchAdd(
              *value, uint64_t{1}, std::memory_order_relaxed)
        : value->fetch_add(uint64_t{1}, std::memory_order_relaxed);
    if ((i & 63u) == 0)
      checksum += wrapped
          ? tigonkv::engine::mem_access::HwccAtomicLoad(
                *value, std::memory_order_relaxed)
          : value->load(std::memory_order_relaxed);
  }
  if (value->load(std::memory_order_relaxed) != kIterations) std::abort();
  return checksum;
}

// Real B+Tree domain/atomic adapters (typed range charge and TreeAtomicFetchAdd);
// the node array and the counter live in HWCC.
uint64_t RunBtreeDomainAdapter(bool wrapped, BenchmarkEngine &bench) {
  auto *nodes = bench.btree_nodes();
  auto *counter =
      new (bench.btree_counter_storage()) std::atomic<uint64_t>{0};
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    auto &node = nodes[(i * 17) & 255u];
    if (wrapped) {
      latency_sim::testing::ChargeRangeForTest(
          latency_sim::MemoryDomain::kHwcc, latency_sim::AccessKind::kRead,
          &node, sizeof(node));
    }
    node = node * 3 + i;
    checksum += wrapped
        ? btreeolc_cxl::TreeAtomicFetchAdd(*counter, uint64_t{1},
                                           std::memory_order_relaxed)
        : counter->fetch_add(uint64_t{1}, std::memory_order_relaxed);
    checksum += node;
  }
  if (counter->load(std::memory_order_relaxed) != kIterations) std::abort();
  return checksum;
}

// The REAL RegionAllocator path over a HWCC-owned region.
uint64_t RunAllocator(bool wrapped, BenchmarkEngine &bench) {
  (void)wrapped;
  auto *region = bench.allocator_region();
  auto *counter = bench.allocator_counter();
  auto allocator = tigonkv::engine::RegionAllocator::Initialize(
      region, kIterations / 8 * 128, 2, 0, /*control_is_hwcc=*/true,
      /*block_is_hwcc=*/true);
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    const uint32_t shard = static_cast<uint32_t>(i & 1u);
    void *block = allocator.Allocate(
        80, tigonkv::engine::AllocationDomain::kHwccMetadata, counter, shard);
    allocator.Free(block, 80,
                   tigonkv::engine::AllocationDomain::kHwccMetadata, counter,
                   shard, shard);
    checksum ^= reinterpret_cast<uintptr_t>(block);
  }
  return checksum;
}

// The REAL MPSC transport ring: a live MPSCRingBuffer constructed in the
// registered HWCC range; every enqueue/dequeue runs the real reservation,
// ready and head/tail/count protocol.
uint64_t RunRingEnqueueDequeue(bool wrapped, BenchmarkEngine &bench) {
  (void)wrapped;
  star::MPSCRingBuffer *ring =
      new (bench.ring_storage()) star::MPSCRingBuffer(kEntryStructSize,
                                                      kRingEntries);
  char payload[] = "ring";
  char output[256]{};
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    assert(ring->enqueue(payload, sizeof(payload)));
    checksum += ring->dequeue(output, sizeof(output));
  }
  assert(std::memcmp(output, payload, sizeof(payload)) == 0);
  return checksum;
}

// The REAL MPSC ring construction path (allocation + header/entry init).
uint64_t RunRingConstruct(bool wrapped, BenchmarkEngine &bench) {
  (void)wrapped;
  auto *ring = new (bench.ring_construct_storage())
      star::MPSCRingBuffer(kEntryStructSize, 4);
  return ring->get_entry_num() + ring->get_entry_size();
}

// The REAL TwoPLPasha SCC write-through bulk (64B/256B) over a real smeta
// and a real SWCC shared payload: SharedPayloadRead/Write charges adjacent to
// the actual do_read/do_write/finish_write calls, exactly like production.
uint64_t RunSccBulk(bool wrapped, BenchmarkEngine &bench, uint64_t bytes) {
  (void)wrapped;
  auto *payload = bench.scc_payload();
  auto *smeta = bench.scc_smeta();
  star::scc_manager->init_scc_metadata(smeta, 0);
  std::array<char, 256> src{};
  std::array<char, 256> dst{};
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < kIterations; ++i) {
    src[0] = static_cast<char>(i);
    latency_sim::testing::ChargeRangeForTest(
        latency_sim::MemoryDomain::kSwcc, latency_sim::AccessKind::kWrite,
        payload->data, bytes);
    star::scc_manager->do_write(smeta, 0, payload->data, src.data(), bytes);
    star::scc_manager->finish_write(smeta, 0, payload, bytes);
    latency_sim::testing::ChargeRangeForTest(
        latency_sim::MemoryDomain::kSwcc, latency_sim::AccessKind::kRead,
        payload->data, bytes);
    star::scc_manager->do_read(smeta, 0, dst.data(), payload->data, bytes);
    checksum += dst[0];
  }
  return checksum;
}

uint64_t RunSccBulk64(bool wrapped, BenchmarkEngine &bench) {
  return RunSccBulk(wrapped, bench, 64);
}

uint64_t RunSccBulk256(bool wrapped, BenchmarkEngine &bench) {
  return RunSccBulk(wrapped, bench, 256);
}

// Real short KV facade: Put/Get/Delete/Scan through the engine.
uint64_t RunKvOps(bool wrapped, BenchmarkEngine &bench) {
  (void)wrapped;
  uint64_t checksum = 0;
  const std::string end = BenchKey(512);
  for (uint64_t i = 0; i < kKvIterations; ++i) {
    const std::string key = BenchKey(i % 512);
    if (!bench.engine().Put(key, BenchValue("v")).ok()) std::abort();
    const auto got = bench.engine().Get(key);
    if (!got.status.ok() || got.value != BenchValue("v")) std::abort();
    const auto scan = bench.engine().Scan(key, end, 4);
    if (!scan.status.ok() || scan.items.empty()) std::abort();
    if (!bench.engine().Delete(key).ok()) std::abort();
    checksum += got.value[0];
  }
  return checksum;
}

struct Summary {
  double median;
  double min;
  double max;
  double p95;
};

Summary Summarize(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const size_t p95_index = (values.size() * 95 + 99) / 100 - 1;
  return {values[values.size() / 2], values.front(), values.back(),
          values[p95_index]};
}

double Measure(Runner runner, bool wrapped, uint64_t iterations,
               uint64_t *sink, BenchmarkEngine &bench) {
  const auto begin = std::chrono::steady_clock::now();
  if (wrapped) {
    // The explicit scope is required by the always-active compile-on
    // simulator; in a compile-off build the scope is a no-op.
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kForeground);
    *sink ^= runner(true, bench);
  } else {
    *sink ^= runner(false, bench);
  }
  const auto end = std::chrono::steady_clock::now();
  return static_cast<double>(
             std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
                 .count()) /
         static_cast<double>(iterations);
}

void Report(std::string_view name, Runner runner, bool has_raw,
            uint64_t iterations, BenchmarkEngine &bench) {
  std::vector<double> raw;
  std::vector<double> wrapped;
  raw.reserve(kSamples);
  wrapped.reserve(kSamples);
  uint64_t sink = 0;
  for (size_t sample = 0; sample < kSamples; ++sample) {
    // Interleave raw and wrapped baselines on the same addresses so drift in
    // one half of the process does not become a fake mode comparison.
    if (has_raw) raw.push_back(Measure(runner, false, iterations, &sink, bench));
    wrapped.push_back(Measure(runner, true, iterations, &sink, bench));
  }
  const Summary wrapped_summary = Summarize(wrapped);
  if (has_raw) {
    const Summary raw_summary = Summarize(raw);
    std::printf(
        "case=%.*s samples=%zu wrapped_median_ns_op=%.3f "
        "wrapped_min_ns_op=%.3f wrapped_max_ns_op=%.3f "
        "wrapped_p95_ns_op=%.3f raw_median_ns_op=%.3f raw_min_ns_op=%.3f "
        "raw_max_ns_op=%.3f raw_p95_ns_op=%.3f\n",
        static_cast<int>(name.size()), name.data(), kSamples,
        wrapped_summary.median, wrapped_summary.min, wrapped_summary.max,
        wrapped_summary.p95, raw_summary.median, raw_summary.min,
        raw_summary.max, raw_summary.p95);
  } else {
    std::printf(
        "case=%.*s samples=%zu wrapped_median_ns_op=%.3f "
        "wrapped_min_ns_op=%.3f wrapped_max_ns_op=%.3f "
        "wrapped_p95_ns_op=%.3f\n",
        static_cast<int>(name.size()), name.data(), kSamples,
        wrapped_summary.median, wrapped_summary.min, wrapped_summary.max,
        wrapped_summary.p95);
  }
  if (sink == UINT64_MAX) std::abort();
}

}  // namespace

int main(int argc, char **argv) {
  const int cpu = argc > 1 ? std::atoi(argv[1]) : -1;
  if (cpu >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(cpu), &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0)
      std::perror("sched_setaffinity");
  }

  std::printf("variant=%s cpu=%d samples=%zu iterations=%llu kv_iterations=%llu\n",
#if defined(LATENCY_SIM_COMPILE_OFF)
              "compile_off",
#else
              "compile_on_zero",
#endif
              cpu, kSamples, static_cast<unsigned long long>(kIterations),
              static_cast<unsigned long long>(kKvIterations));

  BenchmarkEngine bench;
  bench.UseComponentPools();
  Report("typed_load_store", RunOrdinary, true, kIterations, bench);
  Report("atomic_cas", RunAtomic, true, kIterations, bench);
  Report("btree_domain_atomic", RunBtreeDomainAdapter, true, kIterations, bench);
  Report("allocator", RunAllocator, false, kIterations, bench);
  Report("ring_enqueue_dequeue", RunRingEnqueueDequeue, false, kIterations,
         bench);
  Report("ring_construct", RunRingConstruct, false, 1, bench);
  bench.UseEnginePools();
  Report("scc_bulk_64", RunSccBulk64, false, kIterations, bench);
  Report("scc_bulk_256", RunSccBulk256, false, kIterations, bench);
  Report("kv_put_get_delete_scan", RunKvOps, false, kKvIterations, bench);
  return 0;
}
