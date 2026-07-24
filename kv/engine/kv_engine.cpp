#include "kv/engine/kv_engine.h"

#include "common/CXL_EBR.h"
#include "kv/engine/kv_partition.h"
#include "protocol/TwoPLPasha/TwoPLPashaSCCWriteThrough.h"

#include <stdexcept>

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
  auto ebr = std::make_unique<star::CXL_EBR>(config.vm_count,
                                             config.foreground_worker_count_per_vm,
                                             &pool->allocator());
  ebr->thread_init_ebr_meta(config.node_id, 0);
  auto scc = std::make_unique<star::TwoPLPashaSCCWriteThrough>();
  star::scc_manager = scc.get();
  auto engine = std::unique_ptr<KVEngine>(new KVEngine(config, std::move(pool),
                                                        std::move(ebr), std::move(scc)));
  const bool attach = !reset;
  for (uint32_t partition = 0; partition < config.partition_count; ++partition) {
    if (engine->OwnerForPartition(partition) != config.node_id) continue;
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
  if (partition == nullptr) return Status::Error(StatusCode::kOwnerViolation, "remote owner requires forwarding");
  try { partition->PutPrivate(key, value); return Status::Ok(); }
  catch (const std::bad_alloc &) { return Status::Error(StatusCode::kOutOfMemory, "private arena exhausted"); }
  catch (const std::exception &e) { return Status::Error(StatusCode::kCorruption, e.what()); }
}

GetResult KVEngine::Get(std::string_view key) {
  auto *partition = OwnedPartition(key);
  if (partition == nullptr) return {Status::Error(StatusCode::kOwnerViolation, "remote owner requires forwarding"), {}};
  std::string value;
  return partition->GetPrivate(key, &value) ? GetResult{Status::Ok(), std::move(value)}
                                            : GetResult{Status::Error(StatusCode::kNotFound, "key not found"), {}};
}

Status KVEngine::Delete(std::string_view key) {
  auto *partition = OwnedPartition(key);
  if (partition == nullptr) return Status::Error(StatusCode::kOwnerViolation, "remote owner requires forwarding");
  return partition->DeletePrivate(key) ? Status::Ok()
                                       : Status::Error(StatusCode::kNotFound, "key not found");
}

Status KVEngine::MoveOut(std::string_view key) {
  auto *partition = OwnedPartition(key);
  if (partition == nullptr) return Status::Error(StatusCode::kOwnerViolation, "remote owner requires forwarding");
  return partition->MoveOutPrivate(key, config_.node_id) ? Status::Ok()
                                                        : Status::Error(StatusCode::kNotFound, "shared key not found or busy");
}

}  // namespace tigonkv::engine
