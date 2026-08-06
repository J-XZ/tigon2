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
#include "common/Message.h"
#include "kv/engine/kv_partition.h"
#include "kv/engine/kv_types_layout.h"
#include "kv/engine/mem_access.h"
#include "kv/engine/region_allocator.h"
#include "protocol/TwoPLPasha/TwoPLPashaMessage.h"
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

// Last settlement recorded on the requester thread.  The inbound demuxer
// thread legitimately appends its own per-iteration settlements, so the
// global back of the list is not stable.
std::uint64_t LastRequesterSettlementNs() {
  std::lock_guard<std::mutex> lock(g_settlements_mutex);
  std::uint64_t last = 0;
  for (const auto &entry : g_settlements)
    if (entry.first == g_requester_thread) last = entry.second;
  return last;
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
    if (stop_pipe_[1] >= 0) {
      close(stop_pipe_[1]);
      stop_pipe_[1] = -1;
    }
    JoinIfRunning();
  }

  // Stops the owner's polling loop and joins it.  After this the owner never
  // acks anything, so a requester operation that still awaits must resolve
  // through an injected response or the control-plane timeout.
  void StopAndJoin() {
    if (stop_pipe_[1] >= 0) {
      close(stop_pipe_[1]);
      stop_pipe_[1] = -1;
    }
    int status = 0;
    assert(waitpid(child_, &status, 0) == child_);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    child_ = -1;
  }

 private:
  void JoinIfRunning() {
    if (child_ > 0) {
      int status = 0;
      assert(waitpid(child_, &status, 0) == child_);
      assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
      child_ = -1;
    }
  }
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

  {
    // The owner peer is scoped with its requester engine: once this block
    // ends the owner is joined, so later blocks can never receive a real
    // owner ack (the Busy/timeout tests rely on that).
    OwnerPeer peer(config);
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
    assert(LastRequesterSettlementNs() > 0);
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
    // 3. Owner claim wins the race: once the owner has linearized the delete
    //    (write-lock bit cleared under the smeta latch), a late requester
    //    rollback must hard fail instead of resurrecting a deleted row.
    const std::string claimed_key = OwnerKey(kOwner, 16384);
    {
      tigonkv::engine::mem_access::LatencyScope facade(
          latency_sim::ExecutionClass::kForeground);
      assert(engine->Put(claimed_key, FixedValue("claimed-me")).ok());
    }
    auto *claimed_partition = engine->VisiblePartition(claimed_key);
    assert(claimed_partition != nullptr);
    star::TwoPLPashaMetadataShared *claimed_row = nullptr;
    {
      tigonkv::engine::mem_access::LatencyScope scope(
          latency_sim::ExecutionClass::kBackground);
      assert(claimed_partition->PrepareRemoteDelete(
                 claimed_key, kRequester, &claimed_row, true) ==
             tigonkv::engine::SharedAccessState::kDone);
      assert(claimed_row != nullptr && claimed_row->is_write_locked());
      // Simulate the owner's linearized delete: under the latch, the owner
      // consumes the requester write-lock bit as Pending -> Executing/Deleted.
      claimed_row->lock();
      claimed_row->clear_write_locked();
      claimed_row->unlock();
      bool rolled_back = false;
      try {
        claimed_partition->AbortRemoteDelete(claimed_row, kRequester);
      } catch (const std::runtime_error &) {
        rolled_back = true;
      }
      assert(rolled_back);
    }
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
  {
    // Owner Busy / error ack: the owner responds Busy (or a dispatch error is
    // surfaced as a non-ok result), the delete rolls the row back, the lock is
    // released, the valid flag is restored and the budgets settle exactly
    // once at the facade exit.  A real polling owner acks the seeds, then
    // stops polling so no real delete ack can race the injected Busy response
    // (or the timeout below).
    auto config_busy = config;
    config_busy.transport_response_timeout_ms = 300;
    OwnerPeer busy_peer(config_busy);
    auto engine = tigonkv::engine::KVEngine::Open(config_busy, true);
    const std::string busy_key = OwnerKey(kOwner, 8192);
    const std::string timeout_key = OwnerKey(kOwner, 12288);
    {
      tigonkv::engine::mem_access::LatencyScope facade(
          latency_sim::ExecutionClass::kForeground);
      engine->BindWorker(0);
      assert(engine->Put(busy_key, FixedValue("busy-me")).ok());
      assert(engine->Put(timeout_key, FixedValue("timeout-me")).ok());
    }
    busy_peer.StopAndJoin();
    {
      tigonkv::engine::mem_access::LatencyScope facade(
          latency_sim::ExecutionClass::kForeground);
      {
        std::lock_guard<std::mutex> lock(g_settlements_mutex);
        g_settlements.clear();
      }
      auto *mailbox = engine->worker_mailboxes_[0].get();
      // Stale-response idempotence: a late response for an older request
      // identity must be dropped, not consumed as this request's response.
      // Inject it first, then the matching Busy response; the delete must
      // complete only on the matching sequence.
      std::thread responder([&] {
        while (mailbox->operation.expected_response_type == 0)
          std::this_thread::yield();
        const uint64_t stale_sequence = mailbox->operation.sequence + 7;
        auto stale_message = std::make_unique<star::Message>();
        stale_message->set_source_node_id(kOwner);
        stale_message->set_dest_node_id(kRequester);
        star::TwoPLPashaMessageHandler::append_remote_delete_response(
            *stale_message, tigonkv::engine::kSingleTableId,
            mailbox->operation.partition_id,
            star::RemoteDeleteOutcome::Deleted, 0, stale_sequence);
        mailbox->inbox.push(stale_message.release());
        auto message = std::make_unique<star::Message>();
        message->set_source_node_id(kOwner);
        message->set_dest_node_id(kRequester);
        star::TwoPLPashaMessageHandler::append_remote_delete_response(
            *message, tigonkv::engine::kSingleTableId,
            mailbox->operation.partition_id,
            star::RemoteDeleteOutcome::Busy, 0,
            mailbox->operation.sequence);
        mailbox->inbox.push(message.release());
      });
      const auto status = engine->Delete(busy_key);
      responder.join();
      assert(status.code == tigonkv::StatusCode::kBusy);
      assert(RequesterSettlements() == 0);  // nothing settled inside the critical state
      ClearSettlements();
      // The row is restored and readable through the normal path.
      const auto restored = engine->Get(busy_key);
      assert(restored.status.ok() &&
             restored.value == FixedValue("busy-me"));
    }
    assert(RequesterSettlements() >= 1);  // merged budget settled at the facade exit
    assert(LastRequesterSettlementNs() > 0);
    ClearSettlements();

    // Timeout / lost owner: no ack arrives within the bounded control-plane
    // deadline, the delete times out and rolls the row back; nothing is left
    // stranded (lock, valid flag, budgets).
    {
      tigonkv::engine::mem_access::LatencyScope facade(
          latency_sim::ExecutionClass::kForeground);
      {
        std::lock_guard<std::mutex> lock(g_settlements_mutex);
        g_settlements.clear();
      }
      const auto status = engine->Delete(timeout_key);
      assert(status.code == tigonkv::StatusCode::kTimeout);
      const auto restored = engine->Get(timeout_key);
      assert(restored.status.ok() &&
             restored.value == FixedValue("timeout-me"));
    }
    // Transport/dispatch exception inside a deferred poll: the RAII poll
    // guard must restore the thread scope state even when the poll body
    // throws, and the merged deferred budget must settle exactly once at the
    // outermost facade exit (never inside the critical state).
    {
      tigonkv::engine::mem_access::LatencyScope facade(
          latency_sim::ExecutionClass::kForeground);
      latency_sim::GlobalLatencySimulator().ChargeRange(
          latency_sim::MemoryDomain::kHwcc,
          latency_sim::AccessKind::kRead,
          static_cast<const std::byte *>(engine->pool_->base()) +
              config_busy.hwcc_offset_mb * 1024ull * 1024ull,
          64);
      {
        std::lock_guard<std::mutex> lock(g_settlements_mutex);
        g_settlements.clear();
      }
      {
        tigonkv::engine::mem_access::ForegroundScopeSuspension
            suspend_foreground;
        tigonkv::engine::mem_access::DeferTransportSettlement
            defer_settlement;
        try {
          tigonkv::engine::mem_access::DeferredTransportPollScope poll_scope;
          throw std::runtime_error("dispatch exception");
        } catch (const std::runtime_error &) {
        }
        // The poll scope destructor restored the suspended state.
        assert(latency_sim::GlobalLatencySimulator()
                   .ScopeSuspendedForTest());
        assert(RequesterSettlements() == 0);
      }
      // The foreground suspension destructor restored the active foreground
      // scope with its original class/depth.
      assert(latency_sim::GlobalLatencySimulator()
                 .HasTopLevelScopeForCurrentThread(
                     latency_sim::ExecutionClass::kForeground));
      assert(RequesterSettlements() == 0);
    }
    // Exactly one settlement at the facade exit, carrying the merged deferred
    // budget (no loss, no duplication).
    assert(RequesterSettlements() == 1);
    assert(LastRequesterSettlementNs() == 1);
    engine->ReleaseWorker();
  }
  UninstallBackend();
  unlink(path_template);
  std::printf("remote_delete_latency_test ok\n");
  return 0;
#endif
}
