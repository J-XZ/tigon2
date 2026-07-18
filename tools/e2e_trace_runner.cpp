#include "kv/kv_store.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

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

void Barrier(const std::string &phase, uint32_t node, bool final) {
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
    uint64_t seen = 0;
    for (uint64_t i = 0; i < count; ++i)
      if (std::filesystem::exists(prefix + std::to_string(i))) ++seen;
    if (seen == count) return;
    if (std::chrono::steady_clock::now() >= deadline)
      Fail("E2E barrier timeout for phase " + phase);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
}

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
    const std::string trace_config = Env("TIGONKV_E2E_TRACE_CONFIG_JSONC", "CXLKV_E2E_TRACE_CONFIG_JSONC");
    if (!trace_config.empty()) {
      std::ifstream config_file(trace_config);
      if (!config_file) Fail("cannot open trace config: " + trace_config);
    }
    const std::string policy_config = Env("TIGONKV_POLICY_CONFIG_JSON", "CXLKV_POLICY_CONFIG_JSON");
    (void)policy_config;  // policy is intentionally independent of the KV API.
    trace = Env("TIGONKV_E2E_TRACE_FILE", "CXLKV_E2E_TRACE_FILE");
    if (trace.empty()) Fail("TIGONKV_E2E_TRACE_FILE is required for direct trace replay");
    const bool reset = Env("TIGONKV_E2E_RESET", "CXLKV_E2E_RESET", "0") == "1";
    store = KVStore::Create(config, reset);
    std::ifstream input(trace);
    if (!input) Fail("cannot open trace: " + trace);
    phase = Env("TIGONKV_E2E_TRACE_PHASE", "CXLKV_E2E_TRACE_PHASE", "run");
    Barrier(phase, config.node_id, false);
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
      // cxlkv's format starts KEY immediately after LEN; accept one or more
      // spaces as well for hand-written traces.
      while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
      if (pos > line.size() || line.size() - pos < key_len) Fail(trace + ": key length mismatch at line " + std::to_string(line_no));
      const std::string key = line.substr(pos, key_len);
      last_key = key;
      for (size_t tail = pos + key_len; tail < line.size(); ++tail)
        if (line[tail] != ' ' && line[tail] != '\t') Fail(trace + ": trailing bytes after key at line " + std::to_string(line_no));
      Status status;
      if (op == "PUT") status = store->Put(key, std::string(len, 'x'));
      else if (op == "GET") { if (len != 0) Fail("GET LEN must be zero"); status = store->Get(key).status; if (status.code == StatusCode::kNotFound) status = Status::Ok(); }
      else if (op == "DELETE") { if (len != 0) Fail("DELETE LEN must be zero"); status = store->Delete(key); if (status.code == StatusCode::kNotFound) status = Status::Ok(); }
      else if (op == "SCAN") {
        ScanResult result = store->Scan(key, len);
        status = result.status;
        if (status.ok()) {
          if (result.items.size() > len) Fail("SCAN returned more than requested at line " + std::to_string(line_no));
          for (size_t i = 0; i < result.items.size(); ++i) {
            if (result.items[i].key < key || (i != 0 && result.items[i - 1].key >= result.items[i].key))
              Fail("SCAN result ordering mismatch at line " + std::to_string(line_no));
          }
        }
      }
      else Fail("unknown operation at line " + std::to_string(line_no));
      if (!status.ok()) Fail("operation failed at line " + std::to_string(line_no) + ": " + status.message);
      ++ops;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    if (!store->Checkpoint().ok()) Fail("checkpoint failed before final barrier");
    Barrier(phase, config.node_id, true);
    const std::string heartbeat = Env("TIGONKV_E2E_TRACE_HEARTBEAT_SEC", "CXLKV_E2E_TRACE_HEARTBEAT_SEC", "0");
    if (heartbeat != "0") std::cout << "E2E_TRACE_HEARTBEAT phase=" << phase << " node=" << config.node_id << " ops=" << ops << "\n";
    std::cout << "E2E_TRACE_TIME_US phase=" << phase << " node=" << config.node_id << " ops=" << ops << " elapsed_us=" << elapsed << "\n";
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
