#include "kv/engine/kv_engine.h"
#include "common/CXLMemory.h"
#include "common/Encoder.h"
#include "common/Message.h"
#include "common/MessagePiece.h"
#include "common/MPSCRingBuffer.h"
#include "kv/engine/latency_inject.h"
#include "protocol/TwoPLPasha/TwoPLPashaMessage.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <algorithm>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {

void SetTestRangePartitioning(tigonkv::Config *config) {
  config->partition_ranges.clear();
  std::string lower;
  for (uint32_t partition = 0; partition < config->partition_count; ++partition) {
    std::string upper;
    if (partition + 1 != config->partition_count)
      upper.assign(1, static_cast<char>((partition + 1) * 256 /
                                        config->partition_count));
    config->partition_ranges.push_back({lower, upper});
    lower = std::move(upper);
  }
}

tigonkv::Config ConfigFor(const std::string &path, uint32_t vm_count = 1,
                           uint32_t node_id = 0) {
  tigonkv::Config config;
  config.shared_memory_path = path;
  // §11.10: physical HWCC must hold static (layout/transport/EBR/allocator)
  // plus vm_count * owner dynamic budget; keep budget below hwcc.size_mb.
  config.size_mb = 32;
  config.hwcc_offset_mb = 0;
  config.hwcc_size_mb = 16;
  config.swcc_offset_mb = 16;
  config.swcc_size_mb = 16;
  config.hw_cc_budget_mb = 4;
  config.vm_count = vm_count;
  config.node_id = node_id;
  config.partition_count = 8;
  config.fixed_key_size = 32;
  config.fixed_value_size = 128;
  config.foreground_worker_count_per_vm = 1;
  config.transport_ring_total_mb = 1;
  SetTestRangePartitioning(&config);
  return config;
}

std::string ScanEndKey(uint32_t fixed_key_size = 32) {
  return std::string(fixed_key_size, static_cast<char>(0xff));
}

std::string FixedValue(std::string_view value_text,
                       uint32_t fixed_value_size = 128) {
  std::string value(fixed_value_size, '\0');
  std::memcpy(value.data(), value_text.data(), value_text.size());
  return value;
}

// A reset VM must wait for every owner to publish its private arena.  Keep the
// unit fixture faithful to that protocol by starting a real joining owner,
// rather than weakening KVEngine::Open for single-process tests.
class JoiningPeer {
 public:
  explicit JoiningPeer(tigonkv::Config config) : config_(std::move(config)) {
    assert(pipe2(stop_pipe_, O_CLOEXEC) == 0);
    child_ = fork();
    assert(child_ >= 0);
    if (child_ == 0) {
      close(stop_pipe_[1]);
      assert(fcntl(stop_pipe_[0], F_SETFL,
                   fcntl(stop_pipe_[0], F_GETFL) | O_NONBLOCK) == 0);
      std::unique_ptr<tigonkv::engine::KVEngine> engine;
      for (;;) {
        try {
          engine = tigonkv::engine::KVEngine::Open(config_, false);
          break;
        } catch (const std::exception &) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      engine->BindWorker(0);
      char ignored;
      for (;;) {
        const ssize_t stopped = read(stop_pipe_[0], &ignored, 1);
        if (stopped == 0) break;
        assert(stopped < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        engine->PollTransport();
        std::this_thread::yield();
      }
      engine->ReleaseWorker();
      _exit(0);
    }
    close(stop_pipe_[0]);
  }

  ~JoiningPeer() {
    if (child_ <= 0) return;
    close(stop_pipe_[1]);
    int status = 0;
    assert(waitpid(child_, &status, 0) == child_);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }

  JoiningPeer(const JoiningPeer &) = delete;
  JoiningPeer &operator=(const JoiningPeer &) = delete;

 private:
  tigonkv::Config config_;
  int stop_pipe_[2]{-1, -1};
  pid_t child_{-1};
};

}  // namespace

int main() {
  char corrupt_template[] = "/tmp/tigonkv-engine-corrupt-XXXXXX";
  const int corrupt_fd = mkstemp(corrupt_template);
  assert(corrupt_fd >= 0);
  close(corrupt_fd);
  const pid_t corrupt_child = fork();
  assert(corrupt_child >= 0);
  if (corrupt_child == 0) {
    const rlimit no_core{0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    const auto corrupt_config = ConfigFor(corrupt_template);
    auto engine = tigonkv::engine::KVEngine::Open(corrupt_config, true);
    void *root = nullptr;
    star::CXLMemory::wait_and_retrieve_cxl_shared_data(
        star::CXLMemory::cxl_transport_root_index, &root);
    auto *rings = static_cast<star::MPSCRingBuffer *>(root);
    std::array<char, 64> malformed{};
    while (!rings[0].enqueue(
        malformed.data(), malformed.size()))
      std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    _exit(90);
  }
  int corrupt_status = 0;
  assert(waitpid(corrupt_child, &corrupt_status, 0) == corrupt_child);
  assert(WIFSIGNALED(corrupt_status) && WTERMSIG(corrupt_status) == SIGABRT);
  unlink(corrupt_template);

  char misroute_template[] = "/tmp/tigonkv-engine-misroute-XXXXXX";
  const int misroute_fd = mkstemp(misroute_template);
  assert(misroute_fd >= 0);
  close(misroute_fd);
  const pid_t misroute_child = fork();
  assert(misroute_child >= 0);
  if (misroute_child == 0) {
    const rlimit no_core{0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    const auto config = ConfigFor(misroute_template, 2, 0);
    auto peer = JoiningPeer(ConfigFor(misroute_template, 2, 1));
    auto engine = tigonkv::engine::KVEngine::Open(config, true);
    std::string wrong_owner_key;
    for (uint32_t i = 0; i < 1000; ++i) {
      wrong_owner_key = "misroute-" + std::to_string(i);
      if (engine->OwnerForKey(wrong_owner_key) == 1) break;
    }
    void *root = nullptr;
    star::CXLMemory::wait_and_retrieve_cxl_shared_data(
        star::CXLMemory::cxl_transport_root_index, &root);
    auto *rings = static_cast<star::MPSCRingBuffer *>(root);
    star::Message request;
    request.set_source_node_id(1);
    request.set_dest_node_id(0);
    request.set_worker_id(0);
    const uint32_t piece_bytes = star::MessagePiece::get_header_size() +
        config.fixed_key_size + sizeof(uint64_t) + sizeof(uint32_t);
    star::Encoder encoder(request.data);
    encoder << star::MessagePiece::construct_message_piece_header(
        static_cast<uint32_t>(star::TwoPLPashaMessage::DATA_MIGRATION_REQUEST),
        piece_bytes, tigonkv::engine::kSingleTableId,
        engine->PartitionForKey(wrong_owner_key));
    std::string fixed_key(config.fixed_key_size, '\0');
    std::memcpy(fixed_key.data(), wrong_owner_key.data(), wrong_owner_key.size());
    encoder.write_n_bytes(fixed_key.data(), fixed_key.size());
    encoder << uint64_t{7} << uint32_t{0};
    request.flush();
    while (!rings[0].enqueue(
        request.get_raw_ptr(), request.get_message_length()))
      std::this_thread::yield();
    for (;;) engine->PollTransport();
  }
  int misroute_status = 0;
  assert(waitpid(misroute_child, &misroute_status, 0) ==
         misroute_child);
  assert(WIFSIGNALED(misroute_status) &&
         WTERMSIG(misroute_status) == SIGABRT);
  unlink(misroute_template);


  char path_template[] = "/tmp/tigonkv-engine-XXXXXX";
  const int fd = mkstemp(path_template);
  assert(fd >= 0);
  close(fd);
  const std::string path(path_template);

  const auto single_owner = ConfigFor(path);
  {
    auto engine = tigonkv::engine::KVEngine::Open(single_owner, true);
    assert(star::CXLMemory::bound_owner_shard() == single_owner.node_id);
    {
      const auto mem = engine->Memory();
      assert(mem.physical_hwcc_capacity_bytes ==
             single_owner.hwcc_size_mb * 1024ULL * 1024ULL);
      assert(mem.owner_migration_dynamic_budget_bytes > 0);
      assert(mem.allocator_local_dram_bytes == 0);
    }
    // Range partition routing is stable and owner assignment remains index-aligned.
    for (uint32_t i = 0; i < 4096; ++i) {
      const std::string key = "route-oracle-" + std::to_string(i);
      const uint32_t partition = engine->PartitionForKey(key);
      assert(partition < single_owner.partition_count);
      assert(engine->OwnerForKey(key) ==
             partition % single_owner.vm_count);
    }
    assert(engine->Put("alpha", "one").ok());
    assert(engine->Put("alpha", "updated").ok());
    const auto found = engine->Get("alpha");
    assert(found.status.ok() && found.value == FixedValue("updated"));
    const auto cas = engine->CompareExchange("alpha", "updated", "cas-value");
    assert(cas.status.ok() && cas.exchanged);
    const auto cas_failed = engine->CompareExchange("alpha", "updated", "ignored");
    assert(cas_failed.status.code == tigonkv::StatusCode::kCompareFailed && !cas_failed.exchanged);
    assert(engine->Put("counter", FixedValue("1")).ok());
    const auto incremented = engine->Increment("counter", 2);
    assert(incremented.status.ok() && incremented.value == 3);
    const auto scan = engine->Scan("alpha", ScanEndKey(), 0);
    assert(scan.status.ok() && scan.items.size() == 2);
    assert(scan.items[0].key == "alpha" &&
           scan.items[0].value == FixedValue("cas-value"));
    assert(scan.items[1].key == "counter" &&
           scan.items[1].value == FixedValue("3"));
    {
      const auto rt = engine->EngineRuntime();
      assert(rt.scan_partition_probes >= 1);
      assert(rt.scan_migrate_rpcs == 0);
    }
    assert(engine->Delete("alpha").ok());
    assert(engine->Get("alpha").status.code == tigonkv::StatusCode::kNotFound);
    std::atomic<bool> worker_bound{false};
    std::atomic<bool> release_worker{false};
    std::thread worker_owner([&] {
      engine->BindWorker(0);
      worker_bound.store(true, std::memory_order_release);
      while (!release_worker.load(std::memory_order_acquire))
        std::this_thread::yield();
      engine->ReleaseWorker();
    });
    while (!worker_bound.load(std::memory_order_acquire))
      std::this_thread::yield();
    bool duplicate_worker_rejected = false;
    try {
      engine->BindWorker(0);
    } catch (const std::runtime_error &) {
      duplicate_worker_rejected = true;
    }
    assert(duplicate_worker_rejected);
    release_worker.store(true, std::memory_order_release);
    worker_owner.join();
    assert(engine->Put("persist", "value").ok());
  }
  {
    auto attached = tigonkv::engine::KVEngine::Open(single_owner, false);
    const auto found = attached->Get("persist");
    assert(found.status.ok() && found.value == FixedValue("value"));
  }
  {
    auto changed_contract = single_owner;
    changed_contract.transport_ring_total_mb = 2;
    bool rejected = false;
    try {
      (void)tigonkv::engine::KVEngine::Open(changed_contract, false);
    } catch (const std::runtime_error &) {
      rejected = true;
    }
    assert(rejected);
    auto local_wiring_only = single_owner;
    local_wiring_only.device_path = "/dev/not-used-for-file-backed-test";
    local_wiring_only.network_base_ssh_port += 1;
    auto attached = tigonkv::engine::KVEngine::Open(local_wiring_only, false);
    assert(attached->Get("persist").status.ok());
  }
  unlink(path.c_str());

  // §5.2 PreparePartitionSharedScan: cold / already-shared / EOF / bad args.
  {
    char pscan_template[] = "/tmp/tigonkv-engine-pscan-XXXXXX";
    const int pscan_fd = mkstemp(pscan_template);
    assert(pscan_fd >= 0);
    close(pscan_fd);
    auto pscan_config = ConfigFor(pscan_template);
    auto engine = tigonkv::engine::KVEngine::Open(pscan_config, true);
    uint32_t part = 0;
    std::vector<std::string> owned;
    for (int i = 0; i < 64; ++i) {
      const std::string k = "pscan-" + std::to_string(i);
      assert(engine->Put(k, "v").ok());
    }
    part = engine->PartitionForKey("pscan-0");
    for (int i = 0; i < 64; ++i) {
      const std::string k = "pscan-" + std::to_string(i);
      if (engine->PartitionForKey(k) == part) owned.push_back(k);
    }
    assert(!owned.empty());
    std::sort(owned.begin(), owned.end());
    const std::string scan_max(pscan_config.fixed_key_size,
                               static_cast<char>(0xff));
    assert(engine
               ->PreparePartitionSharedScan(part, owned.front(), scan_max, 2)
               .ok());
    assert(engine
               ->PreparePartitionSharedScan(part, owned.front(), scan_max, 2)
               .ok());
    assert(engine
               ->PreparePartitionSharedScan(part, owned.front(), scan_max, 64)
               .ok());
    assert(engine
               ->PreparePartitionSharedScan(999, "x", scan_max, 2)
               .code == tigonkv::StatusCode::kInvalidArgument);
    assert(engine
               ->PreparePartitionSharedScan(part, "x", scan_max, 0)
               .ok());
    unlink(pscan_template);
  }

  // §11.12 / §15.1: single-VM Scan oracle — exact key set vs start/limit.
  {
    char oracle_template[] = "/tmp/tigonkv-engine-scan-oracle-XXXXXX";
    const int oracle_fd = mkstemp(oracle_template);
    assert(oracle_fd >= 0);
    close(oracle_fd);
    const std::string oracle_path(oracle_template);
    auto oracle_config = ConfigFor(oracle_path);
    oracle_config.partition_count = 16;
    SetTestRangePartitioning(&oracle_config);
    auto engine = tigonkv::engine::KVEngine::Open(oracle_config, true);
    std::vector<std::string> keys;
    for (int i = 0; i < 64; ++i) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "k%02d", i);
      keys.emplace_back(buf);
      assert(engine->Put(keys.back(), std::string("v") + buf).ok());
    }
    std::sort(keys.begin(), keys.end());
    const auto expect_scan = [&](std::string_view start, uint64_t limit) {
      std::vector<std::string> expected;
      for (const auto &key : keys) {
        if (key < start) continue;
        expected.push_back(key);
        if (limit != 0 && expected.size() >= limit) break;
      }
      const auto got = engine->Scan(start, ScanEndKey(), limit);
      assert(got.status.ok());
      assert(got.items.size() == expected.size());
      for (size_t i = 0; i < expected.size(); ++i) {
        assert(got.items[i].key == expected[i]);
        assert(got.items[i].value == FixedValue(std::string("v") + expected[i]));
      }
    };
    expect_scan("", 0);
    expect_scan("", 7);
    expect_scan("k00", 1);
    expect_scan("k00", 0);
    expect_scan("k10", 5);
    expect_scan("k63", 1);
    expect_scan("k63", 10);
    expect_scan("k99", 10);  // past end → empty
    expect_scan("k05", 0);
    // Mid-key that is not present still returns the next key onward.
    expect_scan("k0a", 3);
    unlink(oracle_path.c_str());
  }

  char routed_template[] = "/tmp/tigonkv-engine-route-XXXXXX";
  const int routed_fd = mkstemp(routed_template);
  assert(routed_fd >= 0);
  close(routed_fd);
  const std::string routed_path(routed_template);
  auto node_zero = ConfigFor(routed_path, 2, 0);
  node_zero.foreground_worker_count_per_vm = 4;
  auto node_one_config = ConfigFor(routed_path, 2, 1);
  node_one_config.foreground_worker_count_per_vm = 4;
  {
    std::unique_ptr<tigonkv::engine::KVEngine> engine;
    {
      // The later child is the real peer for this test.  It cannot join until
      // after the parent has seeded the owner-0 state, so use a short-lived
      // owner only to satisfy first-layout initialization.
      auto bootstrap = JoiningPeer(node_one_config);
      engine = tigonkv::engine::KVEngine::Open(node_zero, true);
    }
    assert(star::CXLMemory::bound_owner_shard() == 0);
    for (const std::string_view key : {"H-route", "a-route"}) {
      const uint32_t partition = engine->PartitionForKey(key);
      assert(engine->OwnerForKey(key) == partition % node_zero.vm_count);
    }
    // Constructing every partition (owners 0 and 1) must not rebind the
    // process-level allocator owner away from this VM (§11.3).
    assert(star::CXLMemory::bound_owner_shard() == 0);

    const std::string owner_zero_key = "H-owner-zero";
    const std::string owner_one_key = "a-owner-one";
    assert(engine->OwnerForKey(owner_zero_key) == 0);
    assert(engine->OwnerForKey(owner_one_key) == 1);
    assert(engine->Put(owner_zero_key, "owner-zero").ok());
    constexpr uint32_t kRemoteScanRows = 1024;
    uint32_t remote_scan_rows = 0;
    for (uint32_t i = 0; remote_scan_rows < kRemoteScanRows; ++i) {
      const std::string key = "H0-bulk-" + std::to_string(i);
      assert(engine->Put(key, "bulk").ok());
      ++remote_scan_rows;
    }
    // Populate one remote partition past a Scan page boundary. The owner
    // moves each authoritative prefix in without a persistent pin; the
    // requester reads it only through CXL.
    std::vector<std::string> promoted_scan_keys;
    uint32_t promoted_partition = UINT32_MAX;
    for (uint32_t i = 0; promoted_scan_keys.size() < 130; ++i) {
      char key[32];
      std::snprintf(key, sizeof(key), "H-hybrid-%08u", i);
      const uint32_t partition = engine->PartitionForKey(key);
      if (promoted_partition == UINT32_MAX) promoted_partition = partition;
      if (partition != promoted_partition) continue;
      assert(engine->Put(key, "owner-authority").ok());
      promoted_scan_keys.emplace_back(key);
    }
    std::vector<std::string> concurrent_insert_keys;
    for (uint32_t i = 0; concurrent_insert_keys.size() < 4; ++i) {
      const std::string key =
          "H-hybrid-00000000-insert-" + std::to_string(i);
      concurrent_insert_keys.push_back(key);
    }
    int scan_ready[2];
    assert(pipe2(scan_ready, O_CLOEXEC | O_NONBLOCK) == 0);
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
      close(scan_ready[0]);
      auto node_one = tigonkv::engine::KVEngine::Open(node_one_config, false);
      if (star::CXLMemory::bound_owner_shard() != 1) _exit(30);
      if (!node_one->Put(owner_one_key, "owner-one").ok()) _exit(1);
      for (const auto &key : promoted_scan_keys) {
        const auto promoted = node_one->Get(key);
        if (!promoted.status.ok() ||
            promoted.value != FixedValue("owner-authority"))
          _exit(14);
      }
      // KVEngine exposes one-shot primitives.  Mirror the production
      // KVStore boundary here rather than restoring an Engine-local Scan
      // retry after a successful range move-in.
      auto scan_with_facade_retry = [&](std::string_view start,
                                        uint64_t limit) {
        tigonkv::ScanResult result;
        for (uint32_t attempt = 0; attempt != 64; ++attempt) {
          result = node_one->Scan(start, ScanEndKey(), limit);
          if (result.status.code != tigonkv::StatusCode::kBusy) return result;
          std::this_thread::yield();
        }
        return result;
      };
      const uint64_t tx_before_authoritative_scan = node_one->NetworkTxBytes();
      const auto authoritative_scan = scan_with_facade_retry(
          promoted_scan_keys.front(), 100);
      const uint64_t authoritative_scan_tx =
          node_one->NetworkTxBytes() - tx_before_authoritative_scan;
      if (!authoritative_scan.status.ok() || authoritative_scan.items.size() != 100)
        _exit(15);
      for (size_t i = 0; i < authoritative_scan.items.size(); ++i) {
        const auto &item = authoritative_scan.items[i];
        if (item.key != promoted_scan_keys[i] ||
            item.value != FixedValue("owner-authority"))
          _exit(16);
      }
      // A complete CXL range does not send an owner-value RPC.  If migration
      // was needed, the only frame remains the original scan-migration one.
      (void)authoritative_scan_tx;
      const auto boundary_scan = scan_with_facade_retry(promoted_scan_keys[63], 3);
      if (!boundary_scan.status.ok() || boundary_scan.items.size() != 3)
        _exit(29);
      for (size_t i = 0; i < boundary_scan.items.size(); ++i)
        if (boundary_scan.items[i].key != promoted_scan_keys[63 + i])
          _exit(30);
      if (write(scan_ready[1], "s", 1) != 1) _exit(28);

      std::atomic<bool> start_concurrent_scans{false};
      std::atomic<bool> concurrent_scan_failed{false};
      std::vector<std::thread> scan_threads;
      // This fixture's owner process has one foreground service loop, hence
      // it can only drain the matching original worker-0 inbox.  Multiworker
      // transport is covered by the symmetric 4VM runners; do not introduce
      // a test-only shared dispatcher just to make this asymmetric fork pass.
      for (uint32_t worker = 0; worker < 1; ++worker) {
        scan_threads.emplace_back([&, worker] {
          node_one->BindWorker(worker);
          while (!start_concurrent_scans.load(std::memory_order_acquire))
            std::this_thread::yield();
          const auto scan = scan_with_facade_retry(
              promoted_scan_keys.front(), 100);
          if (!scan.status.ok() || scan.items.size() != 100) {
            concurrent_scan_failed.store(true, std::memory_order_release);
          } else {
            for (size_t i = 1; i < scan.items.size(); ++i)
              if (scan.items[i - 1].key >= scan.items[i].key)
                concurrent_scan_failed.store(true,
                                             std::memory_order_release);
          }
          node_one->ReleaseWorker();
        });
      }
      start_concurrent_scans.store(true, std::memory_order_release);
      for (auto &thread : scan_threads) thread.join();
      if (concurrent_scan_failed.load(std::memory_order_acquire)) _exit(18);

      const auto limited_scan = scan_with_facade_retry(
          promoted_scan_keys.front(), 17);
      if (!limited_scan.status.ok() || limited_scan.items.size() != 17) _exit(12);
      for (size_t i = 1; i < limited_scan.items.size(); ++i) {
        if (limited_scan.items[i - 1].key >= limited_scan.items[i].key) _exit(13);
      }
      if (!node_one->Put(owner_zero_key, "forwarded").ok()) _exit(3);
      const uint64_t tx_after_remote_update = node_one->NetworkTxBytes();
      const auto read = node_one->Get(owner_zero_key);
      if (!read.status.ok() || read.value != FixedValue("forwarded")) _exit(4);
      if (node_one->NetworkTxBytes() != tx_after_remote_update) _exit(24);
      const uint64_t tx_after_promotion = node_one->NetworkTxBytes();
      const auto shared_read = node_one->Get(owner_zero_key);
      if (!shared_read.status.ok() ||
          shared_read.value != FixedValue("forwarded"))
        _exit(19);
      if (!node_one->Put(owner_zero_key, "shared-put").ok()) _exit(20);
      if (node_one->NetworkTxBytes() != tx_after_promotion) _exit(21);
      const auto cas = node_one->CompareExchange(owner_zero_key, "shared-put", "cas-forwarded");
      if (!cas.status.ok() || !cas.exchanged) _exit(5);
      const auto cas_miss = node_one->CompareExchange(owner_zero_key, "forwarded", "ignored");
      if (cas_miss.status.code != tigonkv::StatusCode::kCompareFailed || cas_miss.exchanged) _exit(6);
      // The GET above promoted this row.  Both CAS operations must use the
      // non-owner shared fast path rather than send another fixed transport frame.
      if (node_one->NetworkTxBytes() != tx_after_promotion) _exit(11);
      const std::string counter_key = "H-counter";
      if (counter_key.empty() ||
          !node_one->Put(counter_key, FixedValue("1")).ok()) _exit(7);
      const auto increment = node_one->Increment(counter_key, 2);
      if (!increment.status.ok() || increment.value != 3) _exit(8);
      const uint64_t tx_after_remote_increment = node_one->NetworkTxBytes();
      const auto promoted_counter = node_one->Get(counter_key);
      if (!promoted_counter.status.ok() ||
          promoted_counter.value != FixedValue("3")) _exit(22);
      if (node_one->NetworkTxBytes() != tx_after_remote_increment) _exit(23);
      const std::string cas_create_key = "H-cas-create";
      const auto cas_create =
          node_one->CompareExchange(cas_create_key, "", "created");
      if (!cas_create.status.ok() || !cas_create.exchanged) _exit(25);
      const auto created = node_one->Get(cas_create_key);
      if (!created.status.ok() || created.value != FixedValue("created"))
        _exit(26);
      const std::string cas_race_key = "H-cas-race";
      std::atomic<uint32_t> cas_winners{0};
      std::atomic<bool> cas_protocol_failed{false};
      std::vector<std::thread> cas_threads;
      for (uint32_t worker = 0; worker < 4; ++worker) {
        cas_threads.emplace_back([&, worker] {
          node_one->BindWorker(worker);
          tigonkv::CasResult result;
          // KVEngine is the one-shot primitive; KVStore is the only
          // production Busy-retry facade.  Exercise the same bounded retry
          // here so a remote-create loser observes the published winner and
          // becomes CompareFailed rather than being misclassified as a wire
          // protocol failure.
          for (uint32_t attempt = 0; attempt != 64; ++attempt) {
            result = node_one->CompareExchange(cas_race_key, "", "winner");
            if (result.status.code != tigonkv::StatusCode::kBusy) break;
            std::this_thread::yield();
          }
          if (result.status.ok() && result.exchanged)
            cas_winners.fetch_add(1, std::memory_order_relaxed);
          else if (result.status.code != tigonkv::StatusCode::kCompareFailed)
            cas_protocol_failed.store(true, std::memory_order_relaxed);
          node_one->ReleaseWorker();
        });
      }
      for (auto &thread : cas_threads) thread.join();
      if (cas_winners.load() != 1 || cas_protocol_failed.load()) _exit(27);
      if (!node_one->Delete(owner_zero_key).ok()) _exit(9);
      if (node_one->Get(owner_zero_key).status.code != tigonkv::StatusCode::kNotFound) _exit(10);
      close(scan_ready[1]);
      _exit(0);
    }
    close(scan_ready[1]);
    int status = 0;
    bool concurrent_scan_started = false;
    uint32_t removals_during_scan = 0;
    uint32_t inserts_during_scan = 0;
    size_t removal_key = 0;
    for (;;) {
      engine->PollTransport();
      if (!concurrent_scan_started) {
        char marker = 0;
        concurrent_scan_started = read(scan_ready[0], &marker, 1) == 1;
      }
      if (concurrent_scan_started && removals_during_scan < 4) {
        const auto moved =
            engine->MoveOut(promoted_scan_keys[removal_key]);
        removal_key = (removal_key + 1) % promoted_scan_keys.size();
        if (moved.ok()) ++removals_during_scan;
      }
      if (concurrent_scan_started &&
          inserts_during_scan < concurrent_insert_keys.size()) {
        if (engine->Put(concurrent_insert_keys[inserts_during_scan],
                        "concurrent-insert").ok())
          ++inserts_during_scan;
      }
      const pid_t done = waitpid(child, &status, WNOHANG);
      if (done == child) break;
      assert(done == 0);
    }
    close(scan_ready[0]);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(removals_during_scan > 0);
    assert(inserts_during_scan == concurrent_insert_keys.size());
    assert(engine->NetworkTxBytes() > 0 && engine->NetworkRxBytes() > 0);
    const auto engine_runtime = engine->EngineRuntime();
    assert(engine_runtime.migration_in > 0);
    assert(engine_runtime.shared_swcc_flushes >= engine_runtime.migration_in);
  }
  unlink(routed_path.c_str());

  // §14.17 / §11.8: bounded single-key history — private, shared, Forward,
  // move-in/out. Deterministic barriers + small-state checks (no full
  // linearizability checker).
  {
    char hist_template[] = "/tmp/tigonkv-engine-history-XXXXXX";
    const int hist_fd = mkstemp(hist_template);
    assert(hist_fd >= 0);
    close(hist_fd);
    const std::string hist_path(hist_template);

    // --- Private-only history on a single owner ---
    {
      auto cfg = ConfigFor(hist_path);
      cfg.foreground_worker_count_per_vm = 4;
      auto engine = tigonkv::engine::KVEngine::Open(cfg, true);
      const std::string key = "hist-private";
      assert(engine->Put(key, "v0").ok());

      // Barriered Put then concurrent Gets: each Get must observe v0 or v1.
      std::atomic<bool> put_done{false};
      std::atomic<uint32_t> get_ok{0};
      std::atomic<bool> get_illegal{false};
      auto get_after_busy = [&](std::string_view read_key) {
        tigonkv::GetResult result;
        for (uint32_t attempt = 0; attempt != 64; ++attempt) {
          result = engine->Get(read_key);
          if (result.status.code != tigonkv::StatusCode::kBusy) return result;
          std::this_thread::yield();
        }
        return result;
      };
      std::vector<std::thread> readers;
      for (uint32_t w = 0; w < 4; ++w) {
        readers.emplace_back([&, w] {
          engine->BindWorker(w);
          while (!put_done.load(std::memory_order_acquire)) {
            const auto g = get_after_busy(key);
            if (!g.status.ok()) {
              get_illegal.store(true, std::memory_order_relaxed);
              break;
            }
            if (g.value != FixedValue("v0") && g.value != FixedValue("v1")) {
              get_illegal.store(true, std::memory_order_relaxed);
              break;
            }
            std::this_thread::yield();
          }
          const auto after = get_after_busy(key);
          if (after.status.ok() &&
              (after.value == FixedValue("v0") ||
               after.value == FixedValue("v1")))
            get_ok.fetch_add(1, std::memory_order_relaxed);
          else
            get_illegal.store(true, std::memory_order_relaxed);
          engine->ReleaseWorker();
        });
      }
      tigonkv::Status put_status;
      for (uint32_t attempt = 0; attempt != 64; ++attempt) {
        put_status = engine->Put(key, "v1");
        if (put_status.code != tigonkv::StatusCode::kBusy) break;
        std::this_thread::yield();
      }
      assert(put_status.ok());
      put_done.store(true, std::memory_order_release);
      for (auto &t : readers) t.join();
      assert(!get_illegal.load());
      assert(get_ok.load() == 4);
      assert(engine->Get(key).value == FixedValue("v1"));

      // CAS: exactly one winner from empty expected on a fresh key.
      const std::string cas_key = "hist-cas-private";
      std::atomic<uint32_t> winners{0};
      std::atomic<bool> cas_bad{false};
      std::vector<std::thread> casters;
      for (uint32_t w = 0; w < 4; ++w) {
        casters.emplace_back([&, w] {
          engine->BindWorker(w);
          tigonkv::CasResult r;
          // KVEngine exposes one primitive attempt. A create-race loser may
          // observe the owner's still-invalid placeholder as Busy; the facade
          // is the sole operation-level retry boundary.
          for (uint32_t attempt = 0; attempt != 64; ++attempt) {
            r = engine->CompareExchange(cas_key, "", "won");
            if (r.status.code != tigonkv::StatusCode::kBusy) break;
            std::this_thread::yield();
          }
          if (r.status.ok() && r.exchanged)
            winners.fetch_add(1, std::memory_order_relaxed);
          else if (r.status.code != tigonkv::StatusCode::kCompareFailed)
            cas_bad.store(true, std::memory_order_relaxed);
          engine->ReleaseWorker();
        });
      }
      for (auto &t : casters) t.join();
      assert(!cas_bad.load());
      assert(winners.load() == 1);
      assert(engine->Get(cas_key).value == FixedValue("won"));

      // Increment: N concurrent +1 from "0" → final == N.
      const std::string inc_key = "hist-inc-private";
      assert(engine->Put(inc_key, FixedValue("0")).ok());
      constexpr uint32_t kIncWorkers = 4;
      constexpr uint32_t kIncPerWorker = 25;
      std::atomic<bool> inc_bad{false};
      std::vector<std::thread> inc_threads;
      for (uint32_t w = 0; w < kIncWorkers; ++w) {
        inc_threads.emplace_back([&, w] {
          engine->BindWorker(w);
          for (uint32_t i = 0; i < kIncPerWorker; ++i) {
            for (;;) {
              const auto r = engine->Increment(inc_key, 1);
              if (r.status.ok()) break;
              if (r.status.code != tigonkv::StatusCode::kBusy) {
                inc_bad.store(true, std::memory_order_relaxed);
                engine->ReleaseWorker();
                return;
              }
            }
          }
          engine->ReleaseWorker();
        });
      }
      for (auto &t : inc_threads) t.join();
      assert(!inc_bad.load());
      const auto final_inc = engine->Get(inc_key);
      assert(final_inc.status.ok());
      assert(final_inc.value ==
             FixedValue(std::to_string(kIncWorkers * kIncPerWorker)));

      // Delete then Put: Get after delete is NotFound; after put sees new value.
      assert(engine->Delete(key).ok());
      assert(engine->Get(key).status.code == tigonkv::StatusCode::kNotFound);
      assert(engine->Put(key, "v2").ok());
      assert(engine->Get(key).value == FixedValue("v2"));
    }

    // --- Cross-node: Forward, move-in, shared read, move-out, remote Increment ---
    {
      auto node0_cfg = ConfigFor(hist_path, 2, 0);
      node0_cfg.foreground_worker_count_per_vm = 2;
      auto node1_cfg = ConfigFor(hist_path, 2, 1);
      node1_cfg.foreground_worker_count_per_vm = 2;
      std::unique_ptr<tigonkv::engine::KVEngine> engine0;
      {
        auto bootstrap = JoiningPeer(node1_cfg);
        engine0 = tigonkv::engine::KVEngine::Open(node0_cfg, true);
      }

      const std::string owned0 = "H-hist-owner0";
      const std::string owned1 = "a-hist-owner1";
      assert(engine0->OwnerForKey(owned0) == 0);
      assert(engine0->OwnerForKey(owned1) == 1);
      assert(engine0->Put(owned0, "owner0-v1").ok());

      // child→parent: phase1 done; parent→child: parent done (EOF on close).
      int child_to_parent[2];
      int parent_to_child[2];
      assert(pipe2(child_to_parent, O_CLOEXEC) == 0);
      assert(pipe2(parent_to_child, O_CLOEXEC) == 0);
      const pid_t child = fork();
      assert(child >= 0);
      if (child == 0) {
        close(child_to_parent[0]);
        close(parent_to_child[1]);
        auto engine1 = tigonkv::engine::KVEngine::Open(node1_cfg, false);
        // Remote Get → Forward migrate-in → shared authority.
        const auto g1 = engine1->Get(owned0);
        if (!g1.status.ok() || g1.value != FixedValue("owner0-v1")) _exit(41);
        const auto g2 = engine1->Get(owned0);
        if (!g2.status.ok() || g2.value != FixedValue("owner0-v1")) _exit(42);

        // Concurrent CAS on the migrated key: at most one exchange succeeds.
        std::atomic<uint32_t> shared_winners{0};
        std::atomic<bool> shared_bad{false};
        std::thread cas_a([&] {
          engine1->BindWorker(0);
          for (;;) {
            const auto r =
                engine1->CompareExchange(owned0, "owner0-v1", "cas-remote");
            if (r.status.ok() && r.exchanged) {
              shared_winners.fetch_add(1, std::memory_order_relaxed);
              break;
            }
            if (r.status.code == tigonkv::StatusCode::kCompareFailed) break;
            if (r.status.code != tigonkv::StatusCode::kBusy) {
              shared_bad.store(true, std::memory_order_relaxed);
              break;
            }
          }
          engine1->ReleaseWorker();
        });
        std::thread cas_b([&] {
          engine1->BindWorker(1);
          for (;;) {
            const auto r =
                engine1->CompareExchange(owned0, "owner0-v1", "cas-remote-b");
            if (r.status.ok() && r.exchanged) {
              shared_winners.fetch_add(1, std::memory_order_relaxed);
              break;
            }
            if (r.status.code == tigonkv::StatusCode::kCompareFailed) break;
            if (r.status.code != tigonkv::StatusCode::kBusy) {
              shared_bad.store(true, std::memory_order_relaxed);
              break;
            }
          }
          engine1->ReleaseWorker();
        });
        cas_a.join();
        cas_b.join();
        if (shared_bad.load() || shared_winners.load() > 1) _exit(43);
        const auto after_cas = engine1->Get(owned0);
        if (!after_cas.status.ok()) _exit(44);
        if (after_cas.value != FixedValue("cas-remote") &&
            after_cas.value != FixedValue("cas-remote-b") &&
            after_cas.value != FixedValue("owner0-v1"))
          _exit(45);

        // Seed a key this node owns for parent's Forward Increment history.
        if (!engine1->Put(owned1, "0").ok()) _exit(46);
        const char ready = 1;
        if (write(child_to_parent[1], &ready, 1) != 1) _exit(47);
        close(child_to_parent[1]);

        // Non-blocking wait for parent EOF while serving Forward requests.
        {
          const int flags = fcntl(parent_to_child[0], F_GETFL, 0);
          if (flags < 0 ||
              fcntl(parent_to_child[0], F_SETFL, flags | O_NONBLOCK) < 0)
            _exit(48);
        }
        for (;;) {
          engine1->PollTransport();
          char sink = 0;
          const ssize_t n = read(parent_to_child[0], &sink, 1);
          if (n == 0) break;  // parent closed → done
          if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
              errno != EINTR)
            _exit(49);
          std::this_thread::yield();
        }
        close(parent_to_child[0]);
        _exit(0);
      }

      close(child_to_parent[1]);
      close(parent_to_child[0]);
      // Child's first Get Forwards to us; must PollTransport while waiting.
      {
        const int flags = fcntl(child_to_parent[0], F_GETFL, 0);
        assert(flags >= 0);
        assert(fcntl(child_to_parent[0], F_SETFL, flags | O_NONBLOCK) == 0);
      }
      char marker = 0;
      for (;;) {
        engine0->PollTransport();
        const ssize_t n = read(child_to_parent[0], &marker, 1);
        if (n == 1) break;
        if (n == 0) assert(false && "child closed before ready");
        assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                         errno == EINTR));
        std::this_thread::yield();
      }
      close(child_to_parent[0]);

      // After child migrated owned0, MoveOut restores private authority.
      bool moved = false;
      for (int i = 0; i < 64 && !moved; ++i) {
        engine0->PollTransport();
        moved = engine0->MoveOut(owned0).ok();
      }
      assert(moved);
      const auto after_moveout = engine0->Get(owned0);
      assert(after_moveout.status.ok());
      assert(after_moveout.value == FixedValue("cas-remote") ||
             after_moveout.value == FixedValue("cas-remote-b") ||
             after_moveout.value == FixedValue("owner0-v1"));

      // Forward Increment on node1-owned key: returns form serial 1..10.
      for (int expected = 1; expected <= 10; ++expected) {
        for (;;) {
          engine0->PollTransport();
          const auto r = engine0->Increment(owned1, 1);
          if (r.status.ok()) {
            assert(r.value == expected);
            break;
          }
          assert(r.status.code == tigonkv::StatusCode::kBusy);
        }
      }
      const auto inc_get = engine0->Get(owned1);
      assert(inc_get.status.ok() && inc_get.value == FixedValue("10"));

      close(parent_to_child[1]);  // wake child EOF
      int status = 0;
      for (;;) {
        engine0->PollTransport();
        const pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) break;
        assert(done == 0);
      }
      assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    unlink(hist_path.c_str());
  }

  return 0;
}
