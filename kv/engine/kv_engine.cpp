#include "kv/engine/kv_engine.h"

#include "common/CXL_EBR.h"
#include "common/MPSCRingBuffer.h"
#include "kv/engine/kv_partition.h"
#include "kv/engine/kv_messages.h"
#include "protocol/TwoPLPasha/TwoPLPashaSCCWriteThrough.h"

#include <stdexcept>
#include <thread>
#include <chrono>
#include <charconv>
#include <map>

namespace tigonkv::engine {
namespace {

uint64_t Hash(std::string_view key) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : key) { h ^= c; h *= 1099511628211ULL; }
  return h;
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
                   std::unique_ptr<star::CXL_EBR> ebr,
                   std::unique_ptr<star::SCCManager> scc)
    : config_(config), pool_(std::move(pool)), ebr_(std::move(ebr)), scc_(std::move(scc)) {}

KVEngine::~KVEngine() {
  if (star::scc_manager == scc_.get()) star::scc_manager = nullptr;
}

std::unique_ptr<KVEngine> KVEngine::Open(const Config &config, bool reset) {
  config.Validate();
  auto pool = std::make_unique<DualRegionMappedPool>(
      DualRegionMappedPool::Open(config.shared_memory_path, RegionConfig(config), reset));
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
  } else {
    void *root = nullptr;
    star::CXLMemory::wait_and_retrieve_cxl_shared_data(
        star::CXLMemory::cxl_transport_root_index, &root);
    rings = static_cast<star::MPSCRingBuffer *>(root);
  }
  auto ebr = std::make_unique<star::CXL_EBR>(config.vm_count,
                                             config.foreground_worker_count_per_vm,
                                             &pool->allocator());
  ebr->thread_init_ebr_meta(config.node_id, 0);
  auto scc = std::make_unique<star::TwoPLPashaSCCWriteThrough>();
  star::scc_manager = scc.get();
  auto engine = std::unique_ptr<KVEngine>(new KVEngine(config, std::move(pool),
                                                        std::move(ebr), std::move(scc)));
  engine->rings_ = rings;
  for (uint32_t partition = 0; partition < config.partition_count; ++partition) {
    if (engine->OwnerForPartition(partition) != config.node_id) continue;
    const auto &directory = engine->pool_->allocator().layout().partitions[partition];
    // A node may be the first owner to attach after node 0 created the shared
    // layout. It alone creates its owner-private roots; later attaches recover
    // them by offsets.
    const bool attach = directory.private_root != kNullOffset &&
                        directory.shared_root != kNullOffset;
    engine->partitions_.emplace_back(std::make_unique<KVPartition>(
        engine->pool_->allocator(), *engine->ebr_, partition, config.node_id, attach));
  }
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
  const uint32_t partition = PartitionForKey(key);
  for (const auto &entry : partitions_)
    if (entry->partition_id() == partition) return entry.get();
  return nullptr;
}

Status KVEngine::Put(std::string_view key, std::string_view value) {
  auto *partition = OwnedPartition(key);
  if (partition == nullptr) return Forward(KvMessageType::kPut, key, value, nullptr);
  try { partition->PutPrivate(key, value); return Status::Ok(); }
  catch (const std::bad_alloc &) { return Status::Error(StatusCode::kOutOfMemory, "private arena exhausted"); }
  catch (const std::exception &e) { return Status::Error(StatusCode::kCorruption, e.what()); }
}

GetResult KVEngine::Get(std::string_view key) {
  auto *partition = OwnedPartition(key);
  if (partition == nullptr) {
    std::string value;
    auto status = Forward(KvMessageType::kGet, key, {}, &value);
    return {std::move(status), std::move(value)};
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
  if (config_.vm_count != 1)
    return {Status::Error(StatusCode::kOwnerViolation,
                          "cross-owner scan forwarding is not installed"), {}};
  std::map<std::string, std::string> merged;
  const uint64_t per_partition_limit = limit == 0 ? 0 : limit;
  try {
    for (const auto &partition : partitions_) {
      std::vector<std::pair<std::string, std::string>> items;
      if (!partition->ScanOwned(start_key, per_partition_limit, &items))
        return {Status::Error(StatusCode::kCorruption,
                              "partition scan exceeded migration retry budget"), {}};
      for (auto &item : items) merged.emplace(std::move(item));
    }
  } catch (const std::exception &e) {
    return {Status::Error(StatusCode::kCorruption, e.what()), {}};
  }
  ScanResult result{Status::Ok(), {}};
  for (auto &item : merged) {
    if (limit != 0 && result.items.size() >= limit) break;
    result.items.push_back({std::move(item.first), std::move(item.second)});
  }
  return result;
}

CasResult KVEngine::CompareExchange(std::string_view key,
                                    std::string_view expected,
                                    std::string_view desired) {
  auto *partition = OwnedPartition(key);
  if (partition == nullptr) {
    return {Status::Error(StatusCode::kOwnerViolation,
                          "remote CAS forwarding is not installed"), false};
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

void KVEngine::SendTransportMessage(const KvMessage &message) {
  while (!rings_[message.destination_node].enqueue(
      const_cast<char *>(reinterpret_cast<const char *>(&message)), sizeof(message))) {
    PollTransport();
    std::this_thread::yield();
  }
}

Status KVEngine::Forward(KvMessageType type, std::string_view key, std::string_view value,
                         std::string *response_value) {
  const uint32_t owner = OwnerForKey(key);
  const uint64_t request_id = (static_cast<uint64_t>(config_.node_id) << 56) | next_request_id_++;
  SendTransportMessage(MakeRequest(type, config_.node_id, owner, request_id, key, value));
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::seconds(config_.sync_timeout_sec);
  for (;;) {
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
    return {static_cast<StatusCode>(response.status),
            static_cast<StatusCode>(response.status) == StatusCode::kOk ? "" : "forwarded owner operation failed"};
  }
}

void KVEngine::PollTransport() {
  if (rings_ == nullptr) return;
  alignas(64) char bytes[sizeof(KvMessage)];
  const uint64_t received = rings_[config_.node_id].recv(bytes, sizeof(bytes));
  if (received == 0) return;
  if (received != sizeof(KvMessage)) throw std::runtime_error("malformed KV transport entry");
  KvMessage message{};
  std::memcpy(&message, bytes, sizeof(message));
  HandleTransportMessage(message);
}

void KVEngine::HandleTransportMessage(const KvMessage &message) {
  if (message.destination_node != config_.node_id ||
      message.key_size > message.key.size() || message.value_size > message.value.size())
    throw std::runtime_error("invalid KV transport message");
  if (message.type == KvMessageType::kResponse) {
    std::lock_guard<std::mutex> lock(response_mutex_);
    responses_.emplace(message.request_id, message);
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
      // A remote touch is the move-in trigger; a failed promotion means this
      // row was already shared, which is equally valid for the response.
      partition->PromotePrivate(key, config_.node_id);
      response.status = static_cast<uint32_t>(StatusCode::kOk);
      response.value_size = static_cast<uint32_t>(result.size());
      std::memcpy(response.value.data(), result.data(), result.size());
      EnforceMigrationBudget(*partition);
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
  if (partition.hwcc_used_bytes() < hw_budget && !payload_high) return;
  for (uint32_t attempt = 0; attempt < 16; ++attempt) {
    if (partition.MoveOutClockVictim(config_.node_id)) return;
  }
  throw std::runtime_error("migration budget exceeded with no quiescent Clock victim");
}

Status KVEngine::MoveOut(std::string_view key) {
  auto *partition = OwnedPartition(key);
  if (partition == nullptr) return Status::Error(StatusCode::kOwnerViolation, "remote owner requires forwarding");
  return partition->MoveOutPrivate(key, config_.node_id) ? Status::Ok()
                                                        : Status::Error(StatusCode::kNotFound, "shared key not found or busy");
}

}  // namespace tigonkv::engine
