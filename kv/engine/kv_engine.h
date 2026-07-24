#pragma once

#include "kv/engine/region_allocator.h"
#include "kv/kv_store.h"

#include <memory>
#include <string_view>
#include <vector>

namespace star {
class CXL_EBR;
class SCCManager;
}

namespace tigonkv::engine {

class KVPartition;

// Process-local assembly over persistent dual-region state.  Only partitions
// owned by config.node_id are materialized locally; remote transport is added
// by kv_messages in M5 without changing this ownership boundary.
class KVEngine {
 public:
  static std::unique_ptr<KVEngine> Open(const Config &config, bool reset);
  ~KVEngine();

  Status Put(std::string_view key, std::string_view value);
  GetResult Get(std::string_view key);
  Status Delete(std::string_view key);
  Status MoveOut(std::string_view key);
  uint32_t PartitionForKey(std::string_view key) const;
  uint32_t OwnerForKey(std::string_view key) const;

 private:
  KVEngine(const Config &config, std::unique_ptr<DualRegionMappedPool> pool,
           std::unique_ptr<star::CXL_EBR> ebr,
           std::unique_ptr<star::SCCManager> scc);
  KVPartition *OwnedPartition(std::string_view key) const;
  uint32_t OwnerForPartition(uint32_t partition) const;

  Config config_;
  std::unique_ptr<DualRegionMappedPool> pool_;
  std::unique_ptr<star::CXL_EBR> ebr_;
  std::unique_ptr<star::SCCManager> scc_;
  std::vector<std::unique_ptr<KVPartition>> partitions_;
};

}  // namespace tigonkv::engine
