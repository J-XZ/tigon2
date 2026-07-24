#include "kv/engine/kv_engine.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <string>
#include <unistd.h>

namespace {

tigonkv::Config ConfigFor(const std::string &path, uint32_t vm_count = 1,
                           uint32_t node_id = 0) {
  tigonkv::Config config;
  config.shared_memory_path = path;
  config.size_mb = 16;
  config.hwcc_offset_mb = 0;
  config.hwcc_size_mb = 4;
  config.swcc_offset_mb = 4;
  config.swcc_size_mb = 12;
  config.hw_cc_budget_mb = 4;
  config.vm_count = vm_count;
  config.node_id = node_id;
  config.partition_count = 8;
  config.fixed_key_size = 32;
  config.fixed_value_size = 128;
  config.foreground_worker_count_per_vm = 1;
  return config;
}

}  // namespace

int main() {
  char path_template[] = "/tmp/tigonkv-engine-XXXXXX";
  const int fd = mkstemp(path_template);
  assert(fd >= 0);
  close(fd);
  const std::string path(path_template);

  const auto single_owner = ConfigFor(path);
  {
    auto engine = tigonkv::engine::KVEngine::Open(single_owner, true);
    assert(engine->Put("alpha", "one").ok());
    assert(engine->Put("alpha", "updated").ok());
    const auto found = engine->Get("alpha");
    assert(found.status.ok() && found.value == "updated");
    assert(engine->Delete("alpha").ok());
    assert(engine->Get("alpha").status.code == tigonkv::StatusCode::kNotFound);
    assert(engine->Put("persist", "value").ok());
  }
  {
    auto attached = tigonkv::engine::KVEngine::Open(single_owner, false);
    const auto found = attached->Get("persist");
    assert(found.status.ok() && found.value == "value");
  }
  unlink(path.c_str());

  char routed_template[] = "/tmp/tigonkv-engine-route-XXXXXX";
  const int routed_fd = mkstemp(routed_template);
  assert(routed_fd >= 0);
  close(routed_fd);
  const std::string routed_path(routed_template);
  const auto node_zero = ConfigFor(routed_path, 2, 0);
  {
    auto engine = tigonkv::engine::KVEngine::Open(node_zero, true);
    for (uint32_t i = 0; i < 100; ++i) {
      const std::string key = "route-" + std::to_string(i);
      const uint32_t partition = engine->PartitionForKey(key);
      assert(engine->OwnerForKey(key) == partition % node_zero.vm_count);
      if (engine->OwnerForKey(key) == 1) {
        assert(engine->Put(key, "remote").code == tigonkv::StatusCode::kOwnerViolation);
        break;
      }
    }
  }
  unlink(routed_path.c_str());
  return 0;
}
