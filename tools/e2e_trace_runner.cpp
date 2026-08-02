#include "kv/kv_store.h"
#include "tools/e2e_trace_format.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace tigonkv;

namespace {
std::string Env(const char *primary, const char *compat, const std::string &fallback = {}) {
  const char *value = std::getenv(primary);
  if (value && *value) return value;
  value = std::getenv(compat);
  return value && *value ? value : fallback;
}
uint64_t ParseUnsigned(const std::string &text, const std::string &label) {
  size_t used = 0; uint64_t value = std::stoull(text, &used);
  if (used != text.size()) throw std::runtime_error("invalid " + label);
  return value;
}
[[noreturn]] void Fail(const std::string &message) { throw std::runtime_error(message); }

std::string TestValueOverride(uint32_t fixed_value_size) {
  const std::string hex = Env("TIGONKV_E2E_TEST_VALUE_HEX",
                              "CXLKV_E2E_TEST_VALUE_HEX");
  if (hex.empty()) return {};
  if (hex.size() != static_cast<size_t>(fixed_value_size) * 2)
    Fail("TIGONKV_E2E_TEST_VALUE_HEX has wrong fixed-value length");
  std::string value(fixed_value_size, '\0');
  const auto nibble = [](char ch) -> unsigned char {
    if (ch >= '0' && ch <= '9') return static_cast<unsigned char>(ch - '0');
    if (ch >= 'a' && ch <= 'f') return static_cast<unsigned char>(ch - 'a' + 10);
    if (ch >= 'A' && ch <= 'F') return static_cast<unsigned char>(ch - 'A' + 10);
    Fail("TIGONKV_E2E_TEST_VALUE_HEX is not hexadecimal");
  };
  for (uint32_t i = 0; i < fixed_value_size; ++i)
    value[i] = static_cast<char>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
  return value;
}

// Align with cxlkv FixedTraceValue: printable '!'..'~', length=fixed_value_size
// (trace PUT LEN is ignored for the payload, as in cxlkv).
std::string FixedTraceValue(std::mt19937_64 *rng, uint32_t fixed_value_size) {
  const std::string test_value = TestValueOverride(fixed_value_size);
  if (!test_value.empty()) return test_value;
  std::string value;
  value.resize(static_cast<size_t>(fixed_value_size));
  for (uint32_t i = 0; i < fixed_value_size; ++i)
    value[static_cast<size_t>(i)] = static_cast<char>('!' + ((*rng)() % 94U));
  return value;
}

void Barrier(const std::string &phase, uint32_t node, bool final, KVStore *store = nullptr) {
  const std::string dir = Env("TIGONKV_E2E_BARRIER_DIR", "CXLKV_E2E_BARRIER_DIR");
  if (dir.empty()) return;
  const uint64_t count = ParseUnsigned(Env("TIGONKV_E2E_WORKER_COUNT", "CXLKV_E2E_WORKER_COUNT", "1"), "worker count");
  const uint64_t id = ParseUnsigned(Env("TIGONKV_E2E_WORKER_ID", "CXLKV_E2E_WORKER_ID", std::to_string(node)), "worker id");
  if (count == 0 || id >= count) Fail("invalid barrier worker identity");
  std::filesystem::create_directories(dir);
  const std::string prefix = dir + "/" + phase + (final ? ".done." : ".ready.");
  { std::ofstream marker(prefix + std::to_string(id)); marker << "ready\n"; }
  const uint64_t timeout = ParseUnsigned(Env("TIGONKV_E2E_BARRIER_TIMEOUT_SEC", "CXLKV_E2E_BARRIER_TIMEOUT_SEC", "600"), "barrier timeout");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
  for (;;) {
    if (store != nullptr) {
      const Status status = store->PollTransport();
      if (!status.ok()) Fail("transport poll failed at barrier: " + status.message);
    }
    uint64_t seen = 0;
    for (uint64_t i = 0; i < count; ++i)
      if (std::filesystem::exists(prefix + std::to_string(i))) ++seen;
    if (seen == count) return;
    if (std::chrono::steady_clock::now() >= deadline)
      Fail("E2E barrier timeout for phase " + phase);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void DrainTransport(KVStore &store) {
  const uint64_t drain_ms = ParseUnsigned(
      Env("TIGONKV_E2E_TRANSPORT_DRAIN_MS", "CXLKV_E2E_TRANSPORT_DRAIN_MS", "5000"),
      "transport drain duration");
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(drain_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const Status status = store.PollTransport();
    if (!status.ok()) Fail("transport poll failed while draining: " + status.message);
    std::this_thread::yield();
  }
}

// Guest VMs do not share a filesystem, so the host orchestrator cannot use the
// file-marker Barrier directly across them.  Keep a VM's transport alive after
// its timed replay until the host observes that every peer has also finished.
void WaitForHostRelease(const std::string &phase, KVStore &store,
                        bool service_transport = true) {
  const std::string release_file =
      Env("TIGONKV_E2E_RELEASE_FILE", "CXLKV_E2E_RELEASE_FILE");
  if (release_file.empty()) return;
  {
    std::ofstream waiting(release_file + ".waiting");
    if (!waiting) Fail("cannot publish host-release wait marker");
    waiting << "waiting\n";
  }
  const uint64_t timeout = ParseUnsigned(
      Env("TIGONKV_E2E_RELEASE_TIMEOUT_SEC", "CXLKV_E2E_RELEASE_TIMEOUT_SEC", "600"),
      "host release timeout");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
  while (!std::filesystem::exists(release_file)) {
    if (service_transport) {
      const Status status = store.PollTransport();
      if (!status.ok())
        Fail("transport poll failed while waiting for host release: " + status.message);
    }
    if (std::chrono::steady_clock::now() >= deadline)
      Fail("host release timeout for phase " + phase);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

struct ReplayResult {
  uint64_t ops = 0;
  uint64_t scan_ops = 0;
  uint64_t scan_rows_returned = 0;
};

struct ScanExpectState {
  bool expect_nonempty = false;
  bool has_max_key = false;
  std::string max_key;
  std::mutex mutex;

  void NotePut(const std::string &key) {
    if (!expect_nonempty) return;
    std::lock_guard<std::mutex> lock(mutex);
    if (!has_max_key || key > max_key) {
      max_key = key;
      has_max_key = true;
    }
  }

  void CheckScan(const std::string &start_key, size_t limit, size_t rows,
                 uint64_t line_no) {
    if (!expect_nonempty || limit == 0 || rows != 0) return;
    std::lock_guard<std::mutex> lock(mutex);
    if (!has_max_key) return;
    if (start_key < max_key)
      Fail("SCAN returned 0 rows below known max key at line " +
           std::to_string(line_no));
  }
};

bool ScanExpectNonemptyEnabled(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--scan-expect-nonempty") return true;
    if (arg == "--no-scan-expect-nonempty") return false;
  }
  return Env("TIGONKV_E2E_SCAN_EXPECT_NONEMPTY",
             "CXLKV_E2E_SCAN_EXPECT_NONEMPTY", "0") == "1";
}

bool RequireGetFoundEnabled(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--require-get-found") return true;
    if (arg == "--allow-get-not-found") return false;
  }
  return Env("TIGONKV_E2E_REQUIRE_GET_FOUND",
             "CXLKV_E2E_REQUIRE_GET_FOUND", "0") == "1";
}

void SeedScanMaxKey(ScanExpectState *state) {
  const std::string configured =
      Env("TIGONKV_E2E_SCAN_MAX_KEY", "CXLKV_E2E_SCAN_MAX_KEY");
  if (configured.empty() || state == nullptr || !state->expect_nonempty) return;
  std::lock_guard<std::mutex> lock(state->mutex);
  state->max_key = configured;
  state->has_max_key = true;
}

void PrintScanRows(uint32_t node, uint64_t scan_ops, uint64_t rows) {
  std::cout << "E2E_SCAN_ROWS_RETURNED node=" << node
            << " scan_ops=" << scan_ops
            << " rows=" << rows << "\n";
}

ReplayResult ReplayTrace(KVStore &store, const std::string &trace, std::mt19937_64 *rng,
                         uint32_t fixed_key_size, uint32_t fixed_value_size,
                         ScanExpectState *scan_expect = nullptr,
                         std::atomic<uint64_t> *progress_ops = nullptr,
                         bool require_get_found = false) {
  constexpr uint64_t kProgressPublishBatch = 256;
  std::ifstream input(trace);
  if (!input) Fail("cannot open trace: " + trace);
  ReplayResult result;
  uint64_t unpublished_progress = 0;
  std::string line;
  uint64_t line_no = 0;
  while (std::getline(input, line)) {
    ++line_no;
    if (line.empty() || line[0] == '#') continue;
    tigonkv::e2e_trace::Operation operation;
    if (!tigonkv::e2e_trace::ParseLine(line, trace, line_no, &operation)) continue;
    const std::string_view op = operation.name;
    const uint64_t len = operation.length;
    const std::string key =
        tigonkv::e2e_trace::FixedTraceKey(operation.raw_key, fixed_key_size);
    Status status;
    if (op == "PUT") {
      const std::string value = FixedTraceValue(rng, fixed_value_size);
      (void)len;  // cxlkv ignores PUT LEN when synthesizing FixedTraceValue
      status = store.Put(key, value);
      if (status.ok() && scan_expect != nullptr) scan_expect->NotePut(key);
    } else if (op == "GET") {
      if (len != 0) Fail("GET LEN must be zero");
      const GetResult get = store.Get(key);
      status = get.status;
      const std::string test_value = TestValueOverride(fixed_value_size);
      if (!test_value.empty() && status.ok() && get.value != test_value)
        Fail("GET value mismatch at line " + std::to_string(line_no));
      if (!require_get_found && status.code == StatusCode::kNotFound)
        status = Status::Ok();
    } else if (op == "DELETE") {
      if (len != 0) Fail("DELETE LEN must be zero");
      status = store.Delete(key);
      if (status.code == StatusCode::kNotFound) status = Status::Ok();
    } else if (op == "SCAN") {
      ScanResult scan;
      scan = store.Scan(key, {}, len);
      status = scan.status;
      if (status.ok()) {
        ++result.scan_ops;
        result.scan_rows_returned += scan.items.size();
        if (len != 0 && scan.items.size() > len)
          Fail("SCAN returned more than requested at line " + std::to_string(line_no));
        for (size_t i = 0; i < scan.items.size(); ++i) {
          if (scan.items[i].key < key ||
              (i != 0 && scan.items[i - 1].key >= scan.items[i].key))
            Fail("SCAN result ordering mismatch at line " +
                 std::to_string(line_no));
        }
        if (scan_expect != nullptr)
          scan_expect->CheckScan(key, len, scan.items.size(), line_no);
      }
    } else {
      Fail("unknown operation at line " + std::to_string(line_no));
    }
    if (!status.ok()) Fail("operation failed at line " + std::to_string(line_no) + ": " + status.message);
    ++result.ops;
    if (progress_ops != nullptr &&
        ++unpublished_progress == kProgressPublishBatch) {
      progress_ops->fetch_add(unpublished_progress, std::memory_order_relaxed);
      unpublished_progress = 0;
    }
  }
  if (progress_ops != nullptr && unpublished_progress != 0)
    progress_ops->fetch_add(unpublished_progress, std::memory_order_relaxed);
  return result;
}

void PrintTraceTime(const std::string &phase, uint32_t node, uint64_t ops, uint64_t duration_us,
                    uint32_t trace_first, uint32_t trace_workers, uint32_t batch_ops) {
  std::cout << "E2E_TRACE_TIME_US phase=" << phase << " node=" << node
            << " ops=" << ops << " duration_us=" << duration_us
            << " trace_first=" << trace_first << " trace_workers=" << trace_workers
            << " batch_ops=" << batch_ops << "\n";
}

void PrintThreadTopology(uint32_t node, uint64_t foreground,
                         bool cpu_affinity) {
  std::cout << "E2E_THREAD_TOPOLOGY node=" << node
            << " foreground=" << foreground
            << " demuxer=1"
            << " kv_threads=" << (foreground + 1)
            << " affinity="
            << (cpu_affinity ? "distinct_allowed_cpus" : "scheduler")
            << "\n";
}

int RunMultiTrace(const Config &config, bool reset, const std::string &phase,
                  const std::string &trace_dir, uint64_t workers, uint32_t batch_ops,
                  uint64_t value_seed, ScanExpectState *scan_expect,
                  bool require_get_found) {
  const bool stage_markers =
      Env("TIGONKV_E2E_STAGE_MARKERS", "CXLKV_E2E_STAGE_MARKERS", "0") == "1";
  const bool legacy_progress =
      Env("TIGONKV_E2E_PROGRESS", "CXLKV_E2E_PROGRESS", "0") == "1";
  const uint64_t heartbeat_sec = ParseUnsigned(
      Env("TIGONKV_E2E_TRACE_HEARTBEAT_SEC",
          "CXLKV_E2E_TRACE_HEARTBEAT_SEC",
          legacy_progress ? "5" : "0"),
      "heartbeat seconds");
  const bool heartbeat = heartbeat_sec != 0;
  const auto log_stage = [&](const char *stage) {
    if (stage_markers) std::cerr << "E2E_TRACE_STAGE node=" << config.node_id
                                 << " phase=" << phase << " stage=" << stage << "\n"
                                 << std::flush;
  };
  const uint32_t trace_first = static_cast<uint32_t>(ParseUnsigned(
      Env("TIGONKV_E2E_TRACE_FIRST", "CXLKV_E2E_TRACE_FIRST",
          std::to_string(static_cast<uint64_t>(config.node_id) * workers)),
      "trace first"));
  std::vector<std::string> traces;
  traces.reserve(workers);
  for (uint64_t worker = 0; worker < workers; ++worker) {
    const std::string trace = trace_dir + "/worker" + std::to_string(worker) + ".txt";
    if (!std::filesystem::exists(trace)) Fail("missing worker trace: " + trace);
    traces.push_back(trace);
  }
  auto store = KVStore::Create(config, reset);
  log_stage("opened");
  Barrier(phase, config.node_id, false);
  log_stage("barrier_ready");
  std::vector<ReplayResult> results(workers);
  std::vector<std::thread> threads;
  std::mutex error_mutex;
  std::exception_ptr error;
  std::atomic<uint64_t> progress_ops{0};
  std::atomic<uint64_t> ready_workers{0};
  std::atomic<uint64_t> completed_workers{0};
  std::atomic<bool> replay_start{false};
  std::atomic<bool> replay_done{false};
  std::atomic<bool> workers_release{false};
  std::vector<std::chrono::steady_clock::time_point> worker_end(workers);
  for (uint64_t worker = 0; worker < workers; ++worker) {
    threads.emplace_back([&, worker] {
      bool ready_published = false;
      try {
        store->BindWorker(static_cast<uint32_t>(worker));
        std::mt19937_64 rng(value_seed ^ (static_cast<uint64_t>(trace_first + worker) << 32) ^
                            worker);
        ready_workers.fetch_add(1, std::memory_order_release);
        ready_published = true;
        while (!replay_start.load(std::memory_order_acquire))
          std::this_thread::yield();
        results[worker] = ReplayTrace(*store, traces[worker], &rng, config.fixed_key_size,
                                      config.fixed_value_size, scan_expect,
                                      heartbeat ? &progress_ops : nullptr,
                                      require_get_found);
        worker_end[worker] = std::chrono::steady_clock::now();
        if (completed_workers.fetch_add(1, std::memory_order_acq_rel) + 1 ==
            workers)
          replay_done.store(true, std::memory_order_release);
        // Match the original Executor lifetime: a finished foreground worker
        // still serves only its own Message queue until every VM has replayed.
        // The orchestration thread must not poll worker 0 concurrently.
        while (!workers_release.load(std::memory_order_acquire)) {
          const Status status = store->PollTransport();
          if (!status.ok())
            Fail("transport poll failed while serving peer: " + status.message);
          std::this_thread::yield();
        }
        store->ReleaseWorker();
      } catch (...) {
        if (!ready_published)
          ready_workers.fetch_add(1, std::memory_order_release);
        {
          std::lock_guard<std::mutex> lock(error_mutex);
          if (!error) error = std::current_exception();
          if (completed_workers.fetch_add(1, std::memory_order_acq_rel) + 1 ==
              workers)
            replay_done.store(true, std::memory_order_release);
        }
        // A replay error must not make this worker abandon its original SPSC
        // inbox while peers are still awaiting replies.  The orchestrator
        // observes replay_done, releases every worker, and then rethrows the
        // recorded error on its control thread.
        if (ready_published) {
          while (!workers_release.load(std::memory_order_acquire)) {
            const Status status = store->PollTransport();
            if (!status.ok()) break;
            std::this_thread::yield();
          }
          store->ReleaseWorker();
        }
      }
    });
  }
  while (ready_workers.load(std::memory_order_acquire) != workers)
    std::this_thread::yield();
  const auto start = std::chrono::steady_clock::now();
  replay_start.store(true, std::memory_order_release);
  // Reuse the existing orchestration thread for low-frequency progress. A
  // dedicated heartbeat thread would be an unreported CPU resource.
  uint64_t last_progress = 0;
  auto last_print = start;
  while (heartbeat &&
         !replay_done.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto now = std::chrono::steady_clock::now();
    if (now - last_print < std::chrono::seconds(heartbeat_sec)) continue;
    last_print = now;
    const uint64_t current =
        progress_ops.load(std::memory_order_relaxed);
    const auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(
        now - start).count();
    std::cerr << "E2E_TRACE_HEARTBEAT phase=" << phase
              << " node=" << config.node_id
              << " ops=" << (current - last_progress)
              << " total=" << current
              << " elapsed_s=" << elapsed_s << "\n"
              << std::flush;
    last_progress = current;
  }
  {
    std::lock_guard<std::mutex> lock(error_mutex);
    if (error) {
      workers_release.store(true, std::memory_order_release);
      for (auto &thread : threads) thread.join();
      std::rethrow_exception(error);
    }
  }
  log_stage("replay_done");
  const auto end = *std::max_element(worker_end.begin(), worker_end.end());
  const auto duration_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      end - start).count());
  WaitForHostRelease(phase, *store, /*service_transport=*/false);
  log_stage("release_done");
  // The original foreground workers remain the only transport consumers.  Do
  // not release them and then make this unbound orchestration thread poll a
  // worker-local inbox: KVStore deliberately hard-fails that misuse.  While
  // this final barrier waits, the still-bound workers continue serving their
  // own queues exactly as they did through host release.
  Barrier(phase, config.node_id, true);
  log_stage("barrier_done");
  workers_release.store(true, std::memory_order_release);
  for (auto &thread : threads) thread.join();
  uint64_t ops = 0;
  uint64_t scan_ops = 0;
  uint64_t scan_rows = 0;
  for (const auto &result : results) {
    ops += result.ops;
    scan_ops += result.scan_ops;
    scan_rows += result.scan_rows_returned;
  }
  if (scan_expect != nullptr && scan_expect->expect_nonempty && scan_ops != 0 &&
      scan_rows == 0)
    Fail("SCAN expect-nonempty: all " + std::to_string(scan_ops) +
         " scan ops returned 0 rows");
  // Prefer engine TLS aggregate when present; runner counter is the contract
  // line for cross-node summarizers even if DumpStats is truncated.
  const uint64_t store_scan_rows = store->Runtime().scan_rows_returned;
  if (store_scan_rows != scan_rows)
    Fail("scan_rows_returned mismatch runner=" + std::to_string(scan_rows) +
         " store=" + std::to_string(store_scan_rows));
  PrintThreadTopology(config.node_id, workers, config.cpu_affinity);
  PrintTraceTime(phase, config.node_id, ops, duration_us, trace_first,
                 static_cast<uint32_t>(workers), batch_ops);
  PrintScanRows(config.node_id, scan_ops, scan_rows);
  std::cout << store->DumpStats();
  std::cout << "e2e_trace_runner[node" << config.node_id << "]: passed.\n";
  return 0;
}
}  // namespace

int main(int argc, char **argv) {
  std::unique_ptr<KVStore> store;
  std::string trace;
  std::string phase;
  std::string last_op;
  std::string last_key;
  uint32_t node = 0;
  uint64_t line_no = 0;
  uint64_t ops = 0;
  try {
    const std::string experiment = Env("TIGONKV_EXPERIMENT_CONFIG_JSONC", "CXLKV_EXPERIMENT_CONFIG_JSONC", "experiment_config.jsonc");
    Config config = Config::FromJsonc(experiment);
    config.node_id = static_cast<uint32_t>(ParseUnsigned(Env("TIGONKV_NODE_ID", "CXLKV_NODE_ID", "0"), "node id"));
    node = config.node_id;
    if (config.hardware_simulation.fixed_latency.enabled &&
        Env("TIGONKV_E2E_VERBOSE", "CXLKV_E2E_VERBOSE", "0") == "1")
      Fail("latency_inject.enabled=true requires TIGONKV_E2E_VERBOSE=0");
    const std::string trace_config = Env("TIGONKV_E2E_TRACE_CONFIG_JSONC", "CXLKV_E2E_TRACE_CONFIG_JSONC");
    if (!trace_config.empty()) {
      std::ifstream config_file(trace_config);
      if (!config_file) Fail("cannot open trace config: " + trace_config);
    }
    const std::string policy_config = Env("TIGONKV_POLICY_CONFIG_JSON", "CXLKV_POLICY_CONFIG_JSON");
    if (!policy_config.empty())
      Fail("TIGONKV_POLICY_CONFIG_JSON/CXLKV_POLICY_CONFIG_JSON is unsupported; "
           "put the complete latency policy in "
           "tigon_kv.latency_inject inside TIGONKV_EXPERIMENT_CONFIG_JSONC");
    const bool reset = Env("TIGONKV_E2E_RESET", "CXLKV_E2E_RESET", "0") == "1";
    phase = Env("TIGONKV_E2E_TRACE_PHASE", "CXLKV_E2E_TRACE_PHASE", "run");
    const uint32_t batch_ops = static_cast<uint32_t>(ParseUnsigned(
        Env("TIGONKV_E2E_TRACE_BATCH_OPS", "CXLKV_E2E_TRACE_BATCH_OPS", "4096"), "batch_ops"));
    if (batch_ops == 0) Fail("batch_ops must be >= 1");
    const uint64_t value_seed = ParseUnsigned(
        Env("TIGONKV_E2E_TRACE_VALUE_SEED", "CXLKV_E2E_TRACE_VALUE_SEED", "1"), "value seed");
    const uint64_t trace_workers = ParseUnsigned(
        Env("TIGONKV_E2E_TRACE_WORKERS", "CXLKV_E2E_TRACE_WORKERS", "1"), "trace workers");
    const std::string trace_dir = Env("TIGONKV_E2E_TRACE_DIR", "CXLKV_E2E_TRACE_DIR");
    ScanExpectState scan_expect;
    scan_expect.expect_nonempty = ScanExpectNonemptyEnabled(argc, argv);
    SeedScanMaxKey(&scan_expect);
    const bool require_get_found = RequireGetFoundEnabled(argc, argv);
    // Guest YCSB uses TRACE_DIR even for 1 worker/VM; only fall back to a
    // single TRACE_FILE when no directory is provided.
    if (!trace_dir.empty()) {
      if (trace_workers == 0) Fail("TIGONKV_E2E_TRACE_WORKERS must be >= 1");
      return RunMultiTrace(config, reset, phase, trace_dir, trace_workers, batch_ops,
                           value_seed, &scan_expect, require_get_found);
    }
    if (trace_workers > 1)
      Fail("TIGONKV_E2E_TRACE_DIR is required for multi-worker replay");
    trace = Env("TIGONKV_E2E_TRACE_FILE", "CXLKV_E2E_TRACE_FILE");
    if (trace.empty()) Fail("TIGONKV_E2E_TRACE_FILE is required for direct trace replay");
    const uint32_t trace_first = static_cast<uint32_t>(ParseUnsigned(
        Env("TIGONKV_E2E_TRACE_FIRST", "CXLKV_E2E_TRACE_FIRST", "0"), "trace first"));
    store = KVStore::Create(config, reset);
    store->BindWorker(0);
    std::ifstream input(trace);
    if (!input) Fail("cannot open trace: " + trace);
    Barrier(phase, config.node_id, false);
    std::mt19937_64 rng(value_seed ^ (static_cast<uint64_t>(trace_first) << 32));
    auto start = std::chrono::steady_clock::now();
    std::string line;
    uint64_t scan_ops = 0;
    uint64_t scan_rows = 0;
    while (std::getline(input, line)) {
      ++line_no;
      if (line.empty() || line[0] == '#') continue;
      tigonkv::e2e_trace::Operation operation;
      if (!tigonkv::e2e_trace::ParseLine(line, trace, line_no, &operation)) continue;
      const std::string_view op = operation.name;
      const uint64_t len = operation.length;
      last_op.assign(op);
      last_key.assign(operation.raw_key);
      const std::string key = tigonkv::e2e_trace::FixedTraceKey(
          operation.raw_key, config.fixed_key_size);
      Status status;
      if (op == "PUT") {
        const std::string value = FixedTraceValue(&rng, config.fixed_value_size);
        (void)len;
        status = store->Put(key, value);
        if (status.ok()) scan_expect.NotePut(key);
      } else if (op == "GET") {
        if (len != 0) Fail("GET LEN must be zero");
        const GetResult get = store->Get(key);
        status = get.status;
        const std::string test_value = TestValueOverride(config.fixed_value_size);
        if (!test_value.empty() && status.ok() && get.value != test_value)
          Fail("GET value mismatch at line " + std::to_string(line_no));
        if (!require_get_found && status.code == StatusCode::kNotFound)
          status = Status::Ok();
      } else if (op == "DELETE") {
        if (len != 0) Fail("DELETE LEN must be zero");
        status = store->Delete(key);
        if (status.code == StatusCode::kNotFound) status = Status::Ok();
      } else if (op == "SCAN") {
        ScanResult result;
        result = store->Scan(key, {}, len);
        status = result.status;
        if (status.ok()) {
          ++scan_ops;
          scan_rows += result.items.size();
          if (len != 0 && result.items.size() > len)
            Fail("SCAN returned more than requested at line " + std::to_string(line_no));
          for (size_t i = 0; i < result.items.size(); ++i) {
            if (result.items[i].key < key || (i != 0 && result.items[i - 1].key >= result.items[i].key))
              Fail("SCAN result ordering mismatch at line " + std::to_string(line_no));
          }
          scan_expect.CheckScan(key, len, result.items.size(), line_no);
        }
      } else {
        Fail("unknown operation at line " + std::to_string(line_no));
      }
      if (!status.ok()) Fail("operation failed at line " + std::to_string(line_no) + ": " + status.message);
      ++ops;
    }
    if (scan_expect.expect_nonempty && scan_ops != 0 && scan_rows == 0)
      Fail("SCAN expect-nonempty: all " + std::to_string(scan_ops) +
           " scan ops returned 0 rows");
    const uint64_t store_scan_rows = store->Runtime().scan_rows_returned;
    if (store_scan_rows != scan_rows)
      Fail("scan_rows_returned mismatch runner=" + std::to_string(scan_rows) +
           " store=" + std::to_string(store_scan_rows));
    const auto duration_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count());
    WaitForHostRelease(phase, *store);
    DrainTransport(*store);
    Barrier(phase, config.node_id, true, store.get());
    PrintThreadTopology(config.node_id, 1, config.cpu_affinity);
    PrintTraceTime(phase, config.node_id, ops, duration_us, trace_first, 1, batch_ops);
    PrintScanRows(config.node_id, scan_ops, scan_rows);
    std::cout << store->DumpStats();
    std::cout << "e2e_trace_runner[node" << config.node_id << "]: passed.\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "e2e_trace_runner: hard failure: " << e.what() << "\n";
    std::cerr << "E2E_TRACE_FAILURE\n"
              << "node=" << node << "\n"
              << "trace=" << trace << "\n"
              << "phase=" << phase << "\n"
              << "line=" << line_no << "\n"
              << "op=" << last_op << "\n"
              << "key=" << last_key << "\n"
              << "ops_completed=" << ops << "\n";
    if (store) {
      try {
        std::cerr << "partition=" << store->StablePartitionForKey(last_key)
                  << " owner=" << store->OwnerForKey(last_key) << "\n";
        std::cerr << store->DumpStats();
      } catch (const std::exception &diagnostic_error) {
        std::cerr << "diagnostic_error=" << diagnostic_error.what() << "\n";
      }
    }
    return 2;
  }
}
