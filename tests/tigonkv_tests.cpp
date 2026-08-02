#include "kv/kv_store.h"
#include "kv/engine/latency_inject.h"
#include "kv/engine/kv_types_layout.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace tigonkv;

namespace {

bool RelWithDebInfoBuild() {
#ifdef TIGONKV_CMAKE_BUILD_TYPE
  return std::string_view(TIGONKV_CMAKE_BUILD_TYPE) == "RelWithDebInfo";
#else
  return false;
#endif
}

bool ValidateThrows(Config config) {
  try {
    config.Validate();
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

std::string Fixed(std::string_view value, uint32_t width) {
  assert(value.size() <= width);
  std::string fixed(value);
  fixed.resize(width, '\0');
  return fixed;
}

std::string MaxKey(uint32_t width) {
  return std::string(width, static_cast<char>(0xff));
}

void SetTestRangePartitioning(Config *config) {
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

std::string ReplaceOnce(std::string text, const std::string &from,
                        const std::string &to) {
  const size_t position = text.find(from);
  assert(position != std::string::npos);
  text.replace(position, from.size(), to);
  return text;
}

bool ParseTextThrows(const std::string &path, const std::string &text) {
  {
    std::ofstream output(path);
    assert(output.good());
    output << text;
  }
  try {
    (void)Config::FromJsonc(path);
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

std::string RemoveJsonFieldLine(std::string text, std::string_view field) {
  const std::string needle = "\"" + std::string(field) + "\"";
  const size_t position = text.find(needle);
  assert(position != std::string::npos);
  const size_t line_begin = text.rfind('\n', position) + 1;
  size_t line_end = text.find('\n', position);
  if (line_end == std::string::npos) line_end = text.size();
  else ++line_end;
  const std::string_view line(text.data() + line_begin, line_end - line_begin);
  if (line.find(',') != std::string_view::npos) {
    text.erase(line_begin, line_end - line_begin);
  } else {
    const size_t previous_comma = text.rfind(',', line_begin);
    assert(previous_comma != std::string::npos);
    text.erase(previous_comma, line_end - previous_comma);
  }
  return text;
}

std::string ReplaceLatencyObjectWithFalse(std::string text) {
  const std::string needle = "\"latency_inject\":";
  const size_t key = text.find(needle);
  assert(key != std::string::npos);
  const size_t object_begin = text.find('{', key + needle.size());
  assert(object_begin != std::string::npos);
  size_t depth = 1;
  size_t position = object_begin + 1;
  bool in_string = false;
  bool escaped = false;
  for (; position < text.size() && depth != 0; ++position) {
    const char c = text[position];
    if (in_string) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') in_string = false;
      continue;
    }
    if (c == '"') in_string = true;
    else if (c == '{') ++depth;
    else if (c == '}') --depth;
  }
  assert(depth == 0);
  text.replace(object_begin, position - object_begin, "false");
  return text;
}

}  // namespace

void RunConfigOnly() {
  for (const uint32_t width : {8U, 32U}) {
    const auto sentinel = tigonkv::engine::FixedKey::InternalMax(width);
    for (uint32_t i = 0; i < width; ++i)
      assert(static_cast<unsigned char>(sentinel.bytes[i]) == 0xff);
    for (uint32_t i = width; i < tigonkv::engine::kMaxFixedKeyBytes; ++i)
      assert(sentinel.bytes[i] == 0);
    bool short_rejected = false;
    try {
      (void)tigonkv::engine::FixedKey::From(std::string(width - 1, 'x'), width);
    } catch (const std::invalid_argument &) {
      short_rejected = true;
    }
    assert(short_rejected);
  }

  Config zero;
  zero.partition_count = 0;
  zero.partition_ranges.clear();
  assert(ValidateThrows(zero));
  Config one;
  one.vm_count = 4;
  one.node_id = 3;
  one.partition_count = 1;
  one.partition_ranges = {{"", ""}};
  one.Validate();
  assert(one.PartitionForKey(Fixed("any", one.fixed_key_size)) == 0);
  Config seven = one;
  seven.partition_count = 7;
  SetTestRangePartitioning(&seven);
  seven.Validate();

  const auto root = Config::FromJsonc(std::string(TIGONKV_SOURCE_DIR) +
                                      "/experiment_config.jsonc");
  assert(root.hwcc_size_mb == 1024 && root.swcc_size_mb == 31744 &&
         root.partition_count == 4 && root.fixed_key_size == 32 &&
         root.fixed_value_size == 32);
  const auto fixture = Config::FromJsonc(
      std::string(TIGONKV_SOURCE_DIR) + "/tests/fixtures/experiment_config.jsonc");
  assert(fixture.size_mb == 64 && fixture.hwcc_size_mb == 32 &&
         fixture.swcc_size_mb == 32 && fixture.hw_cc_budget_mb == 32);
}

int main(int argc, char **argv) {
  if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--config-only"))
    return 2;
  if (argc == 2) {
    RunConfigOnly();
    return 0;
  }
  const std::string latency_config_path =
      "/tmp/tigonkv-latency-config-" + std::to_string(getpid()) + ".jsonc";
  std::string base_config_text;
  {
    std::ifstream source(std::string(TIGONKV_SOURCE_DIR) +
                         "/experiment_config.jsonc");
    assert(source.good());
    base_config_text.assign(std::istreambuf_iterator<char>(source),
                            std::istreambuf_iterator<char>());
    const std::string needle = "\"swcc_fixed_ns_per_line\": 0";
    const size_t position = base_config_text.find(needle);
    assert(position != std::string::npos);
    std::string text = base_config_text;
    text.replace(position, needle.size(), "\"swcc_fixed_ns_per_line\": 1.25e0");
    std::ofstream output(latency_config_path);
    output << text;
  }
  const Config fractional = Config::FromJsonc(latency_config_path);
  assert(fractional.hardware_simulation.fixed_latency.swcc_fixed_ns_per_line == 1.25);
  assert(fractional.cpu_affinity);

  static constexpr std::string_view kLatencyFields[] = {
      "fixed_latency", "hwcc_access_count", "atomic_count",
      "remote_cache_invalidation", "swcc_fixed_ns_per_line",
      "hwcc_fixed_ns_per_line", "background_enabled",
      "delayed_time_stats_enabled", "read_enabled", "write_enabled",
      "operation_count_enabled", "line_count_enabled", "byte_count_enabled",
      "max_tags", "hwcc_enabled", "owner_private_swcc_enabled",
      "local_dram_enabled", "cas_enabled", "exchange_enabled",
      "fetch_arithmetic_enabled", "fetch_bitwise_enabled",
      "result_breakdown_enabled", "fence_enabled", "wait_notify_enabled",
      "memory_order_breakdown_enabled", "scope_breakdown_enabled",
      "tag_breakdown_enabled", "dirty_handoff_enabled",
      "clean_copy_invalidation_enabled", "dirty_eviction_writeback_enabled",
      "swcc_explicit_visibility_handoff_enabled", "node_count",
      "cache_size_bytes_per_node", "total_cpu_cache_size_bytes",
      "cache_size_bytes_by_node", "cache_instances_per_node", "associativity",
      "capacity_mode", "replacement_policy", "lfu_counter_bits",
      "lfu_aging_interval_accesses", "lfu_tie_breaker",
      "shared_sequencer_offset", "event_log_capacity"};
  for (const std::string_view field : kLatencyFields) {
    assert(ParseTextThrows(latency_config_path,
                           RemoveJsonFieldLine(base_config_text, field)));
  }
  assert(ParseTextThrows(
      latency_config_path,
      ReplaceOnce(base_config_text, "\"enabled\": false",
                  "\"enabled\": \"false\"")));
  assert(ParseTextThrows(
      latency_config_path,
      ReplaceOnce(base_config_text, "\"cache_line_bytes\": 64",
                  "\"cache_line_bytes\": 64.0")));
  assert(ParseTextThrows(
      latency_config_path,
      ReplaceOnce(base_config_text, "\"fixed_latency\": {",
                  "\"fixed_latency\": {\"cache_model\": false,")));
  assert(ParseTextThrows(
      latency_config_path,
      ReplaceOnce(base_config_text, "\"latency_inject\": {",
                  "\"latency_inject\": {\"partition_count\": 16,")));
  assert(ParseTextThrows(
      latency_config_path,
      ReplaceOnce(base_config_text, "\"latency_inject\": {",
                  "\"latency_inject\": {\"enabled\": true,")));
  assert(ParseTextThrows(
      latency_config_path,
      ReplaceOnce(base_config_text, "{\n", "{\n  \"enabled\": false,\n")));
  assert(ParseTextThrows(
      latency_config_path,
      ReplaceOnce(base_config_text, "\"network\": {",
                  "\"network\": {\"enabled\": false,")));
  assert(ParseTextThrows(
      latency_config_path,
      ReplaceOnce(base_config_text, "{\n", "{\n  \"latency_inject\": {},\n")));
  assert(ParseTextThrows(latency_config_path,
                         ReplaceLatencyObjectWithFalse(base_config_text)));
  assert(ParseTextThrows(
      latency_config_path,
      ReplaceOnce(base_config_text, "\"max_tags\": 32",
                  "\"max_tags\": 0")));

  std::string enabled = ReplaceOnce(
      base_config_text, "\"enabled\": false", "\"enabled\": true");
  if (RelWithDebInfoBuild()) {
    const Config parsed_enabled = [&] {
      std::ofstream output(latency_config_path);
      output << enabled;
      output.close();
      return Config::FromJsonc(latency_config_path);
    }();
    assert(parsed_enabled.hardware_simulation.fixed_latency.enabled);
    assert(parsed_enabled.hardware_simulation.fixed_latency.foreground_enabled);
  } else {
    assert(ParseTextThrows(latency_config_path, enabled));
  }
  assert(ParseTextThrows(
      latency_config_path,
      ReplaceOnce(enabled, "\"verbose\": false", "\"verbose\": true")));
  assert(ParseTextThrows(
      latency_config_path,
      ReplaceOnce(
          ReplaceOnce(enabled, "\"verbose\": false", "\"verbose\": true"),
          "\"network\": {", "\"network\": {\"verbose\": false,")));
  assert(ParseTextThrows(
      latency_config_path,
      ReplaceOnce(enabled, "\"extra_check\": false", "\"extra_check\": true")));
  assert(ParseTextThrows(
      latency_config_path,
      ReplaceOnce(enabled, "\"foreground_enabled\": true",
                  "\"foreground_enabled\": false")));
  std::remove(latency_config_path.c_str());

  Config uneven;
  uneven.size_mb = 64;
  uneven.hwcc_size_mb = 32;
  uneven.swcc_offset_mb = 32;
  uneven.swcc_size_mb = 32;
  uneven.hw_cc_budget_mb = 32;
  uneven.vm_count = 2;
  uneven.node_id = 1;
  uneven.partition_count = 7;
  uneven.transport_ring_total_mb = 1;
  SetTestRangePartitioning(&uneven);
  uneven.Validate();
  Config one_sided_boundary = uneven;
  one_sided_boundary.partition_count = 2;
  one_sided_boundary.partition_ranges = {{"", "m"}, {"", ""}};
  one_sided_boundary.Validate();
  assert(one_sided_boundary.partition_ranges[0].upper_key ==
         one_sided_boundary.partition_ranges[1].lower_key);
  assert(one_sided_boundary.PartitionForKey(Fixed("m", 32)) == 1);
  Config missing_boundary = one_sided_boundary;
  missing_boundary.partition_ranges = {{"", ""}, {"", ""}};
  assert(ValidateThrows(missing_boundary));
  // Original modulo owner mapping accepts any positive partition count.  The
  // formal 4VM runner enforces its own partition_count == vm_count == 4
  // preflight; generic layout validation must not reject fewer partitions.
  Config fewer_partitions_than_vms = uneven;
  fewer_partitions_than_vms.vm_count = 4;
  fewer_partitions_than_vms.node_id = 3;
  fewer_partitions_than_vms.partition_count = 1;
  fewer_partitions_than_vms.partition_ranges = {{"", ""}};
  fewer_partitions_than_vms.Validate();
  assert(fewer_partitions_than_vms.PartitionForKey(Fixed("any", 32)) == 0);
  Config reserved_boundary = one_sided_boundary;
  reserved_boundary.partition_ranges[0].upper_key = MaxKey(32);
  reserved_boundary.partition_ranges[1].lower_key = MaxKey(32);
  assert(ValidateThrows(reserved_boundary));
  Config too_many_partitions = uneven;
  too_many_partitions.partition_count = 257;
  assert(ValidateThrows(too_many_partitions));
  Config zero_partitions = uneven;
  zero_partitions.partition_count = 0;
  zero_partitions.partition_ranges.clear();
  assert(ValidateThrows(zero_partitions));
  const std::string path = "/tmp/tigonkv-facade-" + std::to_string(getpid());
  std::remove(path.c_str());
  Config config;
  config.shared_memory_path = path;
  config.size_mb = 64;
  config.hwcc_size_mb = 32;
  config.swcc_offset_mb = 32;
  config.swcc_size_mb = 32;
  config.hw_cc_budget_mb = 32;
  config.vm_count = 1;
  config.partition_count = 8;
  config.fixed_key_size = 32;
  config.fixed_value_size = 128;
  config.foreground_worker_count_per_vm = 4;
  config.transport_ring_total_mb = 1;
  SetTestRangePartitioning(&config);
  Config insufficient_cpu = config;
  insufficient_cpu.cpu_affinity = true;
  insufficient_cpu.vm_core_count_per_vm = 4;
  assert(ValidateThrows(insufficient_cpu));
  Config gated = config;
  gated.hardware_simulation.fixed_latency.enabled = true;
  if (RelWithDebInfoBuild()) {
    gated.verbose = true;
    assert(ValidateThrows(gated));
    gated.verbose = false;
    gated.extra_check = true;
    assert(ValidateThrows(gated));
    gated.extra_check = false;
    gated.hardware_simulation.fixed_latency.foreground_enabled = false;
    gated.hardware_simulation.fixed_latency.background_enabled = true;
    assert(ValidateThrows(gated));
  } else {
    assert(ValidateThrows(gated));
  }
  config.hardware_simulation.fixed_latency.enabled = RelWithDebInfoBuild();
  config.hardware_simulation.fixed_latency.foreground_enabled = true;
  config.hardware_simulation.fixed_latency.delayed_time_stats_enabled = true;
  config.hardware_simulation.fixed_latency.swcc_fixed_ns_per_line = 1;
  config.hardware_simulation.fixed_latency.hwcc_fixed_ns_per_line = 1;
  // HWCC is bounded by the configured physical shared region, not by an
  // arbitrary 1GiB validation ceiling.  Larger verified configurations are
  // permitted when a smaller policy budget cannot satisfy a focused test.
  Config larger_hwcc = config;
  larger_hwcc.size_mb = 4096;
  larger_hwcc.hwcc_offset_mb = 2048;
  larger_hwcc.hwcc_size_mb = 2048;
  larger_hwcc.swcc_offset_mb = 0;
  larger_hwcc.swcc_size_mb = 2048;
  larger_hwcc.hw_cc_budget_mb = 2048;
  larger_hwcc.Validate();
  auto store = KVStore::Create(config, true);
  store->BindWorker(0);
  const auto key = [&](std::string_view text) { return Fixed(text, config.fixed_key_size); };
  const auto value = [&](std::string_view text) { return Fixed(text, config.fixed_value_size); };
  // Public KV calls keep the fixed-width storage contract explicit.  The
  // engine's short-value tolerance exists only for legacy internal tests.
  assert(store->Put("short", value("one")).code == StatusCode::kInvalidArgument);
  assert(store->Put(key("alpha"), "short").code == StatusCode::kInvalidArgument);
  assert(store->Get("short").status.code == StatusCode::kInvalidArgument);
  assert(store->Scan("", MaxKey(config.fixed_key_size), 0).status.code ==
         StatusCode::kInvalidArgument);
  assert(store->Scan(key("alpha"), "short", 0).status.code ==
         StatusCode::kInvalidArgument);
  // Empty and inverted half-open intervals are valid empty scans, and the
  // comparison must be the same fixed-width comparator used by routing.
  assert(store->Scan(key("beta"), key("alpha"), 0).status.ok());
  assert(store->Scan(key("beta"), key("alpha"), 0).items.empty());
  assert(store->CompareExchange(key("alpha"), "short", value("one"))
             .status.code == StatusCode::kInvalidArgument);
  assert(store->Put(key("alpha"), value("one")).ok());
  assert(store->Put(key("beta"), value("two")).ok());
  const auto scan = store->Scan(key("alpha"), MaxKey(config.fixed_key_size), 0);
  assert(scan.status.ok() && scan.items.size() == 2);
  assert(store->CompareExchange(key("alpha"), value("one"), value("three")).exchanged);
  assert(store->Increment(key("counter"), 3).value == 3);
  assert(store->Get(key("alpha")).value == value("three"));
  // The foreground threads below exercise all four worker identities.
  store->ReleaseWorker();
  std::vector<std::thread> workers;
  for (uint32_t worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&, worker] {
      store->BindWorker(worker);
      for (uint32_t i = 0; i < 50; ++i) {
        const std::string worker_key = key(
            "worker-" + std::to_string(worker) + "-" + std::to_string(i));
        assert(store->Put(worker_key, value("value")).ok());
        const auto found = store->Get(worker_key);
        assert(found.status.ok() && found.value == value("value"));
      }
      store->ReleaseWorker();
    });
  }
  for (auto &worker : workers) worker.join();
  const RuntimeStats runtime = store->Runtime();
  assert(runtime.logical_ops == 406);
  assert(runtime.commits == 406);
  assert(runtime.aborts == 0);
  assert(runtime.private_puts == 204);
  assert(runtime.private_gets == 201);
  const MemoryStats memory = store->Memory();
  assert(memory.physical_region_split);
  assert(memory.physical_hwcc_used_bytes == memory.logical_hwcc_used_bytes);
  assert(memory.physical_swcc_used_bytes ==
         memory.owner_private_swcc_used_bytes +
             memory.shared_payload_swcc_used_bytes +
             memory.allocator_swcc_metadata_bytes);
  assert(memory.allocator_shared_overhead_bytes ==
         memory.allocator_hwcc_metadata_bytes +
             memory.allocator_swcc_metadata_bytes);
  assert(memory.physical_hwcc_used_bytes <=
         memory.logical_hwcc_capacity_bytes);
  assert(memory.physical_swcc_used_bytes <=
         memory.logical_swcc_capacity_bytes);
  assert(memory.unclassified_shared_bytes == 0);
  const std::string stats = store->DumpStats();
  assert(stats.find("allocator_shared_overhead_bytes=") != std::string::npos);
  assert(stats.find("network_tx_bytes=") != std::string::npos);
  assert(stats.find("TIGONKV_HARDWARE_SIM_STATS\n") != std::string::npos);
  assert(stats.find("\nswcc_raw=") == std::string::npos);
  assert(stats.find("\nhwcc_raw=") == std::string::npos);
  assert(stats.find("\nswcc_misses=") == std::string::npos);
  assert(stats.find("\nfixed_latency_enabled=") != std::string::npos);
  assert(stats.find("\nhwcc_access_count_enabled=") != std::string::npos);
  assert(stats.find("\natomic_count_enabled=") != std::string::npos);
  assert(stats.find("\nremote_cache_invalidation_enabled=") != std::string::npos);
  store->BindWorker(0);
  store->ReleaseWorker();
  store.reset();
  auto attached = KVStore::Create(config, false);
  attached->BindWorker(0);
  assert(attached->Get(key("alpha")).value == value("three"));
  assert(attached->Delete(key("beta")).ok());
  attached->ReleaseWorker();
  std::remove(path.c_str());
  return 0;
}
