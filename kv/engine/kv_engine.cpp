#include "kv/engine/kv_engine.h"

#include "kv/engine/fixed_value.h"

#include "common/CXL_EBR.h"
#include "common/BufferedReader.h"
#include "common/MPSCRingBuffer.h"
#include "kv/engine/kv_partition.h"
#include "kv/engine/kv_migration.h"
#include "kv/engine/mem_access.h"
#include "protocol/TwoPLPasha/TwoPLPashaMessage.h"
#include "protocol/TwoPLPasha/TwoPLPashaSCCWriteThrough.h"

#include <stdexcept>
#include <thread>
#include <charconv>
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

bool IsInternalMaxSentinel(std::string_view key) {
  return !key.empty() && std::all_of(key.begin(), key.end(), [](char byte) {
    return static_cast<unsigned char>(byte) == 0xff;
  });
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
  boolean("model_cxl_search_overhead", false);
  boolean("internal_max_sentinel", true);
  for (uint32_t partition = 0; partition < config.partition_count; ++partition) {
    text("range_lower", config.partition_ranges[partition].lower_key);
    text("range_upper", config.partition_ranges[partition].upper_key);
  }
  boolean("latency_enabled", config.latency_enabled);
  boolean("latency_foreground_enabled", config.latency_foreground_enabled);
  boolean("latency_merge_enabled", config.latency_merge_enabled);
  boolean("latency_stats_enabled", config.latency_stats_enabled);
  u64("latency_cache_line_bytes", config.latency_cache_line_bytes);
  decimal("swcc_read_ns", config.swcc_read_ns);
  decimal("swcc_write_ns", config.swcc_write_ns);
  decimal("swcc_flush_ns", config.swcc_flush_ns);
  decimal("hwcc_read_ns", config.hwcc_read_ns);
  decimal("hwcc_write_ns", config.hwcc_write_ns);
  decimal("hwcc_atomic_load_ns", config.hwcc_atomic_load_ns);
  decimal("hwcc_atomic_store_ns", config.hwcc_atomic_store_ns);
  decimal("hwcc_atomic_rmw_ns", config.hwcc_atomic_rmw_ns);
  text("latency_cache_model", config.latency_cache_model);
  boolean("latency_cache_hits_enabled", config.latency_cache_hits_enabled);
  decimal("latency_cache_fixed_hit_rate", config.latency_cache_fixed_hit_rate);
  u64("latency_cache_capacity_lines", config.latency_cache_capacity_lines);
  u64("latency_cache_associativity", config.latency_cache_associativity);
  decimal("latency_cache_hit_extra_ns", config.latency_cache_hit_extra_ns);
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

// §13: Scan hot path only bumps TLS; flushed once at Scan return.
struct ScanTlsDiag {
  uint64_t partition_probes = 0;
  uint64_t migrate_rpcs = 0;
};
thread_local ScanTlsDiag TlsScanDiag{};

struct ScanTlsFlushGuard {
  std::atomic<uint64_t> *probes;
  std::atomic<uint64_t> *migrates;
  ~ScanTlsFlushGuard() {
    probes->fetch_add(TlsScanDiag.partition_probes, std::memory_order_relaxed);
    migrates->fetch_add(TlsScanDiag.migrate_rpcs, std::memory_order_relaxed);
    TlsScanDiag = {};
  }
};

[[noreturn]] void TransportFatal(uint32_t node_id, const char *stage,
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

enum class RpcResult : uint8_t { kOk = 0, kMissing = 1, kBusy = 2, kNoMemory = 3 };

Status ResultStatus(RpcResult result, const char *operation) {
  switch (result) {
    case RpcResult::kOk: return Status::Ok();
    case RpcResult::kMissing:
      return Status::Error(StatusCode::kNotFound, std::string(operation) + " missing");
    case RpcResult::kBusy:
      return Status::Error(StatusCode::kBusy, std::string(operation) + " busy");
    case RpcResult::kNoMemory:
      return Status::Error(StatusCode::kOutOfMemory, std::string(operation) + " out of memory");
  }
  TransportFatal(0, "result_decode", "unknown RPC result");
}

RpcResult EncodeResult(StatusCode code) {
  switch (code) {
    case StatusCode::kOk: return RpcResult::kOk;
    case StatusCode::kNotFound: return RpcResult::kMissing;
    case StatusCode::kBusy: return RpcResult::kBusy;
    case StatusCode::kOutOfMemory: return RpcResult::kNoMemory;
    default: return RpcResult::kBusy;
  }
}

void InitializeMessage(star::Message *message, uint32_t source, uint32_t destination,
                       uint32_t worker_id, uint64_t operation_sequence) {
  message->set_source_node_id(source);
  message->set_dest_node_id(destination);
  message->set_worker_id(worker_id);
  message->set_transaction_id(operation_sequence);
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

KVEngine::KVEngine(const Config &config, std::unique_ptr<DualRegionMappedPool> pool,
                   star::CXL_EBR *ebr, std::unique_ptr<star::SCCManager> scc)
    : config_(config),
      pool_(std::move(pool)),
      ebr_(ebr),
      scc_(std::move(scc)),
      worker_owners_(config.foreground_worker_count_per_vm) {
  worker_mailboxes_.reserve(config.foreground_worker_count_per_vm);
  for (uint32_t worker = 0; worker < config.foreground_worker_count_per_vm;
       ++worker) {
    auto mailbox = std::make_unique<WorkerMailbox>();
    mailbox->outbound.reserve(config.vm_count);
    for (uint32_t destination = 0; destination < config.vm_count; ++destination)
      mailbox->outbound.push_back(std::make_unique<star::Message>());
    worker_mailboxes_.push_back(std::move(mailbox));
  }
}

KVEngine::~KVEngine() {
  StopInboundDemuxer();
  if (star::scc_manager == scc_.get()) star::scc_manager = nullptr;
  if (star::global_ebr_meta == ebr_) star::global_ebr_meta = nullptr;
  KvMigrationRuntime::Instance().Reset();
}

std::unique_ptr<KVEngine> KVEngine::Open(Config config, bool reset) {
  config.Validate();
  auto affinity_cpus = ResolveAffinityCpus(config);
  auto pool = std::make_unique<DualRegionMappedPool>(
      DualRegionMappedPool::Open(config.shared_memory_path, RegionConfig(config), reset));
  star::CXLMemory::bind_dual_region_allocator(&pool->allocator(), config.node_id);
  star::MPSCRingBuffer *rings = nullptr;
  star::CXL_EBR *ebr = nullptr;
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
  ebr->thread_init_ebr_meta(config.node_id, 0);
  star::global_ebr_meta = ebr;
  auto scc = std::make_unique<star::TwoPLPashaSCCWriteThrough>();
  star::scc_manager = scc.get();
  auto engine = std::unique_ptr<KVEngine>(new KVEngine(config, std::move(pool), ebr,
                                                        std::move(scc)));
  engine->affinity_cpus_ = std::move(affinity_cpus);
  engine->rings_ = rings;
  const uint64_t maximum_piece_bytes = star::MessagePiece::get_header_size() +
      std::max({static_cast<uint64_t>(config.fixed_key_size) + config.fixed_value_size +
                    sizeof(uint64_t) + sizeof(uint32_t),
                static_cast<uint64_t>(config.fixed_key_size) * 2 +
                    sizeof(uint64_t) * 2 + sizeof(uint32_t),
                static_cast<uint64_t>(config.fixed_key_size) + sizeof(uint64_t) +
                    sizeof(uint32_t)});
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
    mem_access::HwccAtomicLoad(&layout.state);
    initialize_owner = layout.state.load(std::memory_order_acquire) ==
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
      mem_access::HwccAtomicLoad(&directory.shared_root);
      if (directory.shared_root.load(std::memory_order_acquire) != kNullOffset)
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
    mem_access::HwccAtomicLoad(&directory.shared_root);
    if (directory.shared_root.load(std::memory_order_acquire) == kNullOffset)
      throw std::runtime_error("tigonkv: layout ready with missing shared root");
    const bool materialize_private =
        engine->OwnerForPartition(partition) == config.node_id;
    engine->partitions_.emplace_back(std::make_unique<KVPartition>(
        engine->pool_->allocator(), *engine->ebr_, partition,
        engine->OwnerForPartition(partition), true, materialize_private));
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
  const auto &layout = engine->pool_->allocator().layout();
  uint64_t static_hwcc = 0;
  for (size_t domain :
       {static_cast<size_t>(AllocationDomain::kHwccLayout),
        static_cast<size_t>(AllocationDomain::kHwccAllocatorMetadata),
        static_cast<size_t>(AllocationDomain::kTransport),
        static_cast<size_t>(AllocationDomain::kHwccEbr)}) {
    static_hwcc +=
        layout.domains[domain].used_bytes.load(std::memory_order_relaxed);
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
  engine->StartInboundDemuxer();
  return engine;
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
  if (IsInternalMaxSentinel(key))
    return Status::Error(StatusCode::kInvalidArgument,
                         "internal max sentinel is reserved");
  const KeyRoute route = RouteForKey(key);
  if (!route.owned_by_this_node) {
    if (route.partition == nullptr)
      return Status::Error(StatusCode::kCorruption, "missing visible partition");
    const auto write_shared = [&](bool record_clock_access = true) {
      const SharedAccessState state =
          route.partition->PutShared(key, config_.node_id, value,
                                     record_clock_access);
      if (state == SharedAccessState::kDone) {
        shared_puts_.fetch_add(1, std::memory_order_relaxed);
        shared_swcc_flushes_.fetch_add(1, std::memory_order_relaxed);
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
        Forward(RpcKind::kMigrate, key, {}, route.partition_id, route.owner);
    if (migrated.ok()) return write_shared(/*record_clock_access=*/false);
    if (migrated.code != StatusCode::kNotFound) return migrated;
    // A true create uses the original REMOTE_INSERT ownership split: the owner
    // inserts an invalid placeholder and copies it into CXL with a requester
    // ref; after the ack the requester publishes only valid (no second data
    // write through the shared payload).
    const Status inserted =
        Forward(RpcKind::kInsert, key, value, route.partition_id, route.owner);
    if (!inserted.ok()) return inserted;
    if (!route.partition->PublishRemotePlaceholder(key, config_.node_id))
      return Status::Error(StatusCode::kBusy,
                           "remote insert placeholder publication failed");
    return Status::Ok();
  }
  try {
    route.partition->PutPrivate(key, value);
    return Status::Ok();
  } catch (const std::bad_alloc &) {
    return Status::Error(StatusCode::kOutOfMemory, "private arena exhausted");
  } catch (const std::runtime_error &e) {
    return Status::Error(std::string_view(e.what()).find("busy") !=
                                 std::string_view::npos
                             ? StatusCode::kBusy
                             : StatusCode::kCorruption,
                         e.what());
  } catch (const std::exception &e) {
    return Status::Error(StatusCode::kCorruption, e.what());
  }
}

GetResult KVEngine::Get(std::string_view key) {
  if (IsInternalMaxSentinel(key))
    return {Status::Error(StatusCode::kInvalidArgument,
                          "internal max sentinel is reserved"), {}};
  const KeyRoute route = RouteForKey(key);
  if (!route.owned_by_this_node) {
    auto *visible = route.partition;
    if (visible == nullptr)
      return {Status::Error(StatusCode::kCorruption, "missing visible partition"), {}};
    std::string shared;
    const SharedAccessState first =
        visible->GetShared(key, config_.node_id, &shared);
    if (first == SharedAccessState::kDone) {
      shared_gets_.fetch_add(1, std::memory_order_relaxed);
      return {Status::Ok(), std::move(shared)};
    }
    if (first == SharedAccessState::kRetry)
      return {Status::Error(StatusCode::kBusy, "shared get contention"), {}};
    const Status migrated =
        Forward(RpcKind::kMigrate, key, {}, route.partition_id, route.owner);
    if (!migrated.ok()) return {migrated, {}};
    const SharedAccessState second = visible->GetShared(
        key, config_.node_id, &shared, /*record_clock_access=*/false);
    if (second == SharedAccessState::kDone) {
      shared_gets_.fetch_add(1, std::memory_order_relaxed);
      return {Status::Ok(), std::move(shared)};
    }
    return {Status::Error(second == SharedAccessState::kRetry
                              ? StatusCode::kBusy
                              : StatusCode::kCorruption,
                          "owner acknowledged migration without readable shared row"), {}};
  }
  try {
    std::string value;
    return route.partition->GetPrivate(key, &value)
               ? GetResult{Status::Ok(), std::move(value)}
               : GetResult{Status::Error(StatusCode::kNotFound, "key not found"),
                           {}};
  } catch (const std::runtime_error &e) {
    return {Status::Error(std::string_view(e.what()).find("busy") !=
                                   std::string_view::npos
                               ? StatusCode::kBusy
                               : StatusCode::kCorruption,
                         e.what()), {}};
  }
}

Status KVEngine::Delete(std::string_view key) {
  if (IsInternalMaxSentinel(key))
    return Status::Error(StatusCode::kInvalidArgument,
                         "internal max sentinel is reserved");
  const KeyRoute route = RouteForKey(key);
  if (!route.owned_by_this_node) {
    star::TwoPLPashaMetadataShared *locked_row = nullptr;
    const auto prepare_delete = [&](bool record_clock_access = true) {
      return route.partition->PrepareRemoteDelete(key, config_.node_id,
                                                  &locked_row,
                                                  record_clock_access);
    };
    SharedAccessState prepared = prepare_delete();
    if (prepared == SharedAccessState::kMissing) {
      const Status migrated =
          Forward(RpcKind::kMigrate, key, {}, route.partition_id, route.owner);
      if (!migrated.ok()) return migrated;
      prepared = prepare_delete(/*record_clock_access=*/false);
    }
    if (prepared == SharedAccessState::kMissing)
      return Status::Error(StatusCode::kNotFound, "key not found");
    if (prepared != SharedAccessState::kDone)
      return Status::Error(StatusCode::kBusy, "remote delete shared row busy");
    const Status deleted = Forward(RpcKind::kDelete, key, {}, route.partition_id,
                                   route.owner);
    // A successful owner callback consumes the requester write/ref pin with
    // the retired row.  On an unsuccessful ack it is still live and must be
    // restored before the facade retries.
    if (!deleted.ok()) {
      route.partition->AbortRemoteDelete(locked_row);
    }
    return deleted;
  }
  try {
    return route.partition->DeletePrivate(key)
               ? Status::Ok()
               : Status::Error(StatusCode::kNotFound, "key not found");
  } catch (const std::runtime_error &e) {
    return Status::Error(std::string_view(e.what()).find("busy") !=
                                 std::string_view::npos
                             ? StatusCode::kBusy
                             : StatusCode::kCorruption,
                         e.what());
  }
}

ScanResult KVEngine::Scan(std::string_view start_key, std::string_view end_key,
                          uint64_t limit) {
  if (IsInternalMaxSentinel(start_key))
    return {Status::Error(StatusCode::kInvalidArgument,
                          "internal max sentinel is reserved"), {}};
  TlsScanDiag = {};
  ScanTlsFlushGuard flush_guard{&scan_partition_probes_, &scan_migrate_rpcs_};
  if (!end_key.empty() && !LessFixed(start_key, end_key, config_.fixed_key_size))
    return {Status::Ok(), {}};

  auto scan_remote = [&](uint32_t partition_id, std::string_view min_key,
                         std::string_view inclusive_max, uint64_t scan_limit,
                         ScanResult *result) -> Status {
    auto *partition = partitions_[partition_id].get();
    KVPartition::SharedScanResult probe = partition->ScanSharedPartition(
        config_.node_id, min_key, scan_limit, inclusive_max);
    ++TlsScanDiag.partition_probes;
    PollTransport();
    if (!probe.status.ok()) return probe.status;
    if (probe.migration_required) {
      const Status migrated = Forward(
          RpcKind::kScanMigrate, min_key, {}, partition_id,
          OwnerForPartition(partition_id), inclusive_max, scan_limit);
      ++TlsScanDiag.migrate_rpcs;
      if (!migrated.ok()) return migrated;
      // The original handler has now moved the range in.  Do not add a
      // second operation retry policy here: the single KVStore facade retry
      // restarts this scan from its public operation boundary.
      return Status::Error(StatusCode::kBusy,
                           "CXL scan range moved in; retry at facade");
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
      std::memset(inclusive.bytes, 0xff, config_.fixed_key_size);
    if (limit != 0 && result.items.size() >= limit) break;
    const uint64_t remaining =
        limit == 0 ? 0 : static_cast<uint64_t>(limit - result.items.size());
    const std::string inclusive_max(inclusive.bytes, config_.fixed_key_size);
    Status status;
    if (OwnerForPartition(partition_id) == config_.node_id) {
      std::vector<std::pair<std::string, std::string>> items;
      const bool ok = partitions_[partition_id]->ScanLocalPartition(
          min_key, remaining, &items, inclusive_max);
      ++TlsScanDiag.partition_probes;
      PollTransport();
      status = ok ? Status::Ok()
                  : Status::Error(StatusCode::kBusy,
                                  "owner scan contention/retry budget");
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
    std::string_view inclusive_max, uint64_t output_limit) {
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
  const FixedKey min = FixedKey::From(start_key, config_.fixed_key_size);
  const FixedKey max = FixedKey::From(inclusive_max, config_.fixed_key_size);
  try {
    star::TwoPLPashaMessageHandler::move_in_scan_range(
        *table, min.bytes, max.bytes, output_limit);
  } catch (const std::bad_alloc &) {
    return Status::Error(StatusCode::kOutOfMemory,
                         "scan range move-in allocation failed");
  }
  return Status::Ok();
}

CasResult KVEngine::CompareExchange(std::string_view key,
                                    std::string_view expected,
                                    std::string_view desired) {
  if (IsInternalMaxSentinel(key))
    return {Status::Error(StatusCode::kInvalidArgument,
                          "internal max sentinel is reserved"), false};
  const KeyRoute route = RouteForKey(key);
  if (!route.owned_by_this_node) {
    auto *visible = route.partition;
    if (visible == nullptr)
      return {Status::Error(StatusCode::kCorruption, "missing visible partition"), false};
    bool exchanged = false;
    auto compare_shared = [&](bool record_clock_access = true) {
      return visible->CompareExchangeShared(key, config_.node_id, expected,
                                            desired, &exchanged,
                                            record_clock_access);
    };
    SharedAccessState state = compare_shared();
    if (state == SharedAccessState::kDone) {
      if (exchanged) {
        shared_puts_.fetch_add(1, std::memory_order_relaxed);
        shared_swcc_flushes_.fetch_add(1, std::memory_order_relaxed);
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
        Forward(RpcKind::kMigrate, key, {}, route.partition_id, route.owner);
    if (migrated.ok()) {
      state = compare_shared(/*record_clock_access=*/false);
      if (state == SharedAccessState::kDone) {
        if (exchanged) {
          shared_puts_.fetch_add(1, std::memory_order_relaxed);
          shared_swcc_flushes_.fetch_add(1, std::memory_order_relaxed);
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
        Forward(RpcKind::kInsert, key, desired, route.partition_id, route.owner);
    if (!inserted.ok()) return {inserted, false};
    if (!visible->PublishRemotePlaceholder(key, config_.node_id))
      return {Status::Error(StatusCode::kBusy,
                            "remote CAS placeholder publication failed"),
              false};
    return {Status::Ok(), true};
  }
  try {
    bool exchanged = false;
    if (!route.partition->CompareExchangePrivate(key, expected, desired, &exchanged))
      return {Status::Error(StatusCode::kNotFound, "key not found"), false};
    return {exchanged ? Status::Ok()
                      : Status::Error(StatusCode::kCompareFailed, "expected value differs"),
            exchanged};
  } catch (const std::bad_alloc &) {
    return {Status::Error(StatusCode::kOutOfMemory, "allocator exhausted"), false};
  } catch (const std::runtime_error &e) {
    return {Status::Error(std::string_view(e.what()).find("busy") !=
                                  std::string_view::npos
                              ? StatusCode::kBusy
                              : StatusCode::kInvalidArgument,
                          e.what()), false};
  } catch (const std::exception &e) {
    return {Status::Error(StatusCode::kInvalidArgument, e.what()), false};
  }
}

IncrementResult KVEngine::Increment(std::string_view key, int64_t delta) {
  if (IsInternalMaxSentinel(key))
    return {Status::Error(StatusCode::kInvalidArgument,
                          "internal max sentinel is reserved"), 0};
  const KeyRoute route = RouteForKey(key);
  if (!route.owned_by_this_node) {
    auto *visible = route.partition;
    if (visible == nullptr)
      return {Status::Error(StatusCode::kCorruption, "missing visible partition"), 0};
    int64_t shared = 0;
    auto increment_shared = [&](bool record_clock_access = true) {
      return visible->IncrementShared(key, config_.node_id, delta, &shared,
                                      record_clock_access);
    };
    SharedAccessState state = increment_shared();
    if (state == SharedAccessState::kDone) {
      shared_puts_.fetch_add(1, std::memory_order_relaxed);
      shared_swcc_flushes_.fetch_add(1, std::memory_order_relaxed);
      return {Status::Ok(), shared};
    }
    if (state == SharedAccessState::kRetry)
      return {Status::Error(StatusCode::kBusy, "shared increment contention"), 0};
    const Status migrated =
        Forward(RpcKind::kMigrate, key, {}, route.partition_id, route.owner);
    if (migrated.ok()) {
      state = increment_shared(/*record_clock_access=*/false);
      if (state == SharedAccessState::kDone) {
        shared_puts_.fetch_add(1, std::memory_order_relaxed);
        shared_swcc_flushes_.fetch_add(1, std::memory_order_relaxed);
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
        Forward(RpcKind::kInsert, key, initial, route.partition_id, route.owner);
    if (!inserted.ok()) return {inserted, 0};
    if (!visible->PublishRemotePlaceholder(key, config_.node_id))
      return {Status::Error(StatusCode::kBusy,
                            "remote increment placeholder publication failed"),
              0};
    return {Status::Ok(), delta};
  }
  try {
    int64_t value = 0;
    if (!route.partition->IncrementPrivate(key, delta, &value))
      return {Status::Error(StatusCode::kNotFound, "key not found"), 0};
    return {Status::Ok(), value};
  } catch (const std::invalid_argument &e) {
    return {Status::Error(StatusCode::kInvalidArgument, e.what()), 0};
  } catch (const std::runtime_error &e) {
    return {Status::Error(std::string_view(e.what()).find("busy") !=
                                  std::string_view::npos
                              ? StatusCode::kBusy
                              : StatusCode::kCorruption,
                          e.what()), 0};
  } catch (const std::exception &e) {
    return {Status::Error(StatusCode::kCorruption, e.what()), 0};
  }
}

MemoryStats KVEngine::Memory() const {
  const auto &regions = pool_->allocator();
  const auto &layout = regions.layout();
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
      stats.logical_hwcc_used_bytes +=
          layout.domains[domain].used_bytes.load(std::memory_order_relaxed);
  }
  stats.logical_hwcc_used_bytes +=
      regions.DynamicHwccUsedBytes(config_.node_id);
  stats.physical_hwcc_used_bytes = stats.logical_hwcc_used_bytes;
  stats.owner_private_swcc_used_bytes =
      regions.OwnerPrivateUsedBytes(config_.node_id);
  stats.shared_payload_swcc_used_bytes =
      regions.SharedPayloadUsedBytes(config_.node_id);
  stats.allocator_hwcc_metadata_bytes = config_.node_id == 0
      ? layout.domains[static_cast<size_t>(AllocationDomain::kHwccAllocatorMetadata)]
            .used_bytes.load(std::memory_order_relaxed)
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
    stats.allocator_swcc_metadata_bytes +=
        layout.domains[static_cast<size_t>(AllocationDomain::kSwccAllocatorMetadata)]
            .used_bytes.load(std::memory_order_relaxed);
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
  stats.shared_gets = shared_gets_.load(std::memory_order_relaxed);
  stats.shared_puts = shared_puts_.load(std::memory_order_relaxed);
  stats.shared_deletes = shared_deletes_.load(std::memory_order_relaxed);
  stats.shared_swcc_flushes = shared_swcc_flushes_.load(std::memory_order_relaxed);
  stats.migration_in = migration_in_.load(std::memory_order_relaxed);
  stats.migration_out = migration_out_.load(std::memory_order_relaxed);
  stats.scan_partition_probes =
      scan_partition_probes_.load(std::memory_order_relaxed);
  stats.scan_migrate_rpcs =
      scan_migrate_rpcs_.load(std::memory_order_relaxed);
  stats.network_tx_bytes = NetworkTxBytes();
  stats.network_rx_bytes = NetworkRxBytes();
  return stats;
}

KVEngine::WorkerMailbox &KVEngine::CurrentMailbox() {
  const uint32_t worker = TlsForegroundWorkerId < 0
      ? 0 : static_cast<uint32_t>(TlsForegroundWorkerId);
  if (worker >= worker_mailboxes_.size())
    TransportFatal(config_.node_id, "worker_mailbox", "foreground worker is not bound");
  return *worker_mailboxes_[worker];
}

star::Message &KVEngine::OutboundMessage(WorkerMailbox &mailbox,
                                         uint32_t destination,
                                         uint64_t operation_sequence) {
  if (destination >= mailbox.outbound.size())
    TransportFatal(config_.node_id, "outbound", "destination exceeds original Message buffers");
  auto &message = *mailbox.outbound[destination];
  message.clear_message_pieces();
  const uint32_t worker = TlsForegroundWorkerId < 0 ? 0 :
      static_cast<uint32_t>(TlsForegroundWorkerId);
  InitializeMessage(&message, config_.node_id, destination, worker,
                    operation_sequence);
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
  network_tx_bytes_.fetch_add(message.get_message_length(), std::memory_order_relaxed);
}

Status KVEngine::Forward(RpcKind type, std::string_view key,
                         std::string_view value, uint32_t partition_id,
                         uint32_t owner, std::string_view scan_max,
                         uint64_t scan_limit) {
  if (owner >= config_.vm_count || partition_id >= config_.partition_count)
    return Status::Error(StatusCode::kInvalidArgument, "invalid RPC route");
  auto *table = KvMigrationRuntime::Instance().TableFor(partition_id);
  if (table == nullptr)
    return Status::Error(StatusCode::kCorruption, "missing RPC table adapter");
  if (table->partitionID() != partition_id ||
      OwnerForPartition(partition_id) != owner)
    TransportFatal(config_.node_id, "forward", "inconsistent RPC partition route");
  WorkerMailbox &mailbox = CurrentMailbox();
  if (mailbox.operation.expected_response_type != 0)
    TransportFatal(config_.node_id, "forward", "reentrant foreground RPC");
  const uint64_t sequence = mailbox.next_operation_sequence++;
  const FixedKey fixed_key = FixedKey::From(key, config_.fixed_key_size);
  FixedKey fixed_scan_max{};
  if (type == RpcKind::kScanMigrate)
    fixed_scan_max = FixedKey::From(scan_max, config_.fixed_key_size);
  std::string fixed_value;
  if (type == RpcKind::kInsert) {
    fixed_value.assign(config_.fixed_value_size, '\0');
    std::memcpy(fixed_value.data(), value.data(),
                std::min(value.size(), fixed_value.size()));
  }
  star::Message &message = OutboundMessage(mailbox, owner, sequence);
  uint32_t expected_response = 0;
  switch (type) {
    case RpcKind::kMigrate:
      star::TwoPLPashaMessageFactory::new_data_migration_message(
          message, *table, fixed_key.bytes, sequence, 0);
      expected_response = static_cast<uint32_t>(star::TwoPLPashaMessage::DATA_MIGRATION_RESPONSE);
      break;
    case RpcKind::kInsert:
      star::TwoPLPashaMessageFactory::new_remote_insert_message(
          message, *table, fixed_key.bytes, fixed_value.data(), sequence, 0);
      expected_response = static_cast<uint32_t>(star::TwoPLPashaMessage::REMOTE_INSERT_RESPONSE);
      break;
    case RpcKind::kDelete:
      star::TwoPLPashaMessageFactory::new_remote_delete_message(message, *table, fixed_key.bytes);
      expected_response = static_cast<uint32_t>(star::TwoPLPashaMessage::REMOTE_DELETE_RESPONSE);
      break;
    case RpcKind::kScanMigrate:
      star::TwoPLPashaMessageFactory::new_data_migration_message_for_scan(
          message, *table, fixed_key.bytes, fixed_scan_max.bytes, scan_limit, sequence, 0);
      expected_response = static_cast<uint32_t>(
          star::TwoPLPashaMessage::DATA_MIGRATION_RESPONSE_FOR_SCAN);
      break;
  }
  mailbox.operation = {expected_response, partition_id, sequence, false,
                       Status::Error(StatusCode::kCorruption, "missing RPC response")};
  SendTransportMessage(message);
  message.clear_message_pieces();
  return AwaitResponse(mailbox);
}

Status KVEngine::RequestMigrate(std::string_view key) {
  const KeyRoute route = RouteForKey(key);
  return Forward(RpcKind::kMigrate, key, {}, route.partition_id, route.owner);
}

Status KVEngine::AwaitResponse(WorkerMailbox &mailbox) {
  while (!mailbox.operation.done) {
    PollTransport();
    if (!mailbox.operation.done) std::this_thread::yield();
  }
  const Status result = mailbox.operation.result;
  mailbox.operation = {};
  return result;
}

void KVEngine::StartInboundDemuxer() {
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
  if (rings_ == nullptr)
    TransportFatal(config_.node_id, "demux", "missing inbound ring");
  star::BufferedReader reader(rings_[config_.node_id]);
  while (!inbound_demuxer_stop_.load(std::memory_order_acquire)) {
    bool progressed = false;
    {
      // The demuxer is the actual thread touching the inbound HWCC ring, so it
      // needs its own foreground scope; scopes do not propagate across threads.
      mem_access::LatencyScope latency_scope(
          latency_sim::ScopeKind::kForeground);
      for (int drained = 0; drained < 64; ++drained) {
        std::unique_ptr<star::Message> message;
        try {
          message = reader.next_message();
        } catch (const std::exception &error) {
          TransportFatal(config_.node_id, "ring_recv", error.what());
        } catch (...) {
          TransportFatal(config_.node_id, "ring_recv", "non-std exception");
        }
        mem_access::DelayActiveScopeNow();
        if (message == nullptr) break;
        if (!message->check_size() || !message->check_deadbeef() ||
            message->get_dest_node_id() != config_.node_id ||
            message->get_source_node_id() >= config_.vm_count ||
            message->get_worker_id() >= worker_mailboxes_.size() ||
            message->get_message_count() != 1 ||
            message->get_message_length() < star::Message::get_prefix_size() +
                star::MessagePiece::get_header_size())
          TransportFatal(config_.node_id, "demux", "malformed original Message", message.get());
        const auto piece = *message->begin();
        if (piece.get_message_length() < star::MessagePiece::get_header_size() ||
            piece.get_message_length() !=
                message->get_message_length() - star::Message::get_prefix_size())
          TransportFatal(config_.node_id, "demux", "malformed MessagePiece framing", message.get());
        network_rx_bytes_.fetch_add(message->get_message_length(), std::memory_order_relaxed);
        worker_mailboxes_[message->get_worker_id()]->inbox.push(message.release());
        progressed = true;
      }
    }
    if (!progressed) std::this_thread::yield();
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
  ebr_->thread_init_ebr_meta(config_.node_id, worker_id);
  star::global_ebr_meta = ebr_;
  worker_owners_[worker_id] = current;
  TlsForegroundWorkerId = static_cast<int32_t>(worker_id);
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
  *owner = std::thread::id{};
  TlsForegroundWorkerId = -1;
}

void KVEngine::PollTransport() {
  WorkerMailbox &mailbox = CurrentMailbox();
  while (!mailbox.inbox.empty()) {
    std::unique_ptr<star::Message> message(mailbox.inbox.front());
    if (!mailbox.inbox.pop())
      TransportFatal(config_.node_id, "worker_inbox", "SPSC pop failed");
    mem_access::IsolatedLatencyScope request_scope(latency_sim::ScopeKind::kForeground);
    DispatchMessage(*message, mailbox);
  }
}

void KVEngine::DispatchMessage(star::Message &message, WorkerMailbox &mailbox) {
  const auto piece = *message.begin();
  if (piece.get_table_id() != kSingleTableId ||
      piece.get_partition_id() >= config_.partition_count)
    TransportFatal(config_.node_id, "dispatch", "invalid table or partition", &message);
  if (IsResponseType(piece.get_message_type()))
    ConsumeTransportResponse(piece, mailbox);
  else
    ServeTransportRequest(message, piece, mailbox);
}

void KVEngine::ConsumeTransportResponse(star::MessagePiece piece,
                                        WorkerMailbox &mailbox) {
  auto &operation = mailbox.operation;
  if (operation.expected_response_type == 0 || operation.done ||
      piece.get_message_type() != operation.expected_response_type ||
      piece.get_partition_id() != operation.partition_id)
    TransportFatal(config_.node_id, "response", "unexpected response for worker operation");
  auto input = piece.toStringPiece();
  uint32_t key_offset = 0;
  if (piece.get_message_type() == static_cast<uint32_t>(star::TwoPLPashaMessage::REMOTE_DELETE_RESPONSE)) {
    if (input.size() != 0)
      TransportFatal(config_.node_id, "response", "remote delete completion has payload");
    operation.result = Status::Ok();
  } else if (piece.get_message_type() == static_cast<uint32_t>(
                 star::TwoPLPashaMessage::DATA_MIGRATION_RESPONSE_FOR_SCAN)) {
    bool success = false;
    star::Decoder decoder(input);
    decoder >> success >> key_offset;
    if (decoder.size() != 0 || key_offset != 0)
      TransportFatal(config_.node_id, "response", "malformed scan migration response");
    operation.result = success ? Status::Ok()
                               : Status::Error(StatusCode::kBusy, "scan migration failed");
  } else {
    uint8_t raw_result = 0;
    star::Decoder decoder(input);
    decoder >> raw_result >> key_offset;
    if (decoder.size() != 0 || key_offset != 0 || raw_result > static_cast<uint8_t>(RpcResult::kNoMemory))
      TransportFatal(config_.node_id, "response", "malformed RPC result response");
    operation.result = ResultStatus(static_cast<RpcResult>(raw_result), "owner RPC");
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
      mailbox, message.get_source_node_id(), message.get_transaction_id());
  const auto make_result_response = [&](uint32_t response_type, RpcResult result,
                                        uint32_t key_offset) {
    const auto size = star::MessagePiece::get_header_size() + sizeof(uint8_t) + sizeof(key_offset);
    star::Encoder encoder(response.data);
    encoder << star::MessagePiece::construct_message_piece_header(
        response_type, size, kSingleTableId, piece.get_partition_id());
    encoder << static_cast<uint8_t>(result) << key_offset;
    response.flush();
  };
  const auto request_bytes = piece.toStringPiece();
  if (type == static_cast<uint32_t>(star::TwoPLPashaMessage::DATA_MIGRATION_REQUEST)) {
    if (piece.get_message_length() != star::MessagePiece::get_header_size() +
            config_.fixed_key_size + sizeof(uint64_t) + sizeof(uint32_t))
      TransportFatal(config_.node_id, "request", "bad migration request length", &message);
    const std::string_view key(request_bytes.data(), config_.fixed_key_size);
    auto trailing = request_bytes;
    trailing.remove_prefix(config_.fixed_key_size);
    uint64_t transaction_id = 0;
    uint32_t key_offset = 0;
    star::Decoder decoder(trailing);
    decoder >> transaction_id >> key_offset;
    if (decoder.size() != 0 || key_offset != 0)
      TransportFatal(config_.node_id, "request", "bad migration request fields", &message);
    auto *partition = OwnedPartition(key);
    if (partition == nullptr || partition->partition_id() != piece.get_partition_id()) {
      std::ostringstream detail;
      detail << "migration delivered to non-owner wire_partition="
             << piece.get_partition_id() << " computed_partition="
             << RouteForKey(key).partition_id << " computed_owner="
             << RouteForKey(key).owner;
      TransportFatal(config_.node_id, "request", detail.str().c_str(), &message);
    }
    bool moved_in = false;
    StatusCode status = StatusCode::kCorruption;
    try {
      status = partition->EnsureInShared(key, message.get_source_node_id(), &moved_in);
    } catch (const std::bad_alloc &) {
      status = StatusCode::kOutOfMemory;
    }
    if (status == StatusCode::kOk && moved_in) {
      migration_in_.fetch_add(1, std::memory_order_relaxed);
      shared_swcc_flushes_.fetch_add(1, std::memory_order_relaxed);
    }
    make_result_response(static_cast<uint32_t>(star::TwoPLPashaMessage::DATA_MIGRATION_RESPONSE),
                         EncodeResult(status), key_offset);
    SendTransportMessage(response);
    response.clear_message_pieces();
    if (status == StatusCode::kOk) EnforceMigrationBudget(*partition);
    return;
  }
  if (type == static_cast<uint32_t>(star::TwoPLPashaMessage::REMOTE_INSERT_REQUEST)) {
    if (piece.get_message_length() != star::MessagePiece::get_header_size() +
            config_.fixed_key_size + config_.fixed_value_size + sizeof(uint64_t) + sizeof(uint32_t))
      TransportFatal(config_.node_id, "request", "bad remote insert request length", &message);
    const std::string_view key(request_bytes.data(), config_.fixed_key_size);
    const std::string_view value(request_bytes.data() + config_.fixed_key_size,
                                 config_.fixed_value_size);
    auto trailing = request_bytes;
    trailing.remove_prefix(config_.fixed_key_size + config_.fixed_value_size);
    uint64_t transaction_id = 0;
    uint32_t key_offset = 0;
    star::Decoder decoder(trailing);
    decoder >> transaction_id >> key_offset;
    if (decoder.size() != 0 || key_offset != 0)
      TransportFatal(config_.node_id, "request", "bad remote insert request fields", &message);
    auto *partition = OwnedPartition(key);
    if (partition == nullptr || partition->partition_id() != piece.get_partition_id())
      TransportFatal(config_.node_id, "request", "insert delivered to non-owner", &message);
    StatusCode status = StatusCode::kCorruption;
    try {
      status = partition->InsertRemotePlaceholder(key, value, message.get_source_node_id());
    } catch (const std::bad_alloc &) {
      status = StatusCode::kOutOfMemory;
    } catch (const std::runtime_error &error) {
      status = std::string_view(error.what()).find("busy") != std::string_view::npos
          ? StatusCode::kBusy : StatusCode::kCorruption;
    }
    make_result_response(static_cast<uint32_t>(star::TwoPLPashaMessage::REMOTE_INSERT_RESPONSE),
                         EncodeResult(status), key_offset);
    SendTransportMessage(response);
    response.clear_message_pieces();
    return;
  }
  if (type == static_cast<uint32_t>(star::TwoPLPashaMessage::REMOTE_DELETE_REQUEST)) {
    if (piece.get_message_length() != star::MessagePiece::get_header_size() + config_.fixed_key_size)
      TransportFatal(config_.node_id, "request", "bad remote delete request length", &message);
    const std::string_view key(request_bytes.data(), config_.fixed_key_size);
    auto *partition = OwnedPartition(key);
    if (partition == nullptr || star::migration_manager == nullptr)
      TransportFatal(config_.node_id, "request", "delete delivered to non-owner", &message);
    const FixedKey fixed_key = FixedKey::From(key, config_.fixed_key_size);
    if (!star::migration_manager->delete_specific_row_and_move_out(
            table, &fixed_key, /*is_delete_local=*/false))
      TransportFatal(config_.node_id, "request", "owner delete lost prepared row", &message);
    const auto size = star::MessagePiece::get_header_size();
    star::Encoder encoder(response.data);
    encoder << star::MessagePiece::construct_message_piece_header(
        static_cast<uint32_t>(star::TwoPLPashaMessage::REMOTE_DELETE_RESPONSE),
        size, kSingleTableId, piece.get_partition_id());
    response.flush();
    SendTransportMessage(response);
    response.clear_message_pieces();
    return;
  }
  if (type == static_cast<uint32_t>(star::TwoPLPashaMessage::DATA_MIGRATION_REQUEST_FOR_SCAN)) {
    if (piece.get_message_length() != star::MessagePiece::get_header_size() +
            config_.fixed_key_size * 2 + sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint32_t))
      TransportFatal(config_.node_id, "request", "bad scan migration request length", &message);
    const std::string_view min_key(request_bytes.data(), config_.fixed_key_size);
    const std::string_view max_key(request_bytes.data() + config_.fixed_key_size,
                                   config_.fixed_key_size);
    auto trailing = request_bytes;
    trailing.remove_prefix(config_.fixed_key_size * 2);
    uint64_t limit = 0, transaction_id = 0;
    uint32_t key_offset = 0;
    star::Decoder decoder(trailing);
    decoder >> limit >> transaction_id >> key_offset;
    if (decoder.size() != 0 || key_offset != 0)
      TransportFatal(config_.node_id, "request", "bad scan migration request fields", &message);
    const Status status = PreparePartitionSharedScan(
        piece.get_partition_id(), min_key, max_key, limit);
    const bool success = status.ok();
    const auto size = star::MessagePiece::get_header_size() + sizeof(success) + sizeof(key_offset);
    star::Encoder encoder(response.data);
    encoder << star::MessagePiece::construct_message_piece_header(
        static_cast<uint32_t>(star::TwoPLPashaMessage::DATA_MIGRATION_RESPONSE_FOR_SCAN),
        size, kSingleTableId, piece.get_partition_id());
    encoder << success << key_offset;
    response.flush();
    SendTransportMessage(response);
    response.clear_message_pieces();
    if (success) EnforceMigrationBudget(*partitions_[piece.get_partition_id()]);
    return;
  }
  TransportFatal(config_.node_id, "request", "unsupported original MessagePiece", &message);
}

void KVEngine::EnforceMigrationBudget(KVPartition &partition) {
  // Use the Open-time clamp after static domains (§11.10), not raw config.
  const uint64_t hw_budget = owner_migration_dynamic_budget_bytes_;
  const uint64_t hw_used = partition.hwcc_used_bytes();
  if (hw_used < hw_budget) return;
  // PolicyClock's original policy is governed solely by its HWCC accounting.
  KvMigrationRuntime::SyncHwCcUsage(partition);
  if (star::migration_manager != nullptr &&
      star::migration_manager->move_row_out(partition.partition_id())) {
    migration_out_.fetch_add(1, std::memory_order_relaxed);
  }
  // An original Clock pass may consume only second chances, or find pinned
  // rows.  The migration already acknowledged above remains valid; leave the
  // next OnDemand pass to resume from the original cursor instead of turning
  // this normal policy result into a synthetic OOM.
}

Status KVEngine::MoveOut(std::string_view key) {
  if (IsInternalMaxSentinel(key))
    return Status::Error(StatusCode::kInvalidArgument,
                         "internal max sentinel is reserved");
  const KeyRoute route = RouteForKey(key);
  if (!route.owned_by_this_node)
    return Status::Error(StatusCode::kOwnerViolation, "remote owner requires forwarding");
  auto *partition = route.partition;
  // Test/fixture-only deterministic move-out. Production eviction continues
  // through PolicyClock::move_row_out from EnforceMigrationBudget.
  if (partition->MoveOutPrivate(key, config_.node_id)) {
    migration_out_.fetch_add(1, std::memory_order_relaxed);
    return Status::Ok();
  }
  return Status::Error(StatusCode::kNotFound, "shared key not found or busy");
}

}  // namespace tigonkv::engine
