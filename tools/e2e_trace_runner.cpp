#include "kv/kv_store.h"

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
uint64_t ReadDecimal(const std::string &line, size_t *pos, const std::string &label) {
  const size_t begin = *pos;
  while (*pos < line.size() && line[*pos] >= '0' && line[*pos] <= '9') ++*pos;
  if (begin == *pos) Fail("missing " + label);
  return ParseUnsigned(line.substr(begin, *pos - begin), label);
}

// Align with cxlkv FixedTraceKey: right-pad spaces to fixed_key_size.
std::string FixedTraceKey(const std::string &key, uint32_t fixed_key_size) {
  if (key.size() > fixed_key_size) {
    Fail("trace key exceeds fixed_key_size: size=" + std::to_string(key.size()) +
         " fixed_key_size=" + std::to_string(fixed_key_size));
  }
  std::string out = key;
  out.resize(static_cast<size_t>(fixed_key_size), ' ');
  return out;
}

// Align with cxlkv FixedTraceValue: printable '!'..'~', length=fixed_value_size
// (trace PUT LEN is ignored for the payload, as in cxlkv).
std::string FixedTraceValue(std::mt19937_64 *rng, uint32_t fixed_value_size) {
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
void WaitForHostRelease(const std::string &phase, KVStore &store) {
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
    const Status status = store.PollTransport();
    if (!status.ok())
      Fail("transport poll failed while waiting for host release: " + status.message);
    if (std::chrono::steady_clock::now() >= deadline)
      Fail("host release timeout for phase " + phase);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

struct ReplayResult {
  uint64_t ops = 0;
};

ReplayResult ReplayTrace(KVStore &store, const std::string &trace, std::mt19937_64 *rng,
                         uint32_t fixed_key_size, uint32_t fixed_value_size,
                         std::atomic<uint64_t> *progress_ops = nullptr) {
  std::ifstream input(trace);
  if (!input) Fail("cannot open trace: " + trace);
  ReplayResult result;
  std::string line;
  uint64_t line_no = 0;
  while (std::getline(input, line)) {
    ++line_no;
    if (line.empty() || line[0] == '#') continue;
    size_t pos = 0;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    const size_t op_begin = pos;
    while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') ++pos;
    if (op_begin == pos) Fail(trace + ": malformed line " + std::to_string(line_no));
    const std::string op = line.substr(op_begin, pos - op_begin);
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    const size_t key_len = ReadDecimal(line, &pos, "key length");
    if (pos >= line.size() || (line[pos] != ' ' && line[pos] != '\t'))
      Fail(trace + ": missing LEN separator at line " + std::to_string(line_no));
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    const size_t len = ReadDecimal(line, &pos, "operation length");
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    if (line.size() - pos < key_len) Fail(trace + ": key length mismatch at line " + std::to_string(line_no));
    const std::string raw_key = line.substr(pos, key_len);
    for (size_t tail = pos + key_len; tail < line.size(); ++tail)
      if (line[tail] != ' ' && line[tail] != '\t')
        Fail(trace + ": trailing bytes after key at line " + std::to_string(line_no));
    const std::string key = FixedTraceKey(raw_key, fixed_key_size);
    Status status;
    if (op == "PUT") {
      status = store.Put(key, FixedTraceValue(rng, fixed_value_size));
      (void)len;  // cxlkv ignores PUT LEN when synthesizing FixedTraceValue
    } else if (op == "GET") {
      if (len != 0) Fail("GET LEN must be zero");
      status = store.Get(key).status;
      if (status.code == StatusCode::kNotFound) status = Status::Ok();
    } else if (op == "DELETE") {
      if (len != 0) Fail("DELETE LEN must be zero");
      status = store.Delete(key);
      if (status.code == StatusCode::kNotFound) status = Status::Ok();
    } else if (op == "SCAN") {
      ScanResult scan = store.Scan(key, len);
      status = scan.status;
      if (status.ok() && len != 0 && scan.items.size() > len)
        Fail("SCAN returned more than requested at line " + std::to_string(line_no));
    } else {
      Fail("unknown operation at line " + std::to_string(line_no));
    }
    if (!status.ok()) Fail("operation failed at line " + std::to_string(line_no) + ": " + status.message);
    ++result.ops;
    if (progress_ops != nullptr) progress_ops->fetch_add(1, std::memory_order_relaxed);
  }
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
                  uint64_t value_seed) {
  const bool stage_markers =
      Env("TIGONKV_E2E_STAGE_MARKERS", "CXLKV_E2E_STAGE_MARKERS", "0") == "1";
  const bool progress =
      Env("TIGONKV_E2E_PROGRESS", "CXLKV_E2E_PROGRESS", "0") == "1";
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
  std::atomic<bool> replay_done{false};
  // Heartbeat so host orchestration can fail-fast on livelock instead of
  // mistaking a long SCAN-heavy run for a hang (YCSB-E ~55s/node is normal).
  std::thread progress_thread;
  if (progress) {
    progress_thread = std::thread([&] {
      uint64_t last = 0;
      const auto heartbeat_start = std::chrono::steady_clock::now();
      auto last_print = std::chrono::steady_clock::now();
      while (!replay_done.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto now = std::chrono::steady_clock::now();
        if (now - last_print < std::chrono::seconds(5)) continue;
        last_print = now;
        const uint64_t cur = progress_ops.load(std::memory_order_relaxed);
        const auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(
            now - heartbeat_start).count();
        std::cerr << "E2E_TRACE_HEARTBEAT node=" << config.node_id << " phase=" << phase
                  << " ops=" << (cur - last) << " total=" << cur
                  << " elapsed_s=" << elapsed_s << "\n"
                  << std::flush;
        last = cur;
      }
    });
  }
  const auto start = std::chrono::steady_clock::now();
  for (uint64_t worker = 0; worker < workers; ++worker) {
    threads.emplace_back([&, worker] {
      try {
        store->BindWorker(static_cast<uint32_t>(worker));
        std::mt19937_64 rng(value_seed ^ (static_cast<uint64_t>(trace_first + worker) << 32) ^
                            worker);
        results[worker] = ReplayTrace(*store, traces[worker], &rng, config.fixed_key_size,
                                      config.fixed_value_size, &progress_ops);
        store->ReleaseWorker();
      } catch (...) {
        try {
          store->ReleaseWorker();
        } catch (...) {
        }
        std::lock_guard<std::mutex> lock(error_mutex);
        if (!error) error = std::current_exception();
      }
    });
  }
  for (auto &thread : threads) thread.join();
  replay_done.store(true, std::memory_order_release);
  if (progress_thread.joinable()) progress_thread.join();
  if (error) std::rethrow_exception(error);
  log_stage("replay_done");
  const auto duration_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start).count());
  WaitForHostRelease(phase, *store);
  log_stage("release_done");
  DrainTransport(*store);
  log_stage("drain_done");
  Barrier(phase, config.node_id, true, store.get());
  log_stage("barrier_done");
  if (!store->Checkpoint().ok()) Fail("checkpoint failed after final barrier");
  log_stage("checkpoint_done");
  uint64_t ops = 0;
  for (const auto &result : results) ops += result.ops;
  PrintThreadTopology(config.node_id, workers, config.cpu_affinity);
  PrintTraceTime(phase, config.node_id, ops, duration_us, trace_first,
                 static_cast<uint32_t>(workers), batch_ops);
  std::cout << store->DumpStats();
  std::cout << "e2e_trace_runner[node" << config.node_id << "]: passed.\n";
  return 0;
}
}  // namespace

int main() {
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
    if (config.latency_enabled &&
        Env("TIGONKV_E2E_VERBOSE", "CXLKV_E2E_VERBOSE", "0") == "1")
      Fail("latency_inject.enabled=true requires TIGONKV_E2E_VERBOSE=0");
    const std::string trace_config = Env("TIGONKV_E2E_TRACE_CONFIG_JSONC", "CXLKV_E2E_TRACE_CONFIG_JSONC");
    if (!trace_config.empty()) {
      std::ifstream config_file(trace_config);
      if (!config_file) Fail("cannot open trace config: " + trace_config);
    }
    const std::string policy_config = Env("TIGONKV_POLICY_CONFIG_JSON", "CXLKV_POLICY_CONFIG_JSON");
    (void)policy_config;
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
    // Guest YCSB uses TRACE_DIR even for 1 worker/VM; only fall back to a
    // single TRACE_FILE when no directory is provided.
    if (!trace_dir.empty()) {
      if (trace_workers == 0) Fail("TIGONKV_E2E_TRACE_WORKERS must be >= 1");
      return RunMultiTrace(config, reset, phase, trace_dir, trace_workers, batch_ops, value_seed);
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
    while (std::getline(input, line)) {
      ++line_no;
      if (line.empty() || line[0] == '#') continue;
      size_t pos = 0;
      while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
      const size_t op_begin = pos;
      while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') ++pos;
      if (op_begin == pos) Fail(trace + ": malformed line " + std::to_string(line_no));
      const std::string op = line.substr(op_begin, pos - op_begin);
      last_op = op;
      while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
      const size_t key_len = ReadDecimal(line, &pos, "key length");
      if (pos >= line.size() || (line[pos] != ' ' && line[pos] != '\t')) Fail(trace + ": missing LEN separator at line " + std::to_string(line_no));
      while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
      const size_t len = ReadDecimal(line, &pos, "operation length");
      while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
      if (pos > line.size() || line.size() - pos < key_len) Fail(trace + ": key length mismatch at line " + std::to_string(line_no));
      const std::string raw_key = line.substr(pos, key_len);
      last_key = raw_key;
      for (size_t tail = pos + key_len; tail < line.size(); ++tail)
        if (line[tail] != ' ' && line[tail] != '\t') Fail(trace + ": trailing bytes after key at line " + std::to_string(line_no));
      const std::string key = FixedTraceKey(raw_key, config.fixed_key_size);
      Status status;
      if (op == "PUT") {
        status = store->Put(key, FixedTraceValue(&rng, config.fixed_value_size));
        (void)len;
      } else if (op == "GET") {
        if (len != 0) Fail("GET LEN must be zero");
        status = store->Get(key).status;
        if (status.code == StatusCode::kNotFound) status = Status::Ok();
      } else if (op == "DELETE") {
        if (len != 0) Fail("DELETE LEN must be zero");
        status = store->Delete(key);
        if (status.code == StatusCode::kNotFound) status = Status::Ok();
      } else if (op == "SCAN") {
        ScanResult result = store->Scan(key, len);
        status = result.status;
        if (status.ok()) {
          if (len != 0 && result.items.size() > len)
            Fail("SCAN returned more than requested at line " + std::to_string(line_no));
          for (size_t i = 0; i < result.items.size(); ++i) {
            if (result.items[i].key < key || (i != 0 && result.items[i - 1].key >= result.items[i].key))
              Fail("SCAN result ordering mismatch at line " + std::to_string(line_no));
          }
        }
      } else {
        Fail("unknown operation at line " + std::to_string(line_no));
      }
      if (!status.ok()) Fail("operation failed at line " + std::to_string(line_no) + ": " + status.message);
      ++ops;
    }
    const auto duration_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count());
    WaitForHostRelease(phase, *store);
    DrainTransport(*store);
    Barrier(phase, config.node_id, true, store.get());
    if (!store->Checkpoint().ok()) Fail("checkpoint failed after final barrier");
    const std::string heartbeat = Env("TIGONKV_E2E_TRACE_HEARTBEAT_SEC", "CXLKV_E2E_TRACE_HEARTBEAT_SEC", "0");
    if (heartbeat != "0")
      std::cout << "E2E_TRACE_HEARTBEAT phase=" << phase << " node=" << config.node_id
                << " ops=" << ops << " total=" << ops << " elapsed_s=0\n";
    PrintThreadTopology(config.node_id, 1, config.cpu_affinity);
    PrintTraceTime(phase, config.node_id, ops, duration_us, trace_first, 1, batch_ops);
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
