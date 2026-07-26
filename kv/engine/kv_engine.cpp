#include "kv/engine/kv_engine.h"

#include "common/CXL_EBR.h"
#include "common/MPSCRingBuffer.h"
#include "kv/engine/kv_partition.h"
#include "kv/engine/kv_migration.h"
#include "kv/engine/kv_messages.h"
#include "protocol/TwoPLPasha/TwoPLPashaSCCWriteThrough.h"

#include <stdexcept>
#include <thread>
#include <chrono>
#include <charconv>
#include <cstring>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <queue>
#include <fstream>
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
// PollTransport must not pop/serve deferred requests.  Handling Put/Get or
// another ScanRequest here mutates the same B+trees under OLC and livelocks
// scan() restart loops (GDB: yield counts > 10M on YCSB-E 4x4).
// Response/item/done traffic is applied by the inbound demuxer thread, so
// nested FG polls only need to skip ServeDeferred.
thread_local uint32_t TlsRequestServeDepth = 0;
// Bound by BindWorker; UINT32_MAX means unbound (unit tests drain all queues).
thread_local uint32_t TlsForegroundWorkerId = UINT32_MAX;

struct RequestServeDepthGuard {
  RequestServeDepthGuard() { ++TlsRequestServeDepth; }
  ~RequestServeDepthGuard() { --TlsRequestServeDepth; }
  RequestServeDepthGuard(const RequestServeDepthGuard &) = delete;
  RequestServeDepthGuard &operator=(const RequestServeDepthGuard &) = delete;
};

std::string EncodeU64(uint64_t value) {
  std::string encoded(sizeof(value), '\0');
  for (size_t i = 0; i < encoded.size(); ++i)
    encoded[i] = static_cast<char>(value >> (i * 8));
  return encoded;
}

bool DecodeU64(std::string_view encoded, uint64_t *value) {
  if (encoded.size() != sizeof(*value)) return false;
  *value = 0;
  for (size_t i = 0; i < encoded.size(); ++i)
    *value |= static_cast<uint64_t>(static_cast<unsigned char>(encoded[i])) << (i * 8);
  return true;
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
    : config_(config), pool_(std::move(pool)), ebr_(ebr), scc_(std::move(scc)) {}

KVEngine::~KVEngine() {
  StopInboundDemuxer();
  if (star::scc_manager == scc_.get()) star::scc_manager = nullptr;
  if (star::global_ebr_meta == ebr_) star::global_ebr_meta = nullptr;
  KvMigrationRuntime::Instance().Reset();
}

std::unique_ptr<KVEngine> KVEngine::Open(const Config &config, bool reset) {
  config.Validate();
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
    // +1 EBR slot for the inbound demuxer (Tigon IncomingDispatcher analogue).
    new (ebr) star::CXL_EBR(config.vm_count,
                            config.foreground_worker_count_per_vm + 1);
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
  engine->rings_ = rings;
  for (uint32_t partition = 0; partition < config.partition_count; ++partition) {
    const auto &directory = engine->pool_->allocator().layout().partitions[partition];
    // All VMs reconstruct the persistent roots so a non-owner can use the
    // shared-tree fast path after promotion.  Only the owner is allowed to
    // invoke private-row operations; binding the partition to its stable owner
    // keeps its private arena and tree nodes in the correct allocation shard.
    const bool attach = directory.private_root != kNullOffset &&
                        directory.shared_root.load(std::memory_order_acquire) !=
                            kNullOffset;
    engine->partitions_.emplace_back(std::make_unique<KVPartition>(
        engine->pool_->allocator(), *engine->ebr_, partition,
        engine->OwnerForPartition(partition), attach));
  }
  std::vector<KVPartition *> partition_ptrs;
  partition_ptrs.reserve(engine->partitions_.size());
  for (auto &partition : engine->partitions_) partition_ptrs.push_back(partition.get());
  const uint64_t hw_budget = (config.hw_cc_budget_mb * 1024ULL * 1024ULL -
      star::CXL_EBR::max_ebr_retiring_memory) / config.vm_count;
  KvMigrationRuntime::Instance().Install(
      partition_ptrs, config.fixed_key_size, config.fixed_value_size,
      config.node_id, config.partition_count, hw_budget);
  for (auto &partition : engine->partitions_) {
    if (engine->OwnerForPartition(partition->partition_id()) == config.node_id)
      partition->RebuildClockTracker();
  }
  engine->StartInboundDemuxer();
  return engine;
}

uint32_t KVEngine::PartitionForKey(std::string_view key) const {
  return static_cast<uint32_t>(Hash(key) % config_.partition_count);
}

uint32_t KVEngine::OwnerForKey(std::string_view key) const {
  return OwnerForPartition(PartitionForKey(key));
}

uint32_t KVEngine::OwnerForPartition(uint32_t partition) const {
  return partition % config_.vm_count;
}

KVPartition *KVEngine::OwnedPartition(std::string_view key) const {
  const uint32_t owner = OwnerForKey(key);
  if (owner != config_.node_id) return nullptr;
  return VisiblePartition(key);
}

KVPartition *KVEngine::VisiblePartition(std::string_view key) const {
  const uint32_t partition = PartitionForKey(key);
  for (const auto &entry : partitions_)
    if (entry->partition_id() == partition) return entry.get();
  return nullptr;
}

Status KVEngine::Put(std::string_view key, std::string_view value) {
  auto *partition = OwnedPartition(key);
  if (partition == nullptr) {
    auto *visible = VisiblePartition(key);
    if (visible != nullptr && visible->PutShared(key, config_.node_id, value)) {
      shared_puts_.fetch_add(1, std::memory_order_relaxed);
      shared_swcc_flushes_.fetch_add(1, std::memory_order_relaxed);
      return Status::Ok();
    }
    // Put miss: do not DATA_MIGRATION first. Load/create paths would issue a
    // NotFound migrate+Put pair per key and fill MPSC rings until nested
    // Serve/Send deadlocks. Owner PutPrivate creates/updates; Get migrates.
    return Forward(KvMessageType::kPut, key, value, nullptr);
  }
  try { partition->PutPrivate(key, value); return Status::Ok(); }
  catch (const std::bad_alloc &) { return Status::Error(StatusCode::kOutOfMemory, "private arena exhausted"); }
  catch (const std::exception &e) { return Status::Error(StatusCode::kCorruption, e.what()); }
}

GetResult KVEngine::Get(std::string_view key) {
  auto *partition = OwnedPartition(key);
  if (partition == nullptr) {
    auto *visible = VisiblePartition(key);
    // shared-hit CXL; miss → migrate RPC then CXL retry (not owner value-Forward).
    for (int attempt = 0; attempt < 4; ++attempt) {
      std::string shared;
      if (visible != nullptr && visible->GetShared(key, config_.node_id, &shared)) {
        shared_gets_.fetch_add(1, std::memory_order_relaxed);
        return {Status::Ok(), std::move(shared)};
      }
      const Status migrated = RequestMigrate(key);
      if (migrated.code == StatusCode::kNotFound)
        return {Status::Error(StatusCode::kNotFound, "key not found"), {}};
      if (!migrated.ok() && migrated.code != StatusCode::kOutOfMemory)
        return {migrated, {}};
      if (migrated.code == StatusCode::kOutOfMemory && attempt == 3)
        return {migrated, {}};
    }
    return {Status::Error(StatusCode::kNotFound, "key not found after migrate"), {}};
  }
  std::string value;
  return partition->GetPrivate(key, &value) ? GetResult{Status::Ok(), std::move(value)}
                                            : GetResult{Status::Error(StatusCode::kNotFound, "key not found"), {}};
}

Status KVEngine::Delete(std::string_view key) {
  auto *partition = OwnedPartition(key);
  if (partition == nullptr) return Forward(KvMessageType::kDelete, key, {}, nullptr);
  return partition->DeletePrivate(key) ? Status::Ok()
                                       : Status::Error(StatusCode::kNotFound, "key not found");
}

ScanResult KVEngine::Scan(std::string_view start_key, uint64_t limit) {
  constexpr uint64_t kScanSafetyLimit = 1024 * 1024;
  if (limit > kScanSafetyLimit)
    return {Status::Error(StatusCode::kInvalidArgument, "scan limit exceeds safety cap"), {}};
  constexpr uint64_t kPageSize = 64;
  const uint64_t target = limit == 0 ? kScanSafetyLimit : limit;
  const uint64_t request_limit = kPageSize + 1;  // one cursor duplicate + one page

  struct Source {
    uint32_t node = 0;
    std::string cursor;
    bool has_cursor = false;
    bool more = false;
    std::vector<ScanItem> items;
    size_t next = 0;
  };
  std::vector<Source> sources;
  sources.reserve(config_.vm_count * 2);

  auto load_page = [&](Source *source, std::vector<ScanItem> raw) -> Status {
    source->items.clear();
    source->next = 0;
    for (auto &item : raw) {
      if (source->has_cursor && item.key <= source->cursor) continue;
      source->items.push_back(std::move(item));
      if (source->items.size() == kPageSize) break;
    }
    // A full request can contain either a complete page or cursor + page.  A
    // later cursor request disambiguates the boundary without materializing
    // the remaining owner result.
    source->more = raw.size() == request_limit;
    return Status::Ok();
  };

  Source local;
  local.node = config_.node_id;
  ScanResult local_page = ScanOwnedPartitions(start_key, request_limit);
  if (!local_page.status.ok()) return local_page;
  load_page(&local, std::move(local_page.items));
  sources.push_back(std::move(local));

  // CXL-first: seed remote-owned partitions from local shared trees before RPC
  // (TwoPLPasha remote CXL table scan). Owner RPC remains authoritative for
  // private rows; heap merge dedups overlapping migrated keys.
  for (const auto &partition : partitions_) {
    if (OwnerForPartition(partition->partition_id()) == config_.node_id) continue;
    std::vector<std::pair<std::string, std::string>> shared_items;
    if (!partition->ScanSharedOnly(start_key, request_limit, &shared_items,
                                   config_.node_id))
      continue;
    if (shared_items.empty()) continue;
    Source cxl;
    cxl.node = OwnerForPartition(partition->partition_id());
    std::vector<ScanItem> raw;
    raw.reserve(shared_items.size());
    for (auto &item : shared_items)
      raw.push_back({std::move(item.first), std::move(item.second)});
    load_page(&cxl, std::move(raw));
    if (!cxl.items.empty()) sources.push_back(std::move(cxl));
  }

  auto acquire_scan_rpc = [this] {
    std::unique_lock<std::mutex> lock(scan_rpc_mutex_);
    scan_rpc_cv_.wait(lock, [&] { return inflight_scan_rpcs_ < kMaxInflightScanRpcs; });
    ++inflight_scan_rpcs_;
  };
  auto release_scan_rpc = [this] {
    {
      std::lock_guard<std::mutex> lock(scan_rpc_mutex_);
      --inflight_scan_rpcs_;
    }
    scan_rpc_cv_.notify_one();
  };

  // Fan-out ScanRPCs (processor-style): register + send all, then await.
  struct RemoteInflights {
    uint32_t node = 0;
    uint64_t request_id = 0;
  };
  std::vector<RemoteInflights> inflight;
  inflight.reserve(config_.vm_count);
  for (uint32_t node = 0; node < config_.vm_count; ++node) {
    if (node == config_.node_id) continue;
    const uint64_t request_id = NextRequestId(config_.node_id);
    {
      std::lock_guard<std::mutex> lock(pending_scan_mutex_);
      pending_scans_.emplace(request_id, PendingScan{});
    }
    acquire_scan_rpc();
    try {
      SendTransportMessage(MakeRequest(KvMessageType::kScanRequest, config_.node_id, node,
                                       request_id, start_key, EncodeU64(request_limit)));
    } catch (const std::exception &e) {
      std::lock_guard<std::mutex> lock(pending_scan_mutex_);
      pending_scans_.erase(request_id);
      release_scan_rpc();
      return {Status::Error(StatusCode::kInvalidArgument, e.what()), {}};
    }
    inflight.push_back({node, request_id});
  }
  for (const auto &slot : inflight) {
    std::vector<ScanItem> remote;
    Status status = Status::Ok();
    try {
      status = AwaitScan(slot.request_id, &remote);
    } catch (const std::exception &e) {
      release_scan_rpc();
      return {Status::Error(StatusCode::kInvalidArgument, e.what()), {}};
    }
    release_scan_rpc();
    if (!status.ok()) return {status, {}};
    Source source;
    source.node = slot.node;
    load_page(&source, std::move(remote));
    sources.push_back(std::move(source));
  }

  auto refill = [&](Source *source) -> Status {
    if (!source->more) return Status::Ok();
    const uint64_t request_id = NextRequestId(config_.node_id);
    {
      std::lock_guard<std::mutex> lock(pending_scan_mutex_);
      pending_scans_.emplace(request_id, PendingScan{});
    }
    acquire_scan_rpc();
    Status status = Status::Ok();
    std::vector<ScanItem> raw;
    try {
      SendTransportMessage(MakeRequest(KvMessageType::kScanRequest, config_.node_id,
                                       source->node, request_id, source->cursor,
                                       EncodeU64(request_limit)));
      status = AwaitScan(request_id, &raw);
    } catch (const std::exception &e) {
      std::lock_guard<std::mutex> lock(pending_scan_mutex_);
      pending_scans_.erase(request_id);
      release_scan_rpc();
      return Status::Error(StatusCode::kInvalidArgument, e.what());
    }
    release_scan_rpc();
    if (!status.ok()) return status;
    return load_page(source, std::move(raw));
  };

  struct HeapItem { std::string_view key; size_t source; };
  auto compare = [](const HeapItem &left, const HeapItem &right) {
    return left.key > right.key;
  };
  std::priority_queue<HeapItem, std::vector<HeapItem>, decltype(compare)> heap(compare);
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
      (!heap.empty() || std::any_of(sources.begin(), sources.end(),
                                    [](const Source &source) { return source.more; })))
    return {Status::Error(StatusCode::kInvalidArgument, "scan result exceeds safety cap"), {}};
  return result;
}

ScanResult KVEngine::ScanOwnedPartitions(std::string_view start_key, uint64_t limit) {
  // Raise serve depth for the whole owned walk so progress PollTransport cannot
  // nest Put/Get/Scan serves that restart OLC readers on the same trees.
  // Processor-style: per-partition vectors + heap merge (no std::map materialize).
  std::vector<std::vector<ScanItem>> parts;
  parts.reserve(partitions_.size());
  const uint64_t per_partition_limit = limit == 0 ? 0 : limit;
  const std::function<void()> progress = [this] { PollTransport(); };
  try {
    {
      RequestServeDepthGuard depth_guard;
      for (const auto &partition : partitions_) {
        if (OwnerForPartition(partition->partition_id()) != config_.node_id) continue;
        std::vector<std::pair<std::string, std::string>> items;
        if (!partition->ScanOwned(start_key, per_partition_limit, &items, &progress))
          return {Status::Error(StatusCode::kCorruption,
                                "partition scan exceeded migration retry budget"), {}};
        std::vector<ScanItem> page;
        page.reserve(items.size());
        for (auto &item : items)
          page.push_back({std::move(item.first), std::move(item.second)});
        parts.push_back(std::move(page));
        PollTransport();
      }
    }
    PollTransport();
  } catch (const std::exception &e) {
    return {Status::Error(StatusCode::kCorruption, e.what()), {}};
  }
  struct HeapItem {
    std::string_view key;
    size_t part = 0;
    size_t index = 0;
  };
  auto compare = [](const HeapItem &left, const HeapItem &right) {
    return left.key > right.key;
  };
  std::priority_queue<HeapItem, std::vector<HeapItem>, decltype(compare)> heap(compare);
  for (size_t i = 0; i < parts.size(); ++i) {
    if (!parts[i].empty()) heap.push({parts[i][0].key, i, 0});
  }
  ScanResult result{Status::Ok(), {}};
  while (!heap.empty()) {
    if (limit != 0 && result.items.size() >= limit) break;
    const HeapItem top = heap.top();
    heap.pop();
    ScanItem row = std::move(parts[top.part][top.index]);
    if (result.items.empty() || result.items.back().key != row.key)
      result.items.push_back(std::move(row));
    const size_t next = top.index + 1;
    if (next < parts[top.part].size())
      heap.push({parts[top.part][next].key, top.part, next});
  }
  return result;
}

CasResult KVEngine::CompareExchange(std::string_view key,
                                    std::string_view expected,
                                    std::string_view desired) {
  auto *partition = OwnedPartition(key);
  if (partition == nullptr) {
    auto *visible = VisiblePartition(key);
    auto try_shared = [&](bool *exchanged) -> bool {
      return visible != nullptr && visible->CompareExchangeShared(
          key, config_.node_id, expected, desired, exchanged);
    };
    bool exchanged = false;
    if (try_shared(&exchanged)) {
      if (exchanged) {
        shared_puts_.fetch_add(1, std::memory_order_relaxed);
        shared_swcc_flushes_.fetch_add(1, std::memory_order_relaxed);
      }
      return {exchanged ? Status::Ok()
                         : Status::Error(StatusCode::kCompareFailed, "expected value differs"),
              exchanged};
    }
    const Status migrated = RequestMigrate(key);
    exchanged = false;
    if (try_shared(&exchanged)) {
      if (exchanged) {
        shared_puts_.fetch_add(1, std::memory_order_relaxed);
        shared_swcc_flushes_.fetch_add(1, std::memory_order_relaxed);
      }
      return {exchanged ? Status::Ok()
                         : Status::Error(StatusCode::kCompareFailed, "expected value differs"),
              exchanged};
    }
    if (migrated.code == StatusCode::kNotFound)
      return {Status::Error(StatusCode::kNotFound, "key not found"), false};
    return ForwardCompareExchange(key, expected, desired);
  }
  try {
    bool exchanged = false;
    if (!partition->CompareExchangePrivate(key, expected, desired, &exchanged))
      return {Status::Error(StatusCode::kNotFound, "key not found"), false};
    return {exchanged ? Status::Ok()
                      : Status::Error(StatusCode::kCompareFailed, "expected value differs"),
            exchanged};
  } catch (const std::bad_alloc &) {
    return {Status::Error(StatusCode::kOutOfMemory, "allocator exhausted"), false};
  } catch (const std::exception &e) {
    return {Status::Error(StatusCode::kInvalidArgument, e.what()), false};
  }
}

IncrementResult KVEngine::Increment(std::string_view key, int64_t delta) {
  auto *partition = OwnedPartition(key);
  if (partition == nullptr) {
    auto *visible = VisiblePartition(key);
    int64_t shared = 0;
    if (visible != nullptr && visible->IncrementShared(key, config_.node_id, delta, &shared)) {
      shared_puts_.fetch_add(1, std::memory_order_relaxed);
      shared_swcc_flushes_.fetch_add(1, std::memory_order_relaxed);
      return {Status::Ok(), shared};
    }
    // Same as Put: avoid migrate-before-create storms; owner IncrementPrivate.
    std::string value;
    const auto status = Forward(KvMessageType::kIncrement, key,
                                std::to_string(delta), &value);
    if (!status.ok()) return {status, 0};
    int64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
      return {Status::Error(StatusCode::kCorruption, "malformed forwarded increment response"), 0};
    return {Status::Ok(), result};
  }
  try {
    int64_t value = 0;
    if (!partition->IncrementPrivate(key, delta, &value))
      return {Status::Error(StatusCode::kNotFound, "key not found"), 0};
    return {Status::Ok(), value};
  } catch (const std::invalid_argument &e) {
    return {Status::Error(StatusCode::kInvalidArgument, e.what()), 0};
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
  stats.owner_private_swcc_used_bytes = layout.domains[static_cast<size_t>(
      AllocationDomain::kOwnerPrivateSwcc)].used_bytes.load(std::memory_order_relaxed);
  stats.shared_payload_swcc_used_bytes = layout.domains[static_cast<size_t>(
      AllocationDomain::kSharedPayloadSwcc)].used_bytes.load(std::memory_order_relaxed);
  stats.allocator_shared_overhead_bytes = layout.domains[static_cast<size_t>(
      AllocationDomain::kAllocatorMetadata)].used_bytes.load(std::memory_order_relaxed);
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
    pool_->allocator().FlushCheckpointRanges();
    return Status::Ok();
  } catch (const std::exception &e) {
    return Status::Error(StatusCode::kCorruption, e.what());
  }
}

void KVEngine::SendTransportMessage(const KvMessage &message) {
  if (rings_ == nullptr || message.destination_node >= config_.vm_count)
    throw std::runtime_error("KV transport destination out of range");
  unsigned spins = 0;
  while (!rings_[message.destination_node].enqueue(
      const_cast<char *>(reinterpret_cast<const char *>(&message)), sizeof(message))) {
    // Peer demuxers free ring slots. When called from nested Serve (depth>0)
    // we must not ServeDeferred, but top-level Send during Await may briefly
    // help demux by yielding; sleeping avoids tight livelock on full rings.
    if ((++spins & 63u) == 0)
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    else
      std::this_thread::yield();
  }
  network_tx_bytes_.fetch_add(sizeof(message), std::memory_order_relaxed);
}

Status KVEngine::Forward(KvMessageType type, std::string_view key, std::string_view value,
                         std::string *response_value) {
  const uint32_t owner = OwnerForKey(key);
  const uint64_t request_id = NextRequestId(config_.node_id);
  SendTransportMessage(MakeRequest(type, config_.node_id, owner, request_id, key, value));
  return AwaitResponse(request_id, response_value);
}

Status KVEngine::RequestMigrate(std::string_view key) {
  return Forward(KvMessageType::kMigrate, key, {}, nullptr);
}

Status KVEngine::AwaitResponse(uint64_t request_id, std::string *response_value) {
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::seconds(config_.sync_timeout_sec);
  for (;;) {
    // Help serve deferred requests so peers waiting on us make progress, while
    // the demuxer independently publishes our response into responses_.
    PollTransport();
    std::lock_guard<std::mutex> lock(response_mutex_);
    auto it = responses_.find(request_id);
    if (it == responses_.end()) {
      if (std::chrono::steady_clock::now() >= deadline)
        return Status::Error(StatusCode::kCorruption, "forwarded owner response timed out");
      std::this_thread::yield();
      continue;
    }
    KvMessage response = it->second;
    responses_.erase(it);
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

Status KVEngine::AwaitScan(uint64_t request_id, std::vector<ScanItem> *items) {
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::seconds(config_.sync_timeout_sec);
  for (;;) {
    for (int i = 0; i < 4; ++i) PollTransport();
    std::lock_guard<std::mutex> lock(pending_scan_mutex_);
    auto it = pending_scans_.find(request_id);
    if (it == pending_scans_.end())
      return Status::Error(StatusCode::kCorruption, "missing forwarded scan state");
    if (it->second.done) {
      const Status status{it->second.status,
          it->second.status == StatusCode::kOk ? "" : "forwarded owner scan failed"};
      if (status.ok() && items != nullptr) *items = std::move(it->second.items);
      pending_scans_.erase(it);
      return status;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      pending_scans_.erase(it);
      return Status::Error(StatusCode::kCorruption, "forwarded owner scan timed out");
    }
    std::this_thread::yield();
  }
}

CasResult KVEngine::ForwardCompareExchange(std::string_view key,
                                           std::string_view expected,
                                           std::string_view desired) {
  const uint32_t owner = OwnerForKey(key);
  const uint64_t request_id = NextRequestId(config_.node_id);
  SendTransportMessage(MakeRequest(KvMessageType::kCasPrepare, config_.node_id, owner,
                                   request_id, key, expected));
  Status status = AwaitResponse(request_id, nullptr);
  if (!status.ok()) return {std::move(status), false};
  SendTransportMessage(MakeRequest(KvMessageType::kCasCommit, config_.node_id, owner,
                                   request_id, key, desired));
  status = AwaitResponse(request_id, nullptr);
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
  ebr_->thread_init_ebr_meta(config_.node_id, inbound_demuxer_worker_id_);
  star::global_ebr_meta = ebr_;
  while (!inbound_demuxer_stop_.load(std::memory_order_acquire)) {
    bool progressed = false;
    try {
      if (rings_ != nullptr) {
        for (int drained = 0; drained < 64; ++drained) {
          alignas(64) char bytes[sizeof(KvMessage)];
          const uint64_t received = rings_[config_.node_id].recv(bytes, sizeof(bytes));
          if (received == 0) break;
          if (received != sizeof(KvMessage))
            throw std::runtime_error("malformed KV transport entry");
          KvMessage message{};
          std::memcpy(&message, bytes, sizeof(message));
          network_rx_bytes_.fetch_add(received, std::memory_order_relaxed);
          DemuxTransportMessage(message);
          progressed = true;
        }
      }
    } catch (...) {
      // Keep demuxing; FG paths surface hard failures to callers.
    }
    if (!progressed) std::this_thread::sleep_for(std::chrono::microseconds(20));
  }
}

void KVEngine::DemuxTransportMessage(const KvMessage &message) {
  if (message.destination_node != config_.node_id ||
      message.source_node >= config_.vm_count ||
      message.key_size > message.key.size() || message.value_size > message.value.size())
    throw std::runtime_error("invalid KV transport message");
  if (message.type == KvMessageType::kResponse) {
    std::lock_guard<std::mutex> lock(response_mutex_);
    responses_.emplace(message.request_id, message);
    return;
  }
  if (message.type == KvMessageType::kScanItem || message.type == KvMessageType::kScanDone) {
    std::lock_guard<std::mutex> lock(pending_scan_mutex_);
    auto it = pending_scans_.find(message.request_id);
    if (it == pending_scans_.end()) return;
    if (message.type == KvMessageType::kScanItem) {
      it->second.items.push_back({std::string(message.key.data(), message.key_size),
                                  std::string(message.value.data(), message.value_size)});
    } else {
      it->second.status = static_cast<StatusCode>(message.status);
      it->second.done = true;
    }
    return;
  }
  // Request path: demuxer → shared deferred FIFO (IncomingDispatcher style).
  // Any FG PollTransport may serve — required so Forward/Await cannot pin
  // requests on a non-progressing worker shard.
  std::lock_guard<std::mutex> lock(deferred_request_mutex_);
  deferred_transport_requests_.push_back(message);
}

void KVEngine::ServeDeferredRequests(int max_count) {
  if (TlsRequestServeDepth != 0) return;
  for (int i = 0; i < max_count; ++i) {
    KvMessage deferred;
    {
      std::lock_guard<std::mutex> lock(deferred_request_mutex_);
      if (deferred_transport_requests_.empty()) break;
      deferred = deferred_transport_requests_.front();
      deferred_transport_requests_.pop_front();
    }
    try {
      ServeTransportRequest(deferred);
    } catch (const std::exception &) {
      // Never poison AwaitResponse/PollTransport: a failed serve must not
      // abort the waiting FG worker. Prefer dropping the bad request.
    } catch (...) {
    }
  }
}

void KVEngine::PollTransport() {
  // FG cooperative serve only — no MPSC recv (demuxer owns that).
  ServeDeferredRequests(8);
}

void KVEngine::BindWorker(uint32_t worker_id) {
  if (ebr_ == nullptr)
    throw std::runtime_error("BindWorker requires an open EBR instance");
  if (worker_id >= config_.foreground_worker_count_per_vm)
    throw std::invalid_argument("BindWorker worker_id exceeds foreground_worker_count_per_vm");
  ebr_->thread_init_ebr_meta(config_.node_id, worker_id);
  star::global_ebr_meta = ebr_;
  TlsForegroundWorkerId = worker_id;
}

void KVEngine::ServeScanRequest(const KvMessage &message) {
  const std::string_view key(message.key.data(), message.key_size);
  const std::string_view value(message.value.data(), message.value_size);
  uint64_t limit = 0;
  Status status = Status::Ok();
  std::vector<ScanItem> items;
  if (!DecodeU64(value, &limit)) {
    status = Status::Error(StatusCode::kInvalidArgument, "invalid scan limit payload");
  } else {
    const auto scan = ScanOwnedPartitions(key, limit);
    status = scan.status;
    items = scan.items;
  }
  if (status.ok()) {
    for (size_t i = 0; i < items.size(); ++i) {
      SendTransportMessage(MakeRequest(KvMessageType::kScanItem, config_.node_id,
                                       message.source_node, message.request_id,
                                       items[i].key, items[i].value));
      // Cooperative deferred serve (not MPSC recv) between ScanItem bursts.
      if ((i + 1) % 8 == 0) PollTransport();
    }
  }
  KvMessage done = MakeRequest(KvMessageType::kScanDone, config_.node_id,
                               message.source_node, message.request_id, {});
  done.status = static_cast<uint32_t>(status.code);
  SendTransportMessage(done);
}

void KVEngine::ServeTransportRequest(const KvMessage &message) {
  if (message.destination_node != config_.node_id ||
      message.key_size > message.key.size() || message.value_size > message.value.size())
    throw std::runtime_error("invalid KV transport message");
  // Responses must never reach the serve path (demuxer applies them).
  if (message.type == KvMessageType::kResponse ||
      message.type == KvMessageType::kScanItem ||
      message.type == KvMessageType::kScanDone)
    throw std::runtime_error("response traffic must not enter ServeTransportRequest");

  RequestServeDepthGuard depth_guard;
  if (message.type == KvMessageType::kScanRequest) {
    ServeScanRequest(message);
    return;
  }
  const std::string_view key(message.key.data(), message.key_size);
  const std::string_view value(message.value.data(), message.value_size);
  KvMessage response = MakeRequest(KvMessageType::kResponse, config_.node_id,
                                   message.source_node, message.request_id, key);
  auto *partition = OwnedPartition(key);
  if (partition == nullptr) {
    response.status = static_cast<uint32_t>(StatusCode::kCorruption);
  } else if (message.type == KvMessageType::kPut) {
    try {
      partition->PutPrivate(key, value);
      response.status = static_cast<uint32_t>(StatusCode::kOk);
    } catch (const std::bad_alloc &) {
      response.status = static_cast<uint32_t>(StatusCode::kOutOfMemory);
    } catch (const std::exception &error) {
      response.status = static_cast<uint32_t>(StatusCode::kCorruption);
      const size_t count = std::min(response.value.size(), std::strlen(error.what()));
      std::memcpy(response.value.data(), error.what(), count);
      response.value_size = static_cast<uint32_t>(count);
    } catch (...) {
      response.status = static_cast<uint32_t>(StatusCode::kCorruption);
    }
  } else if (message.type == KvMessageType::kDelete) {
    response.status = static_cast<uint32_t>(partition->DeletePrivate(key)
        ? StatusCode::kOk : StatusCode::kNotFound);
  } else if (message.type == KvMessageType::kGet) {
    std::string result;
    if (!partition->GetPrivate(key, &result)) {
      response.status = static_cast<uint32_t>(StatusCode::kNotFound);
    } else {
      // Residual owner GET: migrate before reply (move_in on request path).
      // Budget after Send so the requester is not blocked behind move_out.
      try {
        bool moved_in = false;
        const StatusCode migrated =
            partition->EnsureInShared(key, config_.node_id, &moved_in);
        if (migrated == StatusCode::kOk && moved_in) {
          migration_in_.fetch_add(1, std::memory_order_relaxed);
          shared_swcc_flushes_.fetch_add(1, std::memory_order_relaxed);
        }
      } catch (const std::exception &) {
      } catch (...) {
      }
      response.status = static_cast<uint32_t>(StatusCode::kOk);
      response.value_size = static_cast<uint32_t>(result.size());
      std::memcpy(response.value.data(), result.data(), result.size());
      SendTransportMessage(response);
      try {
        EnforceMigrationBudget(*partition);
      } catch (const std::exception &) {
      } catch (...) {
      }
      return;
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
      if (migrated == StatusCode::kOk) {
        try {
          EnforceMigrationBudget(*partition);
        } catch (const std::exception &) {
        } catch (...) {
        }
      }
      return;
    } catch (const std::bad_alloc &) {
      response.status = static_cast<uint32_t>(StatusCode::kOutOfMemory);
    } catch (const std::exception &error) {
      response.status = static_cast<uint32_t>(StatusCode::kCorruption);
      const size_t count = std::min(response.value.size(), std::strlen(error.what()));
      std::memcpy(response.value.data(), error.what(), count);
      response.value_size = static_cast<uint32_t>(count);
    } catch (...) {
      response.status = static_cast<uint32_t>(StatusCode::kCorruption);
    }
  } else if (message.type == KvMessageType::kIncrement) {
    int64_t delta = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), delta);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
      response.status = static_cast<uint32_t>(StatusCode::kInvalidArgument);
    } else {
      try {
        int64_t result = 0;
        if (!partition->IncrementPrivate(key, delta, &result)) {
          response.status = static_cast<uint32_t>(StatusCode::kNotFound);
        } else {
          const std::string encoded = std::to_string(result);
          response.status = static_cast<uint32_t>(StatusCode::kOk);
          response.value_size = static_cast<uint32_t>(encoded.size());
          std::memcpy(response.value.data(), encoded.data(), encoded.size());
        }
      } catch (const std::invalid_argument &) {
        response.status = static_cast<uint32_t>(StatusCode::kInvalidArgument);
      } catch (...) {
        response.status = static_cast<uint32_t>(StatusCode::kCorruption);
      }
    }
  } else if (message.type == KvMessageType::kCasPrepare) {
    std::lock_guard<std::mutex> lock(pending_cas_mutex_);
    pending_cas_[message.request_id] = {message.source_node, std::string(key), std::string(value)};
    response.status = static_cast<uint32_t>(StatusCode::kOk);
  } else if (message.type == KvMessageType::kCasCommit) {
    PendingCas pending;
    bool found = false;
    {
      std::lock_guard<std::mutex> lock(pending_cas_mutex_);
      auto it = pending_cas_.find(message.request_id);
      if (it != pending_cas_.end()) {
        pending = std::move(it->second);
        pending_cas_.erase(it);
        found = pending.source_node == message.source_node && pending.key == key;
      }
    }
    if (!found) {
      response.status = static_cast<uint32_t>(StatusCode::kInvalidArgument);
    } else {
      try {
        bool exchanged = false;
        if (!partition->CompareExchangePrivate(key, pending.expected, value, &exchanged))
          response.status = static_cast<uint32_t>(StatusCode::kNotFound);
        else
          response.status = static_cast<uint32_t>(exchanged ? StatusCode::kOk
                                                            : StatusCode::kCompareFailed);
      } catch (const std::invalid_argument &) {
        response.status = static_cast<uint32_t>(StatusCode::kInvalidArgument);
      } catch (...) {
        response.status = static_cast<uint32_t>(StatusCode::kCorruption);
      }
    }
  } else {
    response.status = static_cast<uint32_t>(StatusCode::kInvalidArgument);
  }
  SendTransportMessage(response);
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
  for (uint32_t attempt = 0; attempt < 16; ++attempt) {
    if (partition.MoveOutClockVictim(config_.node_id)) {
      migration_out_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  // Soft-fail: callers on the serve path must still deliver responses. Hard
  // throwing here caused "forwarded owner response timed out" under YCSB-A.
}

Status KVEngine::MoveOut(std::string_view key) {
  auto *partition = OwnedPartition(key);
  if (partition == nullptr) return Status::Error(StatusCode::kOwnerViolation, "remote owner requires forwarding");
  if (partition->MoveOutPrivate(key, config_.node_id)) {
    migration_out_.fetch_add(1, std::memory_order_relaxed);
    return Status::Ok();
  }
  return Status::Error(StatusCode::kNotFound, "shared key not found or busy");
}

}  // namespace tigonkv::engine
