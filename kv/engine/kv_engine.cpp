#include "kv/engine/kv_engine.h"

#include "kv/engine/fixed_value.h"

#include "common/CXL_EBR.h"
#include "common/BufferedReader.h"
#include "common/MPSCRingBuffer.h"
#include "core/CxlIncomingDispatcher.h"
#include "kv/engine/kv_partition.h"
#include "kv/engine/kv_migration.h"
#include "kv/engine/kv_worker_context.h"
#include "kv/engine/mem_access.h"
#include "protocol/TwoPLPasha/TwoPLPashaMessage.h"
#include "protocol/TwoPLPasha/TwoPLPashaSCCWriteThrough.h"

#include <stdexcept>
#include <thread>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <fstream>
#include <sched.h>
#include <unistd.h>

namespace tigonkv::engine {
namespace {

// Test-only Open failpoints: KVEngine::Open() throws at the named phase so the
// rollback path (workers joined, gate disabled, registrations cleared,
// allocator/EBR/SCC bindings cleared, mapping released) can be verified
// deterministically.  Compiled out of release (NDEBUG) production builds.
#ifndef NDEBUG
void MaybeThrowOpenFailpoint(const char *phase) {
  const char *target = std::getenv("TIGONKV_TEST_OPEN_FAILPOINT");
  if (target != nullptr && target[0] != '\0' &&
      std::strcmp(target, phase) == 0) {
    throw std::runtime_error(std::string("tigonkv: injected Open failpoint at ")
                             + phase);
  }
}
#else
void MaybeThrowOpenFailpoint(const char *phase) { (void)phase; }
#endif

// Shared layout identity.  This deliberately hashes parsed canonical fields,
// not JSON spelling or node-local wiring: every attaching VM must agree on
// routing, persistent layout and latency contract before it dereferences an
// offset in the shared pool.
class LayoutDigest {
 public:
  void Bytes(const void *data, size_t bytes) {
    const auto *input = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < bytes; ++i) {
      value_ ^= input[i];
      value_ *= 1099511628211ULL;
    }
  }
  void U64(uint64_t value) {
    for (size_t byte = 0; byte < sizeof(value); ++byte) {
      const unsigned char part = static_cast<unsigned char>(value >> (byte * 8));
      Bytes(&part, sizeof(part));
    }
  }
  void Bool(bool value) { U64(value ? 1 : 0); }
  void Double(double value) { Bytes(&value, sizeof(value)); }
  void String(std::string_view value) {
    U64(value.size());
    Bytes(value.data(), value.size());
  }
  void Field(std::string_view name) { String(name); }
  uint64_t value() const { return value_; }

 private:
  uint64_t value_ = 1469598103934665603ULL;
};

bool IsInternalMaxSentinel(std::string_view key, uint32_t fixed_key_size) {
  if (key.size() != fixed_key_size) return false;
  return FixedKey::From(key, fixed_key_size).Compare(
             FixedKey::InternalMax(fixed_key_size)) == 0;
}

// TwoPLPasha's scan callback uses an inclusive max key while the KV facade
// deliberately exposes the usual exclusive upper bound.  Keys are fixed-size
// unsigned byte strings, so this conversion is exact and needs no protocol
// mode bit.
bool ExclusiveToInclusive(std::string_view exclusive, uint32_t key_size,
                          FixedKey *inclusive) {
  if (inclusive == nullptr || exclusive.size() != key_size) return false;
  *inclusive = FixedKey::From(exclusive, key_size);
  for (size_t i = key_size; i != 0; --i) {
    auto &byte = reinterpret_cast<unsigned char *>(inclusive->bytes)[i - 1];
    if (byte != 0) {
      --byte;
      return true;
    }
    byte = 0xff;
  }
  return false;
}

bool LessFixed(std::string_view left, std::string_view right, uint32_t key_size) {
  return FixedKey::From(left, key_size).Compare(FixedKey::From(right, key_size)) < 0;
}

uint64_t SharedLayoutConfigDigest(const Config &config) {
  LayoutDigest digest;
  const auto u64 = [&](std::string_view name, uint64_t value) {
    digest.Field(name); digest.U64(value);
  };
  const auto boolean = [&](std::string_view name, bool value) {
    digest.Field(name); digest.Bool(value);
  };
  const auto decimal = [&](std::string_view name, double value) {
    digest.Field(name); digest.Double(value);
  };
  const auto text = [&](std::string_view name, std::string_view value) {
    digest.Field(name); digest.String(value);
  };
  u64("layout_version", kSharedLayoutVersion);
  u64("partition_count", config.partition_count);
  u64("vm_count", config.vm_count);
  u64("foreground_worker_count", config.foreground_worker_count_per_vm);
  u64("fixed_key_size", config.fixed_key_size);
  u64("fixed_value_size", config.fixed_value_size);
  u64("total_pool_bytes", config.size_mb * 1024ULL * 1024ULL);
  u64("hwcc_offset_bytes", config.hwcc_offset_mb * 1024ULL * 1024ULL);
  u64("hwcc_size_bytes", config.hwcc_size_mb * 1024ULL * 1024ULL);
  u64("swcc_offset_bytes", config.swcc_offset_mb * 1024ULL * 1024ULL);
  u64("swcc_size_bytes", config.swcc_size_mb * 1024ULL * 1024ULL);
  decimal("owner_private_swcc_fraction", config.owner_private_swcc_fraction);
  u64("transport_ring_bytes", config.transport_ring_total_mb * 1024ULL * 1024ULL);
  u64("hwcc_budget_bytes", config.hw_cc_budget_mb * 1024ULL * 1024ULL);
  text("migration_policy", config.migration_policy);
  text("when_to_move_out", config.when_to_move_out);
  text("scc_mechanism", config.scc_mechanism);
  boolean("enable_phantom_detection", true);
  boolean("enable_scc", true);
  boolean("enable_migration_optimization", true);
  boolean("model_cxl_search_overhead", false);
  boolean("internal_max_sentinel", true);
  for (uint32_t partition = 0; partition < config.partition_count; ++partition) {
    text("range_lower", config.partition_ranges[partition].lower_key);
    text("range_upper", config.partition_ranges[partition].upper_key);
  }
  const auto &fixed = config.hardware_simulation;
  u64("fixed_latency.cache_line_bytes", fixed.cache_line_bytes);
  decimal("fixed_latency.swcc_fixed_ns_per_line", fixed.swcc_fixed_ns_per_line);
  decimal("fixed_latency.hwcc_fixed_ns_per_line", fixed.hwcc_fixed_ns_per_line);
  return digest.value();
}

uint64_t CurrentRssKb() {
  std::ifstream statm("/proc/self/statm");
  uint64_t pages = 0;
  uint64_t resident = 0;
  if (!(statm >> pages >> resident)) return 0;
  const long page_size = ::sysconf(_SC_PAGESIZE);
  return page_size > 0 ? resident * static_cast<uint64_t>(page_size) / 1024 : 0;
}

// The original Message header carries the foreground worker identity.  The
// TLS is only a process-local handle to that worker's SPSC inbox; it is never
// serialized or published in shared memory.
thread_local int32_t TlsForegroundWorkerId = -1;
thread_local uint64_t *TlsWorkerMaxTid = nullptr;

// Original CXL_EBR has no leave state: one foreground operation enters the
// current epoch before touching tree/row state and keeps that epoch while it
// waits for RPC.  Nested facade helpers (notably PollTransport while awaiting
// a response) must therefore reuse the outer entry rather than manufacture a
// second operation boundary.
thread_local uint32_t TlsEbrOperationDepth = 0;

class EbrOperationScope {
 public:
  explicit EbrOperationScope(star::CXL_EBR *ebr) : ebr_(ebr) {
    if (ebr_ == nullptr) throw std::runtime_error("null CXL_EBR");
    if (TlsEbrOperationDepth++ == 0) ebr_->enter_critical_section();
  }
  ~EbrOperationScope() { --TlsEbrOperationDepth; }

 private:
  star::CXL_EBR *ebr_;
};

class ScopedKvEbrMetaBinding {
 public:
  ScopedKvEbrMetaBinding(star::CXL_EBR *ebr,
                         star::CXL_EBR::EBRMetaLocal *meta)
      : ebr_(ebr) {
    ebr_->bind_external_ebr_meta(meta);
  }
  ~ScopedKvEbrMetaBinding() { ebr_->unbind_external_ebr_meta(); }
  ScopedKvEbrMetaBinding(const ScopedKvEbrMetaBinding &) = delete;

 private:
  star::CXL_EBR *ebr_;
};

void AddRuntimeStats(RuntimeStats *total, const RuntimeStats &part) {
  total->logical_ops += part.logical_ops;
  total->commits += part.commits;
  total->aborts += part.aborts;
  total->retries += part.retries;
  total->private_gets += part.private_gets;
  total->private_puts += part.private_puts;
  total->private_deletes += part.private_deletes;
  total->private_swcc_flushes += part.private_swcc_flushes;
  total->shared_gets += part.shared_gets;
  total->shared_puts += part.shared_puts;
  total->shared_deletes += part.shared_deletes;
  total->shared_swcc_flushes += part.shared_swcc_flushes;
  total->migration_in += part.migration_in;
  total->migration_out += part.migration_out;
  total->network_tx_bytes += part.network_tx_bytes;
  total->network_rx_bytes += part.network_rx_bytes;
  total->scan_rows_returned += part.scan_rows_returned;
  total->scan_ops += part.scan_ops;
  total->scan_partition_probes += part.scan_partition_probes;
  total->scan_migrate_rpcs += part.scan_migrate_rpcs;
}

[[noreturn]] void ProtocolFatal(uint32_t node_id, const char *stage,
                                const char *detail,
                                star::Message *message = nullptr) {
  std::fprintf(stderr,
      "TIGONKV_TRANSPORT_FATAL node=%u stage=%s detail=%s",
      node_id, stage, detail);
  if (message != nullptr) {
    std::fprintf(stderr,
        " source=%llu destination=%llu worker=%llu count=%llu bytes=%llu",
        static_cast<unsigned long long>(message->get_source_node_id()),
        static_cast<unsigned long long>(message->get_dest_node_id()),
        static_cast<unsigned long long>(message->get_worker_id()),
        static_cast<unsigned long long>(message->get_message_count()),
        static_cast<unsigned long long>(message->get_message_length()));
  }
  std::fputc('\n', stderr);
  std::fflush(stderr);
  std::abort();
}

[[noreturn]] void TransportFatal(uint32_t node_id, const char *stage,
                                 const char *detail,
                                 star::Message *message = nullptr) {
  ProtocolFatal(node_id, stage, detail, message);
}

void InitializeMessage(star::Message *message, uint32_t source, uint32_t destination,
                       uint32_t worker_id) {
  message->set_source_node_id(source);
  message->set_dest_node_id(destination);
  message->set_worker_id(worker_id);
}

bool IsResponseType(uint32_t type) {
  using MessageType = star::TwoPLPashaMessage;
  return type == static_cast<uint32_t>(MessageType::DATA_MIGRATION_RESPONSE) ||
      type == static_cast<uint32_t>(MessageType::DATA_MIGRATION_RESPONSE_FOR_SCAN) ||
      type == static_cast<uint32_t>(MessageType::REMOTE_INSERT_RESPONSE) ||
      type == static_cast<uint32_t>(MessageType::REMOTE_DELETE_RESPONSE);
}

std::vector<int> AllowedCpus() {
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  if (::sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
    throw std::runtime_error(
        "sched_getaffinity failed: " + std::string(std::strerror(errno)));
  std::vector<int> cpus;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    if (CPU_ISSET(cpu, &allowed)) cpus.push_back(cpu);
  return cpus;
}

std::vector<int> ResolveAffinityCpus(const Config &config) {
  if (!config.cpu_affinity) return {};
  const size_t required =
      static_cast<size_t>(config.foreground_worker_count_per_vm) + 1;
  auto allowed = AllowedCpus();
  if (allowed.size() < required)
    throw std::runtime_error(
        "cpu_affinity requires " + std::to_string(required) +
        " allowed CPUs (foreground workers + inbound demuxer), found " +
        std::to_string(allowed.size()));
  return allowed;
}

void BindCurrentThreadToCpuIndex(const std::vector<int> &allowed,
                                 uint32_t index) {
  if (allowed.empty()) return;
  if (index >= allowed.size())
    throw std::runtime_error("cpu affinity index exceeds allowed CPU set");
  cpu_set_t target;
  CPU_ZERO(&target);
  CPU_SET(allowed[index], &target);
  if (::sched_setaffinity(0, sizeof(target), &target) != 0)
    throw std::runtime_error(
        "sched_setaffinity failed: " + std::string(std::strerror(errno)));
}

DualRegionConfig RegionConfig(const Config &config) {
  DualRegionConfig region;
  region.total_pool_bytes = config.size_mb * 1024ULL * 1024ULL;
  region.hwcc_offset_bytes = config.hwcc_offset_mb * 1024ULL * 1024ULL;
  region.hwcc_size_bytes = config.hwcc_size_mb * 1024ULL * 1024ULL;
  region.swcc_offset_bytes = config.swcc_offset_mb * 1024ULL * 1024ULL;
  region.swcc_size_bytes = config.swcc_size_mb * 1024ULL * 1024ULL;
  region.config_hash = SharedLayoutConfigDigest(config);
  region.vm_count = config.vm_count;
  region.partition_count = config.partition_count;
  region.fixed_key_size = config.fixed_key_size;
  region.fixed_value_size = config.fixed_value_size;
  region.owner_private_swcc_fraction = config.owner_private_swcc_fraction;
  return region;
}


}  // namespace

uint64_t &CurrentKvWorkerMaxTid() {
  if (TlsWorkerMaxTid == nullptr)
    throw std::runtime_error("KV commit TID used without a bound worker context");
  return *TlsWorkerMaxTid;
}

void BindKvWorkerMaxTid(uint64_t *max_tid) {
  if (max_tid == nullptr || TlsWorkerMaxTid != nullptr)
    throw std::runtime_error("invalid KV worker TID context bind");
  TlsWorkerMaxTid = max_tid;
}

void ReleaseKvWorkerMaxTid() {
  if (TlsWorkerMaxTid == nullptr)
    throw std::runtime_error("invalid KV worker TID context release");
  TlsWorkerMaxTid = nullptr;
}

KVEngine::KVEngine(const Config &config, std::unique_ptr<DualRegionMappedPool> pool,
                   star::CXL_EBR *ebr, std::unique_ptr<star::SCCManager> scc)
    : config_(config),
      pool_(std::move(pool)),
      ebr_(ebr),
      scc_(std::move(scc)),
      worker_owners_(config.foreground_worker_count_per_vm),
      worker_runtime_(config.foreground_worker_count_per_vm) {
  worker_mailboxes_.reserve(config.foreground_worker_count_per_vm);
  for (uint32_t worker = 0; worker < config.foreground_worker_count_per_vm;
       ++worker) {
    ebr_->initialize_ebr_meta(worker_runtime_[worker].ebr_meta,
                               config.node_id, worker);
    auto mailbox = std::make_unique<WorkerMailbox>();
    mailbox->outbound.reserve(config.vm_count);
    for (uint32_t destination = 0; destination < config.vm_count; ++destination)
      mailbox->outbound.push_back(std::make_unique<star::Message>());
    worker_mailboxes_.push_back(std::move(mailbox));
  }
}

KVEngine::~KVEngine() {
  if (shutdown_complete_) return;
  try {
    Shutdown();
  } catch (...) {
    // Destruction cannot report an active worker to its caller.  Refusing to
    // unmap a live pool is a hard safety invariant, so an omitted explicit
    // ReleaseWorker/Shutdown is treated as a programming error rather than
    // silently tearing down protocol state underneath a worker.
    std::terminate();
  }
}

void KVEngine::Shutdown() {
  if (shutdown_complete_) return;
  {
    std::lock_guard<std::mutex> lock(worker_owner_mutex_);
    const auto active = std::count_if(
        worker_owners_.begin(), worker_owners_.end(),
        [](const std::thread::id &owner) { return owner != std::thread::id{}; });
    if (active != 0)
      throw std::runtime_error(
          "KVEngine::Shutdown requires every foreground worker to ReleaseWorker");
  }

  // Quiesce all asynchronous protocol activity before touching any global or
  // mapped state.  The demuxer is the sole MPSC consumer and must be joined
  // before rings_ or its callback-owned mailboxes disappear.
  StopInboundDemuxer();
  KvMigrationRuntime::Instance().Reset();
  partitions_.clear();

  if (star::scc_manager == scc_.get()) star::scc_manager = nullptr;
  if (star::global_ebr_meta == ebr_) star::global_ebr_meta = nullptr;
  star::CXL_EBR::clear_dual_region_allocator();
  star::CXLMemory::clear_dual_region_allocator();
  scc_.reset();
  rings_ = nullptr;
  ebr_ = nullptr;

  // All workers are joined; clear the pool registrations at the quiescent
  // boundary before the allocator unmaps the registered ranges.  This is a
  // lifecycle reset, not a runtime disable.
#if !defined(LATENCY_SIM_COMPILE_OFF)
  latency_sim::GlobalLatencySimulator().ClearPoolRegistrations();
#endif

  worker_mailboxes_.clear();
  pool_.reset();
  shutdown_complete_ = true;
}

std::unique_ptr<KVEngine> KVEngine::Open(Config config, bool reset) {
#if !defined(LATENCY_SIM_COMPILE_OFF)
  auto &simulator = latency_sim::GlobalLatencySimulator();
#endif
  std::unique_ptr<DualRegionMappedPool> pool;
  std::unique_ptr<KVEngine> engine;
  std::unique_ptr<star::SCCManager> scc;
  star::CXL_EBR *ebr = nullptr;
  bool lifecycle_touched = false;
  auto rollback = [&]() noexcept {
    if (!lifecycle_touched) return;
    if (engine) {
      try {
        engine->Shutdown();
      } catch (...) {
        // Open has no live foreground workers.  Continue clearing every
        // process-local binding even if a future shutdown check is tightened.
      }
      engine.reset();
    }
    KvMigrationRuntime::Instance().Reset();
    if (star::scc_manager == scc.get()) star::scc_manager = nullptr;
    if (star::global_ebr_meta == ebr) star::global_ebr_meta = nullptr;
    star::CXL_EBR::clear_dual_region_allocator();
    star::CXLMemory::clear_dual_region_allocator();
    scc.reset();
#if !defined(LATENCY_SIM_COMPILE_OFF)
    simulator.ClearPoolRegistrations();
#endif
    pool.reset();
  };
  try {
    config.Validate();
    auto affinity_cpus = ResolveAffinityCpus(config);
    // Isolate pool open from any previous simulator lifecycle in this process
    // (tests create sequential stores).  Pool open itself runs before the
    // registration below; fixed latency is configured after registration.
#if !defined(LATENCY_SIM_COMPILE_OFF)
    simulator.ClearPoolRegistrations();
#endif
    lifecycle_touched = true;
    pool = std::make_unique<DualRegionMappedPool>(
        DualRegionMappedPool::Open(config.shared_memory_path, RegionConfig(config), reset));
    MaybeThrowOpenFailpoint("after-pool-mapping");
  // The pool Open already registered the immutable HWCC/SWCC mapping
  // boundaries with the 0/0 model; drop that registration and re-apply the
  // real three-field configuration.  Once configured the simulator stays
  // active for the whole lifecycle; under LATENCY_SIM_COMPILE_OFF none of this
  // exists.
#if !defined(LATENCY_SIM_COMPILE_OFF)
  simulator.ClearPoolRegistrations();
  const DualRegionConfig region_config = RegionConfig(config);
  simulator.RegisterPool(
      latency_sim::MemoryDomain::kHwcc,
      static_cast<const std::byte *>(pool->base()) +
          region_config.hwcc_offset_bytes,
      region_config.hwcc_size_bytes);
  simulator.RegisterPool(
      latency_sim::MemoryDomain::kSwcc,
      static_cast<const std::byte *>(pool->base()) +
          region_config.swcc_offset_bytes,
      region_config.swcc_size_bytes);
  simulator.Configure(config.hardware_simulation);
#endif
  MaybeThrowOpenFailpoint("after-registration");
  mem_access::LatencyScope open_scope(latency_sim::ExecutionClass::kBackground);
  star::CXLMemory::bind_dual_region_allocator(&pool->allocator(), config.node_id);
  star::MPSCRingBuffer *rings = nullptr;
  if (reset) {
    rings = static_cast<star::MPSCRingBuffer *>(star::cxl_memory.cxlalloc_malloc_wrapper(
        sizeof(star::MPSCRingBuffer) * config.vm_count,
        star::CXLMemory::TRANSPORT_ALLOCATION));
    // 2048-byte records with the fixed message payload above; transport is a
    // global HWCC allocation published before any remote node attaches.
    const uint64_t entries = std::max<uint64_t>(1,
        (config.transport_ring_total_mb * 1024ULL * 1024ULL / config.vm_count) / 2048ULL);
    for (uint32_t node = 0; node < config.vm_count; ++node)
      new (&rings[node]) star::MPSCRingBuffer(2048, entries);
    star::CXLMemory::commit_shared_data_initialization(
        star::CXLMemory::cxl_transport_root_index, rings);

    // Original Tigon Coordinator::initCXLEBR: place CXL_EBR in CXL (MISC→HWCC),
    // publish via cxl_global_ebr_meta_root_index for peer attach.
    ebr = static_cast<star::CXL_EBR *>(star::cxl_memory.cxlalloc_malloc_wrapper(
        sizeof(star::CXL_EBR), star::CXLMemory::MISC_ALLOCATION));
    new (ebr) star::CXL_EBR(config.vm_count,
                            config.foreground_worker_count_per_vm);
    star::CXLMemory::commit_shared_data_initialization(
        star::CXLMemory::cxl_global_ebr_meta_root_index, ebr);
    pool->allocator().FinalizeStaticHwccLayout();
    pool->allocator().PublishStaticHwccLayout();
  } else {
    void *root = nullptr;
    star::CXLMemory::wait_and_retrieve_cxl_shared_data(
        star::CXLMemory::cxl_transport_root_index, &root);
    rings = static_cast<star::MPSCRingBuffer *>(root);
    void *ebr_root = nullptr;
    star::CXLMemory::wait_and_retrieve_cxl_shared_data(
        star::CXLMemory::cxl_global_ebr_meta_root_index, &ebr_root);
    ebr = static_cast<star::CXL_EBR *>(ebr_root);
  }
  if (!pool->allocator().IsHwccAddress(ebr))
    throw std::runtime_error("tigonkv: CXL_EBR must reside in HWCC");
  // Process-local allocator binding (VA not portable across VMs).
  star::CXL_EBR::bind_dual_region_allocator(&pool->allocator());
  star::global_ebr_meta = ebr;
  scc = std::make_unique<star::TwoPLPashaSCCWriteThrough>();
  star::scc_manager = scc.get();
  engine = std::unique_ptr<KVEngine>(new KVEngine(config, std::move(pool), ebr,
                                                   std::move(scc)));
  // Root/sentinel construction is not a foreground operation, but it must
  // advance a concrete worker-owned TID slot rather than an unbounded TLS
  // high-water mark.  Worker 0 owns this bootstrap slot after BindWorker.
  ScopedKvEbrMetaBinding bootstrap_ebr(
      ebr, &engine->worker_runtime_[0].ebr_meta);
  ScopedKvWorkerTidBinding bootstrap_tid(&engine->worker_runtime_[0].max_tid);
  engine->affinity_cpus_ = std::move(affinity_cpus);
  engine->rings_ = rings;
  const uint64_t maximum_piece_bytes = std::max({
      static_cast<uint64_t>(star::TwoPLPashaMessageFactory::
          data_migration_request_size(config.fixed_key_size)),
      static_cast<uint64_t>(star::TwoPLPashaMessageFactory::
          scan_migration_request_size(config.fixed_key_size)),
      static_cast<uint64_t>(star::TwoPLPashaMessageFactory::
          remote_insert_request_size(config.fixed_key_size,
                                     config.fixed_value_size)),
      static_cast<uint64_t>(star::TwoPLPashaMessageFactory::
          remote_delete_request_size(config.fixed_key_size)),
      static_cast<uint64_t>(star::TwoPLPashaMessageFactory::
          bool_key_offset_response_size()),
      static_cast<uint64_t>(star::TwoPLPashaMessageFactory::
          status_key_offset_response_size())});
  const uint64_t maximum_message_bytes =
      star::Message::get_prefix_size() + maximum_piece_bytes;
  if (maximum_message_bytes > star::BufferedReader::BUFFER_SIZE)
    throw std::runtime_error("tigonkv: maximum Message exceeds BufferedReader buffer");
  for (uint32_t node = 0; node < config.vm_count; ++node) {
    if (rings[node].get_entry_size() < maximum_message_bytes) {
      std::ostringstream detail;
      detail << "tigonkv: transport ring entry is smaller than maximum Message"
             << " (node=" << node << " entry=" << rings[node].get_entry_size()
             << " required=" << maximum_message_bytes << ")";
      throw std::runtime_error(detail.str());
    }
  }

  // A joining VM attaches the static HWCC layout with reset=false, but still
  // has to materialize its own SWCC arena/root while the first startup is
  // Initializing.  Once Ready is published, the same path is a pure attach.
  bool initialize_owner = reset;
  if (!reset) {
    const auto &layout = engine->pool_->allocator().layout();
    initialize_owner = mem_access::HwccAtomicLoad(
        layout.state, std::memory_order_acquire) ==
        static_cast<uint32_t>(LayoutState::kInitializing);
  }
  if (initialize_owner) {
    // Phase one: a VM constructs only the roots that it owns.  In particular,
    // reset VM0 never writes another VM's owner-private arena merely because
    // it happens to publish the static HWCC layout.
    engine->pool_->allocator().InitializeOwnerPrivateArenas(config.node_id);
    std::vector<std::unique_ptr<KVPartition>> initializer_partitions;
    for (uint32_t partition = 0; partition < config.partition_count; ++partition) {
      if (engine->OwnerForPartition(partition) != config.node_id) continue;
      const auto &directory = engine->pool_->allocator().layout().partitions[partition];
      if (mem_access::HwccAtomicLoad(directory.shared_root,
                                     std::memory_order_acquire) != kNullOffset)
        throw std::runtime_error(
            "tigonkv: owner initialization found an already-published shared root");
      initializer_partitions.emplace_back(std::make_unique<KVPartition>(
          engine->pool_->allocator(), *engine->ebr_, partition,
          engine->OwnerForPartition(partition), false, true));
    }
    engine->pool_->allocator().PublishOwnerInitialized(config.node_id);
    if (config.node_id == 0)
      engine->pool_->allocator().WaitForOwnersAndPublishReady();
    else
      engine->pool_->allocator().WaitUntilReady();
  } else {
    // Reattach after Ready observes the immutable layout only.  Re-running
    // owner initialization here would publish a second root.
    engine->pool_->allocator().WaitUntilReady();
    // The dynamic arena controls persist in this owner's private SWCC, but
    // their RegionAllocator handles are process-local.  Reconstruct only the
    // attaching owner's handles; never initialize or bind another owner's
    // private allocator from this VM.
    engine->pool_->allocator().BindOwnerPrivateAllocators(config.node_id);
  }

  // Regular non-owning handles are reconstructed only after Ready made every
  // root visible, for both reset and attach.
  for (uint32_t partition = 0; partition < config.partition_count; ++partition) {
    const auto &directory = engine->pool_->allocator().layout().partitions[partition];
    if (mem_access::HwccAtomicLoad(directory.shared_root,
                                   std::memory_order_acquire) == kNullOffset)
      throw std::runtime_error("tigonkv: layout ready with missing shared root");
    const bool materialize_private =
        engine->OwnerForPartition(partition) == config.node_id;
    engine->partitions_.emplace_back(std::make_unique<KVPartition>(
        engine->pool_->allocator(), *engine->ebr_, partition,
        engine->OwnerForPartition(partition), true, materialize_private));
    MaybeThrowOpenFailpoint("during-partition-init");
  }
  if (star::CXLMemory::bound_owner_shard() != config.node_id)
    throw std::runtime_error(
        "tigonkv: process allocator owner rebound after partition construct");
  for (uint32_t partition = 0; partition < engine->partitions_.size();
       ++partition) {
    if (engine->partitions_[partition]->partition_id() != partition)
      throw std::runtime_error(
          "tigonkv: partitions_ vector index must equal partition_id");
  }
  static_assert(kSingleTableId == 0, "single-table contract");
  std::vector<KVPartition *> partition_ptrs;
  partition_ptrs.reserve(engine->partitions_.size());
  for (auto &partition : engine->partitions_) partition_ptrs.push_back(partition.get());
  // §11.10: hw_cc_budget_mb is the full configured Clock allowance (typically
  // equal to hwcc.size_mb). Open measures static HWCC domains and clamps the
  // per-owner dynamic pool to what remains; operators do not under-configure
  // the JSONC for layout/allocator/transport/EBR headroom.
  const uint64_t budget_bytes = config.hw_cc_budget_mb * 1024ULL * 1024ULL;
  const uint64_t ebr_reserve = star::CXL_EBR::max_ebr_retiring_memory;
  if (budget_bytes <= ebr_reserve)
    throw std::runtime_error(
        "tigonkv: hw_cc_budget_mb must exceed CXL_EBR::max_ebr_retiring_memory");
  const uint64_t configured_clock_total = budget_bytes - ebr_reserve;
  uint64_t static_hwcc = 0;
  for (size_t domain :
       {static_cast<size_t>(AllocationDomain::kHwccLayout),
        static_cast<size_t>(AllocationDomain::kHwccAllocatorMetadata),
        static_cast<size_t>(AllocationDomain::kTransport),
        static_cast<size_t>(AllocationDomain::kHwccEbr)}) {
    static_hwcc += engine->pool_->allocator().ReadStaticDomainUsedBytes(
        static_cast<AllocationDomain>(domain));
  }
  const uint64_t physical_hwcc = config.hwcc_size_mb * 1024ULL * 1024ULL;
  const uint64_t remaining_after_static =
      physical_hwcc > static_hwcc ? physical_hwcc - static_hwcc : 0;
  if (remaining_after_static < static_cast<uint64_t>(config.vm_count)) {
    std::ostringstream detail;
    detail << "tigonkv: no HWCC capacity left for Clock dynamic budget after "
              "static domains (static="
           << static_hwcc << " physical=" << physical_hwcc
           << " remaining=" << remaining_after_static
           << " vm_count=" << config.vm_count << ") (§11.10)";
    throw std::runtime_error(detail.str());
  }
  const uint64_t effective_clock_total =
      std::min(configured_clock_total, remaining_after_static);
  const uint64_t hw_budget = effective_clock_total / config.vm_count;
  if (hw_budget == 0)
    throw std::runtime_error(
        "tigonkv: per-owner Clock dynamic HWCC budget is zero after clamping "
        "to remaining-after-static (§11.10)");
  engine->owner_migration_dynamic_budget_bytes_ = hw_budget;
  KvMigrationRuntime::Instance().Install(
      partition_ptrs, config.fixed_key_size, config.fixed_value_size,
      config.node_id, config.partition_count, hw_budget);
  // Clock tracker nodes persist in owner-private SWCC; attach
  // does not rebuild a process-heap tracker (§11.14).
  MaybeThrowOpenFailpoint("during-worker-start");
  MaybeThrowOpenFailpoint("before-demuxer");
  engine->StartInboundDemuxer();
  MaybeThrowOpenFailpoint("after-demuxer");
  return engine;
  } catch (...) {
    rollback();
    throw;
  }
}

uint32_t KVEngine::PartitionForKey(std::string_view key) const {
  return RouteForKey(key).partition_id;
}

uint32_t KVEngine::OwnerForKey(std::string_view key) const {
  return RouteForKey(key).owner;
}

uint32_t KVEngine::OwnerForPartition(uint32_t partition) const {
  return partition % config_.vm_count;
}

KVEngine::KeyRoute KVEngine::RouteForKey(std::string_view key) const {
  KeyRoute route;
  route.partition_id = config_.PartitionForKey(key);
  route.owner = route.partition_id % config_.vm_count;
  route.owned_by_this_node = (route.owner == config_.node_id);
  if (route.partition_id < partitions_.size())
    route.partition = partitions_[route.partition_id].get();
  return route;
}

KVPartition *KVEngine::OwnedPartition(std::string_view key) const {
  const KeyRoute route = RouteForKey(key);
  return route.owned_by_this_node ? route.partition : nullptr;
}

KVPartition *KVEngine::VisiblePartition(std::string_view key) const {
  return RouteForKey(key).partition;
}

Status KVEngine::Put(std::string_view key, std::string_view value) {
  RequireBoundWorker();
  PollTransport();
  if (IsInternalMaxSentinel(key, config_.fixed_key_size))
    return Status::Error(StatusCode::kInvalidArgument,
                         "internal max sentinel is reserved");
  const KeyRoute route = RouteForKey(key);
  if (!route.owned_by_this_node) {
    if (route.partition == nullptr)
      return Status::Error(StatusCode::kCorruption, "missing visible partition");
    const auto write_shared = [&](bool record_clock_access = true) {
      EbrOperationScope ebr_scope(ebr_);
      const SharedAccessState state =
          route.partition->PutShared(key, config_.node_id, value,
                                     record_clock_access);
      if (state == SharedAccessState::kDone) {
        ++CurrentWorkerRuntime().shared_puts;
        ++CurrentWorkerRuntime().shared_swcc_flushes;
        return Status::Ok();
      }
      return Status::Error(state == SharedAccessState::kRetry
                               ? StatusCode::kBusy
                               : StatusCode::kNotFound,
                           "shared row unavailable");
    };
    const Status first = write_shared();
    if (first.code != StatusCode::kNotFound) return first;
    // Existing remote rows follow the original TwoPLPasha path: owner moves
    // the row in, then the requester performs the shared write itself.
    const Status migrated =
        Forward(star::TwoPLPashaMessage::DATA_MIGRATION_REQUEST, key, {}, route.partition_id, route.owner);
    if (migrated.ok()) return write_shared(/*record_clock_access=*/false);
    if (migrated.code != StatusCode::kNotFound) return migrated;
    // A true create uses the original REMOTE_INSERT ownership split: the owner
    // inserts an invalid placeholder and copies it into CXL with a requester
    // ref; after the ack the requester publishes only valid (no second data
    // write through the shared payload).
    const Status inserted =
        Forward(star::TwoPLPashaMessage::REMOTE_INSERT_REQUEST, key, value, route.partition_id, route.owner);
    if (!inserted.ok()) return inserted;
    if (!route.partition->PublishRemotePlaceholder(key, config_.node_id))
      return Status::Error(StatusCode::kBusy,
                           "remote insert placeholder publication failed");
    return Status::Ok();
  }
  try {
    EbrOperationScope ebr_scope(ebr_);
    switch (route.partition->PutPrivate(key, value)) {
      case star::RowOutcome::kDone:
        return Status::Ok();
      case star::RowOutcome::kBusy:
        return Status::Error(StatusCode::kBusy, "private put contention");
      case star::RowOutcome::kMissing:
        ProtocolFatal(config_.node_id, "private put", "unexpected missing outcome");
    }
  } catch (const std::bad_alloc &) {
    return Status::Error(StatusCode::kOutOfMemory, "private arena exhausted");
  } catch (const std::invalid_argument &e) {
    return Status::Error(StatusCode::kInvalidArgument, e.what());
  } catch (const std::runtime_error &e) {
    ProtocolFatal(config_.node_id, "private put", e.what());
  } catch (const std::exception &e) {
    ProtocolFatal(config_.node_id, "private put", e.what());
  }
}

GetResult KVEngine::Get(std::string_view key) {
  RequireBoundWorker();
  PollTransport();
  if (IsInternalMaxSentinel(key, config_.fixed_key_size))
    return {Status::Error(StatusCode::kInvalidArgument,
                          "internal max sentinel is reserved"), {}};
  const KeyRoute route = RouteForKey(key);
  if (!route.owned_by_this_node) {
    auto *visible = route.partition;
    if (visible == nullptr)
      return {Status::Error(StatusCode::kCorruption, "missing visible partition"), {}};
    std::string shared;
    const auto get_shared = [&](bool record_clock_access = true) {
      EbrOperationScope ebr_scope(ebr_);
      return visible->GetShared(key, config_.node_id, &shared,
                               record_clock_access);
    };
    const SharedAccessState first = get_shared();
    if (first == SharedAccessState::kDone) {
      ++CurrentWorkerRuntime().shared_gets;
      return {Status::Ok(), std::move(shared)};
    }
    if (first == SharedAccessState::kRetry)
      return {Status::Error(StatusCode::kBusy, "shared get contention"), {}};
    const Status migrated =
        Forward(star::TwoPLPashaMessage::DATA_MIGRATION_REQUEST, key, {}, route.partition_id, route.owner);
    if (!migrated.ok()) return {migrated, {}};
    const SharedAccessState second =
        get_shared(/*record_clock_access=*/false);
    if (second == SharedAccessState::kDone) {
      ++CurrentWorkerRuntime().shared_gets;
      return {Status::Ok(), std::move(shared)};
    }
    return {Status::Error(second == SharedAccessState::kRetry
                              ? StatusCode::kBusy
                              : StatusCode::kCorruption,
                          "owner acknowledged migration without readable shared row"), {}};
  }
  try {
    EbrOperationScope ebr_scope(ebr_);
    std::string value;
    switch (route.partition->GetPrivate(key, &value)) {
      case star::RowOutcome::kDone:
        return {Status::Ok(), std::move(value)};
      case star::RowOutcome::kMissing:
        return {Status::Error(StatusCode::kNotFound, "key not found"), {}};
      case star::RowOutcome::kBusy:
        return {Status::Error(StatusCode::kBusy, "private get contention"), {}};
    }
  } catch (const std::runtime_error &e) {
    ProtocolFatal(config_.node_id, "private get", e.what());
  } catch (const std::exception &e) {
    ProtocolFatal(config_.node_id, "private get", e.what());
  }
}

Status KVEngine::Delete(std::string_view key) {
  RequireBoundWorker();
  PollTransport();
  if (IsInternalMaxSentinel(key, config_.fixed_key_size))
    return Status::Error(StatusCode::kInvalidArgument,
                         "internal max sentinel is reserved");
  const KeyRoute route = RouteForKey(key);
  if (!route.owned_by_this_node) {
    star::TwoPLPashaMetadataShared *locked_row = nullptr;
    const auto prepare_delete = [&](bool record_clock_access = true) {
      EbrOperationScope ebr_scope(ebr_);
      return route.partition->PrepareRemoteDelete(key, config_.node_id,
                                                  &locked_row,
                                                  record_clock_access);
    };
    SharedAccessState prepared = prepare_delete();
    if (prepared == SharedAccessState::kMissing) {
      const Status migrated =
          Forward(star::TwoPLPashaMessage::DATA_MIGRATION_REQUEST, key, {}, route.partition_id, route.owner);
      if (!migrated.ok()) return migrated;
      prepared = prepare_delete(/*record_clock_access=*/false);
    }
    if (prepared == SharedAccessState::kMissing)
      return Status::Error(StatusCode::kNotFound, "key not found");
    if (prepared != SharedAccessState::kDone)
      return Status::Error(StatusCode::kBusy, "remote delete shared row busy");
    // The row is now write_locked with valid cleared (the critical
    // intermediate state).  Until the owner ack commits the delete or the
    // rollback restores the row, no latency busy-wait may happen on this
    // thread: the cooperative wait inside Forward suspends the foreground
    // budget, and every transport poll defers its settlement into the same
    // deferred segment, so everything settles exactly once at the outermost
    // scope exit, after the lock and SCC guards are released.  Cooperative
    // transport processing itself is unchanged.  The RAII guard releases the
    // defer mode on every return, including exceptions from Forward.
    mem_access::DeferTransportSettlement defer_settlement;
    Status deleted;
    try {
      deleted = Forward(star::TwoPLPashaMessage::REMOTE_DELETE_REQUEST, key, {}, route.partition_id,
                        route.owner);
    } catch (...) {
      // Exception safety: restore the row so the lock is never stranded, then
      // unwind; the deferred budgets settle at the outermost scope exit.
      try {
        route.partition->AbortRemoteDelete(locked_row, config_.node_id);
      } catch (...) {
      }
      throw;
    }
    // A successful owner callback consumes the requester write/ref pin with
    // the retired row.  On an unsuccessful ack it is still live and must be
    // restored before the facade retries.
    if (!deleted.ok()) {
      route.partition->AbortRemoteDelete(locked_row, config_.node_id);
    }
    return deleted;
  }
  try {
    EbrOperationScope ebr_scope(ebr_);
    switch (route.partition->DeletePrivate(key)) {
      case star::RowOutcome::kDone:
        return Status::Ok();
      case star::RowOutcome::kMissing:
        return Status::Error(StatusCode::kNotFound, "key not found");
      case star::RowOutcome::kBusy:
        return Status::Error(StatusCode::kBusy, "private delete contention");
    }
  } catch (const std::runtime_error &e) {
    ProtocolFatal(config_.node_id, "private delete", e.what());
  } catch (const std::exception &e) {
    ProtocolFatal(config_.node_id, "private delete", e.what());
  }
}

ScanResult KVEngine::Scan(std::string_view start_key, std::string_view end_key,
                          uint64_t limit) {
  RequireBoundWorker();
  PollTransport();
  if (IsInternalMaxSentinel(start_key, config_.fixed_key_size))
    return {Status::Error(StatusCode::kInvalidArgument,
                          "internal max sentinel is reserved"), {}};
  RuntimeStats &runtime = CurrentWorkerRuntime();
  if (!end_key.empty() && !LessFixed(start_key, end_key, config_.fixed_key_size))
    return {Status::Ok(), {}};

  auto scan_remote = [&](uint32_t partition_id, std::string_view min_key,
                         std::string_view inclusive_max, uint64_t scan_limit,
                         ScanResult *result) -> Status {
    auto *partition = partitions_[partition_id].get();
    // First probe: master adjacency only. K1 on a cold first probe would accept
    // a distant migrated island, then move_in(min, limit) only fills the first
    // limit+1 private keys and never reaches that island's trailing hole.
    KVPartition::SharedScanResult probe;
    {
      EbrOperationScope ebr_scope(ebr_);
      probe = partition->ScanSharedPartition(
          config_.node_id, min_key, scan_limit, inclusive_max,
          /*allow_lower_bound_left_boundary=*/false);
    }
    ++runtime.scan_partition_probes;
    PollTransport();
    if (!probe.status.ok()) return probe.status;
    if (probe.migration_required) {
      // §3.9.1: while the owner already has a range migrate in flight, Busy at
      // the facade without another Forward. Do not skip probes via TLS (Rel25k
      // stall) and do not Busy inside ScanShared (Rel20k livelock).
      if (partition->ScanRangeMigrateInFlight())
        return Status::Error(StatusCode::kBusy,
                             "owner scan range migrate in flight");
      const Status migrated = Forward(
          star::TwoPLPashaMessage::DATA_MIGRATION_REQUEST_FOR_SCAN, min_key, {}, partition_id,
          OwnerForPartition(partition_id), inclusive_max, scan_limit);
      ++runtime.scan_migrate_rpcs;
      if (!migrated.ok()) return migrated;
      // Prescribed continuation: same min/max/limit, with K1 so a post-move-in
      // lower-bound row without migrated predecessor can pass. A second
      // incomplete probe remains Busy for the facade boundary.
      {
        EbrOperationScope ebr_scope(ebr_);
        probe = partition->ScanSharedPartition(
            config_.node_id, min_key, scan_limit, inclusive_max,
            /*allow_lower_bound_left_boundary=*/true);
      }
      ++runtime.scan_partition_probes;
      PollTransport();
      if (!probe.status.ok()) return probe.status;
      if (probe.migration_required)
        return Status::Error(StatusCode::kBusy,
                             "CXL scan range remains incomplete");
    }
    if (!probe.scan_success)
      return Status::Error(StatusCode::kCorruption, "CXL probe incomplete");
    for (auto &item : probe.items)
      result->items.push_back({std::move(item.first), std::move(item.second)});
    return Status::Ok();
  };

  ScanResult result{Status::Ok(), {}};
  uint32_t partition_id = config_.PartitionForKey(start_key);
  for (; partition_id < config_.partition_count; ++partition_id) {
    const auto &range = config_.partition_ranges[partition_id];
    const std::string_view min_key =
        partition_id == config_.PartitionForKey(start_key)
            ? start_key : std::string_view(range.lower_key);
    if (!end_key.empty() && !LessFixed(min_key, end_key, config_.fixed_key_size))
      break;
    std::string_view exclusive_max = end_key;
    if (!range.upper_key.empty() &&
        (exclusive_max.empty() ||
         LessFixed(range.upper_key, exclusive_max, config_.fixed_key_size)))
      exclusive_max = range.upper_key;
    FixedKey inclusive{};
    if (!exclusive_max.empty() &&
        !ExclusiveToInclusive(exclusive_max, config_.fixed_key_size, &inclusive))
      break;
    if (exclusive_max.empty())
      inclusive = FixedKey::InternalMax(config_.fixed_key_size);
    if (limit != 0 && result.items.size() >= limit) break;
    const uint64_t remaining =
        limit == 0 ? 0 : static_cast<uint64_t>(limit - result.items.size());
    const std::string inclusive_max(inclusive.bytes, config_.fixed_key_size);
    Status status;
    if (OwnerForPartition(partition_id) == config_.node_id) {
      std::vector<std::pair<std::string, std::string>> items;
      bool ok = false;
      {
        EbrOperationScope ebr_scope(ebr_);
        ok = partitions_[partition_id]->ScanLocalPartition(
            min_key, remaining, &items, inclusive_max);
      }
      ++runtime.scan_partition_probes;
      PollTransport();
      status = ok ? Status::Ok()
                  : Status::Error(StatusCode::kBusy,
                                  "owner scan contention");
      for (auto &item : items)
        result.items.push_back({std::move(item.first), std::move(item.second)});
    } else {
      status = scan_remote(partition_id, min_key, inclusive_max, remaining,
                           &result);
    }
    if (!status.ok()) return {status, {}};
    if (!end_key.empty() && exclusive_max == end_key) break;
  }
  return result;
}

Status KVEngine::PreparePartitionSharedScan(
    uint32_t partition_id, std::string_view start_key,
    std::string_view inclusive_max, uint64_t output_limit,
    bool retain_inflight_on_success) {
  RequireBoundWorker();
  EbrOperationScope ebr_scope(ebr_);
  if (partition_id >= partitions_.size() ||
      partition_id >= config_.partition_count)
    return Status::Error(StatusCode::kInvalidArgument,
                         "scan migrate partition_id out of range");
  if (OwnerForPartition(partition_id) != config_.node_id)
    return Status::Error(StatusCode::kOwnerViolation,
                         "scan migrate routed to non-owner");
  // The scan request has no KV-specific page/exhaustion protocol.  Reuse the
  // original handler's ITable callback: it moves [min,max] and the one right
  // boundary, leaves requester refs at zero, and relies on the requester to
  // re-run the same CXL scan after the original bool response.
  auto *table = KvMigrationRuntime::Instance().TableFor(partition_id);
  if (table == nullptr)
    return Status::Error(StatusCode::kCorruption,
                         "scan migration table adapter is unavailable");
  auto *partition = table->partition();
  if (partition == nullptr)
    return Status::Error(StatusCode::kCorruption,
                         "scan migration partition handle is unavailable");
  // One in-flight range migrate per owner partition (HWCC; §3.9.1). Covers
  // move_in; optional retain through flush+OnDemand move_out. Do not Busy
  // remote ScanShared.
  if (!partition->TryBeginScanRangeMigrate())
    return Status::Error(StatusCode::kBusy,
                         "scan range migrate already in progress");
  struct ScanRangeMigrateGuard {
    KVPartition *partition = nullptr;
    bool release = true;
    ~ScanRangeMigrateGuard() {
      if (release && partition != nullptr) partition->EndScanRangeMigrate();
    }
  } guard{partition, true};
  const FixedKey min = FixedKey::From(start_key, config_.fixed_key_size);
  const FixedKey max = FixedKey::From(inclusive_max, config_.fixed_key_size);
  try {
    star::TwoPLPashaMessageHandler::move_in_scan_range(
        *table, min.bytes, max.bytes, output_limit);
  } catch (const std::bad_alloc &) {
    return Status::Error(StatusCode::kOutOfMemory,
                         "scan range move-in allocation failed");
  }
  if (retain_inflight_on_success) guard.release = false;
  return Status::Ok();
}

CasResult KVEngine::CompareExchange(std::string_view key,
                                    std::string_view expected,
                                    std::string_view desired) {
  RequireBoundWorker();
  PollTransport();
  if (IsInternalMaxSentinel(key, config_.fixed_key_size))
    return {Status::Error(StatusCode::kInvalidArgument,
                          "internal max sentinel is reserved"), false};
  const KeyRoute route = RouteForKey(key);
  if (!route.owned_by_this_node) {
    auto *visible = route.partition;
    if (visible == nullptr)
      return {Status::Error(StatusCode::kCorruption, "missing visible partition"), false};
    bool exchanged = false;
    auto compare_shared = [&](bool record_clock_access = true) {
      EbrOperationScope ebr_scope(ebr_);
      return visible->CompareExchangeShared(key, config_.node_id, expected,
                                            desired, &exchanged,
                                            record_clock_access);
    };
    SharedAccessState state = compare_shared();
    if (state == SharedAccessState::kDone) {
      if (exchanged) {
        ++CurrentWorkerRuntime().shared_puts;
        ++CurrentWorkerRuntime().shared_swcc_flushes;
      }
      return {exchanged ? Status::Ok()
                        : Status::Error(StatusCode::kCompareFailed,
                                        "expected value differs"), exchanged};
    }
    if (state == SharedAccessState::kRetry)
      return {Status::Error(StatusCode::kBusy, "shared cas contention"), false};
    // Match original remote write acquisition: owner only moves an existing
    // row in; requester then takes the shared write/ref path itself.
    const Status migrated =
        Forward(star::TwoPLPashaMessage::DATA_MIGRATION_REQUEST, key, {}, route.partition_id, route.owner);
    if (migrated.ok()) {
      state = compare_shared(/*record_clock_access=*/false);
      if (state == SharedAccessState::kDone) {
        if (exchanged) {
          ++CurrentWorkerRuntime().shared_puts;
          ++CurrentWorkerRuntime().shared_swcc_flushes;
        }
        return {exchanged ? Status::Ok()
                          : Status::Error(StatusCode::kCompareFailed,
                                          "expected value differs"),
                exchanged};
      }
      return {Status::Error(state == SharedAccessState::kRetry
                                ? StatusCode::kBusy
                                : StatusCode::kCorruption,
                            "migrated shared CAS unavailable"),
              false};
    }
    if (migrated.code != StatusCode::kNotFound || !expected.empty())
      return {migrated, false};
    // The established empty-expected create sentinel shares REMOTE_INSERT;
    // it does not revive a separate owner-side CAS protocol.
    const Status inserted =
        Forward(star::TwoPLPashaMessage::REMOTE_INSERT_REQUEST, key, desired, route.partition_id, route.owner);
    if (!inserted.ok()) return {inserted, false};
    if (!visible->PublishRemotePlaceholder(key, config_.node_id))
      return {Status::Error(StatusCode::kBusy,
                            "remote CAS placeholder publication failed"),
              false};
    return {Status::Ok(), true};
  }
  try {
    EbrOperationScope ebr_scope(ebr_);
    bool exchanged = false;
    switch (route.partition->CompareExchangePrivate(key, expected, desired,
                                                     &exchanged)) {
      case star::RowOutcome::kDone:
        return {exchanged ? Status::Ok()
                          : Status::Error(StatusCode::kCompareFailed,
                                          "expected value differs"),
                exchanged};
      case star::RowOutcome::kMissing:
        return {Status::Error(StatusCode::kNotFound, "key not found"), false};
      case star::RowOutcome::kBusy:
        return {Status::Error(StatusCode::kBusy, "private cas contention"), false};
    }
  } catch (const std::bad_alloc &) {
    return {Status::Error(StatusCode::kOutOfMemory, "allocator exhausted"), false};
  } catch (const std::invalid_argument &e) {
    return {Status::Error(StatusCode::kInvalidArgument, e.what()), false};
  } catch (const std::runtime_error &e) {
    ProtocolFatal(config_.node_id, "private cas", e.what());
  } catch (const std::exception &e) {
    ProtocolFatal(config_.node_id, "private cas", e.what());
  }
}

IncrementResult KVEngine::Increment(std::string_view key, int64_t delta) {
  RequireBoundWorker();
  PollTransport();
  if (IsInternalMaxSentinel(key, config_.fixed_key_size))
    return {Status::Error(StatusCode::kInvalidArgument,
                          "internal max sentinel is reserved"), 0};
  const KeyRoute route = RouteForKey(key);
  if (!route.owned_by_this_node) {
    auto *visible = route.partition;
    if (visible == nullptr)
      return {Status::Error(StatusCode::kCorruption, "missing visible partition"), 0};
    int64_t shared = 0;
    auto increment_shared = [&](bool record_clock_access = true) {
      EbrOperationScope ebr_scope(ebr_);
      return visible->IncrementShared(key, config_.node_id, delta, &shared,
                                      record_clock_access);
    };
    SharedAccessState state = increment_shared();
    if (state == SharedAccessState::kDone) {
      ++CurrentWorkerRuntime().shared_puts;
      ++CurrentWorkerRuntime().shared_swcc_flushes;
      return {Status::Ok(), shared};
    }
    if (state == SharedAccessState::kRetry)
      return {Status::Error(StatusCode::kBusy, "shared increment contention"), 0};
    const Status migrated =
        Forward(star::TwoPLPashaMessage::DATA_MIGRATION_REQUEST, key, {}, route.partition_id, route.owner);
    if (migrated.ok()) {
      state = increment_shared(/*record_clock_access=*/false);
      if (state == SharedAccessState::kDone) {
        ++CurrentWorkerRuntime().shared_puts;
        ++CurrentWorkerRuntime().shared_swcc_flushes;
        return {Status::Ok(), shared};
      }
      return {Status::Error(state == SharedAccessState::kRetry
                                ? StatusCode::kBusy
                                : StatusCode::kCorruption,
                            "migrated shared increment unavailable"),
              0};
    }
    if (migrated.code != StatusCode::kNotFound) return {migrated, 0};
    std::string initial;
    if (!EncodeCanonicalFixedDecimal(delta, config_.fixed_value_size, &initial))
      return {Status::Error(StatusCode::kInvalidArgument,
                            "increment value exceeds fixed value size"),
              0};
    const Status inserted =
        Forward(star::TwoPLPashaMessage::REMOTE_INSERT_REQUEST, key, initial, route.partition_id, route.owner);
    if (!inserted.ok()) return {inserted, 0};
    if (!visible->PublishRemotePlaceholder(key, config_.node_id))
      return {Status::Error(StatusCode::kBusy,
                            "remote increment placeholder publication failed"),
              0};
    return {Status::Ok(), delta};
  }
  try {
    EbrOperationScope ebr_scope(ebr_);
    int64_t value = 0;
    switch (route.partition->IncrementPrivate(key, delta, &value)) {
      case star::RowOutcome::kDone:
        return {Status::Ok(), value};
      case star::RowOutcome::kMissing:
        return {Status::Error(StatusCode::kNotFound, "key not found"), 0};
      case star::RowOutcome::kBusy:
        return {Status::Error(StatusCode::kBusy, "private increment contention"), 0};
    }
  } catch (const std::invalid_argument &e) {
    return {Status::Error(StatusCode::kInvalidArgument, e.what()), 0};
  } catch (const std::runtime_error &e) {
    ProtocolFatal(config_.node_id, "private increment", e.what());
  } catch (const std::exception &e) {
    ProtocolFatal(config_.node_id, "private increment", e.what());
  }
}

MemoryStats KVEngine::Memory() const {
  // Snapshot/maintenance query that reads HWCC layout counters and
  // owner-private SWCC control words; runs inside its own background scope.
  mem_access::LatencyScope latency_scope(latency_sim::ExecutionClass::kBackground);
  const auto &regions = pool_->allocator();
  MemoryStats stats;
  stats.allocator_mode = "dual_region";
  stats.physical_region_split = true;
  stats.total_pool_capacity_bytes = pool_->bytes();
  stats.logical_hwcc_capacity_bytes = config_.hwcc_size_mb * 1024ULL * 1024ULL;
  stats.logical_swcc_capacity_bytes = config_.swcc_size_mb * 1024ULL * 1024ULL;
  // Dynamic allocator counters are owner-private SWCC.  A per-VM snapshot
  // must never read another VM's control words; the runner aggregates these
  // components and reports the globally static HWCC prefix once from VM0.
  if (config_.node_id == 0) {
    for (size_t domain :
         {static_cast<size_t>(AllocationDomain::kHwccEbr),
          static_cast<size_t>(AllocationDomain::kHwccLayout),
          static_cast<size_t>(AllocationDomain::kTransport),
          static_cast<size_t>(AllocationDomain::kHwccAllocatorMetadata)})
      stats.logical_hwcc_used_bytes += regions.ReadStaticDomainUsedBytes(
          static_cast<AllocationDomain>(domain));
  }
  stats.logical_hwcc_used_bytes +=
      regions.DynamicHwccUsedBytes(config_.node_id);
  stats.physical_hwcc_used_bytes = stats.logical_hwcc_used_bytes;
  stats.owner_private_swcc_used_bytes =
      regions.OwnerPrivateUsedBytes(config_.node_id);
  stats.shared_payload_swcc_used_bytes =
      regions.SharedPayloadUsedBytes(config_.node_id);
  stats.allocator_hwcc_metadata_bytes = config_.node_id == 0
      ? regions.ReadStaticDomainUsedBytes(
            AllocationDomain::kHwccAllocatorMetadata)
      : 0;
  const uint64_t arena_header_bytes =
      (sizeof(OwnerPrivateArenaHeader) + RegionAllocator::kAlignment - 1) &
      ~(RegionAllocator::kAlignment - 1);
  uint64_t local_arena_headers = 0;
  for (uint32_t partition = config_.node_id;
       partition < config_.partition_count; partition += config_.vm_count)
    local_arena_headers += arena_header_bytes;
  stats.allocator_swcc_metadata_bytes = local_arena_headers;
  if (config_.node_id == 0)
    stats.allocator_swcc_metadata_bytes += regions.ReadStaticDomainUsedBytes(
        AllocationDomain::kSwccAllocatorMetadata);
  stats.allocator_shared_overhead_bytes =
      stats.allocator_hwcc_metadata_bytes +
      stats.allocator_swcc_metadata_bytes;
  stats.physical_swcc_used_bytes =
      stats.owner_private_swcc_used_bytes +
      stats.shared_payload_swcc_used_bytes +
      stats.allocator_swcc_metadata_bytes;
  // Physical capacity vs Clock dynamic limit (§11.10). Clock links live in
  // owner-private SWCC after §11.14, so process-heap tracker DRAM is zero.
  stats.physical_hwcc_capacity_bytes = config_.hwcc_size_mb * 1024ULL * 1024ULL;
  // Open-time clamp (§11.10); do not recompute from raw config alone.
  stats.owner_migration_dynamic_budget_bytes =
      owner_migration_dynamic_budget_bytes_;
  stats.allocator_local_dram_bytes = 0;
  stats.rss_kb = CurrentRssKb();
  return stats;
}

RuntimeStats KVEngine::EngineRuntime() const {
  RuntimeStats stats;
  // Called after a foreground stage has quiesced. Worker slots are single
  // writer, so foreground operations never contend on a global statistic.
  for (const WorkerRuntime &worker : worker_runtime_)
    AddRuntimeStats(&stats, worker.stats);
  // The inbound demuxer has no foreground slot. Its receive bytes are the
  // only process-level statistic and are added exactly once here.
  stats.network_rx_bytes +=
      demux_network_rx_bytes_.load(std::memory_order_relaxed);
  return stats;
}

uint64_t KVEngine::NetworkTxBytes() const {
  return EngineRuntime().network_tx_bytes;
}

uint64_t KVEngine::NetworkRxBytes() const {
  return demux_network_rx_bytes_.load(std::memory_order_relaxed);
}

KVEngine::WorkerMailbox &KVEngine::CurrentMailbox() {
  RequireBoundWorker();
  const uint32_t worker = static_cast<uint32_t>(TlsForegroundWorkerId);
  return *worker_mailboxes_[worker];
}

void KVEngine::RequireBoundWorker() const {
  if (TlsForegroundWorkerId < 0 ||
      static_cast<uint32_t>(TlsForegroundWorkerId) >= worker_mailboxes_.size()) {
    TransportFatal(config_.node_id, "worker_binding",
                   "foreground worker is not bound");
  }
}

RuntimeStats &KVEngine::CurrentWorkerRuntime() {
  RequireBoundWorker();
  return worker_runtime_[static_cast<uint32_t>(TlsForegroundWorkerId)].stats;
}

star::Message &KVEngine::OutboundMessage(WorkerMailbox &mailbox,
                                         uint32_t destination) {
  if (destination >= mailbox.outbound.size())
    TransportFatal(config_.node_id, "outbound", "destination exceeds original Message buffers");
  auto &message = *mailbox.outbound[destination];
  message.clear_message_pieces();
  RequireBoundWorker();
  const uint32_t worker = static_cast<uint32_t>(TlsForegroundWorkerId);
  InitializeMessage(&message, config_.node_id, destination, worker);
  return message;
}

void KVEngine::SendTransportMessage(star::Message &message) {
  if (rings_ == nullptr || !message.check_size() || !message.check_deadbeef() ||
      message.get_source_node_id() != config_.node_id ||
      message.get_dest_node_id() >= config_.vm_count ||
      message.get_worker_id() >= config_.foreground_worker_count_per_vm ||
      message.get_message_count() != 1 ||
      message.get_message_length() > rings_[message.get_dest_node_id()].get_entry_size())
    TransportFatal(config_.node_id, "send_validate", "invalid original Message frame", &message);
  try {
    while (!rings_[message.get_dest_node_id()].enqueue(
        message.get_raw_ptr(), message.get_message_length()))
      std::this_thread::yield();
  } catch (const std::exception &error) {
    TransportFatal(config_.node_id, "send_ring", error.what(), &message);
  }
  CurrentWorkerRuntime().network_tx_bytes += message.get_message_length();
}

Status KVEngine::Forward(star::TwoPLPashaMessage type, std::string_view key,
                         std::string_view value, uint32_t partition_id,
                         uint32_t owner, std::string_view scan_max,
                         uint64_t scan_limit) {
  if (owner >= config_.vm_count || partition_id >= config_.partition_count)
    return Status::Error(StatusCode::kInvalidArgument, "invalid RPC route");
  if (partition_id >= partitions_.size() ||
      partitions_[partition_id] == nullptr ||
      OwnerForPartition(partition_id) != owner)
    TransportFatal(config_.node_id, "forward", "inconsistent RPC partition route");
  WorkerMailbox &mailbox = CurrentMailbox();
  if (mailbox.operation.expected_response_type != 0)
    TransportFatal(config_.node_id, "forward", "reentrant foreground RPC");
  const uint64_t sequence = mailbox.next_operation_sequence++;
  const FixedKey fixed_key = FixedKey::From(key, config_.fixed_key_size);
  FixedKey fixed_scan_max{};
  if (type == star::TwoPLPashaMessage::DATA_MIGRATION_REQUEST_FOR_SCAN)
    fixed_scan_max = FixedKey::From(scan_max, config_.fixed_key_size);
  std::string fixed_value;
  if (type == star::TwoPLPashaMessage::REMOTE_INSERT_REQUEST) {
    if (value.size() != config_.fixed_value_size)
      return Status::Error(StatusCode::kInvalidArgument,
                           "remote insert value must match fixed value size");
    fixed_value.assign(value);
  }
  star::Message &message = OutboundMessage(mailbox, owner);
  uint32_t expected_response = 0;
  switch (type) {
    case star::TwoPLPashaMessage::DATA_MIGRATION_REQUEST:
      star::TwoPLPashaMessageFactory::new_data_migration_message(
          message, kSingleTableId, partition_id, fixed_key.bytes,
          config_.fixed_key_size, sequence, 0);
      expected_response = static_cast<uint32_t>(star::TwoPLPashaMessage::DATA_MIGRATION_RESPONSE);
      break;
    case star::TwoPLPashaMessage::REMOTE_INSERT_REQUEST:
      star::TwoPLPashaMessageFactory::new_remote_insert_message(
          message, kSingleTableId, partition_id, fixed_key.bytes,
          config_.fixed_key_size, fixed_value.data(), config_.fixed_value_size,
          sequence, 0);
      expected_response = static_cast<uint32_t>(star::TwoPLPashaMessage::REMOTE_INSERT_RESPONSE);
      break;
    case star::TwoPLPashaMessage::REMOTE_DELETE_REQUEST:
      star::TwoPLPashaMessageFactory::new_remote_delete_message(
          message, kSingleTableId, partition_id, fixed_key.bytes,
          config_.fixed_key_size);
      expected_response = static_cast<uint32_t>(star::TwoPLPashaMessage::REMOTE_DELETE_RESPONSE);
      break;
    case star::TwoPLPashaMessage::DATA_MIGRATION_REQUEST_FOR_SCAN:
      star::TwoPLPashaMessageFactory::new_data_migration_message_for_scan(
          message, kSingleTableId, partition_id, fixed_key.bytes,
          fixed_scan_max.bytes, config_.fixed_key_size, scan_limit, sequence,
          0);
      expected_response = static_cast<uint32_t>(
          star::TwoPLPashaMessage::DATA_MIGRATION_RESPONSE_FOR_SCAN);
      break;
  }
  mailbox.operation = {expected_response, owner, partition_id, false,
                       Status::Error(StatusCode::kCorruption, "missing RPC response")};
  SendTransportMessage(message);
  message.clear_message_pieces();
  // The request phase ends immediately after the transport publication.  The
  // cooperative wait deliberately has no active latency scope: any peer
  // request it services is charged as that peer's independent request, and
  // the response starts a fresh local continuation below.
  // The suspension only deactivates the scope; its pending delay is settled at
  // the outermost scope exit after the caller's EBR and other guards are gone.
  // RAII restores the foreground scope on every return path.
  mem_access::ForegroundScopeSuspension suspend_foreground;
  const Status result = AwaitResponse(mailbox);
  return result;
}

Status KVEngine::RequestMigrate(std::string_view key) {
  const KeyRoute route = RouteForKey(key);
  return Forward(star::TwoPLPashaMessage::DATA_MIGRATION_REQUEST, key, {},
                 route.partition_id, route.owner);
}

Status KVEngine::AwaitResponse(WorkerMailbox &mailbox) {
  // Bounded cooperative wait: a lost owner (crash, partition, disconnect)
  // must surface as a timeout so the remote operation can roll back its
  // intermediate row state instead of busy-looping forever.
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(config_.transport_response_timeout_ms);
  while (!mailbox.operation.done) {
    PollTransport();
    if (mailbox.operation.done) break;
    if (std::chrono::steady_clock::now() >= deadline) {
      mailbox.operation = {};
      return Status::Error(StatusCode::kTimeout,
                           "transport response timeout");
    }
    std::this_thread::yield();
  }
  const Status result = mailbox.operation.result;
  mailbox.operation = {};
  return result;
}

void KVEngine::StartInboundDemuxer() {
  if (rings_ == nullptr)
    TransportFatal(config_.node_id, "demux", "missing inbound ring");
  inbound_demuxer_worker_id_ = config_.foreground_worker_count_per_vm;
  inbound_demuxer_stop_.store(false, std::memory_order_release);
  inbound_demuxer_ = std::thread([this] { InboundDemuxerLoop(); });
}

void KVEngine::StopInboundDemuxer() {
  inbound_demuxer_stop_.store(true, std::memory_order_release);
  if (inbound_demuxer_.joinable()) inbound_demuxer_.join();
}

void KVEngine::InboundDemuxerLoop() {
  try {
    BindCurrentThreadToCpuIndex(affinity_cpus_, inbound_demuxer_worker_id_);
  } catch (const std::exception &error) {
    TransportFatal(config_.node_id, "demux_affinity", error.what());
  }
  try {
    star::RunCxlIncomingLoop(
        rings_[config_.node_id], config_.node_id,
        config_.foreground_worker_count_per_vm, inbound_demuxer_stop_,
        [this](uint32_t worker_id, std::unique_ptr<star::Message> message) {
          if (!message->check_size() || !message->check_deadbeef() ||
              message->get_source_node_id() >= config_.vm_count ||
              message->get_message_count() != 1 ||
              message->get_message_length() < star::Message::get_prefix_size() +
                  star::MessagePiece::get_header_size())
            TransportFatal(config_.node_id, "demux", "malformed original Message",
                           message.get());
          const auto piece = *message->begin();
          if (piece.get_message_length() < star::MessagePiece::get_header_size() ||
              piece.get_message_length() !=
                  message->get_message_length() - star::Message::get_prefix_size())
            TransportFatal(config_.node_id, "demux", "malformed MessagePiece framing",
                           message.get());
          demux_network_rx_bytes_.fetch_add(message->get_message_length(),
                                            std::memory_order_relaxed);
          worker_mailboxes_[worker_id]->inbox.push(message.release());
        });
  } catch (const std::exception &error) {
    TransportFatal(config_.node_id, "ring_recv", error.what());
  } catch (...) {
    TransportFatal(config_.node_id, "ring_recv", "non-std exception");
  }
}

void KVEngine::BindWorker(uint32_t worker_id) {
  if (ebr_ == nullptr)
    throw std::runtime_error("BindWorker requires an open EBR instance");
  if (worker_id >= config_.foreground_worker_count_per_vm)
    throw std::invalid_argument("BindWorker worker_id exceeds foreground_worker_count_per_vm");
  std::lock_guard<std::mutex> lock(worker_owner_mutex_);
  const auto current = std::this_thread::get_id();
  if (std::find(worker_owners_.begin(), worker_owners_.end(), current) !=
      worker_owners_.end())
    throw std::runtime_error("BindWorker calling thread is already bound");
  if (worker_owners_[worker_id] != std::thread::id{})
    throw std::runtime_error("BindWorker worker_id is already owned");
  BindCurrentThreadToCpuIndex(affinity_cpus_, worker_id);
  ebr_->bind_external_ebr_meta(&worker_runtime_[worker_id].ebr_meta);
  star::global_ebr_meta = ebr_;
  worker_owners_[worker_id] = current;
  TlsForegroundWorkerId = static_cast<int32_t>(worker_id);
  BindKvWorkerMaxTid(&worker_runtime_[worker_id].max_tid);
}

void KVEngine::ReleaseWorker() {
  if (ebr_ == nullptr)
    throw std::runtime_error("ReleaseWorker requires an open EBR instance");
  std::lock_guard<std::mutex> lock(worker_owner_mutex_);
  const auto current = std::this_thread::get_id();
  auto owner = std::find(worker_owners_.begin(), worker_owners_.end(),
                         current);
  if (owner == worker_owners_.end())
    throw std::runtime_error("ReleaseWorker called by an unbound thread");
  ReleaseKvWorkerMaxTid();
  ebr_->unbind_external_ebr_meta();
  *owner = std::thread::id{};
  TlsForegroundWorkerId = -1;
}

void KVEngine::PollTransport() {
  // Cooperative transport is never allowed to busy-wait or settle latency
  // while an operation-wide foreground/EBR section is active.  Suspend the
  // outer foreground scope first, run the complete poll in background, and
  // let the background guard settle only after each narrowed EBR dispatch has
  // been destroyed.  The suspension then restores the foreground scope.
  mem_access::ForegroundScopeSuspension suspend_foreground;
#if !defined(LATENCY_SIM_COMPILE_OFF)
  // Deferred mode (remote delete critical state): the RAII guard routes the
  // poll's budget into the single deferred segment instead of settling, so no
  // busy-wait happens while the caller still holds the row's write lock /
  // invalid state; the merged deferred budget settles exactly once at the
  // outermost scope exit, after the delete commit/rollback released the lock
  // and the SCC guards.  The guard is exception-safe: a throwing poll body
  // still restores the thread's scope state before the exception propagates
  // to the delete rollback path.
  mem_access::DeferredTransportPollScope deferred_poll;
#endif
  latency_sim::ScopeGuard background_scope(
      latency_sim::ExecutionClass::kBackground);
  PollTransportImpl();
}

void KVEngine::PollTransportImpl() {
  RequireBoundWorker();
  WorkerMailbox &mailbox = CurrentMailbox();
  const bool awaiting = mailbox.operation.expected_response_type != 0;

  // Every message dispatch runs inside a narrowed EBR critical section while
  // the outer background scope above remains active.  Thus no dispatch can
  // settle/busy-wait latency while EBR, latches, locks or ring state is held.
  auto dispatch_one = [&](std::unique_ptr<star::Message> message) {
    EbrOperationScope ebr_scope(ebr_);
    DispatchMessage(*message, mailbox);
  };

  // Finish requests deferred from a previous await that already completed.
  if (!awaiting && !mailbox.deferred_requests.empty()) {
    std::vector<std::unique_ptr<star::Message>> held;
    held.swap(mailbox.deferred_requests);
    for (auto &message : held) dispatch_one(std::move(message));
  }

  // §3.9.1: while awaiting, peel requests off the FIFO so a matching response
  // behind them is handled before nesting move_in_scan_range.
  while (!mailbox.inbox.empty()) {
    std::unique_ptr<star::Message> message(mailbox.inbox.front());
    if (!mailbox.inbox.pop())
      TransportFatal(config_.node_id, "worker_inbox", "SPSC pop failed");
    const auto piece = *message->begin();
    if (awaiting && !IsResponseType(piece.get_message_type())) {
      mailbox.deferred_requests.push_back(std::move(message));
      continue;
    }
    dispatch_one(std::move(message));
    if (awaiting && mailbox.operation.done) break;
  }

  std::vector<std::unique_ptr<star::Message>> held;
  held.swap(mailbox.deferred_requests);

  if (awaiting && mailbox.operation.done) {
    // Our response arrived: Busy-reject deferred scan-migrates (handler sees
    // operation.done) and keep other requests for the next poll.
    for (auto &message : held) {
      const auto piece = *message->begin();
      if (piece.get_message_type() == static_cast<uint32_t>(
              star::TwoPLPashaMessage::DATA_MIGRATION_REQUEST_FOR_SCAN)) {
        dispatch_one(std::move(message));
      } else {
        mailbox.deferred_requests.push_back(std::move(message));
      }
    }
    return;
  }

  for (size_t i = 0; i < held.size(); ++i) {
    dispatch_one(std::move(held[i]));
    if (awaiting && mailbox.operation.done) {
      for (size_t j = i + 1; j < held.size(); ++j) {
        const auto piece = *held[j]->begin();
        if (piece.get_message_type() == static_cast<uint32_t>(
                star::TwoPLPashaMessage::DATA_MIGRATION_REQUEST_FOR_SCAN)) {
          dispatch_one(std::move(held[j]));
        } else {
          mailbox.deferred_requests.push_back(std::move(held[j]));
        }
      }
      return;
    }
  }
}

void KVEngine::DispatchMessage(star::Message &message, WorkerMailbox &mailbox) {
  // The KV wire deliberately retains the original Message container but does
  // not batch independent operations.  Reject a multi-piece frame instead of
  // silently dispatching only its first piece (the former hand-written path).
  if (message.get_message_count() != 1)
    TransportFatal(config_.node_id, "dispatch", "KV Message must contain one MessagePiece",
                   &message);
  const auto piece = *message.begin();
  if (piece.get_table_id() != kSingleTableId ||
      piece.get_partition_id() >= config_.partition_count)
    TransportFatal(config_.node_id, "dispatch", "invalid table or partition", &message);
  if (IsResponseType(piece.get_message_type()))
    ConsumeTransportResponse(message, piece, mailbox);
  else
    ServeTransportRequest(message, piece, mailbox);
  // Point-migrate handlers may run OnDemand move_out before this send.
  // Scan-migrate handlers flush inside after_response before move_out (§3.9.1),
  // so this call is a no-op when that path already published.
  FlushOutboundMessages(mailbox);
}

void KVEngine::ConsumeTransportResponse(star::Message &message,
                                        star::MessagePiece piece,
                                        WorkerMailbox &mailbox) {
  auto &operation = mailbox.operation;
  if (operation.expected_response_type == 0 || operation.done ||
      piece.get_message_type() != operation.expected_response_type ||
      piece.get_partition_id() != operation.partition_id ||
      message.get_source_node_id() != operation.expected_source_owner)
    TransportFatal(config_.node_id, "response", "unexpected response for worker operation");
  uint32_t key_offset = 0;
  if (piece.get_message_type() == static_cast<uint32_t>(star::TwoPLPashaMessage::REMOTE_DELETE_RESPONSE)) {
    star::RemoteDeleteOutcome delete_outcome{};
    if (!star::TwoPLPashaMessageHandler::decode_remote_delete_response(
            piece, delete_outcome, key_offset) || key_offset != 0)
      TransportFatal(config_.node_id, "response", "malformed remote delete response");
    operation.result = delete_outcome == star::RemoteDeleteOutcome::Deleted
                           ? Status::Ok()
                           : Status::Error(StatusCode::kBusy,
                                           "owner delete busy");
  } else if (piece.get_message_type() == static_cast<uint32_t>(
                 star::TwoPLPashaMessage::DATA_MIGRATION_RESPONSE_FOR_SCAN)) {
    bool success = false;
    if (!star::TwoPLPashaMessageHandler::decode_scan_migration_response(
            piece, success, key_offset) || key_offset != 0)
      TransportFatal(config_.node_id, "response", "malformed scan migration response");
    operation.result = success ? Status::Ok()
                               : Status::Error(StatusCode::kBusy, "scan migration failed");
  } else {
    star::MigrationResponseOutcome migration_outcome{};
    star::RemoteInsertOutcome insert_outcome{};
    const bool decoded = piece.get_message_type() == static_cast<uint32_t>(
                         star::TwoPLPashaMessage::DATA_MIGRATION_RESPONSE)
        ? star::TwoPLPashaMessageHandler::decode_data_migration_response(
              piece, migration_outcome, key_offset)
        : star::TwoPLPashaMessageHandler::decode_remote_insert_response(
              piece, insert_outcome, key_offset);
    if (!decoded || key_offset != 0)
      TransportFatal(config_.node_id, "response", "malformed owner response");
    if (piece.get_message_type() == static_cast<uint32_t>(
            star::TwoPLPashaMessage::DATA_MIGRATION_RESPONSE)) {
      switch (migration_outcome) {
        case star::MigrationResponseOutcome::Migrated:
          operation.result = Status::Ok(); break;
        case star::MigrationResponseOutcome::Missing:
          operation.result = Status::Error(StatusCode::kNotFound, "owner row missing"); break;
        case star::MigrationResponseOutcome::Busy:
          operation.result = Status::Error(StatusCode::kBusy, "owner migration busy"); break;
        case star::MigrationResponseOutcome::NoMemory:
          operation.result = Status::Error(StatusCode::kOutOfMemory, "owner migration out of memory"); break;
      }
    } else {
      switch (insert_outcome) {
        case star::RemoteInsertOutcome::Inserted:
          operation.result = Status::Ok(); break;
        case star::RemoteInsertOutcome::AlreadyExists:
          operation.result = Status::Error(StatusCode::kBusy, "owner insert already exists"); break;
        case star::RemoteInsertOutcome::Busy:
          operation.result = Status::Error(StatusCode::kBusy, "owner insert busy"); break;
        case star::RemoteInsertOutcome::NoMemory:
          operation.result = Status::Error(StatusCode::kOutOfMemory, "owner insert out of memory"); break;
      }
    }
  }
  operation.done = true;
}

void KVEngine::ServeTransportRequest(star::Message &message,
                                     star::MessagePiece piece,
                                     WorkerMailbox &mailbox) {
  if (piece.get_table_id() != kSingleTableId ||
      piece.get_partition_id() >= config_.partition_count)
    TransportFatal(config_.node_id, "request", "invalid table or partition", &message);
  auto *table = KvMigrationRuntime::Instance().TableFor(piece.get_partition_id());
  if (table == nullptr || table->tableID() != kSingleTableId ||
      table->partitionID() != piece.get_partition_id())
    TransportFatal(config_.node_id, "request", "missing table adapter", &message);
  const uint32_t type = piece.get_message_type();
  star::Message &response = OutboundMessage(
      mailbox, message.get_source_node_id());

  if (type == static_cast<uint32_t>(
                  star::TwoPLPashaMessage::DATA_MIGRATION_REQUEST)) {
    const bool framed = star::TwoPLPashaMessageHandler::
        data_migration_request_handler(
            piece, response, *table, config_.fixed_key_size,
            [this, &message, piece](const void *raw_key) {
              const std::string_view key(
                  static_cast<const char *>(raw_key), config_.fixed_key_size);
              auto *partition = OwnedPartition(key);
              if (partition == nullptr ||
                  partition->partition_id() != piece.get_partition_id()) {
                TransportFatal(config_.node_id, "request",
                               "migration delivered to non-owner", &message);
              }
              bool moved_in = false;
              StatusCode status = StatusCode::kCorruption;
              try {
                status = partition->EnsureInShared(
                    key, message.get_source_node_id(), &moved_in);
              } catch (const std::bad_alloc &) {
                return star::MigrationResponseOutcome::NoMemory;
              } catch (const std::exception &error) {
                ProtocolFatal(config_.node_id, "request", error.what(), &message);
              }
              if (status == StatusCode::kOk && moved_in) {
                ++CurrentWorkerRuntime().migration_in;
                ++CurrentWorkerRuntime().shared_swcc_flushes;
              }
              switch (status) {
                case StatusCode::kOk:
                  return star::MigrationResponseOutcome::Migrated;
                case StatusCode::kNotFound:
                  return star::MigrationResponseOutcome::Missing;
                case StatusCode::kBusy:
                  return star::MigrationResponseOutcome::Busy;
                case StatusCode::kOutOfMemory:
                  return star::MigrationResponseOutcome::NoMemory;
                case StatusCode::kAlreadyExists:
                case StatusCode::kCompareFailed:
                case StatusCode::kInvalidArgument:
                case StatusCode::kCorruption:
                case StatusCode::kOwnerViolation:
                  ProtocolFatal(config_.node_id, "request",
                                "invalid migration result", &message);
              }
            },
            [this, partition_id = piece.get_partition_id()] {
              // Skip OnDemand move_out while any scan-range migrate holds
              // private leaf locks in scanForUpdate (leaf ↔ Clock ABBA).
              auto *partition = partitions_[partition_id].get();
              if (partition != nullptr && partition->ScanRangeMigrateInFlight())
                return;
              if (star::migration_manager != nullptr &&
                  star::migration_manager->when_to_move_out ==
                      star::MigrationManager::OnDemand &&
                  star::migration_manager->move_row_out(partition_id))
                ++CurrentWorkerRuntime().migration_out;
            });
    if (!framed)
      TransportFatal(config_.node_id, "request",
                     "bad migration request fields", &message);
    return;
  }

  if (type == static_cast<uint32_t>(
                  star::TwoPLPashaMessage::REMOTE_INSERT_REQUEST)) {
    const bool framed = star::TwoPLPashaMessageHandler::
        remote_insert_request_handler(
            piece, response, *table, config_.fixed_key_size,
            config_.fixed_value_size,
            [this, &message, piece](const void *raw_key, const void *raw_value) {
              const std::string_view key(
                  static_cast<const char *>(raw_key), config_.fixed_key_size);
              const std::string_view value(
                  static_cast<const char *>(raw_value), config_.fixed_value_size);
              auto *partition = OwnedPartition(key);
              if (partition == nullptr ||
                  partition->partition_id() != piece.get_partition_id())
                TransportFatal(config_.node_id, "request",
                               "insert delivered to non-owner", &message);
              try {
                const StatusCode status = partition->InsertRemotePlaceholder(
                    key, value, message.get_source_node_id());
                switch (status) {
                  case StatusCode::kOk:
                    return star::RemoteInsertOutcome::Inserted;
                  case StatusCode::kAlreadyExists:
                    return star::RemoteInsertOutcome::AlreadyExists;
                  case StatusCode::kBusy:
                    return star::RemoteInsertOutcome::Busy;
                  case StatusCode::kOutOfMemory:
                    return star::RemoteInsertOutcome::NoMemory;
                  case StatusCode::kNotFound:
                  case StatusCode::kCompareFailed:
                  case StatusCode::kInvalidArgument:
                  case StatusCode::kCorruption:
                  case StatusCode::kOwnerViolation:
                    ProtocolFatal(config_.node_id, "request",
                                  "invalid remote insert result", &message);
                }
              } catch (const std::bad_alloc &) {
                return star::RemoteInsertOutcome::NoMemory;
              } catch (const std::exception &error) {
                ProtocolFatal(config_.node_id, "request", error.what(), &message);
              }
            });
    if (!framed)
      TransportFatal(config_.node_id, "request",
                     "bad remote insert request fields", &message);
    return;
  }

  if (type == static_cast<uint32_t>(
                  star::TwoPLPashaMessage::REMOTE_DELETE_REQUEST)) {
    const bool framed = star::TwoPLPashaMessageHandler::
        remote_delete_request_handler(
            piece, response, *table, config_.fixed_key_size,
            [this, &message, piece, table](const void *raw_key) {
              const std::string_view key(
                  static_cast<const char *>(raw_key), config_.fixed_key_size);
              auto *partition = OwnedPartition(key);
              if (partition == nullptr || star::migration_manager == nullptr)
                TransportFatal(config_.node_id, "request",
                               "delete delivered to non-owner", &message);
              const FixedKey fixed_key = FixedKey::From(
                  key, config_.fixed_key_size);
              return star::migration_manager->delete_specific_row_and_move_out(
                         table, &fixed_key, /*is_delete_local=*/false)
                         ? star::RemoteDeleteOutcome::Deleted
                         : star::RemoteDeleteOutcome::Busy;
            });
    if (!framed)
      TransportFatal(config_.node_id, "request",
                     "bad remote delete request fields", &message);
    return;
  }

  if (type == static_cast<uint32_t>(
                  star::TwoPLPashaMessage::DATA_MIGRATION_REQUEST_FOR_SCAN)) {
    const bool framed = star::TwoPLPashaMessageHandler::
        data_migration_request_for_scan_handler(
            piece, response, *table, config_.fixed_key_size,
            [this, piece, &mailbox](const void *raw_min_key, const void *raw_max_key,
                          uint64_t limit) {
              // §3.9.1: once our own RPC response is in, or another range
              // migrate is already running while we await, Busy without nesting
              // move_in_scan_range so AwaitResponse can complete.
              if (mailbox.operation.done) return false;
              auto *partition = partitions_[piece.get_partition_id()].get();
              if (mailbox.operation.expected_response_type != 0 &&
                  partition != nullptr &&
                  partition->ScanRangeMigrateInFlight())
                return false;
              const std::string_view min_key(
                  static_cast<const char *>(raw_min_key), config_.fixed_key_size);
              const std::string_view max_key(
                  static_cast<const char *>(raw_max_key), config_.fixed_key_size);
              return PreparePartitionSharedScan(
                         piece.get_partition_id(), min_key, max_key, limit)
                  .ok();
            },
            [this, partition_id = piece.get_partition_id(), &mailbox] {
              // §3.9.1: publish the scan-migrate bool response before OnDemand
              // move_out so AwaitResponse peers are not stuck behind the Clock
              // victim walk. Skipping move_out entirely filled CXL and stalled
              // Rel20k mid-run — keep OnDemand reclaim after Flush.
              FlushOutboundMessages(mailbox);
              if (star::migration_manager != nullptr &&
                  star::migration_manager->when_to_move_out ==
                      star::MigrationManager::OnDemand &&
                  star::migration_manager->move_row_out(partition_id))
                ++CurrentWorkerRuntime().migration_out;
            });
    if (!framed)
      TransportFatal(config_.node_id, "request",
                     "bad scan migration request fields", &message);
    return;
  }

  TransportFatal(config_.node_id, "request",
                 "unsupported original MessagePiece", &message);
}
void KVEngine::FlushOutboundMessages(WorkerMailbox &mailbox) {
  // This is intentionally a direct synchronous CXL branch, as in master
  // Executor::flush_messages() when no output thread is configured.  The
  // per-worker buffers are reset only after their completed frame was copied
  // into the ring.
  for (auto &outbound : mailbox.outbound) {
    if (outbound->get_message_count() == 0) continue;
    SendTransportMessage(*outbound);
    outbound->clear_message_pieces();
  }
}

}  // namespace tigonkv::engine
