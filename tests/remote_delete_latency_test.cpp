// Remote Delete protocol tests.  The cases exercise the production owner
// claim, requester rollback, ABA interleaving, late/duplicate responses and
// sequence exhaustion on both compile-on and compile-off builds.
#include "common/CXLMemory.h"
#include "common/Message.h"
#include "common/MPSCRingBuffer.h"
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

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <string_view>

namespace {

constexpr uint32_t kVmCount = 2;
constexpr uint32_t kRequester = 0;
constexpr uint32_t kOwner = 1;
constexpr uint32_t kFixedKeySize = 32;
constexpr uint32_t kFixedValueSize = 128;

struct RemoteDeleteOperationRecord {
  uint32_t partition_id = 0;
  uint32_t reserved = 0;
  uint64_t sequence = 0;
  tigonkv::engine::RegionOffset target_row = tigonkv::engine::kNullOffset;
};

RemoteDeleteOperationRecord ReadRemoteDeleteOperation(int fd) {
  RemoteDeleteOperationRecord record;
  auto *bytes = reinterpret_cast<char *>(&record);
  size_t read_total = 0;
  while (read_total < sizeof(record)) {
    const ssize_t count = ::read(fd, bytes + read_total,
                                 sizeof(record) - read_total);
    if (count > 0) {
      read_total += static_cast<size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    assert(false && "remote delete operation record read failed");
  }
  return record;
}

void EnqueueResponse(tigonkv::engine::KVEngine *engine,
                     std::unique_ptr<star::Message> message) {
  message->set_worker_id(0);
  while (!engine->rings_[kRequester].enqueue(message->get_raw_ptr(),
                                             message->get_message_length()))
    std::this_thread::yield();
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
  explicit OwnerPeer(tigonkv::Config config, bool with_claim_gate = false)
      : claim_gate_enabled_(with_claim_gate) {
    config.node_id = kOwner;
    assert(pipe2(stop_pipe_, O_CLOEXEC) == 0);
    if (claim_gate_enabled_) {
      assert(pipe(claim_command_) == 0);
      assert(pipe(claim_ready_) == 0);
      assert(pipe(claim_release_) == 0);
      SetClaimGateEnvironment();
    }
    child_ = fork();
    assert(child_ >= 0);
    if (child_ == 0) {
      close(stop_pipe_[1]);
      if (claim_gate_enabled_) {
        close(claim_command_[1]);
        close(claim_ready_[0]);
        close(claim_release_[1]);
      }
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
    if (claim_gate_enabled_) {
      close(claim_command_[0]);
      close(claim_ready_[1]);
      close(claim_release_[0]);
    }
  }

  ~OwnerPeer() {
    if (stop_pipe_[1] >= 0) {
      close(stop_pipe_[1]);
      stop_pipe_[1] = -1;
    }
    JoinIfRunning();
    CloseClaimGate();
  }

  void PauseNextRemoteDeleteClaim() {
    if (!claim_gate_enabled_) return;
    const char command = 1;
    assert(write(claim_command_[1], &command, 1) == 1);
    char ready = 0;
    assert(read(claim_ready_[0], &ready, 1) == 1);
    assert(ready == 1);
  }

  void AllowNextRemoteDeleteClaim() {
    if (!claim_gate_enabled_) return;
    const char command = 0;
    assert(write(claim_command_[1], &command, 1) == 1);
  }

  void ReleasePausedRemoteDeleteClaim() {
    if (!claim_gate_enabled_) return;
    const char release = 1;
    assert(write(claim_release_[1], &release, 1) == 1);
  }

  bool ClaimGateEnabled() const { return claim_gate_enabled_; }

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

  void SetClaimGateEnvironment() {
    assert(setenv("TIGONKV_TEST_REMOTE_DELETE_CLAIM_COMMAND_FD",
                  std::to_string(claim_command_[0]).c_str(), 1) == 0);
    assert(setenv("TIGONKV_TEST_REMOTE_DELETE_CLAIM_READY_FD",
                  std::to_string(claim_ready_[1]).c_str(), 1) == 0);
    assert(setenv("TIGONKV_TEST_REMOTE_DELETE_CLAIM_RELEASE_FD",
                  std::to_string(claim_release_[0]).c_str(), 1) == 0);
  }

  void CloseClaimGate() {
    if (!claim_gate_enabled_) return;
    close(claim_command_[1]);
    close(claim_ready_[0]);
    close(claim_release_[1]);
    assert(unsetenv("TIGONKV_TEST_REMOTE_DELETE_CLAIM_COMMAND_FD") == 0);
    assert(unsetenv("TIGONKV_TEST_REMOTE_DELETE_CLAIM_READY_FD") == 0);
    assert(unsetenv("TIGONKV_TEST_REMOTE_DELETE_CLAIM_RELEASE_FD") == 0);
    claim_gate_enabled_ = false;
  }

  int stop_pipe_[2];
  pid_t child_;
  bool claim_gate_enabled_ = false;
  int claim_command_[2] = {-1, -1};
  int claim_ready_[2] = {-1, -1};
  int claim_release_[2] = {-1, -1};
};

}  // namespace

int main() {
  char path_template[] = "/tmp/tigonkv-rd-protocol-XXXXXX";
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
      // 1. Remote delete success: the row is prepared (write_locked + valid
      //    cleared), the delete request is forwarded and the owner acks.
      assert(engine->Delete(remote_key).ok());

      const auto gone = engine->Get(remote_key);
      assert(gone.status.code == tigonkv::StatusCode::kNotFound);
      // The requester-side row state is clean: a fresh Put/Get on the same
      // key works (no stranded lock, no stale pin).
      assert(engine->Put(remote_key, FixedValue("recreated")).ok());
      const auto recreated = engine->Get(remote_key);
      assert(recreated.status.ok() &&
             recreated.value == FixedValue("recreated"));
    }
    // 2. Rollback path: prepare the delete of a second remote row and abort
    //    it (the owner-Busy / failed-ack path calls exactly this), asserting
    //    the lock is released, the valid flag is restored and the ref count
    //    is decremented back.
    const std::string rollback_key = OwnerKey(kOwner, 4096);
    {
      tigonkv::engine::mem_access::LatencyScope facade(
          latency_sim::ExecutionClass::kForeground);
      assert(engine->Put(rollback_key, FixedValue("rollback-me")).ok());
    }
    auto *partition = engine->VisiblePartition(rollback_key);
    assert(partition != nullptr);
    tigonkv::engine::RegionOffset locked_row =
        tigonkv::engine::kNullOffset;
    uint64_t rollback_tag = 0;
    auto &rollback_control = engine->pool_->allocator().layout()
        .remote_delete_controls[kRequester][0];
    const auto rollback_fixed_key =
        tigonkv::engine::FixedKey::From(rollback_key, kFixedKeySize);
    const uint64_t rollback_sequence =
        engine->ReserveOperationSequence(*engine->worker_mailboxes_[0]);
    {
      tigonkv::engine::mem_access::LatencyScope scope(
          latency_sim::ExecutionClass::kBackground);
      assert(partition->PrepareRemoteDelete(rollback_key, kRequester,
                                            &locked_row, true) ==
             tigonkv::engine::SharedAccessState::kDone);
      assert(locked_row != tigonkv::engine::kNullOffset);
      assert(rollback_control.PublishPending(
          kRequester, 0, partition->partition_id(), rollback_sequence,
          locked_row, rollback_fixed_key));
      rollback_tag = rollback_control.LoadControl().tag;
      assert(rollback_control.LoadControl().state ==
             tigonkv::engine::RemoteDeleteControlState::kPending);
    }
    {
      tigonkv::engine::mem_access::LatencyScope scope(
          latency_sim::ExecutionClass::kBackground);
      assert(rollback_control.CompareExchange(
          rollback_tag,
          tigonkv::engine::RemoteDeleteControlState::kPending,
          tigonkv::engine::RemoteDeleteControlState::kCancelled));
      partition->AbortRemoteDelete(locked_row, kRequester);
      assert(rollback_control.AcknowledgeAndClear(
          rollback_tag,
          tigonkv::engine::RemoteDeleteControlState::kCancelled));
    }
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
    tigonkv::engine::RegionOffset claimed_row =
        tigonkv::engine::kNullOffset;
    uint64_t claimed_tag = 0;
    auto &claimed_control = engine->pool_->allocator().layout()
        .remote_delete_controls[kRequester][0];
    const auto claimed_fixed_key =
        tigonkv::engine::FixedKey::From(claimed_key, kFixedKeySize);
    const uint64_t claimed_sequence =
        engine->ReserveOperationSequence(*engine->worker_mailboxes_[0]);
    {
      tigonkv::engine::mem_access::LatencyScope scope(
          latency_sim::ExecutionClass::kBackground);
      assert(claimed_partition->PrepareRemoteDelete(
                 claimed_key, kRequester, &claimed_row, true) ==
             tigonkv::engine::SharedAccessState::kDone);
      assert(claimed_row != tigonkv::engine::kNullOffset);
      assert(claimed_control.PublishPending(
          kRequester, 0, claimed_partition->partition_id(), claimed_sequence,
          claimed_row, claimed_fixed_key));
      claimed_tag = claimed_control.LoadControl().tag;
      // Owner claim is a control-slot fact, independent of the row write bit.
      assert(claimed_control.CompareExchange(
          claimed_tag,
          tigonkv::engine::RemoteDeleteControlState::kPending,
          tigonkv::engine::RemoteDeleteControlState::kExecuting));
      assert(!claimed_control.CompareExchange(
          claimed_tag,
          tigonkv::engine::RemoteDeleteControlState::kPending,
          tigonkv::engine::RemoteDeleteControlState::kCancelled));
      assert(claimed_control.LoadControl().state ==
             tigonkv::engine::RemoteDeleteControlState::kExecuting);
    }
    // The owner may reject an unmodified claimed row and perform the only
    // legal cleanup; the requester must never call Abort after Executing.
    {
      tigonkv::engine::mem_access::LatencyScope scope(
          latency_sim::ExecutionClass::kBackground);
      claimed_partition->AbortRemoteDelete(claimed_row, kRequester);
      assert(claimed_control.CompareExchange(
          claimed_tag,
          tigonkv::engine::RemoteDeleteControlState::kExecuting,
          tigonkv::engine::RemoteDeleteControlState::kRejected));
      assert(claimed_control.AcknowledgeAndClear(
          claimed_tag,
          tigonkv::engine::RemoteDeleteControlState::kRejected));
    }
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
    // Production ABA interleaving: the owner is paused immediately before
    // the exact claim CAS for A.  A times out and cancels, the same requester
    // worker publishes B for the same row, and only then is the owner allowed
    // to resume.  A's CAS must fail against Pending(tagB); B must complete
    // through the real forked transport path.
    char aba_path_template[] = "/tmp/tigonkv-rd-aba-XXXXXX";
    const int aba_fd = mkstemp(aba_path_template);
    assert(aba_fd >= 0);
    close(aba_fd);
    auto config_aba = config;
    config_aba.shared_memory_path = aba_path_template;
    config_aba.transport_response_timeout_ms = 1000;
    OwnerPeer aba_peer(config_aba, true);
    auto engine = tigonkv::engine::KVEngine::Open(config_aba, true);
    const std::string aba_key = OwnerKey(kOwner, 24576);
    {
      tigonkv::engine::mem_access::LatencyScope facade(
          latency_sim::ExecutionClass::kForeground);
      engine->BindWorker(0);
      assert(engine->Put(aba_key, FixedValue("aba-me")).ok());
      engine->ReleaseWorker();
    }

    if (aba_peer.ClaimGateEnabled()) {
      tigonkv::Status first_status = tigonkv::Status::Ok();
      std::thread first([&] {
        tigonkv::engine::mem_access::LatencyScope facade(
            latency_sim::ExecutionClass::kForeground);
        engine->BindWorker(0);
        first_status = engine->Delete(aba_key);
        engine->ReleaseWorker();
      });
      aba_peer.PauseNextRemoteDeleteClaim();
      first.join();
      assert(first_status.code == tigonkv::StatusCode::kTimeout);

      tigonkv::Status second_status = tigonkv::Status::Error(
          tigonkv::StatusCode::kCorruption, "B did not run");
      tigonkv::GetResult second_get;
      std::thread second([&] {
        tigonkv::engine::mem_access::LatencyScope facade(
            latency_sim::ExecutionClass::kForeground);
        engine->BindWorker(0);
        second_status = engine->Delete(aba_key);
        if (second_status.ok()) second_get = engine->Get(aba_key);
        engine->ReleaseWorker();
      });
      aba_peer.ReleasePausedRemoteDeleteClaim();
      aba_peer.AllowNextRemoteDeleteClaim();
      second.join();
      assert(second_status.ok());
      assert(second_get.status.code == tigonkv::StatusCode::kNotFound);
    }
    unlink(aba_path_template);
  }
  {
    // First create a real retired identity through a lost-owner timeout.  The
    // later injected Deleted/Failed responses use this identity and therefore
    // exercise the production retired-response filter rather than mutating a
    // private test ledger.
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
    int response_pipe[2];
    assert(pipe(response_pipe) == 0);
    assert(setenv("TIGONKV_TEST_REMOTE_DELETE_RESPONSE_FD",
                  std::to_string(response_pipe[1]).c_str(), 1) == 0);
    RemoteDeleteOperationRecord retired_operation;
    {
      tigonkv::engine::mem_access::LatencyScope facade(
          latency_sim::ExecutionClass::kForeground);
      const auto status = engine->Delete(timeout_key);
      retired_operation = ReadRemoteDeleteOperation(response_pipe[0]);
      assert(status.code == tigonkv::StatusCode::kTimeout);
      const auto restored = engine->Get(timeout_key);
      assert(restored.status.ok() &&
             restored.value == FixedValue("timeout-me"));
    }
    assert(unsetenv("TIGONKV_TEST_REMOTE_DELETE_RESPONSE_FD") == 0);

    assert(setenv("TIGONKV_TEST_REMOTE_DELETE_RESPONSE_FD",
                  std::to_string(response_pipe[1]).c_str(), 1) == 0);
    {
      tigonkv::engine::mem_access::LatencyScope facade(
          latency_sim::ExecutionClass::kForeground);
      // A responder thread communicates only through the operation-record
      // pipe and the real inbound MPSC ring.  It never reads or writes the
      // requester's mailbox, whose operation slot and SPSC inbox remain
      // single-thread-owned.
      std::thread responder([&] {
        const auto current = ReadRemoteDeleteOperation(response_pipe[0]);
        assert(current.partition_id == retired_operation.partition_id);
        assert(current.sequence != retired_operation.sequence);
        tigonkv::engine::mem_access::LatencyScope owner_scope(
            latency_sim::ExecutionClass::kBackground);
        auto &busy_control = engine->pool_->allocator().layout()
            .remote_delete_controls[kRequester][0];
        assert(busy_control.CompareExchange(
            current.sequence,
            tigonkv::engine::RemoteDeleteControlState::kPending,
            tigonkv::engine::RemoteDeleteControlState::kExecuting));
        engine->VisiblePartition(busy_key)->AbortRemoteDelete(
            current.target_row, kRequester);
        busy_control.PublishError(
            tigonkv::engine::RemoteDeleteControlError::kRejected);
        assert(busy_control.CompareExchange(
            current.sequence,
            tigonkv::engine::RemoteDeleteControlState::kExecuting,
            tigonkv::engine::RemoteDeleteControlState::kRejected));
        auto stale_message = std::make_unique<star::Message>();
        stale_message->set_source_node_id(kOwner);
        stale_message->set_dest_node_id(kRequester);
        star::TwoPLPashaMessageHandler::append_remote_delete_response(
            *stale_message, tigonkv::engine::kSingleTableId,
            retired_operation.partition_id,
            star::RemoteDeleteOutcome::Deleted, 0,
            retired_operation.target_row, retired_operation.sequence);
        EnqueueResponse(engine.get(), std::move(stale_message));
        auto failed_stale = std::make_unique<star::Message>();
        failed_stale->set_source_node_id(kOwner);
        failed_stale->set_dest_node_id(kRequester);
        star::TwoPLPashaMessageHandler::append_remote_delete_response(
            *failed_stale, tigonkv::engine::kSingleTableId,
            retired_operation.partition_id,
            star::RemoteDeleteOutcome::Failed, 0,
            retired_operation.target_row, retired_operation.sequence);
        EnqueueResponse(engine.get(), std::move(failed_stale));
        auto message = std::make_unique<star::Message>();
        message->set_source_node_id(kOwner);
        message->set_dest_node_id(kRequester);
        star::TwoPLPashaMessageHandler::append_remote_delete_response(
            *message, tigonkv::engine::kSingleTableId,
            current.partition_id,
            star::RemoteDeleteOutcome::Busy, 0, current.target_row,
            current.sequence);
        EnqueueResponse(engine.get(), std::move(message));
        auto duplicate = std::make_unique<star::Message>();
        duplicate->set_source_node_id(kOwner);
        duplicate->set_dest_node_id(kRequester);
        star::TwoPLPashaMessageHandler::append_remote_delete_response(
            *duplicate, tigonkv::engine::kSingleTableId,
            current.partition_id,
            star::RemoteDeleteOutcome::Busy, 0, current.target_row,
            current.sequence);
        EnqueueResponse(engine.get(), std::move(duplicate));
      });
      const auto status = engine->Delete(busy_key);
      responder.join();
      assert(status.code == tigonkv::StatusCode::kBusy);
      // The row is restored and readable through the normal path.
      const auto restored = engine->Get(busy_key);
      assert(restored.status.ok() &&
             restored.value == FixedValue("busy-me"));
    }
    assert(unsetenv("TIGONKV_TEST_REMOTE_DELETE_RESPONSE_FD") == 0);
    close(response_pipe[0]);
    close(response_pipe[1]);
    // A sequence may be allocated at UINT64_MAX exactly once, but the next
    // allocation must fail instead of wrapping into a reusable identity.
    auto *mailbox = engine->worker_mailboxes_[0].get();
    mailbox->next_operation_sequence =
        tigonkv::engine::kRemoteDeleteMaxTag;
    assert(engine->ReserveOperationSequence(*mailbox) ==
           tigonkv::engine::kRemoteDeleteMaxTag);
    assert(mailbox->next_operation_sequence == 0);
    bool sequence_exhausted = false;
    try {
      (void)engine->ReserveOperationSequence(*mailbox);
    } catch (const std::overflow_error &) {
      sequence_exhausted = true;
    }
    assert(sequence_exhausted);
    engine->ReleaseWorker();
  }
  unlink(path_template);
  std::printf("remote_delete_test ok\n");
  return 0;
}
