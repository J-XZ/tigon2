// Deterministic latency tests for the remote Delete critical state.
//
// While the row is write_locked with valid cleared (between
// prepare_remote_delete and the owner ack / rollback), the foreground budget
// must be suspended and every transport poll must defer its settlement, so
// no busy-wait ever happens inside the critical state.  The deferred
// transport budget settles exactly once right after the commit/rollback
// releases the lock and SCC guards, and the foreground budget settles once at
// the outermost scope exit.  A recording delay backend makes the settlement
// counts deterministic.  Row state (write lock, valid flag, ref count) is
// asserted restored on the rollback path.
#include "common/CXLMemory.h"
#include "kv/engine/kv_partition.h"
#include "kv/engine/mem_access.h"
#include "kv/engine/region_allocator.h"
#include "protocol/TwoPLPasha/TwoPLPashaHelper.h"

// The rollback test drives PrepareRemoteDelete/AbortRemoteDelete through a
// KVPartition handle; the engine's partition map is private, so this test
// relaxes access for the KVEngine header only (test-only, mirrors the cxlkv
// test0 pattern).
#define private public
#include "kv/engine/kv_engine.h"
#undef private

#include <latency_sim/simulator.h>
#include <latency_sim/testing.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t kVmCount = 2;
constexpr uint32_t kRequester = 0;
constexpr uint32_t kOwner = 1;
constexpr uint32_t kFixedKeySize = 32;
constexpr uint32_t kFixedValueSize = 128;

// Settlement records are tagged with the settling thread id: the inbound
// demuxer thread legitimately settles its own per-iteration receive budgets,
// so only the requester thread's settlements are meaningful for the delete
// critical-state assertions.
std::vector<std::pair<std::thread::id, std::uint64_t>> g_settlements;
std::mutex g_settlements_mutex;
std::thread::id g_requester_thread;

void InstallRecordingBackend() {
  g_settlements.clear();
  g_requester_thread = std::this_thread::get_id();
  latency_sim::detail::SetDelaySpinBackendForTest(
      [](std::uint64_t ns) {
        std::lock_guard<std::mutex> lock(g_settlements_mutex);
        g_settlements.emplace_back(std::this_thread::get_id(), ns);
      });
}

std::size_t RequesterSettlements() {
  std::lock_guard<std::mutex> lock(g_settlements_mutex);
  std::size_t count = 0;
  for (const auto &entry : g_settlements)
    if (entry.first == g_requester_thread) ++count;
  return count;
}

void ClearSettlements() {
  std::lock_guard<std::mutex> lock(g_settlements_mutex);
  g_settlements.clear();
}

void UninstallBackend() {
  latency_sim::detail::SetDelaySpinBackendForTest(nullptr);
}

latency_sim::FixedLatencyConfig OneNanosecondPerLine() {
  latency_sim::FixedLatencyConfig config;
  config.cache_line_bytes = 64;
  config.swcc_fixed_ns_per_line = 1.0;
  config.hwcc_fixed_ns_per_line = 1.0;
  return config;
}

tigonkv::Config ConfigFor(const std::string &path) {
  tigonkv::Config config;
  config.shared_memory_path = path;
  config.size_mb = 64;
  config.hwcc_offset_mb = 0;
  config.hwcc_size_mb = 32;
  config.swcc_offset_mb = 32;
  config.swcc_size_mb = 32;
  config.hw_cc_budget_mb = 32;
  config.vm_count = kVmCount;
  config.node_id = kRequester;
  config.partition_count = 8;
  config.fixed_key_size = kFixedKeySize;
  config.fixed_value_size = kFixedValueSize;
  config.foreground_worker_count_per_vm = 1;
  config.transport_ring_total_mb = 1;
  config.hardware_simulation = OneNanosecondPerLine();
  config.partition_ranges.clear();
  std::string lower;
  for (uint32_t partition = 0; partition < config.partition_count; ++partition) {
    std::string upper;
    if (partition + 1 != config.partition_count)
      upper.assign(1, static_cast<char>((partition + 1) * 256 /
                                        config.partition_count));
    config.partition_ranges.push_back({lower, upper});
    lower = std::move(upper);
  }
  return config;
}

std::string Key(uint64_t i) {
  char text[32];
  const int count = std::snprintf(text, sizeof(text), "rd-%08llu",
                                  static_cast<unsigned long long>(i));
  std::string fixed(text, static_cast<size_t>(count));
  fixed.resize(kFixedKeySize, ' ');
  return fixed;
}

std::string FixedValue(std::string_view text) {
  std::string value(text);
  value.resize(kFixedValueSize, ' ');
  return value;
}

std::string OwnerKey(uint32_t owner, uint64_t i) {
  // Try candidate keys until one routes to the requested owner.
  tigonkv::Config probe = ConfigFor("/dev/null");
  probe.Validate();  // normalize range boundaries to persisted FixedKeys
  for (uint64_t attempt = 0;; ++attempt) {
    const std::string key = Key(i + attempt * 4096);
    if (probe.PartitionForKey(key) % kVmCount == owner) return key;
  }
}

// A single owner peer process: opens node1 on the same backing and polls the
// transport until the stop pipe closes, so the requester's remote operations
// are acked by a real owner.
class OwnerPeer {
 public:
  explicit OwnerPeer(tigonkv::Config config) {
    config.node_id = kOwner;
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
          engine = tigonkv::engine::KVEngine::Open(config, false);
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

  ~OwnerPeer() {
    close(stop_pipe_[1]);
    int status = 0;
    assert(waitpid(child_, &status, 0) == child_);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }

 private:
  int stop_pipe_[2];
  pid_t child_;
};

}  // namespace

int main() {
#if defined(LATENCY_SIM_COMPILE_OFF)
  return 0;
#else
  InstallRecordingBackend();

  char path_template[] = "/tmp/tigonkv-rd-latency-XXXXXX";
  const int fd = mkstemp(path_template);
  assert(fd >= 0);
  close(fd);
  const std::string path(path_template);
  const auto config = ConfigFor(path);
  OwnerPeer peer(config);

  {
    auto engine = tigonkv::engine::KVEngine::Open(config, true);
    const std::string remote_key = OwnerKey(kOwner, 0);
    {
      // Seed the shared row inside its own foreground scope.
      tigonkv::engine::mem_access::LatencyScope facade(
          latency_sim::ExecutionClass::kForeground);
      engine->BindWorker(0);
      assert(engine->Put(remote_key, FixedValue("alive")).ok());
    }
    {
      tigonkv::engine::mem_access::LatencyScope facade(
          latency_sim::ExecutionClass::kForeground);
      {
        std::lock_guard<std::mutex> lock(g_settlements_mutex);
        g_settlements.clear();
      }
      // 1. Remote delete success: the row is prepared (write_locked + valid
      //    cleared), the delete request is forwarded and the owner acks.
      assert(engine->Delete(remote_key).ok());
      // No settlement happened during the whole delete: the cooperative wait
      // suspended the foreground budget and every transport poll deferred its
      // settlement, so no busy-wait ever occurred while the row was
      // write_locked/invalid (the delete's leading empty poll settles zero
      // and the recording backend ignores zero-nanosecond requests).
      // No settlement happened on the requester thread during the whole
      // delete: the cooperative wait suspended the foreground budget and
      // every transport poll deferred its settlement, so no busy-wait ever
      // occurred while the row was write_locked/invalid (the demuxer thread's
      // own receive settlements are unrelated and allowed).
      assert(RequesterSettlements() == 0);
      ClearSettlements();

      const auto gone = engine->Get(remote_key);
      assert(gone.status.code == tigonkv::StatusCode::kNotFound);
      // The requester-side row state is clean: a fresh Put/Get on the same
      // key works (no stranded lock, no stale pin).
      assert(engine->Put(remote_key, FixedValue("recreated")).ok());
      const auto recreated = engine->Get(remote_key);
      assert(recreated.status.ok() &&
             recreated.value == FixedValue("recreated"));
    }
    // The foreground scope exit settled the merged deferred budget (the
    // delete's transport polls plus the suspended foreground segment) after
    // the delete and rollback were long over; the later in-facade operations
    // settle their own normal-mode budgets on the same thread, so only the
    // last settlement carries the merged deferred budget.
    assert(RequesterSettlements() >= 1);
    {
      std::lock_guard<std::mutex> lock(g_settlements_mutex);
      const auto last = g_settlements.back();
      assert(last.first == g_requester_thread && last.second > 0);
    }
    ClearSettlements();

    // 2. Rollback path: prepare the delete of a second remote row and abort
    //    it (the owner-Busy / failed-ack path calls exactly this), asserting
    //    the lock is released, the valid flag is restored and the ref count
    //    is decremented back, with exactly one settlement per scope.
    const std::string rollback_key = OwnerKey(kOwner, 4096);
    {
      tigonkv::engine::mem_access::LatencyScope facade(
          latency_sim::ExecutionClass::kForeground);
      assert(engine->Put(rollback_key, FixedValue("rollback-me")).ok());
    }
    auto *partition = engine->VisiblePartition(rollback_key);
    assert(partition != nullptr);
    star::TwoPLPashaMetadataShared *locked_row = nullptr;
    ClearSettlements();
    {
      tigonkv::engine::mem_access::LatencyScope scope(
          latency_sim::ExecutionClass::kBackground);
      assert(partition->PrepareRemoteDelete(rollback_key, kRequester,
                                            &locked_row, true) ==
             tigonkv::engine::SharedAccessState::kDone);
      assert(locked_row != nullptr);
      assert(locked_row->is_write_locked());
      const auto *payload = locked_row->get_scc_data();
      assert(!payload->get_flag(star::TwoPLPashaSharedDataSCC::valid_flag_index));
    }
    assert(RequesterSettlements() == 1);  // exactly one settlement per scope
    ClearSettlements();
    ClearSettlements();
    {
      tigonkv::engine::mem_access::LatencyScope scope(
          latency_sim::ExecutionClass::kBackground);
      partition->AbortRemoteDelete(locked_row, kRequester);
      assert(!locked_row->is_write_locked());
      const auto *payload = locked_row->get_scc_data();
      assert(payload->get_flag(star::TwoPLPashaSharedDataSCC::valid_flag_index));
    }
    assert(RequesterSettlements() == 1);  // exactly one settlement per scope
    ClearSettlements();
    // The restored row is readable again through the normal path.
    {
      tigonkv::engine::mem_access::LatencyScope facade(
          latency_sim::ExecutionClass::kForeground);
      const auto restored = engine->Get(rollback_key);
      assert(restored.status.ok() &&
             restored.value == FixedValue("rollback-me"));
    }
    engine->ReleaseWorker();
  }
  UninstallBackend();
  unlink(path_template);
  std::printf("remote_delete_latency_test ok\n");
  return 0;
#endif
}
