#include "kv/kv_store.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

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
}

int main() {
  try {
    const std::string experiment = Env("TIGONKV_EXPERIMENT_CONFIG_JSONC", "CXLKV_EXPERIMENT_CONFIG_JSONC", "experiment_config.jsonc");
    Config config = Config::FromJsonc(experiment);
    config.node_id = static_cast<uint32_t>(ParseUnsigned(Env("TIGONKV_NODE_ID", "CXLKV_NODE_ID", "0"), "node id"));
    const std::string trace_config = Env("TIGONKV_E2E_TRACE_CONFIG_JSONC", "CXLKV_E2E_TRACE_CONFIG_JSONC");
    if (!trace_config.empty()) {
      std::ifstream config_file(trace_config);
      if (!config_file) Fail("cannot open trace config: " + trace_config);
    }
    const std::string policy_config = Env("TIGONKV_POLICY_CONFIG_JSON", "CXLKV_POLICY_CONFIG_JSON");
    (void)policy_config;  // policy is intentionally independent of the KV API.
    const std::string trace = Env("TIGONKV_E2E_TRACE_FILE", "CXLKV_E2E_TRACE_FILE");
    if (trace.empty()) Fail("TIGONKV_E2E_TRACE_FILE is required for direct trace replay");
    auto store = KVStore::Create(config, false);
    std::ifstream input(trace);
    if (!input) Fail("cannot open trace: " + trace);
    uint64_t ops = 0, line_no = 0;
    auto start = std::chrono::steady_clock::now();
    std::string line;
    while (std::getline(input, line)) {
      ++line_no;
      if (line.empty() || line[0] == '#') continue;
      std::istringstream header(line);
      std::string op, key_len_text, len_text;
      if (!(header >> op >> key_len_text >> len_text)) Fail(trace + ": malformed line " + std::to_string(line_no));
      const size_t key_len = ParseUnsigned(key_len_text, "key length");
      const size_t len = ParseUnsigned(len_text, "operation length");
      size_t prefix = line.find(len_text);
      prefix = line.find_first_not_of(" \t", prefix + len_text.size());
      if (prefix == std::string::npos || line.size() - prefix != key_len) Fail(trace + ": key length mismatch at line " + std::to_string(line_no));
      const std::string key = line.substr(prefix, key_len);
      Status status;
      if (op == "PUT") status = store->Put(key, std::string(len, 'x'));
      else if (op == "GET") { if (len != 0) Fail("GET LEN must be zero"); status = store->Get(key).status; if (status.code == StatusCode::kNotFound) status = Status::Ok(); }
      else if (op == "DELETE") { if (len != 0) Fail("DELETE LEN must be zero"); status = store->Delete(key); if (status.code == StatusCode::kNotFound) status = Status::Ok(); }
      else if (op == "SCAN") status = store->Scan(key, len).status;
      else Fail("unknown operation at line " + std::to_string(line_no));
      if (!status.ok()) Fail("operation failed at line " + std::to_string(line_no) + ": " + status.message);
      ++ops;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    const std::string heartbeat = Env("TIGONKV_E2E_TRACE_HEARTBEAT_SEC", "CXLKV_E2E_TRACE_HEARTBEAT_SEC", "0");
    if (heartbeat != "0") std::cout << "E2E_TRACE_HEARTBEAT phase=" << Env("TIGONKV_E2E_TRACE_PHASE", "CXLKV_E2E_TRACE_PHASE", "run") << " node=" << config.node_id << " ops=" << ops << "\n";
    std::cout << "E2E_TRACE_TIME_US phase=" << Env("TIGONKV_E2E_TRACE_PHASE", "CXLKV_E2E_TRACE_PHASE", "run") << " node=" << config.node_id << " ops=" << ops << " elapsed_us=" << elapsed << "\n";
    std::cout << store->DumpStats();
    std::cout << "e2e_trace_runner[node" << config.node_id << "]: passed.\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "e2e_trace_runner: hard failure: " << e.what() << "\n";
    return 2;
  }
}
