#include "kv/engine/kv_engine.h"

#include "common/CXL_EBR.h"
#include "common/MPSCRingBuffer.h"
#include "kv/engine/kv_partition.h"
#include "kv/engine/kv_migration.h"
#include "kv/engine/mem_access.h"
#include "kv/engine/kv_messages.h"
#include "protocol/TwoPLPasha/TwoPLPashaSCCWriteThrough.h"

#include <stdexcept>
#include <thread>
#include <chrono>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <queue>
#include <sstream>
#include <fstream>
#include <sched.h>
#include <unistd.h>

namespace tigonkv::engine {
namespace {

uint64_t Hash(std::string_view key) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : key) { h ^= c; h *= 1099511628211ULL; }
  return h;
}

// Multiple foreground KVEngine instances can coexist in one VM. Transport
// responses are delivered through a node-wide ring, so their request IDs must
// be unique across the process rather than merely per engine instance.
std::atomic<uint64_t> ProcessRequestSequence{1};

uint64_t NextRequestId(uint32_t node_id) {
  constexpr uint64_t kSequenceMask = (1ULL << 56) - 1;
  const uint64_t sequence = ProcessRequestSequence.fetch_add(1, std::memory_order_relaxed);
  return (static_cast<uint64_t>(node_id) << 56) | (sequence & kSequenceMask);
}

uint64_t CurrentRssKb() {
  std::ifstream statm("/proc/self/statm");
  uint64_t pages = 0;
  uint64_t resident = 0;
  if (!(statm >> pages >> resident)) return 0;
  const long page_size = ::sysconf(_SC_PAGESIZE);
  return page_size > 0 ? resident * static_cast<uint64_t>(page_size) / 1024 : 0;
}

// While serving a request (or walking owned trees for a local Scan), nested
// PollTransport must not pop/serve deferred requests. Handling another
// Put/Get request here mutates the same B+trees under OLC and livelocks
// scan() restart loops (GDB: yield counts > 10M on YCSB-E 4x4).
// Response/item/done traffic is applied by the inbound demuxer thread, so
// nested FG polls only need to skip ServeDeferred.
thread_local uint32_t TlsRequestServeDepth = 0;

struct RequestServeDepthGuard {
  RequestServeDepthGuard() { ++TlsRequestServeDepth; }
  ~RequestServeDepthGuard() { --TlsRequestServeDepth; }
  RequestServeDepthGuard(const RequestServeDepthGuard &) = delete;
  RequestServeDepthGuard &operator=(const RequestServeDepthGuard &) = delete;
};

[[noreturn]] void TransportFatal(uint32_t node_id, const char *stage,
                                 const char *detail,
                                 const KvMessage *message = nullptr) {
  std::fprintf(stderr,
      "TIGONKV_TRANSPORT_FATAL node=%u stage=%s detail=%s",
      node_id, stage, detail);
  if (message != nullptr) {
    std::fprintf(stderr,
        " type=%u source=%u destination=%u request_id=%llu key_size=%u value_size=%u",
        static_cast<unsigned>(message->type), message->source_node,
        message->destination_node,
        static_cast<unsigned long long>(message->request_id),
        message->key_size, message->value_size);
  }
  std::fputc('\n', stderr);
  std::fflush(stderr);
  std::abort();
}

bool ValidMessageType(KvMessageType type) {
  switch (type) {
    case KvMessageType::kPut:
    case KvMessageType::kDelete:
    case KvMessageType::kIncrement:
    case KvMessageType::kCas:
    case KvMessageType::kResponse:
    case KvMessageType::kMigrate:
    case KvMessageType::kScanMigrate:
      return true;
    case KvMessageType::kGet:  // reserved; owner-value GET deleted (§10.4)
    case KvMessageType::kCasCommitReserved:
      return false;
    default:
      return false;
  }
}

bool ValidStatusCode(uint32_t status) {
  return status <= static_cast<uint32_t>(StatusCode::kBusy);
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
  region.config_hash = Hash(config.shared_memory_path) ^
                       (static_cast<uint64_t>(config.partition_count) << 32) ^
                       config.vm_count ^ config.fixed_value_size;
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
      worker_owners_(config.foreground_worker_count_per_vm) {}

KVEngine::~KVEngine() {
  StopInboundDemuxer();
  if (star::scc_manager == scc_.get()) star::scc_manager = nullptr;
  if (star::global_ebr_meta == ebr_) star::global_ebr_meta = nullptr;
  KvMigrationRuntime::Instance().Reset();
}

std::unique_ptr<KVEngine> KVEngine::Open(const Config &config, bool reset) {
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
  for (uint32_t partition = 0; partition < config.partition_count; ++partition) {
    const auto &directory = engine->pool_->allocator().layout().partitions[partition];
    // All VMs reconstruct the persistent roots so a non-owner can use the
    // shared-tree fast path after promotion.  Only the owner is allowed to
    // invoke private-row operations; binding the partition to its stable owner
    // keeps its private arena and tree nodes in the correct allocation shard.
    mem_access::HwccRead(&directory.private_root,
                         sizeof(directory.private_root));
    const RegionOffset private_root = directory.private_root;
    mem_access::HwccAtomicLoad(&directory.shared_root);
    const bool attach = private_root != kNullOffset &&
                        directory.shared_root.load(std::memory_order_acquire) !=
                            kNullOffset;
    engine->partitions_.emplace_back(std::make_unique<KVPartition>(
        engine->pool_->allocator(), *engine->ebr_, partition,
        engine->OwnerForPartition(partition), attach));
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
  const uint64_t hw_budget = (config.hw_cc_budget_mb * 1024ULL * 1024ULL -
      star::CXL_EBR::max_ebr_retiring_memory) / config.vm_count;
  KvMigrationRuntime::Instance().Install(
      partition_ptrs, config.fixed_key_size, config.fixed_value_size,
      config.node_id, config.partition_count, hw_budget);
  // Clock list head/tail/cursor and PrivateRow links persist in SWCC; attach
  // does not rebuild a process-heap tracker (§11.14).
  engine->StartInboundDemuxer();
  if (reset) engine->pool_->allocator().PublishReady();
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
  route.partition_id =
      static_cast<uint32_t>(Hash(key) % config_.partition_count);
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
  MarkLayoutDirty();
  const KeyRoute route = RouteForKey(key);
  if (!route.owned_by_this_node) {
    for (int attempt = 0; attempt < 8; ++attempt) {
      if (route.partition == nullptr) break;
      const SharedAccessState state =
          route.partition->PutShared(key, config_.node_id, value);
      if (state == SharedAccessState::kDone) {
        shared_puts_.fetch_add(1, std::memory_order_relaxed);
        shared_swcc_flushes_.fetch_add(1, std::memory_order_relaxed);
        return Status::Ok();
      }
      if (state == SharedAccessState::kRetry) {
        std::this_thread::yield();
        continue;
      }
      break;  // kMissing → Forward
    }
    return Forward(KvMessageType::kPut, key, value, nullptr, route.owner);
  }
  try {
    route.partition->PutPrivate(key, value);
    return Status::Ok();
  } catch (const std::bad_alloc &) {
    return Status::Error(StatusCode::kOutOfMemory, "private arena exhausted");
  } catch (const std::runtime_error &e) {
    if (std::string_view(e.what()).find("busy") != std::string_view::npos)
      return Status::Error(StatusCode::kBusy, e.what());
    return Status::Error(StatusCode::kCorruption, e.what());
  } catch (const std::exception &e) {
    return Status::Error(StatusCode::kCorruption, e.what());
  }
}

GetResult KVEngine::Get(std::string_view key) {
  MarkLayoutDirty();  // A remote miss may perform the original move-in.
  const KeyRoute route = RouteForKey(key);
  if (!route.owned_by_this_node) {
    auto *visible = route.partition;
    for (int attempt = 0; attempt < 8; ++attempt) {
      std::string shared;
      if (visible == nullptr) break;
      const SharedAccessState state =
          visible->GetShared(key, config_.node_id, &shared);
      if (state == SharedAccessState::kDone) {
        shared_gets_.fetch_add(1, std::memory_order_relaxed);
        return {Status::Ok(), std::move(shared)};
      }
      if (state == SharedAccessState::kRetry) {
        std::this_thread::yield();
        continue;
      }
      // kMissing → migrate then retry shared.
      const Status migrated =
          Forward(KvMessageType::kMigrate, key, {}, nullptr, route.owner);
      if (migrated.code == StatusCode::kNotFound)
        return {Status::Error(StatusCode::kNotFound, "key not found"), {}};
      if (!migrated.ok() && migrated.code != StatusCode::kOutOfMemory)
        return {migrated, {}};
      if (migrated.code == StatusCode::kOutOfMemory && attempt == 7)
        return {migrated, {}};
    }
    return {Status::Error(StatusCode::kBusy, "shared get retry budget exceeded"),
            {}};
  }
  try {
    std::string value;
    return route.partition->GetPrivate(key, &value)
               ? GetResult{Status::Ok(), std::move(value)}
               : GetResult{Status::Error(StatusCode::kNotFound, "key not found"),
                           {}};
  } catch (const std::runtime_error &e) {
    if (std::string_view(e.what()).find("busy") != std::string_view::npos)
      return {Status::Error(StatusCode::kBusy, e.what()), {}};
    return {Status::Error(StatusCode::kCorruption, e.what()), {}};
  }
}

Status KVEngine::Delete(std::string_view key) {
  MarkLayoutDirty();
  const KeyRoute route = RouteForKey(key);
  if (!route.owned_by_this_node)
    return Forward(KvMessageType::kDelete, key, {}, nullptr, route.owner);
  try {
    return route.partition->DeletePrivate(key)
               ? Status::Ok()
               : Status::Error(StatusCode::kNotFound, "key not found");
  } catch (const std::runtime_error &e) {
    if (std::string_view(e.what()).find("busy") != std::string_view::npos)
      return Status::Error(StatusCode::kBusy, e.what());
    return Status::Error(StatusCode::kCorruption, e.what());
  }
}

ScanResult KVEngine::Scan(std::string_view start_key, uint64_t limit) {
  constexpr uint64_t kScanSafetyLimit = 1024 * 1024;
  if (limit > kScanSafetyLimit)
    return {Status::Error(StatusCode::kInvalidArgument, "scan limit exceeds safety cap"), {}};
  constexpr uint64_t kPageSize = 64;
  const uint64_t target = limit == 0 ? kScanSafetyLimit : limit;
  const auto scan_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);

  struct Source {
    uint32_t partition_id = 0;
    uint32_t owner = 0;
    bool local = false;
    std::string cursor;
    bool has_cursor = false;
    bool owner_exhausted_for_cursor = false;
    bool owner_no_predecessor_for_cursor = false;
    uint32_t migration_attempts = 0;
    bool more = false;
    std::vector<ScanItem> items;
    size_t next = 0;
  };

  auto load_items = [&](Source *source,
                        std::vector<std::pair<std::string, std::string>> raw,
                        bool more) {
    source->items.clear();
    source->next = 0;
    for (auto &item : raw) {
      if (source->has_cursor && item.first <= source->cursor) continue;
      source->items.push_back({std::move(item.first), std::move(item.second)});
      if (source->items.size() == kPageSize) break;
    }
    // If every returned key was a cursor duplicate, this source made no
    // progress; clearing more prevents an infinite refill loop (§6).
    source->more = more && !source->items.empty();
  };

  auto probe_remote = [&](Source *source) -> Status {
    auto *partition = partitions_[source->partition_id].get();
    constexpr uint32_t kMaxMigrations = 4;
    for (;;) {
      if (std::chrono::steady_clock::now() >= scan_deadline)
        return Status::Error(StatusCode::kBusy, "CXL scan retry deadline exceeded");
      const uint64_t page_cap = std::min<uint64_t>(
          kPageSize, std::max<uint64_t>(1, (target + config_.partition_count - 1) /
                                               config_.partition_count));
      const std::string_view cursor =
          source->has_cursor ? std::string_view(source->cursor) : start_key;
      KVPartition::SharedScanProbeResult probe;
      {
        RequestServeDepthGuard depth_guard;
        probe = partition->ProbeSharedScanPage(
            cursor, page_cap, source->owner_exhausted_for_cursor,
            source->has_cursor, source->owner_no_predecessor_for_cursor);
      }
      PollTransport();
      source->owner_exhausted_for_cursor = false;
      source->owner_no_predecessor_for_cursor = false;
      if (!probe.status.ok()) {
        if (probe.status.code == StatusCode::kBusy) {
          std::this_thread::yield();
          continue;
        }
        return probe.status;
      }
      if (probe.migration_required) {
        if (source->migration_attempts >= kMaxMigrations)
          return Status::Error(StatusCode::kBusy,
                               "partition scan migration attempt limit");
        ++source->migration_attempts;
        bool exhausted = false;
        bool no_predecessor = false;
        const uint32_t flags =
            source->has_cursor ? kScanMigrateFlagCursorDuplicate : 0;
        const auto payload =
            EncodeScanMigrateRequest(source->partition_id, flags, page_cap);
        const uint64_t request_id = NextRequestId(config_.node_id);
        auto pending = RegisterPendingResponse(request_id);
        try {
          SendTransportMessage(MakeRequest(
              KvMessageType::kScanMigrate, config_.node_id, source->owner,
              request_id, cursor, payload));
        } catch (...) {
          RemovePendingResponse(request_id);
          throw;
        }
        std::string response_value;
        const Status migrated =
            AwaitResponse(request_id, pending, &response_value);
        if (!migrated.ok()) {
          if (migrated.code == StatusCode::kBusy) {
            PollTransport();
            std::this_thread::yield();
            continue;
          }
          return migrated;
        }
        uint32_t resp_part = 0;
        if (!DecodeScanMigrateResponse(response_value, &resp_part, &exhausted,
                                       &no_predecessor) ||
            resp_part != source->partition_id)
          return Status::Error(StatusCode::kCorruption,
                               "malformed scan migrate response");
        source->owner_exhausted_for_cursor = exhausted;
        source->owner_no_predecessor_for_cursor = no_predecessor;
        continue;
      }
      if (!probe.scan_success)
        return Status::Error(StatusCode::kCorruption, "CXL probe incomplete");
      load_items(source, std::move(probe.items), probe.more);
      source->migration_attempts = 0;
      return Status::Ok();
    }
  };

  auto probe_local = [&](Source *source) -> Status {
    auto *partition = partitions_[source->partition_id].get();
    const uint64_t page_cap = std::min<uint64_t>(
        kPageSize, std::max<uint64_t>(1, (target + config_.partition_count - 1) /
                                             config_.partition_count));
    const uint64_t fetch = page_cap + 1;
    const std::string_view cursor =
        source->has_cursor ? std::string_view(source->cursor) : start_key;
    std::vector<std::pair<std::string, std::string>> items;
    const std::function<void()> progress = [this] { PollTransport(); };
    bool ok = false;
    {
      RequestServeDepthGuard depth_guard;
      ok = partition->ScanOwned(cursor, fetch, &items, &progress);
    }
    PollTransport();
    if (!ok)
      return Status::Error(StatusCode::kBusy,
                           "owner scan contention/retry budget");
    const bool more = items.size() == fetch;
    if (more && !items.empty()) items.pop_back();
    load_items(source, std::move(items), more);
    return Status::Ok();
  };

  std::vector<Source> sources;
  sources.reserve(config_.partition_count);
  for (uint32_t partition_id = 0; partition_id < config_.partition_count;
       ++partition_id) {
    Source source;
    source.partition_id = partition_id;
    source.owner = OwnerForPartition(partition_id);
    source.local = (source.owner == config_.node_id);
    Status status = source.local ? probe_local(&source) : probe_remote(&source);
    if (!status.ok()) return {status, {}};
    sources.push_back(std::move(source));
  }

  auto refill = [&](Source *source) -> Status {
    if (!source->more) return Status::Ok();
    return source->local ? probe_local(source) : probe_remote(source);
  };

  struct HeapItem { std::string_view key; size_t source; };
  auto compare = [](const HeapItem &left, const HeapItem &right) {
    return left.key > right.key;
  };
  std::priority_queue<HeapItem, std::vector<HeapItem>, decltype(compare)> heap(
      compare);
  for (size_t i = 0; i < sources.size(); ++i) {
    if (!sources[i].items.empty()) heap.push({sources[i].items[0].key, i});
  }
  ScanResult result{Status::Ok(), {}};
  while (!heap.empty() && result.items.size() < target) {
    const HeapItem item = heap.top();
    heap.pop();
    Source &source = sources[item.source];
    ScanItem row = std::move(source.items[source.next++]);
    source.cursor = row.key;
    source.has_cursor = true;
    if (result.items.empty() || result.items.back().key != row.key)
      result.items.push_back(std::move(row));
    if (source.next == source.items.size()) {
      const Status status = refill(&source);
      if (!status.ok()) return {status, {}};
    }
    if (source.next < source.items.size())
      heap.push({source.items[source.next].key, item.source});
  }
  if (limit == 0 && result.items.size() == kScanSafetyLimit &&
      (!heap.empty() ||
       std::any_of(sources.begin(), sources.end(),
                   [](const Source &source) { return source.more; })))
    return {Status::Error(StatusCode::kInvalidArgument,
                          "scan result exceeds safety cap"),
            {}};
  return result;
}

Status KVEngine::PreparePartitionSharedScan(
    uint32_t partition_id, std::string_view start_key, bool cursor_is_duplicate,
    uint64_t output_limit, uint32_t requester, bool *exhausted_out,
    bool *no_predecessor_out) {
  if (exhausted_out == nullptr)
    return Status::Error(StatusCode::kInvalidArgument, "null exhausted_out");
  *exhausted_out = false;
  if (no_predecessor_out != nullptr) *no_predecessor_out = false;
  constexpr uint64_t kPageSize = 64;
  if (partition_id >= partitions_.size() ||
      partition_id >= config_.partition_count)
    return Status::Error(StatusCode::kInvalidArgument,
                         "scan migrate partition_id out of range");
  if (OwnerForPartition(partition_id) != config_.node_id)
    return Status::Error(StatusCode::kOwnerViolation,
                         "scan migrate routed to non-owner");
  if (output_limit == 0 || output_limit > kPageSize)
    return Status::Error(StatusCode::kInvalidArgument,
                         "scan migrate output_limit out of range");
  auto *partition = partitions_[partition_id].get();
  const uint64_t fetch =
      output_limit + (cursor_is_duplicate ? 1 : 0) + 1;  // +1 right boundary
  std::vector<std::string> keys;
  if (!partition->ScanOwnedKeys(start_key, fetch, &keys))
    return Status::Error(StatusCode::kCorruption,
                         "owner key-only range scan failed");
  *exhausted_out = keys.size() < fetch;
  // Open left edge: first key > start and its private predecessor is absent or
  // strictly below start (outside this page's move-in set) (§4.4/§4.5).
  if (!keys.empty() &&
      FixedKey::From(keys.front(), config_.fixed_key_size)
              .Compare(FixedKey::From(start_key, config_.fixed_key_size)) > 0) {
    std::string predecessor;
    if (!partition->PrivatePredecessorKey(keys.front(), &predecessor)) {
      if (no_predecessor_out != nullptr) *no_predecessor_out = true;
    } else if (FixedKey::From(predecessor, config_.fixed_key_size)
                       .Compare(FixedKey::From(start_key, config_.fixed_key_size)) <
                   0 &&
               no_predecessor_out != nullptr) {
      *no_predecessor_out = true;
    }
  } else if (keys.empty() && no_predecessor_out != nullptr) {
    *no_predecessor_out = true;
  }
  for (const auto &key : keys) {
    bool moved_in = false;
    const StatusCode code =
        partition->EnsureInShared(key, requester, &moved_in);
    // Original TwoPLPashaMessage.h:353-361: NotFound is a race, skip the key.
    if (code == StatusCode::kNotFound) continue;
    if (code != StatusCode::kOk)
      return Status::Error(code, "partition scan range move-in failed");
    if (moved_in) {
      migration_in_.fetch_add(1, std::memory_order_relaxed);
      shared_swcc_flushes_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  // One OnDemand Clock move-out after this partition's preparation (§5.2).
  KvMigrationRuntime::SyncHwCcUsage(*partition);
  partition->MoveOutClockVictim(config_.node_id);
  return Status::Ok();
}

CasResult KVEngine::CompareExchange(std::string_view key,
                                    std::string_view expected,
                                    std::string_view desired) {
  MarkLayoutDirty();
  const KeyRoute route = RouteForKey(key);
  if (!route.owned_by_this_node) {
    auto *visible = route.partition;
    for (int attempt = 0; attempt < 8; ++attempt) {
      bool exchanged = false;
      if (visible == nullptr) break;
      const SharedAccessState state = visible->CompareExchangeShared(
          key, config_.node_id, expected, desired, &exchanged);
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
      if (state == SharedAccessState::kRetry) {
        std::this_thread::yield();
        continue;
      }
      break;  // kMissing
    }
    // Shared miss: single CAS_FWD with combined payload (§10.5).
    return ForwardCompareExchange(key, expected, desired);
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
    if (std::string_view(e.what()).find("busy") != std::string_view::npos)
      return {Status::Error(StatusCode::kBusy, e.what()), false};
    return {Status::Error(StatusCode::kInvalidArgument, e.what()), false};
  } catch (const std::exception &e) {
    return {Status::Error(StatusCode::kInvalidArgument, e.what()), false};
  }
}

IncrementResult KVEngine::Increment(std::string_view key, int64_t delta) {
  MarkLayoutDirty();
  const KeyRoute route = RouteForKey(key);
  if (!route.owned_by_this_node) {
    auto *visible = route.partition;
    for (int attempt = 0; attempt < 8; ++attempt) {
      int64_t shared = 0;
      if (visible == nullptr) break;
      const SharedAccessState state =
          visible->IncrementShared(key, config_.node_id, delta, &shared);
      if (state == SharedAccessState::kDone) {
        shared_puts_.fetch_add(1, std::memory_order_relaxed);
        shared_swcc_flushes_.fetch_add(1, std::memory_order_relaxed);
        return {Status::Ok(), shared};
      }
      if (state == SharedAccessState::kRetry) {
        std::this_thread::yield();
        continue;
      }
      break;
    }
    std::string value;
    const auto status = Forward(KvMessageType::kIncrement, key,
                                std::to_string(delta), &value, route.owner);
    if (!status.ok()) return {status, 0};
    int64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
      return {Status::Error(StatusCode::kCorruption, "malformed forwarded increment response"), 0};
    return {Status::Ok(), result};
  }
  try {
    int64_t value = 0;
    if (!route.partition->IncrementPrivate(key, delta, &value))
      return {Status::Error(StatusCode::kNotFound, "key not found"), 0};
    return {Status::Ok(), value};
  } catch (const std::invalid_argument &e) {
    return {Status::Error(StatusCode::kInvalidArgument, e.what()), 0};
  } catch (const std::runtime_error &e) {
    if (std::string_view(e.what()).find("busy") != std::string_view::npos)
      return {Status::Error(StatusCode::kBusy, e.what()), 0};
    return {Status::Error(StatusCode::kCorruption, e.what()), 0};
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
  for (size_t domain = 0; domain < static_cast<size_t>(AllocationDomain::kOwnerPrivateSwcc);
       ++domain)
    stats.logical_hwcc_used_bytes += layout.domains[domain].used_bytes.load(std::memory_order_relaxed);
  stats.physical_hwcc_used_bytes = stats.logical_hwcc_used_bytes;
  stats.owner_private_swcc_used_bytes = layout.domains[static_cast<size_t>(
      AllocationDomain::kOwnerPrivateSwcc)].used_bytes.load(std::memory_order_relaxed);
  stats.shared_payload_swcc_used_bytes = layout.domains[static_cast<size_t>(
      AllocationDomain::kSharedPayloadSwcc)].used_bytes.load(std::memory_order_relaxed);
  stats.allocator_hwcc_metadata_bytes = layout.domains[static_cast<size_t>(
      AllocationDomain::kHwccAllocatorMetadata)].used_bytes.load(std::memory_order_relaxed);
  stats.allocator_swcc_metadata_bytes = layout.domains[static_cast<size_t>(
      AllocationDomain::kSwccAllocatorMetadata)].used_bytes.load(std::memory_order_relaxed);
  stats.allocator_shared_overhead_bytes =
      stats.allocator_hwcc_metadata_bytes +
      stats.allocator_swcc_metadata_bytes;
  stats.physical_swcc_used_bytes =
      stats.owner_private_swcc_used_bytes +
      stats.shared_payload_swcc_used_bytes +
      stats.allocator_swcc_metadata_bytes;
  for (const auto &partition : partitions_)
    stats.active_shared_rows += partition->migrated_key_count();
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
  stats.abandoned_responses =
      abandoned_responses_.load(std::memory_order_relaxed);
  stats.network_tx_bytes = NetworkTxBytes();
  stats.network_rx_bytes = NetworkRxBytes();
  return stats;
}

Status KVEngine::Checkpoint() {
  try {
    // Poll a bounded batch before reclamation. Synchronous forwarders also
    // poll, so this only drains requests already visible to this VM.
    for (uint32_t i = 0; i < 1024; ++i) PollTransport();
    ebr_->drain_quiescent();
    pool_->allocator().FlushCheckpointRanges(
        config_.node_id, std::chrono::seconds(config_.sync_timeout_sec));
    layout_dirty_.store(false, std::memory_order_release);
    return Status::Ok();
  } catch (const std::exception &e) {
    return Status::Error(StatusCode::kCorruption, e.what());
  }
}

void KVEngine::MarkLayoutDirty() {
  if (!layout_dirty_.exchange(true, std::memory_order_acq_rel))
    pool_->allocator().MarkDirty();
}

void KVEngine::SendTransportMessage(const KvMessage &message) {
  if (rings_ == nullptr || !ValidMessageType(message.type) ||
      message.source_node >= config_.vm_count ||
      message.destination_node >= config_.vm_count ||
      message.key_size > message.key.size() ||
      message.value_size > message.value.size() ||
      !ValidStatusCode(message.status))
    TransportFatal(config_.node_id, "send_validate",
                   "invalid outgoing KV transport message", &message);
  // A deferred owner request has an isolated latency scope. Pay all database
  // access delay after its locks/EBR guards have been released but before the
  // response becomes visible in the peer's ring.
  mem_access::DelayIsolatedScopeNow();
  unsigned spins = 0;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(config_.sync_timeout_sec);
  const size_t wire_size = WireSize(message);
  for (;;) {
    bool enqueued = false;
    try {
      enqueued = rings_[message.destination_node].enqueue(
          const_cast<char *>(reinterpret_cast<const char *>(&message)),
          wire_size);
    } catch (const std::exception &error) {
      TransportFatal(config_.node_id, "ring_enqueue", error.what(), &message);
    } catch (...) {
      TransportFatal(config_.node_id, "ring_enqueue", "non-std exception",
                     &message);
    }
    if (enqueued) break;
    // A failed full-ring reservation/rollback is itself an HWCC access.
    // Settle it before host-speed retries can amplify probe traffic.
    mem_access::DelayActiveScopeNow();
    if (std::chrono::steady_clock::now() >= deadline) {
      const auto snapshot = rings_[message.destination_node].snapshot();
      std::ostringstream detail;
      detail << "transport enqueue timeout destination="
             << message.destination_node << " request_id=" << message.request_id
             << " head=" << snapshot.head << " tail=" << snapshot.tail
             << " count=" << snapshot.count
             << " entries=" << snapshot.entries;
      TransportFatal(config_.node_id, "ring_enqueue_timeout",
                     detail.str().c_str(), &message);
    }
    // Peer demuxers free ring slots. When called from nested Serve (depth>0)
    // we must not ServeDeferred, but top-level Send during Await may briefly
    // help demux by yielding; sleeping avoids tight livelock on full rings.
    if ((++spins & 63u) == 0)
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    else
      std::this_thread::yield();
  }
  network_tx_bytes_.fetch_add(wire_size, std::memory_order_relaxed);
}

Status KVEngine::Forward(KvMessageType type, std::string_view key, std::string_view value,
                         std::string *response_value) {
  return Forward(type, key, value, response_value, OwnerForKey(key));
}

Status KVEngine::Forward(KvMessageType type, std::string_view key, std::string_view value,
                         std::string *response_value, uint32_t owner) {
  const uint64_t request_id = NextRequestId(config_.node_id);
  auto pending = RegisterPendingResponse(request_id);
  try {
    SendTransportMessage(
        MakeRequest(type, config_.node_id, owner, request_id, key, value));
  } catch (...) {
    RemovePendingResponse(request_id);
    throw;
  }
  return AwaitResponse(request_id, pending, response_value);
}

Status KVEngine::RequestMigrate(std::string_view key) {
  const KeyRoute route = RouteForKey(key);
  return Forward(KvMessageType::kMigrate, key, {}, nullptr, route.owner);
}

std::shared_ptr<KVEngine::PendingResponse>
KVEngine::RegisterPendingResponse(uint64_t request_id) {
  auto pending = std::make_shared<PendingResponse>();
  std::lock_guard<std::mutex> lock(pending_response_mutex_);
  if (!pending_responses_.emplace(request_id, pending).second)
    throw std::runtime_error("duplicate pending response request id");
  return pending;
}

void KVEngine::RemovePendingResponse(uint64_t request_id) {
  std::lock_guard<std::mutex> lock(pending_response_mutex_);
  pending_responses_.erase(request_id);
}

void KVEngine::AbandonPendingResponse(uint64_t request_id) {
  // Capacity tracks in-flight workers: each may abandon a small burst of RPCs
  // without unbounded growth (§10.11 / §11.13).
  const size_t max_abandoned = std::max<size_t>(
      256, static_cast<size_t>(config_.foreground_worker_count_per_vm) * 128);
  std::lock_guard<std::mutex> lock(pending_response_mutex_);
  pending_responses_.erase(request_id);
  if (abandoned_request_ids_.insert(request_id).second)
    abandoned_request_order_.push_back(request_id);
  while (abandoned_request_order_.size() > max_abandoned) {
    const uint64_t oldest = abandoned_request_order_.front();
    abandoned_request_order_.pop_front();
    abandoned_request_ids_.erase(oldest);
  }
}

bool KVEngine::ConsumeAbandonedRequestLocked(uint64_t request_id) {
  auto it = abandoned_request_ids_.find(request_id);
  if (it == abandoned_request_ids_.end()) return false;
  abandoned_request_ids_.erase(it);
  for (auto order = abandoned_request_order_.begin();
       order != abandoned_request_order_.end(); ++order) {
    if (*order == request_id) {
      abandoned_request_order_.erase(order);
      break;
    }
  }
  return true;
}

Status KVEngine::AwaitResponse(
    uint64_t request_id, const std::shared_ptr<PendingResponse> &pending,
    std::string *response_value) {
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::seconds(config_.sync_timeout_sec);
  for (;;) {
    // Help serve deferred requests so peers waiting on us make progress, while
    // the demuxer wakes exactly this request when its response arrives. A
    // bounded timed wait preserves cooperative service liveness without a
    // busy-yield loop when the peer has no request for us.
    PollTransport();
    std::unique_lock<std::mutex> lock(pending->mutex);
    if (!pending->done) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        lock.unlock();
        AbandonPendingResponse(request_id);
        return Status::Error(StatusCode::kBusy,
                             "forwarded owner response timed out");
      }
      // Demux notifies this CV both for the matching response and when an
      // inbound request needs cooperative service. Holding pending->mutex
      // across the check and wait prevents a lost wakeup.
      pending->cv.wait_until(lock, deadline);
      continue;
    }
    KvMessage response = pending->message;
    lock.unlock();
    RemovePendingResponse(request_id);
    if (response_value != nullptr)
      response_value->assign(response.value.data(), response.value_size);
    const StatusCode code = static_cast<StatusCode>(response.status);
    std::string message;
    if (code != StatusCode::kOk) {
      message = "forwarded owner operation failed";
      if (response.value_size != 0)
        message += ": " + std::string(response.value.data(), response.value_size);
    }
    return {code, std::move(message)};
  }
}

CasResult KVEngine::ForwardCompareExchange(std::string_view key,
                                           std::string_view expected,
                                           std::string_view desired) {
  const uint32_t owner = RouteForKey(key).owner;
  std::string payload;
  if (!EncodeCasRequest(expected, desired, &payload))
    return {Status::Error(StatusCode::kInvalidArgument,
                          "CAS payload exceeds single-packet capacity"),
            false};
  const uint64_t request_id = NextRequestId(config_.node_id);
  auto pending = RegisterPendingResponse(request_id);
  try {
    SendTransportMessage(MakeRequest(KvMessageType::kCas, config_.node_id, owner,
                                     request_id, key, payload));
  } catch (...) {
    RemovePendingResponse(request_id);
    throw;
  }
  const Status status = AwaitResponse(request_id, pending, nullptr);
  return {status, status.ok()};
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
  while (!inbound_demuxer_stop_.load(std::memory_order_acquire)) {
    bool progressed = false;
    {
      // The demuxer is the actual thread touching the inbound HWCC ring, so it
      // needs its own foreground scope; scopes do not propagate across threads.
      mem_access::LatencyScope latency_scope(
          latency_sim::ScopeKind::kForeground);
      if (rings_ != nullptr) {
        for (int drained = 0; drained < 64; ++drained) {
          alignas(64) char bytes[sizeof(KvMessage)];
          uint64_t received = 0;
          try {
            received = rings_[config_.node_id].recv(bytes, sizeof(bytes));
          } catch (const std::exception &error) {
            TransportFatal(config_.node_id, "ring_recv", error.what());
          } catch (...) {
            TransportFatal(config_.node_id, "ring_recv", "non-std exception");
          }
          // recv has already recorded all inbound HWCC accesses. Pay them
          // before publishing a response notification or deferred request.
          mem_access::DelayActiveScopeNow();
          if (received == 0) break;
          KvMessage message{};
          if (received < WireHeaderBytes() || received > sizeof(KvMessage))
            TransportFatal(config_.node_id, "ring_recv",
                           "malformed KV transport entry");
          std::memcpy(&message, bytes, received);
          if (!ValidWireFrame(received, message))
            TransportFatal(config_.node_id, "ring_recv",
                           "KV wire length/value_size mismatch");
          network_rx_bytes_.fetch_add(received, std::memory_order_relaxed);
          try {
            DemuxTransportMessage(message);
          } catch (const std::exception &error) {
            TransportFatal(config_.node_id, "demux", error.what(), &message);
          } catch (...) {
            TransportFatal(config_.node_id, "demux", "non-std exception", &message);
          }
          progressed = true;
        }
      }
    }
    if (!progressed) std::this_thread::sleep_for(std::chrono::microseconds(20));
  }
}

void KVEngine::DemuxTransportMessage(const KvMessage &message) {
  if (!ValidMessageType(message.type) ||
      message.destination_node != config_.node_id ||
      message.source_node >= config_.vm_count ||
      message.key_size > message.key.size() ||
      message.value_size > message.value.size() ||
      !ValidStatusCode(message.status))
    throw std::runtime_error("invalid KV transport message");
  if (message.type == KvMessageType::kResponse) {
    std::shared_ptr<PendingResponse> pending;
    {
      std::lock_guard<std::mutex> lock(pending_response_mutex_);
      auto it = pending_responses_.find(message.request_id);
      if (it == pending_responses_.end()) {
        if (ConsumeAbandonedRequestLocked(message.request_id)) {
          abandoned_responses_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        throw std::runtime_error("response has no pending request");
      }
      pending = it->second;
    }
    {
      std::lock_guard<std::mutex> lock(pending->mutex);
      if (pending->done)
        throw std::runtime_error("duplicate response for request");
      pending->message = message;
      pending->done = true;
    }
    pending->cv.notify_one();
    return;
  }
  // Request path: demuxer → shared deferred FIFO (IncomingDispatcher style).
  // Any FG PollTransport may serve — required so Forward/Await cannot pin
  // requests on a non-progressing worker shard.
  {
    std::lock_guard<std::mutex> lock(deferred_request_mutex_);
    deferred_transport_requests_.push_back(message);
  }
  WakePendingForwarders();
}

void KVEngine::WakePendingForwarders() {
  std::vector<std::shared_ptr<PendingResponse>> pending;
  {
    std::lock_guard<std::mutex> lock(pending_response_mutex_);
    pending.reserve(pending_responses_.size());
    for (const auto &entry : pending_responses_)
      pending.push_back(entry.second);
  }
  for (const auto &entry : pending) {
    // Synchronize with AwaitResponse's check-then-wait interval. notify_one
    // alone is not sufficient if it races just before wait releases the lock.
    std::lock_guard<std::mutex> lock(entry->mutex);
    entry->cv.notify_one();
  }
}

void KVEngine::ServeDeferredRequests() {
  if (TlsRequestServeDepth != 0) return;
  constexpr size_t kMaxBatch = 64;
  // Reuse one per-worker batch buffer: no heap allocation on PollTransport's
  // hot path and only one shared-FIFO lock acquisition per batch.
  thread_local std::array<KvMessage, kMaxBatch> batch;
  size_t batch_count = 0;
  {
    std::lock_guard<std::mutex> lock(deferred_request_mutex_);
    batch_count = std::min(kMaxBatch, deferred_transport_requests_.size());
    for (size_t i = 0; i < batch_count; ++i) {
      batch[i] = std::move(deferred_transport_requests_.front());
      deferred_transport_requests_.pop_front();
    }
  }
  for (size_t i = 0; i < batch_count; ++i) {
    const KvMessage &deferred = batch[i];
    mem_access::IsolatedLatencyScope request_scope(
        latency_sim::ScopeKind::kForeground);
    try {
      ServeTransportRequest(deferred);
    } catch (const std::exception &error) {
      TransportFatal(config_.node_id, "serve", error.what(), &deferred);
    } catch (...) {
      TransportFatal(config_.node_id, "serve", "non-std exception", &deferred);
    }
  }
}

void KVEngine::PollTransport() {
  // FG cooperative serve only — no MPSC recv (demuxer owns that).
  ServeDeferredRequests();
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
  ebr_->handoff_retired_objects();
  *owner = std::thread::id{};
}

void KVEngine::ServeTransportRequest(const KvMessage &message) {
  if (message.destination_node != config_.node_id ||
      message.key_size > message.key.size() || message.value_size > message.value.size())
    throw std::runtime_error("invalid KV transport message");
  // Responses must never reach the serve path (demuxer applies them).
  if (message.type == KvMessageType::kResponse)
    throw std::runtime_error("response traffic must not enter ServeTransportRequest");

  RequestServeDepthGuard depth_guard;
  const std::string_view key(message.key.data(), message.key_size);
  const std::string_view value(message.value.data(), message.value_size);
  KvMessage response = MakeRequest(KvMessageType::kResponse, config_.node_id,
                                   message.source_node, message.request_id, key);
  if (message.type == KvMessageType::kScanMigrate) {
    MarkLayoutDirty();
    uint32_t partition_id = 0;
    uint32_t flags = 0;
    uint64_t output_limit = 0;
    if (!DecodeScanMigrateRequest(value, &partition_id, &flags, &output_limit)) {
      response.status = static_cast<uint32_t>(StatusCode::kInvalidArgument);
    } else {
      try {
        bool exhausted = false;
        bool no_predecessor = false;
        const Status status = PreparePartitionSharedScan(
            partition_id, key,
            (flags & kScanMigrateFlagCursorDuplicate) != 0, output_limit,
            message.source_node, &exhausted, &no_predecessor);
        response.status = static_cast<uint32_t>(status.code);
        if (status.ok()) {
          const auto encoded =
              EncodeScanMigrateResponse(partition_id, exhausted, no_predecessor);
          response.value_size = static_cast<uint32_t>(encoded.size());
          std::memcpy(response.value.data(), encoded.data(), encoded.size());
        } else {
          response.value_size = static_cast<uint32_t>(
              std::min(status.message.size(), response.value.size()));
          std::memcpy(response.value.data(), status.message.data(),
                      response.value_size);
        }
      } catch (const std::bad_alloc &) {
        response.status = static_cast<uint32_t>(StatusCode::kOutOfMemory);
      }
    }
    SendTransportMessage(response);
    return;
  }
  auto *partition = OwnedPartition(key);
  if (message.type == KvMessageType::kPut ||
      message.type == KvMessageType::kDelete ||
      message.type == KvMessageType::kMigrate ||
      message.type == KvMessageType::kScanMigrate ||
      message.type == KvMessageType::kIncrement ||
      message.type == KvMessageType::kCas)
    MarkLayoutDirty();
  if (partition == nullptr) {
    throw std::runtime_error("request routed to a non-owner node");
  }
  bool check_migration_budget = false;
  auto promote_updated_row = [&] {
    bool moved_in = false;
    const StatusCode migrated =
        partition->EnsureInShared(key, config_.node_id, &moved_in);
    if (migrated == StatusCode::kNotFound)
      throw std::runtime_error(
          "updated owner row disappeared before move-in");
    if (migrated == StatusCode::kOk) {
      check_migration_budget = true;
      if (moved_in) {
        migration_in_.fetch_add(1, std::memory_order_relaxed);
        shared_swcc_flushes_.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };
  if (message.type == KvMessageType::kPut) {
    try {
      const bool inserted = partition->PutPrivate(key, value);
      if (!inserted) promote_updated_row();
      response.status = static_cast<uint32_t>(StatusCode::kOk);
    } catch (const std::bad_alloc &) {
      response.status = static_cast<uint32_t>(StatusCode::kOutOfMemory);
    } catch (const std::runtime_error &e) {
      response.status = static_cast<uint32_t>(
          std::string_view(e.what()).find("busy") != std::string_view::npos
              ? StatusCode::kBusy
              : StatusCode::kCorruption);
    }
  } else if (message.type == KvMessageType::kDelete) {
    try {
      response.status = static_cast<uint32_t>(partition->DeletePrivate(key)
          ? StatusCode::kOk : StatusCode::kNotFound);
    } catch (const std::runtime_error &e) {
      response.status = static_cast<uint32_t>(
          std::string_view(e.what()).find("busy") != std::string_view::npos
              ? StatusCode::kBusy
              : StatusCode::kCorruption);
    }
  } else if (message.type == KvMessageType::kMigrate) {
    try {
      bool moved_in = false;
      const StatusCode migrated =
          partition->EnsureInShared(key, config_.node_id, &moved_in);
      response.status = static_cast<uint32_t>(migrated);
      if (migrated == StatusCode::kOk && moved_in) {
        migration_in_.fetch_add(1, std::memory_order_relaxed);
        shared_swcc_flushes_.fetch_add(1, std::memory_order_relaxed);
      }
      // Ack move_in before OnDemand move_out so the requester can TryPin /
      // CXL-access without racing the same-handler eviction (liveness under
      // YCSB-A). move_in cost remains on the Migrate critical path.
      SendTransportMessage(response);
      if (migrated == StatusCode::kOk)
        EnforceMigrationBudget(*partition);
      return;
    } catch (const std::bad_alloc &) {
      response.status = static_cast<uint32_t>(StatusCode::kOutOfMemory);
    }
  } else if (message.type == KvMessageType::kIncrement) {
    int64_t delta = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), delta);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
      response.status = static_cast<uint32_t>(StatusCode::kInvalidArgument);
    } else {
      try {
        int64_t result = 0;
        bool inserted = false;
        if (!partition->IncrementPrivate(key, delta, &result, &inserted)) {
          response.status = static_cast<uint32_t>(StatusCode::kNotFound);
        } else {
          if (!inserted) promote_updated_row();
          const std::string encoded = std::to_string(result);
          response.status = static_cast<uint32_t>(StatusCode::kOk);
          response.value_size = static_cast<uint32_t>(encoded.size());
          std::memcpy(response.value.data(), encoded.data(), encoded.size());
        }
      } catch (const std::invalid_argument &) {
        response.status = static_cast<uint32_t>(StatusCode::kInvalidArgument);
      }
    }
  } else if (message.type == KvMessageType::kCas) {
    std::string_view expected;
    std::string_view desired;
    if (!DecodeCasRequest(value, &expected, &desired)) {
      response.status = static_cast<uint32_t>(StatusCode::kInvalidArgument);
    } else {
      try {
        bool exchanged = false;
        bool inserted = false;
        if (!partition->CompareExchangePrivate(key, expected, desired, &exchanged,
                                               &inserted))
          response.status = static_cast<uint32_t>(StatusCode::kNotFound);
        else {
          if (!inserted) promote_updated_row();
          response.status = static_cast<uint32_t>(
              exchanged ? StatusCode::kOk : StatusCode::kCompareFailed);
        }
      } catch (const std::invalid_argument &) {
        response.status = static_cast<uint32_t>(StatusCode::kInvalidArgument);
      } catch (const std::runtime_error &e) {
        response.status = static_cast<uint32_t>(
            std::string_view(e.what()).find("busy") != std::string_view::npos
                ? StatusCode::kBusy
                : StatusCode::kCorruption);
      }
    }
  } else {
    throw std::runtime_error("unsupported request message type");
  }
  SendTransportMessage(response);
  if (check_migration_budget) EnforceMigrationBudget(*partition);
}

void KVEngine::EnforceMigrationBudget(KVPartition &partition) {
  const uint64_t hw_budget = (config_.hw_cc_budget_mb * 1024ULL * 1024ULL -
      star::CXL_EBR::max_ebr_retiring_memory) / config_.vm_count;
  const bool payload_high = partition.shared_payload_used_bytes() * 10 >=
      partition.shared_payload_capacity_bytes() * 9;
  const uint64_t hw_used = partition.hwcc_used_bytes();
  if (hw_used < hw_budget && !payload_high) return;
  // PolicyClock budgets against CXLMemory::TOTAL_HW_CC_USAGE.  When only the
  // shared-payload watermark trips, force the counter to the budget so the
  // original move_row_out scan still runs.
  if (payload_high && hw_used < hw_budget)
    star::cxl_memory.set_total_hw_cc_usage(hw_budget);
  else
    KvMigrationRuntime::SyncHwCcUsage(partition);
  for (uint32_t pass = 0; pass < 2; ++pass) {
    for (auto &candidate : partitions_) {
      if (OwnerForPartition(candidate->partition_id()) != config_.node_id)
        continue;
      if (candidate->MoveOutClockVictim(config_.node_id)) {
        migration_out_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }
  throw std::runtime_error(
      "migration budget exceeded without an eligible owner Clock victim");
}

Status KVEngine::MoveOut(std::string_view key) {
  MarkLayoutDirty();
  const KeyRoute route = RouteForKey(key);
  if (!route.owned_by_this_node)
    return Status::Error(StatusCode::kOwnerViolation, "remote owner requires forwarding");
  auto *partition = route.partition;
  auto *clock = KvMigrationRuntime::Instance().clock();
  auto *table =
      KvMigrationRuntime::Instance().TableFor(partition->partition_id());
  const FixedKey fixed_key =
      FixedKey::From(key, config_.fixed_key_size);
  if (clock != nullptr && table != nullptr &&
      clock->move_specific_row_out(table, fixed_key.bytes)) {
    migration_out_.fetch_add(1, std::memory_order_relaxed);
    return Status::Ok();
  }
  return Status::Error(StatusCode::kNotFound, "shared key not found or busy");
}

}  // namespace tigonkv::engine
