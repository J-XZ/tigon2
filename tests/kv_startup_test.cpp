#include "kv/engine/kv_engine.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>

namespace {

void SetTestRangePartitioning(tigonkv::Config *config) {
  const std::string split(1, static_cast<char>(0x80));
  config->partition_ranges = {{"", split}, {split, ""}};
}

tigonkv::Config ConfigFor(const std::string &path, uint32_t node_id) {
  tigonkv::Config config;
  config.shared_memory_path = path;
  config.size_mb = 32;
  config.hwcc_offset_mb = 0;
  config.hwcc_size_mb = 16;
  config.swcc_offset_mb = 16;
  config.swcc_size_mb = 16;
  config.hw_cc_budget_mb = 4;
  config.vm_count = 2;
  config.node_id = node_id;
  config.partition_count = 2;
  config.fixed_key_size = 32;
  config.fixed_value_size = 128;
  config.foreground_worker_count_per_vm = 1;
  config.transport_ring_total_mb = 1;
  SetTestRangePartitioning(&config);
  return config;
}

void WaitForStaticLayout(const char *path) {
  const int fd = open(path, O_RDONLY);
  assert(fd >= 0);
  tigonkv::engine::SharedLayoutHeader header{};
  for (;;) {
    const ssize_t bytes = pread(fd, &header, sizeof(header), 0);
    if (bytes == static_cast<ssize_t>(sizeof(header)) &&
        header.magic == tigonkv::engine::kSharedLayoutMagic &&
        header.layout_version == tigonkv::engine::kSharedLayoutVersion)
      break;
    std::this_thread::yield();
  }
  close(fd);
}

}  // namespace

int main() {
  char path[] = "/tmp/tigonkv-startup-XXXXXX";
  const int seed = mkstemp(path);
  assert(seed >= 0);
  close(seed);

  const pid_t vm0 = fork();
  assert(vm0 >= 0);
  if (vm0 == 0) {
    auto engine = tigonkv::engine::KVEngine::Open(ConfigFor(path, 0), true);
    const auto &layout = engine->Memory();
    (void)layout;
    _exit(0);
  }

  WaitForStaticLayout(path);
  auto vm1_engine = tigonkv::engine::KVEngine::Open(ConfigFor(path, 1), false);
  const auto &layout = vm1_engine->Memory();
  (void)layout;
  const std::string internal_max(32, static_cast<char>(0xff));
  assert(vm1_engine->Get(internal_max).status.code ==
         tigonkv::StatusCode::kInvalidArgument);
  assert(vm1_engine->Put(internal_max, std::string(128, '\0')).code ==
         tigonkv::StatusCode::kInvalidArgument);
  assert(vm1_engine->Scan(internal_max, {}, 1).status.code ==
         tigonkv::StatusCode::kInvalidArgument);
  int status = 0;
  assert(waitpid(vm0, &status, 0) == vm0);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  unlink(path);
  return 0;
}
