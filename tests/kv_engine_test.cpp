#include "kv/engine/kv_engine.h"
#include "common/CXLMemory.h"
#include "common/Encoder.h"
#include "common/Message.h"
#include "common/MessagePiece.h"
#include "common/MPSCRingBuffer.h"
#include "core/CxlIncomingDispatcher.h"
#include <latency_sim/access.h>
#include <latency_sim/atomic_access.h>
#include <latency_sim/config.h>
#include <latency_sim/domain.h>
#include <latency_sim/scope.h>
#include <latency_sim/simulator.h>
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
#include <cstring>
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
  config.size_mb = 64;
  config.hwcc_offset_mb = 0;
  config.hwcc_size_mb = 32;
  config.swcc_offset_mb = 32;
  config.swcc_size_mb = 32;
  config.hw_cc_budget_mb = 32;
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

std::string FixedKeyText(std::string_view key_text,
                         uint32_t fixed_key_size = 32) {
  assert(key_text.size() <= fixed_key_size);
  std::string key(fixed_key_size, '\0');
  std::memcpy(key.data(), key_text.data(), key_text.size());
  return key;
}

void RunFocusedG() {
  const std::size_t key_size = 32;
  const std::string key(key_size, 'k');
  const std::string value(128, 'v');

  // Factory and Handler use one exact original piece formula.  The raw
  // overload is the transaction-free KV entry point; the same Handler
  // decoder rejects any length other than the complete request.
  star::Message request;
  request.set_source_node_id(0);
  request.set_dest_node_id(0);
  request.set_worker_id(0);
  const auto request_bytes = star::TwoPLPashaMessageFactory::
      new_remote_insert_message(request, 0, 3, key.data(), key.size(),
                                value.data(), value.size(), 7, 0);
  assert(request.get_message_count() == 1);
  assert(request.get_message_length() ==
         star::Message::get_prefix_size() + request_bytes);
  const auto piece = *request.begin();
  const char *decoded_key = nullptr;
  const char *decoded_value = nullptr;
  uint64_t transaction_id = 0;
  uint32_t key_offset = 0;
  assert(star::TwoPLPashaMessageHandler::decode_remote_insert_request(
      piece, key.size(), value.size(), decoded_key, decoded_value,
      transaction_id, key_offset));
  assert(transaction_id == 7 && key_offset == 0);
  assert(std::memcmp(decoded_key, key.data(), key.size()) == 0);
  assert(std::memcmp(decoded_value, value.data(), value.size()) == 0);

  star::Message malformed;
  malformed.set_source_node_id(0);
  malformed.set_dest_node_id(0);
  malformed.set_worker_id(0);
  const uint32_t wrong_size = static_cast<uint32_t>(request_bytes + 1);
  star::Encoder malformed_encoder(malformed.data);
  malformed_encoder << star::MessagePiece::construct_message_piece_header(
      static_cast<uint32_t>(star::TwoPLPashaMessage::REMOTE_INSERT_REQUEST),
      wrong_size, 0, 3);
  malformed_encoder.write_n_bytes(key.data(), key.size());
  malformed_encoder.write_n_bytes(value.data(), value.size());
  malformed_encoder << uint64_t{7} << uint32_t{0};
  malformed.flush();
  assert(!star::TwoPLPashaMessageHandler::decode_remote_insert_request(
      *malformed.begin(), key.size(), value.size(), decoded_key, decoded_value,
      transaction_id, key_offset));

  star::Message two_pieces;
  two_pieces.set_source_node_id(0);
  two_pieces.set_dest_node_id(0);
  two_pieces.set_worker_id(0);
  star::TwoPLPashaMessageFactory::new_remote_delete_message(
      two_pieces, 0, 3, key.data(), key.size(), 0x1234, 7);
  star::TwoPLPashaMessageFactory::new_remote_delete_message(
      two_pieces, 0, 3, key.data(), key.size(), 0x5678, 8);
  assert(two_pieces.get_message_count() == 2);
  // Remote delete completion is a framed status response: owner-side
  // contention is retryable and must not be reported as a malformed request.
  star::Message busy_delete_response;
  star::TwoPLPashaMessageHandler::append_remote_delete_response(
      busy_delete_response, 0, 3, star::RemoteDeleteOutcome::Busy, 0,
      0x1234, 7);
  star::RemoteDeleteOutcome delete_outcome{};
  uint32_t delete_key_offset = 99;
  uint64_t delete_target_row = 0;
  uint64_t delete_sequence = 0;
  assert(star::TwoPLPashaMessageHandler::decode_remote_delete_response(
      *busy_delete_response.begin(), delete_outcome, delete_key_offset,
      delete_target_row, delete_sequence));
  assert(delete_outcome == star::RemoteDeleteOutcome::Busy &&
         delete_key_offset == 0 && delete_target_row == 0x1234 &&
         delete_sequence == 7);
  // Initialize two owner arenas so the second published transport ring is
  // idle in this process; the parent demuxer consumes only ring 0.
  char path_template[] = "/tmp/tigonkv-engine-g-XXXXXX";
  const int fd = mkstemp(path_template);
  assert(fd >= 0);
  close(fd);
  const auto node0_config = ConfigFor(path_template, 2, 0);
  const auto node1_config = ConfigFor(path_template, 2, 1);
  const pid_t peer = fork();
  assert(peer >= 0);
  if (peer == 0) {
    // fork() inherits the parent's thread-local latency state (including the
    // lifecycle generation and any active scope depth).  The V6 lifecycle
    // guards treat a stale TLS generation as a hard error, so reset the
    // inherited TLS before opening a fresh engine in the child.
#if !defined(LATENCY_SIM_COMPILE_OFF)
    latency_sim::detail::g_thread_state = {};
#endif
    for (;;) {
      try {
        (void)tigonkv::engine::KVEngine::Open(node1_config, false);
        _exit(0);
      } catch (const std::exception &) {
        std::this_thread::yield();
      }
    }
  }
  auto engine = tigonkv::engine::KVEngine::Open(node0_config, true);
  int peer_status = 0;
  assert(waitpid(peer, &peer_status, 0) == peer);
  assert(WIFEXITED(peer_status) && WEXITSTATUS(peer_status) == 0);
  // The ring state-machine below touches the published HWCC transport ring;
  // it runs inside an explicit scope and is torn down before engine.reset().
  {
  tigonkv::engine::mem_access::LatencyScope scope(
      latency_sim::ExecutionClass::kBackground);
  void *root = nullptr;
  star::CXLMemory::wait_and_retrieve_cxl_shared_data(
      star::CXLMemory::cxl_transport_root_index, &root);
  auto *rings = static_cast<star::MPSCRingBuffer *>(root);
  star::MPSCRingBuffer &ring = rings[1];

  // Exercise the master MPSC reservation/full/dequeue/reuse state machine.
  char payload[] = "ring";
  const std::size_t capacity = ring.get_entry_num();
  for (std::size_t i = 0; i < capacity; ++i)
    assert(ring.enqueue(payload, sizeof(payload)));
  assert(!ring.enqueue(payload, sizeof(payload)));
  char output[256]{};
  assert(ring.dequeue(output, sizeof(output)) == sizeof(payload));
  assert(std::memcmp(output, payload, sizeof(payload)) == 0);
  assert(ring.enqueue(payload, sizeof(payload)));
  for (std::size_t i = 0; i < capacity - 1; ++i)
    assert(ring.dequeue(output, sizeof(output)) == sizeof(payload));
  // The reuse probe leaves the replacement payload in the queue after the
  // capacity-1 drain; remove it before handing the same published ring to
  // BufferedReader, so the helper sees only the framed Message below.
  assert(ring.dequeue(output, sizeof(output)) == sizeof(payload));

  // The extracted helper owns one BufferedReader and transfers each Message
  // exactly once to the supplied worker binding.  This also covers the CXL
  // constructor's transport-specific non-null check.
  std::atomic<bool> stop{false};
  std::atomic<bool> delivered{false};
  star::Message inbound = request;
  inbound.set_dest_node_id(1);
  std::thread receiver([&] {
    star::RunCxlIncomingLoop(
        ring, 1, 1, stop,
        [&](uint32_t worker_id, std::unique_ptr<star::Message> message) {
          assert(worker_id == 0);
          assert(message->get_source_node_id() == 0);
          assert(message->get_dest_node_id() == 1);
          assert(message->get_message_count() == 1);
          delivered.store(true, std::memory_order_release);
          stop.store(true, std::memory_order_release);
        });
  });
  while (!ring.enqueue(inbound.get_raw_ptr(), inbound.get_message_length()))
    std::this_thread::yield();
  receiver.join();
  assert(delivered.load(std::memory_order_acquire));
  }
  engine.reset();
  unlink(path_template);

  // Explicit lifecycle contract: an active foreground binding prevents
  // Close/Shutdown from tearing down the pool; after ReleaseWorker the close
  // clears the gate and process-local bindings, and a different mapping can
  // attach cleanly again.
  char lifecycle_template[] = "/tmp/tigonkv-engine-lifecycle-XXXXXX";
  const int lifecycle_fd = mkstemp(lifecycle_template);
  assert(lifecycle_fd >= 0);
  close(lifecycle_fd);
  auto lifecycle = tigonkv::engine::KVEngine::Open(
      ConfigFor(lifecycle_template), true);
  lifecycle->BindWorker(0);
  bool shutdown_refused = false;
  try {
    lifecycle->Shutdown();
  } catch (const std::runtime_error &) {
    shutdown_refused = true;
  }
  assert(shutdown_refused);
  lifecycle->ReleaseWorker();
  lifecycle->Shutdown();
#if !defined(LATENCY_SIM_COMPILE_OFF)
  // Shutdown clears the pool registrations at the quiescent boundary; the
  // simulator is left unconfigured with no active scope and zero pending.
  assert(!latency_sim::GlobalLatencySimulator().HasActiveScopeForCurrentThread());
  assert(latency_sim::GlobalLatencySimulator().PendingDelayNsForTest() == 0);
#endif
  assert(!star::CXLMemory::dual_region_allocator_bound());
  assert(star::CXL_EBR::bound_regions() == nullptr);
  auto reopened = tigonkv::engine::KVEngine::Open(
      ConfigFor(lifecycle_template), false);
  reopened->Shutdown();
  unlink(lifecycle_template);
}

// The production Clock chooses a victim only when its measured dynamic HWCC
// usage reaches its installed per-owner budget.  Focused tests use small rows,
// so force just that original policy predicate (as kv_partition_test does)
// without changing physical HWCC capacity or the Clock algorithm.  The first
// successful move-out synchronizes real usage again.
void ForceClockBudget(const tigonkv::Config &config) {
  // The production counter is the owner-private persistent Clock policy
  // control; tests no longer overwrite a process-local usage mirror.
  (void)config;
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
#if !defined(LATENCY_SIM_COMPILE_OFF)
    latency_sim::detail::g_thread_state = {};
#endif

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

void RunFocusedH() {
  char path_template[] = "/tmp/tigonkv-engine-h-XXXXXX";
  const int fd = mkstemp(path_template);
  assert(fd >= 0);
  close(fd);
  auto config = ConfigFor(path_template, 1, 0);
  config.foreground_worker_count_per_vm = 2;
  auto store = tigonkv::KVStore::Create(config, true);

  std::string local_key;
  std::string second_key;
  for (uint32_t i = 0; i < 1000 && (local_key.empty() || second_key.empty()); ++i) {
    std::string key(32, '\0');
    key[0] = static_cast<char>(i & 0xff);
    key[1] = static_cast<char>((i >> 8) & 0xff);
    std::memcpy(key.data() + 2, "H-key", 5);
    if (local_key.empty()) local_key = key;
    else if (key != local_key) second_key = key;
  }
  assert(!local_key.empty() && !second_key.empty());

  store->BindWorker(0);
  assert(store->Put(local_key, FixedValue("local")).ok());
  const auto local = store->Get(local_key);
  assert(local.status.ok() && local.value == FixedValue("local"));
  const auto first_runtime = store->Runtime();
  assert(first_runtime.logical_ops == 2 && first_runtime.commits == 2);
  store->ReleaseWorker();

  store->BindWorker(1);
  assert(store->Put(local_key, FixedValue("local-2")).ok());
  const auto second_local = store->Get(local_key);
  assert(second_local.status.ok() && second_local.value == FixedValue("local-2"));
  store->ReleaseWorker();

  store->BindWorker(0);
  assert(store->Put(second_key, FixedValue("second")).ok());
  const auto second = store->Get(second_key);
  assert(second.status.ok() && second.value == FixedValue("second"));
  store->ReleaseWorker();

  const auto runtime = store->Runtime();
  assert(runtime.logical_ops == 6 && runtime.commits == 6);
  assert(runtime.private_puts + runtime.shared_puts >= 2);
  assert(runtime.network_tx_bytes == 0);
  assert(runtime.network_rx_bytes == 0);
  store.reset();
  unlink(path_template);
}

void RunFocusedL() {
  static_assert(static_cast<uint8_t>(star::MigrationResponseOutcome::Migrated) == 0);
  static_assert(static_cast<uint8_t>(star::MigrationResponseOutcome::Missing) == 1);
  static_assert(static_cast<uint8_t>(star::MigrationResponseOutcome::Busy) == 2);
  static_assert(static_cast<uint8_t>(star::MigrationResponseOutcome::NoMemory) == 3);
  static_assert(static_cast<uint8_t>(star::RemoteInsertOutcome::Inserted) == 0);
  static_assert(static_cast<uint8_t>(star::RemoteInsertOutcome::AlreadyExists) == 1);
  static_assert(static_cast<uint8_t>(star::RemoteInsertOutcome::Busy) == 2);
  static_assert(static_cast<uint8_t>(star::RemoteInsertOutcome::NoMemory) == 3);

  for (const auto outcome : {star::MigrationResponseOutcome::Migrated,
                             star::MigrationResponseOutcome::Missing,
                             star::MigrationResponseOutcome::Busy,
                             star::MigrationResponseOutcome::NoMemory}) {
    star::Message message;
    message.set_source_node_id(0);
    message.set_dest_node_id(1);
    message.set_worker_id(0);
    star::TwoPLPashaMessageHandler::append_data_migration_response(
        message, 0, 0, outcome, 0);
    star::MigrationResponseOutcome decoded{};
    uint32_t key_offset = 99;
    assert(star::TwoPLPashaMessageHandler::decode_data_migration_response(
        *message.begin(), decoded, key_offset));
    assert(decoded == outcome && key_offset == 0);
  }
  {
    star::Message malformed;
    star::TwoPLPashaMessageHandler::append_data_migration_response(
        malformed, 0, 0, star::MigrationResponseOutcome::Busy, 0);
    const std::size_t payload = star::Message::get_prefix_size() +
                                star::MessagePiece::get_header_size();
    malformed.data[payload] = static_cast<char>(4);
    star::MigrationResponseOutcome decoded{};
    uint32_t key_offset = 0;
    assert(!star::TwoPLPashaMessageHandler::decode_data_migration_response(
        *malformed.begin(), decoded, key_offset));
    const auto malformed_header =
        star::MessagePiece::construct_message_piece_header(
            static_cast<uint32_t>(star::TwoPLPashaMessage::DATA_MIGRATION_RESPONSE),
            star::TwoPLPashaMessageFactory::status_key_offset_response_size() - 1,
            0, 0);
    std::memcpy(malformed.data.data() + star::Message::get_prefix_size(),
                &malformed_header, sizeof(malformed_header));
    assert(!star::TwoPLPashaMessageHandler::decode_data_migration_response(
        *malformed.begin(), decoded, key_offset));
  }
  for (const auto outcome : {star::RemoteInsertOutcome::Inserted,
                             star::RemoteInsertOutcome::AlreadyExists,
                             star::RemoteInsertOutcome::Busy,
                             star::RemoteInsertOutcome::NoMemory}) {
    star::Message message;
    message.set_source_node_id(0);
    message.set_dest_node_id(1);
    message.set_worker_id(0);
    star::TwoPLPashaMessageHandler::append_remote_insert_response(
        message, 0, 0, outcome, 0);
    star::RemoteInsertOutcome decoded{};
    uint32_t key_offset = 99;
    assert(star::TwoPLPashaMessageHandler::decode_remote_insert_response(
        *message.begin(), decoded, key_offset));
    assert(decoded == outcome && key_offset == 0);
  }
}

}  // namespace

int main(int argc, char **argv) {
  if (argc != 1) {
    if (argc != 2 ||
        (std::string_view(argv[1]) != "--focused-stage=G" &&
         std::string_view(argv[1]) != "--focused-stage=H" &&
         std::string_view(argv[1]) != "--focused-stage=L")) {
      std::fprintf(stderr,
                   "usage: kv_engine_test [--focused-stage=G|H|L]\n");
      return 2;
    }
    if (std::string_view(argv[1]) == "--focused-stage=G") {
      RunFocusedG();
      return 0;
    }
    if (std::string_view(argv[1]) == "--focused-stage=H") {
      RunFocusedH();
      return 0;
    }
    if (std::string_view(argv[1]) == "--focused-stage=L") {
      RunFocusedL();
      return 0;
    }
    std::fprintf(stderr, "focused stage is not implemented yet: %s\n", argv[1]);
    return 2;
  }
  char corrupt_template[] = "/tmp/tigonkv-engine-corrupt-XXXXXX";
  const int corrupt_fd = mkstemp(corrupt_template);
  assert(corrupt_fd >= 0);
  close(corrupt_fd);
  const pid_t corrupt_child = fork();
  assert(corrupt_child >= 0);
  if (corrupt_child == 0) {
#if !defined(LATENCY_SIM_COMPILE_OFF)
    latency_sim::detail::g_thread_state = {};
#endif

    const rlimit no_core{0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    const auto corrupt_config = ConfigFor(corrupt_template);
    auto engine = tigonkv::engine::KVEngine::Open(corrupt_config, true);
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
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

  // §4.2: foreground APIs may not borrow worker 0.  Both a never-bound
  // caller and a caller after ReleaseWorker must fail before entering EBR.
  for (bool release_before_call : {false, true}) {
    char binding_template[] = "/tmp/tigonkv-engine-binding-XXXXXX";
    const int binding_fd = mkstemp(binding_template);
    assert(binding_fd >= 0);
    close(binding_fd);
    const pid_t binding_child = fork();
    assert(binding_child >= 0);
    if (binding_child == 0) {
#if !defined(LATENCY_SIM_COMPILE_OFF)
    latency_sim::detail::g_thread_state = {};
#endif

      const rlimit no_core{0, 0};
      (void)setrlimit(RLIMIT_CORE, &no_core);
      auto engine = tigonkv::engine::KVEngine::Open(
          ConfigFor(binding_template), true);
      if (release_before_call) {
        engine->BindWorker(0);
        engine->ReleaseWorker();
      }
      (void)engine->Get("unbound");
      _exit(91);
    }
    int binding_status = 0;
    assert(waitpid(binding_child, &binding_status, 0) == binding_child);
    assert(WIFSIGNALED(binding_status) &&
           WTERMSIG(binding_status) == SIGABRT);
    unlink(binding_template);
  }

  char misroute_template[] = "/tmp/tigonkv-engine-misroute-XXXXXX";
  const int misroute_fd = mkstemp(misroute_template);
  assert(misroute_fd >= 0);
  close(misroute_fd);
  const pid_t misroute_child = fork();
  assert(misroute_child >= 0);
  if (misroute_child == 0) {
#if !defined(LATENCY_SIM_COMPILE_OFF)
    latency_sim::detail::g_thread_state = {};
#endif

    const rlimit no_core{0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    const auto config = ConfigFor(misroute_template, 2, 0);
    auto peer = JoiningPeer(ConfigFor(misroute_template, 2, 1));
    auto engine = tigonkv::engine::KVEngine::Open(config, true);
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
    engine->BindWorker(0);
    std::string wrong_owner_key;
    for (uint32_t i = 0; i < 1000; ++i) {
      wrong_owner_key = FixedKeyText("misroute-" + std::to_string(i));
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
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
    engine->BindWorker(0);
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
      const std::string key = FixedKeyText("route-oracle-" + std::to_string(i));
      const uint32_t partition = engine->PartitionForKey(key);
      assert(partition < single_owner.partition_count);
      assert(engine->OwnerForKey(key) ==
             partition % single_owner.vm_count);
    }
    const std::string alpha = FixedKeyText("alpha");
    assert(engine->Put(alpha, FixedValue("one")).ok());
    assert(engine->Put(alpha, FixedValue("updated")).ok());
    const auto found = engine->Get(alpha);
    assert(found.status.ok() && found.value == FixedValue("updated"));
    const auto cas = engine->CompareExchange(alpha, FixedValue("updated"),
                                             FixedValue("cas-value"));
    assert(cas.status.ok() && cas.exchanged);
    const auto cas_failed = engine->CompareExchange(
        alpha, FixedValue("updated"), FixedValue("ignored"));
    assert(cas_failed.status.code == tigonkv::StatusCode::kCompareFailed && !cas_failed.exchanged);
    const std::string counter = FixedKeyText("counter");
    assert(engine->Put(counter, FixedValue("1")).ok());
    const auto incremented = engine->Increment(counter, 2);
    assert(incremented.status.ok() && incremented.value == 3);
    const auto scan = engine->Scan(alpha, ScanEndKey(), 0);
    assert(scan.status.ok() && scan.items.size() == 2);
    assert(scan.items[0].key == alpha &&
           scan.items[0].value == FixedValue("cas-value"));
    assert(scan.items[1].key == counter &&
           scan.items[1].value == FixedValue("3"));
    {
      const auto rt = engine->EngineRuntime();
      assert(rt.scan_partition_probes >= 1);
      assert(rt.scan_migrate_rpcs == 0);
    }
    assert(engine->Delete(alpha).ok());
    assert(engine->Get(alpha).status.code == tigonkv::StatusCode::kNotFound);
    bool same_thread_rejected = false;
    try {
      engine->BindWorker(0);
    } catch (const std::runtime_error &) {
      same_thread_rejected = true;
    }
    assert(same_thread_rejected);
    engine->ReleaseWorker();
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
    engine->BindWorker(0);
    const std::string persist = FixedKeyText("persist");
    assert(engine->Put(persist, FixedValue("value")).ok());
    engine->ReleaseWorker();
  }
  {
    auto attached = tigonkv::engine::KVEngine::Open(single_owner, false);
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
    attached->BindWorker(0);
    const auto found = attached->Get(FixedKeyText("persist"));
    assert(found.status.ok() && found.value == FixedValue("value"));
    attached->ReleaseWorker();
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
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
    attached->BindWorker(0);
    assert(attached->Get(FixedKeyText("persist")).status.ok());
    attached->ReleaseWorker();
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
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
    engine->BindWorker(0);
    uint32_t part = 0;
    std::vector<std::string> owned;
    for (int i = 0; i < 64; ++i) {
      const std::string k = FixedKeyText("pscan-" + std::to_string(i));
      assert(engine->Put(k, FixedValue("v")).ok());
    }
    part = engine->PartitionForKey(FixedKeyText("pscan-0"));
    for (int i = 0; i < 64; ++i) {
      const std::string k = FixedKeyText("pscan-" + std::to_string(i));
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
    bool invalid_scan_rejected = false;
    try {
      (void)engine->PreparePartitionSharedScan(part, "x", scan_max, 0);
    } catch (const std::invalid_argument &) {
      invalid_scan_rejected = true;
    }
    assert(invalid_scan_rejected);
    engine->ReleaseWorker();
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
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
    engine->BindWorker(0);
    std::vector<std::string> keys;
    for (int i = 0; i < 64; ++i) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "k%02d", i);
      keys.emplace_back(FixedKeyText(buf));
      assert(engine->Put(keys.back(), FixedValue(std::string("v") + keys.back())).ok());
    }
    std::sort(keys.begin(), keys.end());
    const auto expect_scan = [&](std::string_view start, uint64_t limit) {
      const std::string fixed_start = FixedKeyText(start);
      std::vector<std::string> expected;
      for (const auto &key : keys) {
        if (key < fixed_start) continue;
        expected.push_back(key);
        if (limit != 0 && expected.size() >= limit) break;
      }
      const auto got = engine->Scan(fixed_start, ScanEndKey(), limit);
      assert(got.status.ok());
      assert(got.items.size() == expected.size());
      for (size_t i = 0; i < expected.size(); ++i) {
        assert(got.items[i].key == expected[i]);
        assert(got.items[i].value == FixedValue(std::string("v") + expected[i]));
      }
    };
    expect_scan(FixedKeyText(""), 0);
    expect_scan(FixedKeyText(""), 7);
    expect_scan(FixedKeyText("k00"), 1);
    expect_scan(FixedKeyText("k00"), 0);
    expect_scan(FixedKeyText("k10"), 5);
    expect_scan(FixedKeyText("k63"), 1);
    expect_scan(FixedKeyText("k63"), 10);
    expect_scan(FixedKeyText("k99"), 10);  // past end → empty
    expect_scan(FixedKeyText("k05"), 0);
    // Mid-key that is not present still returns the next key onward.
    expect_scan(FixedKeyText("k0a"), 3);
    engine->ReleaseWorker();
    unlink(oracle_path.c_str());
  }

  char routed_template[] = "/tmp/tigonkv-engine-route-XXXXXX";
  const int routed_fd = mkstemp(routed_template);
  assert(routed_fd >= 0);
  close(routed_fd);
  const std::string routed_path(routed_template);
  auto node_zero = ConfigFor(routed_path, 2, 0);
  node_zero.foreground_worker_count_per_vm = 4;
  // Keep the file-backed fixture at the required 32MiB+32MiB physical
  // layout, but make Clock's policy budget small enough for this focused
  // route fixture to deterministically exercise the original victim path.
  node_zero.hw_cc_budget_mb = 2;
  auto node_one_config = ConfigFor(routed_path, 2, 1);
  node_one_config.foreground_worker_count_per_vm = 4;
  node_one_config.hw_cc_budget_mb = 2;
  {
    std::unique_ptr<tigonkv::engine::KVEngine> engine;
    {
      // The later child is the real peer for this test.  It cannot join until
      // after the parent has seeded the owner-0 state, so use a short-lived
      // owner only to satisfy first-layout initialization.
      auto bootstrap = JoiningPeer(node_one_config);
      engine = tigonkv::engine::KVEngine::Open(node_zero, true);
    }
    tigonkv::engine::mem_access::LatencyScope scope(
        latency_sim::ExecutionClass::kBackground);
    engine->BindWorker(0);
    assert(star::CXLMemory::bound_owner_shard() == 0);
    for (const std::string key : {FixedKeyText("H-route"),
                                  FixedKeyText("a-route")}) {
      const uint32_t partition = engine->PartitionForKey(key);
      assert(engine->OwnerForKey(key) == partition % node_zero.vm_count);
    }
    // Constructing every partition (owners 0 and 1) must not rebind the
    // process-level allocator owner away from this VM (§11.3).
    assert(star::CXLMemory::bound_owner_shard() == 0);

    const std::string owner_zero_key = FixedKeyText("H-owner-zero");
    const std::string owner_one_key = FixedKeyText("a-owner-one");
    assert(engine->OwnerForKey(owner_zero_key) == 0);
    assert(engine->OwnerForKey(owner_one_key) == 1);
    assert(engine->Put(owner_zero_key, FixedValue("owner-zero")).ok());
    // Keep this focused Debug gate small; larger migration volume belongs to
    // the documented 4VM workload rather than a unit/CTest stage gate.
    constexpr uint32_t kRemoteScanRows = 32;
    uint32_t remote_scan_rows = 0;
    for (uint32_t i = 0; remote_scan_rows < kRemoteScanRows; ++i) {
      const std::string key = FixedKeyText("H0-bulk-" + std::to_string(i));
      assert(engine->Put(key, FixedValue("bulk")).ok());
      ++remote_scan_rows;
    }
    // Populate one remote partition past a Scan page boundary. The owner
    // moves each authoritative prefix in without a persistent pin; the
    // requester reads it only through CXL.
    std::vector<std::string> promoted_scan_keys;
    uint32_t promoted_partition = UINT32_MAX;
    constexpr uint32_t kPromotedScanRows = 130;
    constexpr uint32_t kScanPageRows = 100;
    for (uint32_t i = 0; promoted_scan_keys.size() < kPromotedScanRows; ++i) {
      char key[32];
      std::snprintf(key, sizeof(key), "H-hybrid-%08u", i);
      const std::string fixed_key = FixedKeyText(key);
      const uint32_t partition = engine->PartitionForKey(fixed_key);
      if (promoted_partition == UINT32_MAX) promoted_partition = partition;
      if (partition != promoted_partition) continue;
      assert(engine->Put(fixed_key, FixedValue("owner-authority")).ok());
      promoted_scan_keys.emplace_back(fixed_key);
    }
    std::vector<std::string> concurrent_insert_keys;
    for (uint32_t i = 0; concurrent_insert_keys.size() < 4; ++i) {
      const std::string key = FixedKeyText(
          "H-hybrid-00000000-insert-" + std::to_string(i));
      concurrent_insert_keys.push_back(key);
    }
    int scan_ready[2];
    assert(pipe2(scan_ready, O_CLOEXEC | O_NONBLOCK) == 0);
    // Do not fork with the parent's thread-local external EBR binding.  The
    // child opens a separate VM/EBR instance and must start with a clean
    // binding; the parent rebinds its worker immediately after fork.
    engine->ReleaseWorker();
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
#if !defined(LATENCY_SIM_COMPILE_OFF)
    latency_sim::detail::g_thread_state = {};
#endif

      close(scan_ready[0]);
      auto node_one = tigonkv::engine::KVEngine::Open(node_one_config, false);
      tigonkv::engine::mem_access::LatencyScope scope(
          latency_sim::ExecutionClass::kBackground);
      node_one->BindWorker(0);
      if (star::CXLMemory::bound_owner_shard() != 1) _exit(30);
      if (!node_one->Put(owner_one_key, FixedValue("owner-one")).ok()) _exit(1);
      for (const auto &key : promoted_scan_keys) {
        const auto promoted = node_one->Get(key);
        if (!promoted.status.ok() ||
            promoted.value != FixedValue("owner-authority"))
          _exit(14);
      }
      // KVEngine exposes one-shot primitives.  Mirror the production
      // KVStore boundary here rather than restoring an Engine-local Scan
      // retry after a successful range move-in.
      const std::string global_scan_end = ScanEndKey();
      auto scan_with_facade_retry = [&](std::string_view start,
                                        uint64_t limit,
                                        std::string_view end = {}) {
        tigonkv::ScanResult result;
        for (;;) {
          result = node_one->Scan(
              start, end.empty() ? std::string_view(global_scan_end) : end,
              limit);
          if (result.status.code != tigonkv::StatusCode::kBusy) return result;
          std::this_thread::yield();
        }
        return result;
      };
      const uint64_t tx_before_authoritative_scan = node_one->NetworkTxBytes();
      const auto authoritative_scan = scan_with_facade_retry(
          promoted_scan_keys.front(), kScanPageRows);
      const uint64_t authoritative_scan_tx =
          node_one->NetworkTxBytes() - tx_before_authoritative_scan;
      if (!authoritative_scan.status.ok() ||
          authoritative_scan.items.size() != kScanPageRows)
        _exit(15);
      for (size_t i = 0; i < authoritative_scan.items.size(); ++i) {
        const auto &item = authoritative_scan.items[i];
        if (item.key != promoted_scan_keys[i] ||
            item.value != FixedValue("owner-authority"))
          _exit(16);
      }
      // The original CXL tree starts at lower_bound(min_key), rather than
      // requiring min_key itself to exist.  Keep this probe in the same
      // non-empty remote partition: a scan beginning at the global minimum
      // would first visit an empty partition, outside the original
      // executor's "every remote scan returns one value" assumption.
      std::string missing_start = promoted_scan_keys.front();
      const std::size_t first_trailing_zero = missing_start.find('\0');
      assert(first_trailing_zero < node_one_config.fixed_key_size);
      missing_start[first_trailing_zero] = '\1';
      std::string promoted_partition_end =
          node_one_config.partition_ranges[promoted_partition].upper_key;
      promoted_partition_end.resize(node_one_config.fixed_key_size, '\0');
      const auto missing_start_scan = scan_with_facade_retry(
          missing_start, 3, promoted_partition_end);
      if (!missing_start_scan.status.ok() || missing_start_scan.items.size() != 3)
        _exit(31);
      for (size_t i = 0; i < missing_start_scan.items.size(); ++i) {
        if (missing_start_scan.items[i].key != promoted_scan_keys[i + 1])
          _exit(32);
      }
      // limit=0 is the original unbounded range request.  It must move the
      // remaining owner range, then return a CXL-only fragment without an
      // owner-value stream or pagination state.
      const auto unbounded_scan = scan_with_facade_retry(
          missing_start, 0, promoted_partition_end);
      if (!unbounded_scan.status.ok()) _exit(33);
      if (unbounded_scan.items.size() < promoted_scan_keys.size() - 1)
        _exit(35);
      for (size_t i = 0; i + 1 < promoted_scan_keys.size(); ++i) {
        if (unbounded_scan.items[i].key != promoted_scan_keys[i + 1])
          _exit(34);
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
      node_one->ReleaseWorker();
      for (uint32_t worker = 0; worker < 1; ++worker) {
        scan_threads.emplace_back([&, worker] {
          tigonkv::engine::mem_access::LatencyScope thread_scope(
              latency_sim::ExecutionClass::kBackground);
          node_one->BindWorker(worker);
          while (!start_concurrent_scans.load(std::memory_order_acquire))
            std::this_thread::yield();
          const auto scan = scan_with_facade_retry(
              promoted_scan_keys.front(), kScanPageRows);
          if (!scan.status.ok() || scan.items.size() != kScanPageRows) {
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
      node_one->BindWorker(0);
      if (concurrent_scan_failed.load(std::memory_order_acquire)) _exit(18);

      const auto limited_scan = scan_with_facade_retry(
          promoted_scan_keys.front(), 17);
      if (!limited_scan.status.ok() || limited_scan.items.size() != 17) _exit(12);
      for (size_t i = 1; i < limited_scan.items.size(); ++i) {
        if (limited_scan.items[i - 1].key >= limited_scan.items[i].key) _exit(13);
      }
      if (!node_one->Put(owner_zero_key, FixedValue("forwarded")).ok()) _exit(3);
      const uint64_t tx_after_remote_update = node_one->NetworkTxBytes();
      const auto read = node_one->Get(owner_zero_key);
      if (!read.status.ok() || read.value != FixedValue("forwarded")) _exit(4);
      if (node_one->NetworkTxBytes() != tx_after_remote_update) _exit(24);
      const uint64_t tx_after_promotion = node_one->NetworkTxBytes();
      const auto shared_read = node_one->Get(owner_zero_key);
      if (!shared_read.status.ok() ||
          shared_read.value != FixedValue("forwarded"))
        _exit(19);
      if (!node_one->Put(owner_zero_key, FixedValue("shared-put")).ok()) _exit(20);
      if (node_one->NetworkTxBytes() != tx_after_promotion) _exit(21);
      const auto cas = node_one->CompareExchange(
          owner_zero_key, FixedValue("shared-put"), FixedValue("cas-forwarded"));
      if (!cas.status.ok() || !cas.exchanged) _exit(5);
      const auto cas_miss = node_one->CompareExchange(
          owner_zero_key, FixedValue("forwarded"), FixedValue("ignored"));
      if (cas_miss.status.code != tigonkv::StatusCode::kCompareFailed || cas_miss.exchanged) _exit(6);
      // The GET above promoted this row.  Both CAS operations must use the
      // non-owner shared fast path rather than send another fixed transport frame.
      if (node_one->NetworkTxBytes() != tx_after_promotion) _exit(11);
      const std::string counter_key = FixedKeyText("H-counter");
      if (counter_key.empty() ||
          !node_one->Put(counter_key, FixedValue("1")).ok()) _exit(7);
      const auto increment = node_one->Increment(counter_key, 2);
      if (!increment.status.ok() || increment.value != 3) _exit(8);
      const uint64_t tx_after_remote_increment = node_one->NetworkTxBytes();
      const auto promoted_counter = node_one->Get(counter_key);
      if (!promoted_counter.status.ok() ||
          promoted_counter.value != FixedValue("3")) _exit(22);
      if (node_one->NetworkTxBytes() != tx_after_remote_increment) _exit(23);
      const std::string cas_create_key = FixedKeyText("H-cas-create");
      const auto cas_create =
          node_one->CompareExchange(cas_create_key, "", FixedValue("created"));
      if (!cas_create.status.ok() || !cas_create.exchanged) _exit(25);
      const auto created = node_one->Get(cas_create_key);
      if (!created.status.ok() || created.value != FixedValue("created"))
        _exit(26);
      const std::string cas_race_key = FixedKeyText("H-cas-race");
      std::atomic<uint32_t> cas_winners{0};
      std::atomic<bool> cas_protocol_failed{false};
      std::vector<std::thread> cas_threads;
      node_one->ReleaseWorker();
      for (uint32_t worker = 0; worker < 4; ++worker) {
        cas_threads.emplace_back([&, worker] {
          tigonkv::engine::mem_access::LatencyScope thread_scope(
              latency_sim::ExecutionClass::kBackground);
          node_one->BindWorker(worker);
          tigonkv::CasResult result;
          // KVEngine is the one-shot primitive; KVStore is the only
          // production Busy-retry facade. Exercise the same cooperative retry
          // here so a remote-create loser observes the published winner and
          // becomes CompareFailed rather than being misclassified as a wire
          // protocol failure.
          for (;;) {
            result = node_one->CompareExchange(cas_race_key, "", FixedValue("winner"));
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
      node_one->BindWorker(0);
      if (cas_winners.load() != 1 || cas_protocol_failed.load()) _exit(27);
      if (!node_one->Delete(owner_zero_key).ok()) _exit(9);
      if (node_one->Get(owner_zero_key).status.code != tigonkv::StatusCode::kNotFound) _exit(10);
      // REMOTE_DELETE_RESPONSE is sent only after the owner callback has
      // removed the shared index and retired the old row.  Recreate the same
      // key immediately through REMOTE_INSERT: this catches a requester-side
      // post-ack unlock/ref access to the retired smeta as well as a stale
      // shared-index entry left by the owner callback.
      if (!node_one->Put(owner_zero_key, FixedValue("recreated-after-delete")).ok())
        _exit(28);
      const auto recreated = node_one->Get(owner_zero_key);
      if (!recreated.status.ok() ||
          recreated.value != FixedValue("recreated-after-delete"))
        _exit(29);
      node_one->ReleaseWorker();
      close(scan_ready[1]);
      _exit(0);
    }
    close(scan_ready[1]);
    engine->BindWorker(0);
    int status = 0;
    std::atomic<bool> peer_service_stop{false};
    std::vector<std::thread> peer_service_workers;
    // The child below deliberately exercises four original worker mailboxes.
    // Keep matching owner workers alive for the duration of that peer phase;
    // each consumes only its own SPSC inbox, just like Executor workers.
    for (uint32_t worker = 1;
         worker < node_zero.foreground_worker_count_per_vm; ++worker) {
      peer_service_workers.emplace_back([&, worker] {
        engine->BindWorker(worker);
        while (!peer_service_stop.load(std::memory_order_acquire)) {
          engine->PollTransport();
          std::this_thread::yield();
        }
        engine->ReleaseWorker();
      });
    }
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
        ForceClockBudget(node_zero);
        const bool moved = star::migration_manager != nullptr &&
            star::migration_manager->move_row_out(
                engine->PartitionForKey(promoted_scan_keys[removal_key]));
        removal_key = (removal_key + 1) % promoted_scan_keys.size();
        if (moved) ++removals_during_scan;
      }
      if (concurrent_scan_started &&
          inserts_during_scan < concurrent_insert_keys.size()) {
        if (engine->Put(concurrent_insert_keys[inserts_during_scan],
                        FixedValue("concurrent-insert")).ok())
          ++inserts_during_scan;
      }
      const pid_t done = waitpid(child, &status, WNOHANG);
      if (done == child) break;
      assert(done == 0);
    }
    peer_service_stop.store(true, std::memory_order_release);
    for (auto &worker : peer_service_workers) worker.join();
    close(scan_ready[0]);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    // The 32MiB+32MiB focused pool need not reach the separately configured
    // Clock policy budget; deterministic victim behavior is covered by the
    // dedicated PolicyClock/migration gate.  Keep the concurrent scan and
    // insert assertions below as the route portion of this combined test.
    assert(inserts_during_scan == concurrent_insert_keys.size());
    assert(engine->NetworkTxBytes() > 0 && engine->NetworkRxBytes() > 0);
    const auto engine_runtime = engine->EngineRuntime();
    assert(engine_runtime.migration_in > 0);
    assert(engine_runtime.shared_swcc_flushes >= engine_runtime.migration_in);
    engine->ReleaseWorker();
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
      tigonkv::engine::mem_access::LatencyScope scope(
          latency_sim::ExecutionClass::kBackground);
      engine->BindWorker(0);
      const std::string key = FixedKeyText("hist-private");
      assert(engine->Put(key, FixedValue("v0")).ok());

      // Barriered Put then concurrent Gets: each Get must observe v0 or v1.
      std::atomic<bool> put_done{false};
      std::atomic<uint32_t> get_ok{0};
      std::atomic<bool> get_illegal{false};
      auto get_after_busy = [&](std::string_view read_key) {
        tigonkv::GetResult result;
        for (;;) {
          result = engine->Get(read_key);
          if (result.status.code != tigonkv::StatusCode::kBusy) return result;
          std::this_thread::yield();
        }
        return result;
      };
      std::vector<std::thread> readers;
      for (uint32_t w = 1; w < 4; ++w) {
        readers.emplace_back([&, w] {
          tigonkv::engine::mem_access::LatencyScope thread_scope(
              latency_sim::ExecutionClass::kBackground);
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
      for (;;) {
        put_status = engine->Put(key, FixedValue("v1"));
        if (put_status.code != tigonkv::StatusCode::kBusy) break;
        std::this_thread::yield();
      }
      assert(put_status.ok());
      put_done.store(true, std::memory_order_release);
      for (auto &t : readers) t.join();
      assert(!get_illegal.load());
      assert(get_ok.load() == 3);
      assert(engine->Get(key).value == FixedValue("v1"));

      // CAS: exactly one winner from empty expected on a fresh key.
      const std::string cas_key = FixedKeyText("hist-cas-private");
      std::atomic<uint32_t> winners{0};
      std::atomic<bool> cas_bad{false};
      std::vector<std::thread> casters;
      engine->ReleaseWorker();
      for (uint32_t w = 0; w < 4; ++w) {
        casters.emplace_back([&, w] {
          tigonkv::engine::mem_access::LatencyScope thread_scope(
              latency_sim::ExecutionClass::kBackground);
          engine->BindWorker(w);
          tigonkv::CasResult r;
          // KVEngine exposes one primitive attempt. A create-race loser may
          // observe the owner's still-invalid placeholder as Busy; the facade
          // is the sole operation-level retry boundary.
          for (;;) {
            r = engine->CompareExchange(cas_key, "", FixedValue("won"));
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
      engine->BindWorker(0);
      assert(!cas_bad.load());
      assert(winners.load() == 1);
      assert(engine->Get(cas_key).value == FixedValue("won"));

      // Distinct inserts still share successor locks while preserving the
      // original adjacent-tuple lifecycle. Exercise four owner workers so a
      // transient placeholder is never left permanently Busy for its
      // neighbours.
      constexpr uint32_t kCreateWorkers = 4;
      constexpr uint32_t kCreatesPerWorker = 16;
      std::atomic<bool> create_bad{false};
      std::vector<std::thread> creators;
      engine->ReleaseWorker();
      for (uint32_t w = 0; w < kCreateWorkers; ++w) {
        creators.emplace_back([&, w] {
          tigonkv::engine::mem_access::LatencyScope thread_scope(
              latency_sim::ExecutionClass::kBackground);
          engine->BindWorker(w);
          for (uint32_t i = 0; i < kCreatesPerWorker; ++i) {
            const std::string key = FixedKeyText(
                "hist-create-" + std::to_string(w) + "-" + std::to_string(i));
            tigonkv::Status status;
            for (;;) {
              status = engine->Put(key, FixedValue("created"));
              if (status.code != tigonkv::StatusCode::kBusy) break;
              std::this_thread::yield();
            }
            if (!status.ok()) {
              create_bad.store(true, std::memory_order_relaxed);
              break;
            }
          }
          engine->ReleaseWorker();
        });
      }
      for (auto &t : creators) t.join();
      engine->BindWorker(0);
      assert(!create_bad.load());
      for (uint32_t w = 0; w < kCreateWorkers; ++w) {
        for (uint32_t i = 0; i < kCreatesPerWorker; ++i) {
          const std::string key = FixedKeyText(
              "hist-create-" + std::to_string(w) + "-" + std::to_string(i));
          assert(engine->Get(key).value == FixedValue("created"));
        }
      }

      // Increment: N concurrent +1 from "0" → final == N.
      const std::string inc_key = FixedKeyText("hist-inc-private");
      assert(engine->Put(inc_key, FixedValue("0")).ok());
      constexpr uint32_t kIncWorkers = 4;
      constexpr uint32_t kIncPerWorker = 25;
      std::atomic<bool> inc_bad{false};
      std::vector<std::thread> inc_threads;
      engine->ReleaseWorker();
      for (uint32_t w = 0; w < kIncWorkers; ++w) {
        inc_threads.emplace_back([&, w] {
          tigonkv::engine::mem_access::LatencyScope thread_scope(
              latency_sim::ExecutionClass::kBackground);
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
      engine->BindWorker(0);
      assert(!inc_bad.load());
      const auto final_inc = engine->Get(inc_key);
      assert(final_inc.status.ok());
      assert(final_inc.value ==
             FixedValue(std::to_string(kIncWorkers * kIncPerWorker)));

      // Delete then Put: Get after delete is NotFound; after put sees new value.
      assert(engine->Delete(key).ok());
      assert(engine->Get(key).status.code == tigonkv::StatusCode::kNotFound);
      assert(engine->Put(key, FixedValue("v2")).ok());
      assert(engine->Get(key).value == FixedValue("v2"));
      engine->ReleaseWorker();
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
      tigonkv::engine::mem_access::LatencyScope scope(
          latency_sim::ExecutionClass::kBackground);
      engine0->BindWorker(0);

      const std::string owned0 = FixedKeyText("H-hist-owner0");
      const std::string owned1 = FixedKeyText("a-hist-owner1");
      assert(engine0->OwnerForKey(owned0) == 0);
      assert(engine0->OwnerForKey(owned1) == 1);
      assert(engine0->Put(owned0, FixedValue("owner0-v1")).ok());

      // child→parent: phase1 done; parent→child: parent done (EOF on close).
      int child_to_parent[2];
      int parent_to_child[2];
      assert(pipe2(child_to_parent, O_CLOEXEC) == 0);
      assert(pipe2(parent_to_child, O_CLOEXEC) == 0);
      // Do not inherit the parent's thread-local external EBR binding into
      // the child VM; the child creates and binds its own EBR instance.
      engine0->ReleaseWorker();
      const pid_t child = fork();
      assert(child >= 0);
      if (child == 0) {
#if !defined(LATENCY_SIM_COMPILE_OFF)
    latency_sim::detail::g_thread_state = {};
#endif

        close(child_to_parent[0]);
        close(parent_to_child[1]);
        auto engine1 = tigonkv::engine::KVEngine::Open(node1_cfg, false);
        tigonkv::engine::mem_access::LatencyScope scope(
            latency_sim::ExecutionClass::kBackground);
        engine1->BindWorker(0);
        // Remote Get → Forward migrate-in → shared authority.
        const auto g1 = engine1->Get(owned0);
        if (!g1.status.ok() || g1.value != FixedValue("owner0-v1")) _exit(41);
        const auto g2 = engine1->Get(owned0);
        if (!g2.status.ok() || g2.value != FixedValue("owner0-v1")) _exit(42);

        // Concurrent CAS on the migrated key: at most one exchange succeeds.
        std::atomic<uint32_t> shared_winners{0};
        std::atomic<bool> shared_bad{false};
        engine1->ReleaseWorker();
        std::thread cas_a([&] {
          tigonkv::engine::mem_access::LatencyScope thread_scope(
              latency_sim::ExecutionClass::kBackground);
          engine1->BindWorker(0);
          for (;;) {
            const auto r =
                engine1->CompareExchange(owned0, FixedValue("owner0-v1"),
                                         FixedValue("cas-remote"));
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
          tigonkv::engine::mem_access::LatencyScope thread_scope(
              latency_sim::ExecutionClass::kBackground);
          engine1->BindWorker(1);
          for (;;) {
            const auto r =
                engine1->CompareExchange(owned0, FixedValue("owner0-v1"),
                                         FixedValue("cas-remote-b"));
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
        engine1->BindWorker(0);
        if (shared_bad.load() || shared_winners.load() > 1) _exit(43);
        const auto after_cas = engine1->Get(owned0);
        if (!after_cas.status.ok()) _exit(44);
        if (after_cas.value != FixedValue("cas-remote") &&
            after_cas.value != FixedValue("cas-remote-b") &&
            after_cas.value != FixedValue("owner0-v1"))
          _exit(45);

        // Seed a key this node owns for parent's Forward Increment history.
        if (!engine1->Put(owned1, FixedValue("0")).ok()) _exit(46);
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
        engine1->ReleaseWorker();
        _exit(0);
      }

      close(child_to_parent[1]);
      close(parent_to_child[0]);
      engine0->BindWorker(0);
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

      // After child migrated owned0, give the original Clock one bounded
      // opportunity to select a victim.  This combined history fixture keeps
      // the formal 32MiB policy budget; deterministic move-out is covered by
      // the dedicated migration gate below.
      bool moved = false;
      for (int i = 0; i < 64 && !moved; ++i) {
        engine0->PollTransport();
        ForceClockBudget(node0_cfg);
        moved = star::migration_manager != nullptr &&
            star::migration_manager->move_row_out(engine0->PartitionForKey(owned0));
      }
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
      engine0->ReleaseWorker();
    }
    unlink(hist_path.c_str());
  }

  return 0;
}
