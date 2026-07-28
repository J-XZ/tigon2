#include "kv/kv_store.h"
#include "kv/engine/latency_inject.h"

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

int main() {
  const std::string latency_config_path =
      "/tmp/tigonkv-latency-config-" + std::to_string(getpid()) + ".jsonc";
  std::string base_config_text;
  {
    std::ifstream source(std::string(TIGONKV_SOURCE_DIR) +
                         "/experiment_config.jsonc");
    assert(source.good());
    base_config_text.assign(std::istreambuf_iterator<char>(source),
                            std::istreambuf_iterator<char>());
    const std::string needle = "\"swcc_read_ns_per_line\": 0";
    const size_t position = base_config_text.find(needle);
    assert(position != std::string::npos);
    std::string text = base_config_text;
    text.replace(position, needle.size(), "\"swcc_read_ns_per_line\": 1.25e0");
    std::ofstream output(latency_config_path);
    output << text;
  }
  const Config fractional = Config::FromJsonc(latency_config_path);
  assert(fractional.swcc_read_ns == 1.25);
  assert(fractional.cpu_affinity);

  static constexpr std::string_view kLatencyFields[] = {
      "enabled", "foreground_enabled", "merge_enabled", "stats_enabled",
      "cache_line_bytes", "swcc_read_ns_per_line",
      "swcc_write_ns_per_line", "swcc_flush_ns_per_line",
      "hwcc_read_ns_per_line", "hwcc_write_ns_per_line",
      "hwcc_atomic_load_ns", "hwcc_atomic_store_ns",
      "hwcc_atomic_rmw_ns", "cache_model", "cache_hits_enabled",
      "cache_capacity_lines", "cache_associativity",
      "cache_fixed_hit_rate", "cache_hit_extra_ns"};
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
      ReplaceOnce(base_config_text, "\"cache_model\": \"none\"",
                  "\"cache_model\": false")));
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
      ReplaceOnce(base_config_text, "\"cache_capacity_lines\": 4096",
                  "\"cache_capacity_lines\": 0")));

  std::string enabled = ReplaceOnce(
      base_config_text, "\"enabled\": false", "\"enabled\": true");
  enabled = ReplaceOnce(enabled, "\"foreground_enabled\": false",
                        "\"foreground_enabled\": true");
  if (RelWithDebInfoBuild()) {
    const Config parsed_enabled = [&] {
      std::ofstream output(latency_config_path);
      output << enabled;
      output.close();
      return Config::FromJsonc(latency_config_path);
    }();
    assert(parsed_enabled.latency_enabled);
    assert(parsed_enabled.latency_foreground_enabled);
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
  uneven.size_mb = 32;
  uneven.hwcc_size_mb = 16;
  uneven.swcc_offset_mb = 16;
  uneven.swcc_size_mb = 16;
  uneven.hw_cc_budget_mb = 4;
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
  assert(one_sided_boundary.PartitionForKey("m") == 1);
  Config missing_boundary = one_sided_boundary;
  missing_boundary.partition_ranges = {{"", ""}, {"", ""}};
  assert(ValidateThrows(missing_boundary));
  Config reserved_boundary = one_sided_boundary;
  reserved_boundary.partition_ranges[0].upper_key = MaxKey(32);
  reserved_boundary.partition_ranges[1].lower_key = MaxKey(32);
  assert(ValidateThrows(reserved_boundary));
  Config too_many_partitions = uneven;
  too_many_partitions.partition_count = 257;
  assert(ValidateThrows(too_many_partitions));
  const std::string path = "/tmp/tigonkv-facade-" + std::to_string(getpid());
  std::remove(path.c_str());
  Config config;
  config.shared_memory_path = path;
  config.size_mb = 32;
  config.hwcc_size_mb = 16;
  config.swcc_offset_mb = 16;
  config.swcc_size_mb = 16;
  config.hw_cc_budget_mb = 4;
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
  gated.latency_enabled = true;
  if (RelWithDebInfoBuild()) {
    gated.verbose = true;
    assert(ValidateThrows(gated));
    gated.verbose = false;
    gated.extra_check = true;
    assert(ValidateThrows(gated));
    gated.extra_check = false;
    gated.latency_foreground_enabled = false;
    gated.latency_merge_enabled = true;
    assert(ValidateThrows(gated));
  } else {
    assert(ValidateThrows(gated));
  }
  config.latency_enabled = RelWithDebInfoBuild();
  config.latency_foreground_enabled = true;
  config.latency_stats_enabled = true;
  config.swcc_read_ns = 1;
  config.swcc_write_ns = 1;
  auto store = KVStore::Create(config, true);
  const auto key = [&](std::string_view text) { return Fixed(text, config.fixed_key_size); };
  const auto value = [&](std::string_view text) { return Fixed(text, config.fixed_value_size); };
  assert(store->Put(key("alpha"), value("one")).ok());
  assert(store->Put(key("beta"), value("two")).ok());
  const auto scan = store->Scan(key("alpha"), MaxKey(config.fixed_key_size), 0);
  assert(scan.status.ok() && scan.items.size() == 2);
  assert(store->CompareExchange(key("alpha"), value("one"), value("three")).exchanged);
  assert(store->Increment(key("counter"), 3).value == 3);
  assert(store->Get(key("alpha")).value == value("three"));
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
  assert(store->Checkpoint().ok());
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
  assert(stats.find("reclaimed_total_bytes=") != std::string::npos);
  assert(stats.find("network_tx_bytes=") != std::string::npos);
  if (RelWithDebInfoBuild()) {
    assert(stats.find("\nswcc_raw=0\n") == std::string::npos);
    assert(stats.find("\nswcc_misses=0\n") == std::string::npos);
  } else {
    assert(stats.find("\nswcc_raw=0\n") != std::string::npos);
    assert(stats.find("\nhwcc_raw=0\n") != std::string::npos);
  }
  store.reset();
  auto attached = KVStore::Create(config, false);
  assert(attached->Get(key("alpha")).value == value("three"));
  assert(attached->Delete(key("beta")).ok());
  std::remove(path.c_str());
  return 0;
}
