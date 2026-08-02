#include "kv/kv_store.h"

#include "common/CXL_EBR.h"
#include "kv/engine/kv_engine.h"
#include "kv/engine/latency_inject.h"
#include "kv/engine/mem_access.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <regex>
#include <string_view>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <sched.h>
#include <unordered_set>
#include <thread>

namespace tigonkv {
namespace {

// BTreeOLC_CXL's persisted FixedKey is 32 bytes.  Keep the public parser in
// lock-step with that original on-media limit instead of accepting keys that
// the tree will reject later.
constexpr size_t kMaxKey = 32;
constexpr size_t kMaxValue = 4096;
// Busy is contention.  Retry only at the logical operation boundary so YCSB
// and API callers share one contract with the engine's inner primitives.
bool IsInternalMaxKey(std::string_view key, uint32_t fixed_key_size) {
  if (key.size() != fixed_key_size) return false;
  return engine::FixedKey::From(key, fixed_key_size).Compare(
             engine::FixedKey::InternalMax(fixed_key_size)) == 0;
}

template <typename Op>
Status RunWithBusyRetry(KVStore *store, RuntimeStats *runtime, Op &&op) {
  Status status;
  for (;;) {
    status = op();
    if (status.code != StatusCode::kBusy) return status;
    if (runtime != nullptr) ++runtime->retries;
    if (store != nullptr) store->PollTransport();
    std::this_thread::yield();
  }
}

std::string StripComments(std::string text) {
  text = std::regex_replace(text, std::regex(R"(//[^\n\r]*)"), "");
  text = std::regex_replace(text, std::regex(R"((/\*)(.|\n|\r)*?\*/)"), "");
  return text;
}

template <typename T>
bool JsonNumber(const std::string &s, const char *name, T *out) {
  std::regex r(std::string("\\\"") + name + R"(\"\s*:\s*(-?[0-9]+))");
  std::smatch m;
  if (!std::regex_search(s, m, r)) return false;
  long long value = std::stoll(m[1].str());
  if (value < 0) throw std::invalid_argument(std::string("negative config field: ") + name);
  *out = static_cast<T>(value);
  return true;
}

bool JsonBool(const std::string &s, const char *name, bool *out) {
  std::regex r(std::string("\\\"") + name + R"(\"\s*:\s*(true|false))");
  std::smatch m;
  if (!std::regex_search(s, m, r)) return false;
  *out = m[1].str() == "true";
  return true;
}

bool JsonString(const std::string &s, const char *name, std::string *out) {
  std::regex r(std::string("\\\"") + name + R"(\"\s*:\s*\"([^\"]*)\")");
  std::smatch m;
  if (!std::regex_search(s, m, r)) return false;
  *out = m[1].str();
  return true;
}

bool JsonDouble(const std::string &s, const char *name, double *out) {
  std::regex r(std::string("\\\"") + name +
      R"(\"\s*:\s*(-?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?))");
  std::smatch m;
  if (!std::regex_search(s, m, r)) return false;
  *out = std::stod(m[1].str());
  return true;
}

template <typename T>
bool JsonNumberArray(const std::string &s, const char *name, std::vector<T> *out) {
  std::regex r(std::string("\\\"") + name + R"(\"\s*:\s*\[([^\]]*)\])");
  std::smatch m;
  if (!std::regex_search(s, m, r)) return false;
  out->clear();
  std::regex number(R"(-?[0-9]+)");
  for (auto it = std::sregex_iterator(m[1].first, m[1].second, number);
       it != std::sregex_iterator(); ++it) {
    const long long value = std::stoll((*it)[0].str());
    if (value < 0) throw std::invalid_argument(std::string("negative config array field: ") + name);
    out->push_back(static_cast<T>(value));
  }
  return true;
}

bool ExtractObjectBody(const std::string &s, const char *object, std::string *body) {
  std::regex object_re(std::string("\\\"") + object + R"(\"\s*:\s*\{)");
  std::smatch object_match;
  if (!std::regex_search(s, object_match, object_re)) return false;
  const size_t start = static_cast<size_t>(object_match.position() + object_match.length());
  int depth = 1;
  for (size_t pos = start; pos < s.size(); ++pos) {
    if (s[pos] == '{') {
      ++depth;
    } else if (s[pos] == '}') {
      --depth;
      if (depth == 0) {
        *body = s.substr(start, pos - start);
        return true;
      }
    }
  }
  return false;
}

enum class JsonValueKind { kObject, kArray, kString, kNumber, kBool, kNull };

struct JsonMemberSpan {
  std::string key;
  size_t value_begin = 0;
  size_t value_end = 0;
  JsonValueKind kind = JsonValueKind::kNull;
};

void SkipJsonWhitespace(std::string_view text, size_t *position) {
  while (*position < text.size() &&
         (text[*position] == ' ' || text[*position] == '\t' ||
          text[*position] == '\n' || text[*position] == '\r')) {
    ++*position;
  }
}

[[noreturn]] void JsonStructureError(std::string_view detail) {
  throw std::invalid_argument("invalid experiment_config JSON: " +
                              std::string(detail));
}

std::string ParseJsonKey(std::string_view text, size_t *position) {
  if (*position >= text.size() || text[*position] != '"')
    JsonStructureError("object key must be a string");
  const size_t begin = ++*position;
  while (*position < text.size() && text[*position] != '"') {
    // Configuration keys are ASCII identifiers. Rejecting escaped keys avoids
    // two spellings of the same schema field and keeps duplicate checks exact.
    if (text[*position] == '\\')
      JsonStructureError("escaped object keys are not supported");
    if (static_cast<unsigned char>(text[*position]) < 0x20)
      JsonStructureError("control character in object key");
    ++*position;
  }
  if (*position >= text.size()) JsonStructureError("unterminated object key");
  std::string key(text.substr(begin, *position - begin));
  ++*position;
  return key;
}

size_t SkipJsonString(std::string_view text, size_t position) {
  if (position >= text.size() || text[position] != '"')
    JsonStructureError("expected string");
  ++position;
  while (position < text.size()) {
    const char c = text[position++];
    if (c == '"') return position;
    if (c == '\\') {
      if (position >= text.size()) JsonStructureError("unterminated string escape");
      const char escaped = text[position++];
      if (escaped == 'u') {
        for (int digit = 0; digit < 4; ++digit) {
          if (position >= text.size() ||
              !std::isxdigit(static_cast<unsigned char>(text[position++])))
            JsonStructureError("invalid unicode escape");
        }
      } else if (escaped != '"' && escaped != '\\' && escaped != '/' &&
                 escaped != 'b' && escaped != 'f' && escaped != 'n' &&
                 escaped != 'r' && escaped != 't') {
        JsonStructureError("invalid string escape");
      }
    } else if (static_cast<unsigned char>(c) < 0x20) {
      JsonStructureError("control character in string");
    }
  }
  JsonStructureError("unterminated string");
}

size_t SkipJsonValue(std::string_view text, size_t position, JsonValueKind *kind);

size_t SkipJsonObject(std::string_view text, size_t position) {
  if (position >= text.size() || text[position] != '{')
    JsonStructureError("expected object");
  ++position;
  SkipJsonWhitespace(text, &position);
  if (position < text.size() && text[position] == '}') return position + 1;
  for (;;) {
    (void)ParseJsonKey(text, &position);
    SkipJsonWhitespace(text, &position);
    if (position >= text.size() || text[position++] != ':')
      JsonStructureError("missing ':' after object key");
    SkipJsonWhitespace(text, &position);
    JsonValueKind nested_kind{};
    position = SkipJsonValue(text, position, &nested_kind);
    SkipJsonWhitespace(text, &position);
    if (position >= text.size()) JsonStructureError("unterminated object");
    if (text[position] == '}') return position + 1;
    if (text[position++] != ',') JsonStructureError("expected ',' in object");
    SkipJsonWhitespace(text, &position);
    if (position < text.size() && text[position] == '}')
      JsonStructureError("trailing comma in object");
  }
}

size_t SkipJsonArray(std::string_view text, size_t position) {
  if (position >= text.size() || text[position] != '[')
    JsonStructureError("expected array");
  ++position;
  SkipJsonWhitespace(text, &position);
  if (position < text.size() && text[position] == ']') return position + 1;
  for (;;) {
    JsonValueKind nested_kind{};
    position = SkipJsonValue(text, position, &nested_kind);
    SkipJsonWhitespace(text, &position);
    if (position >= text.size()) JsonStructureError("unterminated array");
    if (text[position] == ']') return position + 1;
    if (text[position++] != ',') JsonStructureError("expected ',' in array");
    SkipJsonWhitespace(text, &position);
    if (position < text.size() && text[position] == ']')
      JsonStructureError("trailing comma in array");
  }
}

size_t SkipJsonNumber(std::string_view text, size_t position) {
  const size_t begin = position;
  if (position < text.size() && text[position] == '-') ++position;
  if (position >= text.size()) JsonStructureError("incomplete number");
  if (text[position] == '0') {
    ++position;
  } else {
    if (text[position] < '1' || text[position] > '9')
      JsonStructureError("invalid number");
    while (position < text.size() && std::isdigit(
               static_cast<unsigned char>(text[position])))
      ++position;
  }
  if (position < text.size() && text[position] == '.') {
    ++position;
    const size_t fractional = position;
    while (position < text.size() && std::isdigit(
               static_cast<unsigned char>(text[position])))
      ++position;
    if (position == fractional) JsonStructureError("invalid fractional number");
  }
  if (position < text.size() &&
      (text[position] == 'e' || text[position] == 'E')) {
    ++position;
    if (position < text.size() &&
        (text[position] == '+' || text[position] == '-'))
      ++position;
    const size_t exponent = position;
    while (position < text.size() && std::isdigit(
               static_cast<unsigned char>(text[position])))
      ++position;
    if (position == exponent) JsonStructureError("invalid number exponent");
  }
  if (position == begin) JsonStructureError("invalid number");
  return position;
}

size_t SkipJsonValue(std::string_view text, size_t position, JsonValueKind *kind) {
  SkipJsonWhitespace(text, &position);
  if (position >= text.size()) JsonStructureError("missing value");
  if (text[position] == '{') {
    *kind = JsonValueKind::kObject;
    return SkipJsonObject(text, position);
  }
  if (text[position] == '[') {
    *kind = JsonValueKind::kArray;
    return SkipJsonArray(text, position);
  }
  if (text[position] == '"') {
    *kind = JsonValueKind::kString;
    return SkipJsonString(text, position);
  }
  for (const auto &[literal, literal_kind] :
       {std::pair<std::string_view, JsonValueKind>{"true", JsonValueKind::kBool},
        {"false", JsonValueKind::kBool},
        {"null", JsonValueKind::kNull}}) {
    if (text.substr(position, literal.size()) == literal) {
      *kind = literal_kind;
      return position + literal.size();
    }
  }
  *kind = JsonValueKind::kNumber;
  return SkipJsonNumber(text, position);
}

std::vector<JsonMemberSpan> ParseJsonObjectMembers(
    std::string_view text, size_t object_begin, size_t *object_end = nullptr) {
  if (object_begin >= text.size() || text[object_begin] != '{')
    JsonStructureError("expected object");
  size_t position = object_begin + 1;
  std::vector<JsonMemberSpan> members;
  SkipJsonWhitespace(text, &position);
  if (position < text.size() && text[position] == '}') {
    if (object_end != nullptr) *object_end = position + 1;
    return members;
  }
  for (;;) {
    JsonMemberSpan member;
    member.key = ParseJsonKey(text, &position);
    SkipJsonWhitespace(text, &position);
    if (position >= text.size() || text[position++] != ':')
      JsonStructureError("missing ':' after object key");
    SkipJsonWhitespace(text, &position);
    member.value_begin = position;
    member.value_end = SkipJsonValue(text, position, &member.kind);
    members.push_back(std::move(member));
    position = members.back().value_end;
    SkipJsonWhitespace(text, &position);
    if (position >= text.size()) JsonStructureError("unterminated object");
    if (text[position] == '}') {
      if (object_end != nullptr) *object_end = position + 1;
      return members;
    }
    if (text[position++] != ',') JsonStructureError("expected ',' in object");
    SkipJsonWhitespace(text, &position);
    if (position < text.size() && text[position] == '}')
      JsonStructureError("trailing comma in object");
  }
}

const JsonMemberSpan &RequireUniqueMember(
    const std::vector<JsonMemberSpan> &members, std::string_view name,
    std::string_view object_path) {
  const JsonMemberSpan *found = nullptr;
  for (const auto &member : members) {
    if (member.key != name) continue;
    if (found != nullptr)
      throw std::invalid_argument("duplicate config field: " +
                                  std::string(object_path) + "." +
                                  std::string(name));
    found = &member;
  }
  if (found == nullptr)
    throw std::invalid_argument("missing required config field: " +
                                std::string(object_path) + "." +
                                std::string(name));
  return *found;
}

void RejectUnknownMembers(const std::vector<JsonMemberSpan> &members,
                          const std::vector<std::string_view> &allowed,
                          std::string_view object_path) {
  std::unordered_set<std::string> seen;
  for (const auto &member : members) {
    if (!seen.insert(member.key).second)
      throw std::invalid_argument("duplicate config field: " +
                                  std::string(object_path) + "." + member.key);
    if (std::find(allowed.begin(), allowed.end(), member.key) == allowed.end())
      throw std::invalid_argument("unknown " + std::string(object_path) +
                                  " field: " + member.key);
  }
}

std::string_view MemberValue(std::string_view text,
                             const JsonMemberSpan &member) {
  return text.substr(member.value_begin, member.value_end - member.value_begin);
}

bool ParseStrictBool(std::string_view text, const JsonMemberSpan &member,
                     std::string_view path) {
  if (member.kind != JsonValueKind::kBool)
    throw std::invalid_argument("config field must be boolean: " +
                                std::string(path));
  return MemberValue(text, member) == "true";
}

uint64_t ParseStrictUint64(std::string_view text, const JsonMemberSpan &member,
                           std::string_view path) {
  if (member.kind != JsonValueKind::kNumber)
    throw std::invalid_argument("config field must be unsigned integer: " +
                                std::string(path));
  const std::string_view value = MemberValue(text, member);
  uint64_t parsed = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
    throw std::invalid_argument("config field must be unsigned integer: " +
                                std::string(path));
  return parsed;
}

double ParseStrictDouble(std::string_view text, const JsonMemberSpan &member,
                         std::string_view path) {
  if (member.kind != JsonValueKind::kNumber)
    throw std::invalid_argument("config field must be numeric: " +
                                std::string(path));
  const std::string value(MemberValue(text, member));
  size_t consumed = 0;
  double parsed = 0;
  try {
    parsed = std::stod(value, &consumed);
  } catch (const std::exception &) {
    throw std::invalid_argument("config field must be numeric: " +
                                std::string(path));
  }
  if (consumed != value.size())
    throw std::invalid_argument("config field must be numeric: " +
                                std::string(path));
  return parsed;
}

std::string ParseStrictString(std::string_view text,
                              const JsonMemberSpan &member,
                              std::string_view path) {
  if (member.kind != JsonValueKind::kString)
    throw std::invalid_argument("config field must be string: " +
                                std::string(path));
  const std::string_view value = MemberValue(text, member);
  if (value.size() < 2 || value.front() != '"' || value.back() != '"')
    JsonStructureError("invalid string value");
  if (value.find('\\') != std::string_view::npos)
    throw std::invalid_argument("escaped config string is not supported: " +
                                std::string(path));
  return std::string(value.substr(1, value.size() - 2));
}

std::vector<JsonMemberSpan> ParseJsonArrayElements(std::string_view text,
                                                    size_t array_begin) {
  if (array_begin >= text.size() || text[array_begin] != '[')
    JsonStructureError("expected array");
  size_t position = array_begin + 1;
  std::vector<JsonMemberSpan> elements;
  SkipJsonWhitespace(text, &position);
  if (position < text.size() && text[position] == ']') return elements;
  for (;;) {
    JsonMemberSpan element;
    element.value_begin = position;
    element.value_end = SkipJsonValue(text, position, &element.kind);
    elements.push_back(std::move(element));
    position = elements.back().value_end;
    SkipJsonWhitespace(text, &position);
    if (position >= text.size()) JsonStructureError("unterminated array");
    if (text[position] == ']') return elements;
    if (text[position++] != ',') JsonStructureError("expected ',' in array");
    SkipJsonWhitespace(text, &position);
    if (position < text.size() && text[position] == ']')
      JsonStructureError("trailing comma in array");
  }
}

size_t CountJsonKey(std::string_view text, std::string_view name) {
  size_t count = 0;
  size_t position = 0;
  while (position < text.size()) {
    if (text[position] != '"') {
      ++position;
      continue;
    }
    const size_t begin = position + 1;
    const size_t end = SkipJsonString(text, position);
    size_t after = end;
    SkipJsonWhitespace(text, &after);
    if (after < text.size() && text[after] == ':' &&
        text.substr(begin, end - begin - 1) == name)
      ++count;
    position = end;
  }
  return count;
}

void ParseStrictLatencyConfig(const std::string &text, Config *config) {
  size_t root_begin = 0;
  SkipJsonWhitespace(text, &root_begin);
  size_t root_end = 0;
  const auto root = ParseJsonObjectMembers(text, root_begin, &root_end);
  size_t trailing = root_end;
  SkipJsonWhitespace(text, &trailing);
  if (trailing != text.size())
    JsonStructureError("trailing content after root object");

  RejectUnknownMembers(root,
                       {"shared_memory", "host_cpu", "vm", "network",
                        "sync", "e2e", "tigon_kv"},
                       "$" );
  const auto reject_child_members = [&](std::string_view name,
                                        const std::vector<std::string_view> &allowed) {
    for (const auto &member : root) {
      if (member.key != name) continue;
      if (member.kind != JsonValueKind::kObject)
        return;
      RejectUnknownMembers(ParseJsonObjectMembers(text, member.value_begin),
                           allowed, name);
    }
  };
  reject_child_members("shared_memory",
                       {"size_mb", "path", "device_path", "numa_node",
                        "hwcc", "swcc"});
  for (const auto &root_member : root) {
    if (root_member.key != "shared_memory" ||
        root_member.kind != JsonValueKind::kObject)
      continue;
    const auto shared_members =
        ParseJsonObjectMembers(text, root_member.value_begin);
    for (const auto &shared_member : shared_members) {
      if (shared_member.key != "hwcc" && shared_member.key != "swcc")
        continue;
      if (shared_member.kind != JsonValueKind::kObject)
        continue;
      RejectUnknownMembers(ParseJsonObjectMembers(text, shared_member.value_begin),
                           {"offset_mb", "size_mb"},
                           std::string("shared_memory.") + shared_member.key);
    }
  }
  reject_child_members("host_cpu",
                       {"reserved_cores", "ivshmem_server_cores", "vm_cores"});
  reject_child_members(
      "vm", {"count", "core_count_per_vm", "mem_size_mb_per_vm",
             "storage_path", "first_ip", "bridge_tap_ip", "numa_node",
             "ssh_base_port", "copy_root_img", "use_ivshmem_doorbell",
             "local_ssh_pub_key"});
  reject_child_members("network", {"base_ssh_port", "sriov_nic", "outside_nic"});
  reject_child_members(
      "sync", {"timeout_sec", "vm_ssh_user", "vm_direct_ssh_port",
               "host_rsync_dest", "host_ssh_port", "host_ssh_extra",
               "project_root_on_targets", "vm_ssh_extra"});
  reject_child_members("e2e", {"foreground_worker_count_per_vm"});

  const auto &tigon = RequireUniqueMember(root, "tigon_kv", "$");
  if (tigon.kind != JsonValueKind::kObject)
    throw std::invalid_argument("config field must be object: tigon_kv");
  if (CountJsonKey(text, "tigon_kv") != 1)
    throw std::invalid_argument("tigon_kv must appear exactly once");
  const auto tigon_members =
      ParseJsonObjectMembers(text, tigon.value_begin);
  RejectUnknownMembers(
      tigon_members,
      {"partition_count", "partitioning", "fixed_key_size", "fixed_value_size",
       "hw_cc_budget_mb", "owner_private_swcc_fraction", "migration_policy",
       "when_to_move_out", "scc_mechanism", "transport_ring_total_mb",
       "verbose", "extra_check", "cpu_affinity", "latency_inject"},
      "tigon_kv");

  const auto &verbose = RequireUniqueMember(tigon_members, "verbose", "tigon_kv");
  const auto &extra_check =
      RequireUniqueMember(tigon_members, "extra_check", "tigon_kv");
  if (CountJsonKey(text, "verbose") != 1 ||
      CountJsonKey(text, "extra_check") != 1)
    throw std::invalid_argument(
        "tigon_kv.verbose and tigon_kv.extra_check must appear exactly once");
  config->verbose = ParseStrictBool(text, verbose, "tigon_kv.verbose");
  config->extra_check =
      ParseStrictBool(text, extra_check, "tigon_kv.extra_check");

  const auto &latency =
      RequireUniqueMember(tigon_members, "latency_inject", "tigon_kv");
  if (latency.kind != JsonValueKind::kObject)
    throw std::invalid_argument(
        "config field must be object: tigon_kv.latency_inject");
  if (CountJsonKey(text, "latency_inject") != 1)
    throw std::invalid_argument(
        "tigon_kv.latency_inject must appear exactly once");
  const auto latency_members = ParseJsonObjectMembers(text, latency.value_begin);
  RejectUnknownMembers(
      latency_members,
      {"fixed_latency", "hwcc_access_count", "atomic_count",
       "remote_cache_invalidation"},
      "tigon_kv.latency_inject");
  const auto parse_section = [&](std::string_view name,
                                 const std::vector<std::string_view> &allowed,
                                 const std::vector<std::string_view> &required) {
    const auto &member = RequireUniqueMember(latency_members, name,
                                             "tigon_kv.latency_inject");
    if (member.kind != JsonValueKind::kObject)
      throw std::invalid_argument("config field must be object: tigon_kv.latency_inject." +
                                  std::string(name));
    auto members = ParseJsonObjectMembers(text, member.value_begin);
    RejectUnknownMembers(members, allowed,
                         std::string("tigon_kv.latency_inject.") +
                             std::string(name));
    for (const auto field_name : required)
      (void)RequireUniqueMember(members, field_name,
                                std::string("tigon_kv.latency_inject.") +
                                    std::string(name));
    return members;
  };
  const auto field = [&](const std::vector<JsonMemberSpan> &members,
                         std::string_view section, std::string_view name)
      -> const JsonMemberSpan & {
    return RequireUniqueMember(
        members, name, std::string("tigon_kv.latency_inject.") +
                           std::string(section));
  };
  const std::vector<std::string_view> fixed_fields = {
      "enabled", "cache_line_bytes", "swcc_fixed_ns_per_line",
      "hwcc_fixed_ns_per_line", "foreground_enabled", "background_enabled",
      "delayed_time_stats_enabled"};
  const auto fixed = parse_section("fixed_latency", fixed_fields, fixed_fields);
  const std::vector<std::string_view> access_fields = {
      "enabled", "cache_line_bytes", "read_enabled", "write_enabled",
      "operation_count_enabled", "line_count_enabled", "byte_count_enabled",
      "breakdown_by_scope_enabled", "breakdown_by_tag_enabled", "max_tags"};
  const auto access = parse_section("hwcc_access_count", access_fields, access_fields);
  const std::vector<std::string_view> atomic_fields = {
      "enabled", "hwcc_enabled", "owner_private_swcc_enabled",
      "local_dram_enabled", "load_enabled", "store_enabled", "cas_enabled",
      "exchange_enabled", "fetch_arithmetic_enabled", "fetch_bitwise_enabled",
      "result_breakdown_enabled", "fence_enabled", "wait_notify_enabled",
      "memory_order_breakdown_enabled", "scope_breakdown_enabled",
      "tag_breakdown_enabled", "max_tags"};
  const auto atomic = parse_section("atomic_count", atomic_fields, atomic_fields);
  const std::vector<std::string_view> remote_fields = {
      "enabled", "dirty_handoff_enabled", "clean_copy_invalidation_enabled",
      "dirty_eviction_writeback_enabled",
      "swcc_explicit_visibility_handoff_enabled", "cache_line_bytes",
      "node_count", "cache_size_bytes_per_node", "total_cpu_cache_size_bytes",
      "cache_size_bytes_by_node", "cache_instances_per_node", "associativity",
      "capacity_mode", "replacement_policy", "lfu_counter_bits",
      "lfu_aging_interval_accesses", "lfu_tie_breaker", "scope_breakdown_enabled",
      "tag_breakdown_enabled", "max_tags", "shared_sequencer_offset",
      "event_log_capacity"};
  const auto remote = parse_section("remote_cache_invalidation", remote_fields,
                                    remote_fields);

  auto &sim = config->hardware_simulation;
  auto bool_field = [&](const std::vector<JsonMemberSpan> &members,
                        std::string_view section, std::string_view name) {
    return ParseStrictBool(text, field(members, section, name),
                           std::string("tigon_kv.latency_inject.") +
                               std::string(section) + "." + std::string(name));
  };
  auto u64_field = [&](const std::vector<JsonMemberSpan> &members,
                       std::string_view section, std::string_view name) {
    return ParseStrictUint64(text, field(members, section, name),
                             std::string("tigon_kv.latency_inject.") +
                                 std::string(section) + "." + std::string(name));
  };
  auto double_field = [&](const std::vector<JsonMemberSpan> &members,
                          std::string_view section, std::string_view name) {
    return ParseStrictDouble(text, field(members, section, name),
                             std::string("tigon_kv.latency_inject.") +
                                 std::string(section) + "." + std::string(name));
  };
  auto string_field = [&](const std::vector<JsonMemberSpan> &members,
                          std::string_view section, std::string_view name) {
    return ParseStrictString(text, field(members, section, name),
                             std::string("tigon_kv.latency_inject.") +
                                 std::string(section) + "." + std::string(name));
  };
  auto &f = sim.fixed_latency;
  f.enabled = bool_field(fixed, "fixed_latency", "enabled");
  f.cache_line_bytes = u64_field(fixed, "fixed_latency", "cache_line_bytes");
  f.swcc_fixed_ns_per_line = double_field(fixed, "fixed_latency", "swcc_fixed_ns_per_line");
  f.hwcc_fixed_ns_per_line = double_field(fixed, "fixed_latency", "hwcc_fixed_ns_per_line");
  f.foreground_enabled = bool_field(fixed, "fixed_latency", "foreground_enabled");
  f.background_enabled = bool_field(fixed, "fixed_latency", "background_enabled");
  f.delayed_time_stats_enabled = bool_field(fixed, "fixed_latency", "delayed_time_stats_enabled");
  auto &a = sim.hwcc_access_count;
  a.enabled = bool_field(access, "hwcc_access_count", "enabled");
  a.cache_line_bytes = u64_field(access, "hwcc_access_count", "cache_line_bytes");
  a.read_enabled = bool_field(access, "hwcc_access_count", "read_enabled");
  a.write_enabled = bool_field(access, "hwcc_access_count", "write_enabled");
  a.operation_count_enabled = bool_field(access, "hwcc_access_count", "operation_count_enabled");
  a.line_count_enabled = bool_field(access, "hwcc_access_count", "line_count_enabled");
  a.byte_count_enabled = bool_field(access, "hwcc_access_count", "byte_count_enabled");
  a.breakdown_by_scope_enabled = bool_field(access, "hwcc_access_count", "breakdown_by_scope_enabled");
  a.breakdown_by_tag_enabled = bool_field(access, "hwcc_access_count", "breakdown_by_tag_enabled");
  a.max_tags = u64_field(access, "hwcc_access_count", "max_tags");
  auto &ac = sim.atomic_count;
  ac.enabled = bool_field(atomic, "atomic_count", "enabled");
  ac.hwcc_enabled = bool_field(atomic, "atomic_count", "hwcc_enabled");
  ac.owner_private_swcc_enabled = bool_field(atomic, "atomic_count", "owner_private_swcc_enabled");
  ac.local_dram_enabled = bool_field(atomic, "atomic_count", "local_dram_enabled");
  ac.load_enabled = bool_field(atomic, "atomic_count", "load_enabled");
  ac.store_enabled = bool_field(atomic, "atomic_count", "store_enabled");
  ac.cas_enabled = bool_field(atomic, "atomic_count", "cas_enabled");
  ac.exchange_enabled = bool_field(atomic, "atomic_count", "exchange_enabled");
  ac.fetch_arithmetic_enabled = bool_field(atomic, "atomic_count", "fetch_arithmetic_enabled");
  ac.fetch_bitwise_enabled = bool_field(atomic, "atomic_count", "fetch_bitwise_enabled");
  ac.result_breakdown_enabled = bool_field(atomic, "atomic_count", "result_breakdown_enabled");
  ac.fence_enabled = bool_field(atomic, "atomic_count", "fence_enabled");
  ac.wait_notify_enabled = bool_field(atomic, "atomic_count", "wait_notify_enabled");
  ac.memory_order_breakdown_enabled = bool_field(atomic, "atomic_count", "memory_order_breakdown_enabled");
  ac.scope_breakdown_enabled = bool_field(atomic, "atomic_count", "scope_breakdown_enabled");
  ac.tag_breakdown_enabled = bool_field(atomic, "atomic_count", "tag_breakdown_enabled");
  ac.max_tags = u64_field(atomic, "atomic_count", "max_tags");
  auto &r = sim.remote_cache_invalidation;
  r.enabled = bool_field(remote, "remote_cache_invalidation", "enabled");
  r.dirty_handoff_enabled = bool_field(remote, "remote_cache_invalidation", "dirty_handoff_enabled");
  r.clean_copy_invalidation_enabled = bool_field(remote, "remote_cache_invalidation", "clean_copy_invalidation_enabled");
  r.dirty_eviction_writeback_enabled = bool_field(remote, "remote_cache_invalidation", "dirty_eviction_writeback_enabled");
  r.swcc_explicit_visibility_handoff_enabled = bool_field(remote, "remote_cache_invalidation", "swcc_explicit_visibility_handoff_enabled");
  r.cache_line_bytes = u64_field(remote, "remote_cache_invalidation", "cache_line_bytes");
  r.node_count = u64_field(remote, "remote_cache_invalidation", "node_count");
  r.cache_size_bytes_per_node = u64_field(remote, "remote_cache_invalidation", "cache_size_bytes_per_node");
  r.total_cpu_cache_size_bytes = u64_field(remote, "remote_cache_invalidation", "total_cpu_cache_size_bytes");
  const auto &capacity = field(remote, "remote_cache_invalidation", "cache_size_bytes_by_node");
  if (capacity.kind != JsonValueKind::kArray)
    throw std::invalid_argument("config field must be array: tigon_kv.latency_inject.remote_cache_invalidation.cache_size_bytes_by_node");
  for (const auto &element : ParseJsonArrayElements(text, capacity.value_begin))
    r.cache_size_bytes_by_node.push_back(ParseStrictUint64(
        text, element,
        "tigon_kv.latency_inject.remote_cache_invalidation.cache_size_bytes_by_node"));
  r.cache_instances_per_node = u64_field(remote, "remote_cache_invalidation", "cache_instances_per_node");
  r.associativity = u64_field(remote, "remote_cache_invalidation", "associativity");
  r.capacity_mode = string_field(remote, "remote_cache_invalidation", "capacity_mode");
  r.replacement_policy = string_field(remote, "remote_cache_invalidation", "replacement_policy");
  r.lfu_counter_bits = u64_field(remote, "remote_cache_invalidation", "lfu_counter_bits");
  r.lfu_aging_interval_accesses = u64_field(remote, "remote_cache_invalidation", "lfu_aging_interval_accesses");
  r.lfu_tie_breaker = string_field(remote, "remote_cache_invalidation", "lfu_tie_breaker");
  r.scope_breakdown_enabled = bool_field(remote, "remote_cache_invalidation", "scope_breakdown_enabled");
  r.tag_breakdown_enabled = bool_field(remote, "remote_cache_invalidation", "tag_breakdown_enabled");
  r.max_tags = u64_field(remote, "remote_cache_invalidation", "max_tags");
  r.shared_sequencer_offset = u64_field(remote, "remote_cache_invalidation", "shared_sequencer_offset");
  r.event_log_capacity = u64_field(remote, "remote_cache_invalidation", "event_log_capacity");
  sim.enabled = f.enabled;

}

void ParsePartitioningConfig(const std::string &text, Config *config) {
  size_t root_begin = 0;
  SkipJsonWhitespace(text, &root_begin);
  const auto root = ParseJsonObjectMembers(text, root_begin);
  const auto &tigon = RequireUniqueMember(root, "tigon_kv", "$");
  if (tigon.kind != JsonValueKind::kObject)
    throw std::invalid_argument("config field must be object: tigon_kv");
  const auto tigon_members = ParseJsonObjectMembers(text, tigon.value_begin);
  const auto &partitioning =
      RequireUniqueMember(tigon_members, "partitioning", "tigon_kv");
  if (partitioning.kind != JsonValueKind::kObject)
    throw std::invalid_argument(
        "config field must be object: tigon_kv.partitioning");
  const auto partitioning_members =
      ParseJsonObjectMembers(text, partitioning.value_begin);
  RejectUnknownMembers(partitioning_members, {"strategy", "ranges"},
                       "tigon_kv.partitioning");
  const auto &strategy = RequireUniqueMember(
      partitioning_members, "strategy", "tigon_kv.partitioning");
  if (ParseStrictString(text, strategy, "tigon_kv.partitioning.strategy") != "range")
    throw std::invalid_argument(
        "tigon_kv.partitioning.strategy must be 'range'");
  const auto &ranges = RequireUniqueMember(
      partitioning_members, "ranges", "tigon_kv.partitioning");
  if (ranges.kind != JsonValueKind::kArray)
    throw std::invalid_argument(
        "config field must be array: tigon_kv.partitioning.ranges");
  config->partition_ranges.clear();
  for (const auto &element : ParseJsonArrayElements(text, ranges.value_begin)) {
    if (element.kind != JsonValueKind::kObject)
      throw std::invalid_argument(
          "each tigon_kv.partitioning.ranges item must be an object");
    const auto members = ParseJsonObjectMembers(text, element.value_begin);
    RejectUnknownMembers(members, {"lower_key", "upper_key"},
                         "tigon_kv.partitioning.ranges");
    const auto &lower = RequireUniqueMember(
        members, "lower_key", "tigon_kv.partitioning.ranges");
    const auto &upper = RequireUniqueMember(
        members, "upper_key", "tigon_kv.partitioning.ranges");
    config->partition_ranges.push_back(
        {ParseStrictString(text, lower,
                           "tigon_kv.partitioning.ranges.lower_key"),
         ParseStrictString(text, upper,
                           "tigon_kv.partitioning.ranges.upper_key")});
  }
}

template <typename T>
bool JsonNumberInObject(const std::string &s, const char *object, const char *name, T *out) {
  std::string body;
  if (!ExtractObjectBody(s, object, &body)) return false;
  return JsonNumber(body, name, out);
}

// Accept either a scalar int or an int array; arrays use the first element.
template <typename T>
bool JsonNumberOrFirstArrayInObject(const std::string &s, const char *object, const char *name,
                                    T *out) {
  std::string body;
  if (!ExtractObjectBody(s, object, &body)) return false;
  if (JsonNumber(body, name, out)) return true;
  std::vector<T> values;
  if (!JsonNumberArray(body, name, &values) || values.empty()) return false;
  *out = values.front();
  return true;
}

void ValidateKnownKeys(const std::string &s) {
  static const std::unordered_set<std::string> known = {
      "shared_memory", "size_mb", "path", "device_path", "numa_node",
      "hwcc", "offset_mb", "swcc", "host_cpu", "reserved_cores",
      "ivshmem_server_cores", "vm_cores", "vm", "count", "core_count_per_vm",
      "storage_path", "mem_size_mb_per_vm", "first_ip", "bridge_tap_ip",
      "copy_root_img", "use_ivshmem_doorbell", "local_ssh_pub_key",
      "ssh_base_port",
      "network", "base_ssh_port", "sriov_nic", "outside_nic", "sync", "e2e",
      "foreground_worker_count_per_vm", "tigon_kv", "partition_count",
      "partitioning", "strategy", "ranges", "lower_key", "upper_key",
      "timeout_sec", "vm_ssh_user", "vm_direct_ssh_port", "host_rsync_dest",
      "host_ssh_port", "host_ssh_extra", "project_root_on_targets", "vm_ssh_extra",
      "fixed_key_size", "fixed_value_size", "hw_cc_budget_mb",
      "owner_private_swcc_fraction", "migration_policy", "when_to_move_out",
      "scc_mechanism", "transport_ring_total_mb",
      "verbose", "extra_check", "cpu_affinity",
      "latency_inject", "fixed_latency", "hwcc_access_count", "atomic_count",
      "remote_cache_invalidation", "enabled", "cache_line_bytes",
      "swcc_fixed_ns_per_line", "hwcc_fixed_ns_per_line", "foreground_enabled",
      "background_enabled", "delayed_time_stats_enabled", "read_enabled",
      "write_enabled", "operation_count_enabled", "line_count_enabled",
      "byte_count_enabled", "breakdown_by_scope_enabled",
      "breakdown_by_tag_enabled", "max_tags", "hwcc_enabled",
      "owner_private_swcc_enabled", "local_dram_enabled", "load_enabled",
      "store_enabled", "cas_enabled", "exchange_enabled",
      "fetch_arithmetic_enabled", "fetch_bitwise_enabled",
      "result_breakdown_enabled", "fence_enabled", "wait_notify_enabled",
      "memory_order_breakdown_enabled", "scope_breakdown_enabled",
      "tag_breakdown_enabled",
      "dirty_handoff_enabled", "clean_copy_invalidation_enabled",
      "dirty_eviction_writeback_enabled",
      "swcc_explicit_visibility_handoff_enabled", "node_count",
      "cache_size_bytes_per_node", "total_cpu_cache_size_bytes",
      "cache_size_bytes_by_node", "cache_instances_per_node", "associativity",
      "capacity_mode", "replacement_policy", "lfu_counter_bits",
      "lfu_aging_interval_accesses", "lfu_tie_breaker",
      "shared_sequencer_offset", "event_log_capacity"};
  std::regex key(R"KEY("([A-Za-z0-9_.-]+)"\s*:)KEY");
  for (auto it = std::sregex_iterator(s.begin(), s.end(), key); it != std::sregex_iterator(); ++it) {
    if (!known.count((*it)[1].str()))
      throw std::invalid_argument("unknown experiment_config field: " + (*it)[1].str());
  }
}

}  // namespace

struct KVStore::Impl {
  std::unique_ptr<engine::KVEngine> engine;
};
Config Config::FromJsonc(const std::string &path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open experiment config: " + path);
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  text = StripComments(std::move(text));
  ValidateKnownKeys(text);
  Config c;
  JsonString(text, "path", &c.shared_memory_path);
  JsonString(text, "device_path", &c.device_path);
  JsonNumber(text, "size_mb", &c.size_mb);
  JsonNumberOrFirstArrayInObject(text, "shared_memory", "numa_node", &c.shared_memory_numa_node);
  JsonNumberArray(text, "reserved_cores", &c.host_reserved_cores);
  JsonNumberArray(text, "ivshmem_server_cores", &c.ivshmem_server_cores);
  JsonNumberArray(text, "vm_cores", &c.vm_cores);
  JsonNumber(text, "count", &c.vm_count);
  JsonNumberInObject(text, "vm", "core_count_per_vm", &c.vm_core_count_per_vm);
  JsonString(text, "storage_path", &c.vm_storage_path);
  JsonNumberOrFirstArrayInObject(text, "vm", "numa_node", &c.vm_numa_node);
  if (!JsonNumberInObject(text, "vm", "ssh_base_port", &c.network_base_ssh_port))
    JsonNumber(text, "base_ssh_port", &c.network_base_ssh_port);
  JsonNumber(text, "timeout_sec", &c.sync_timeout_sec);
  JsonNumber(text, "foreground_worker_count_per_vm", &c.foreground_worker_count_per_vm);
  JsonNumber(text, "partition_count", &c.partition_count);
  JsonNumber(text, "fixed_key_size", &c.fixed_key_size);
  JsonNumber(text, "fixed_value_size", &c.fixed_value_size);
  JsonNumber(text, "hw_cc_budget_mb", &c.hw_cc_budget_mb);
  JsonDouble(text, "owner_private_swcc_fraction", &c.owner_private_swcc_fraction);
  JsonString(text, "migration_policy", &c.migration_policy);
  JsonString(text, "when_to_move_out", &c.when_to_move_out);
  JsonString(text, "scc_mechanism", &c.scc_mechanism);
  JsonNumber(text, "transport_ring_total_mb", &c.transport_ring_total_mb);
  JsonBool(text, "cpu_affinity", &c.cpu_affinity);
  JsonNumberInObject(text, "hwcc", "offset_mb", &c.hwcc_offset_mb);
  JsonNumberInObject(text, "hwcc", "size_mb", &c.hwcc_size_mb);
  JsonNumberInObject(text, "swcc", "offset_mb", &c.swcc_offset_mb);
  JsonNumberInObject(text, "swcc", "size_mb", &c.swcc_size_mb);
  ParseStrictLatencyConfig(text, &c);
  ParsePartitioningConfig(text, &c);
  if (c.shared_memory_path == "/mnt/xz_shared_mem" || c.shared_memory_path == "/mnt/xz_shared_mem/")
    c.shared_memory_path = "/mnt/xz_shared_mem/ivshmem_shared_mem";
  struct stat device_stat {};
  if (!c.device_path.empty() && ::stat(c.device_path.c_str(), &device_stat) == 0 &&
      S_ISCHR(device_stat.st_mode)) {
    c.shared_memory_path = c.device_path;
  }
  c.Validate();
  return c;
}

void Config::Validate() {
  if (size_mb == 0 || hwcc_size_mb > size_mb ||
      swcc_size_mb > size_mb || hwcc_offset_mb + hwcc_size_mb > size_mb ||
      swcc_offset_mb + swcc_size_mb > size_mb || fixed_value_size == 0 ||
      (hwcc_offset_mb < swcc_offset_mb + swcc_size_mb &&
       swcc_offset_mb < hwcc_offset_mb + hwcc_size_mb))
    throw std::invalid_argument("invalid shared-memory capacity or HWCC budget");
  if (vm_count == 0 || partition_count == 0 ||
      partition_count > engine::kMaxPartitions ||
      fixed_key_size == 0 || fixed_key_size > kMaxKey ||
      fixed_value_size > kMaxValue || shared_memory_numa_node < -1 || vm_numa_node < -1 ||
      network_base_ssh_port == 0 || sync_timeout_sec == 0 || foreground_worker_count_per_vm == 0 ||
      foreground_worker_count_per_vm > star::CXL_EBR::max_thread_num ||
      foreground_worker_count_per_vm > 255 ||
      vm_count > star::CXL_EBR::max_coordinator_num)
    throw std::invalid_argument("invalid KV configuration");
  if (partition_ranges.size() != partition_count)
    throw std::invalid_argument(
        "tigon_kv.partitioning.ranges must contain partition_count ranges");
  auto normalize_boundary = [this](std::string *boundary) {
    if (boundary->empty()) return;
    if (boundary->size() > fixed_key_size)
      throw std::invalid_argument("partition range boundary exceeds fixed_key_size");
    boundary->resize(fixed_key_size, '\0');
  };
  if (!partition_ranges.front().lower_key.empty())
    throw std::invalid_argument("first partition lower_key must be empty");
  if (!partition_ranges.back().upper_key.empty())
    throw std::invalid_argument("last partition upper_key must be empty");
  // A boundary may be written on either adjacent range.  Normalize it once
  // into both sides so all later routing is fixed-width bytewise comparison.
  for (uint32_t partition = 1; partition < partition_count; ++partition) {
    std::string &left = partition_ranges[partition - 1].upper_key;
    std::string &right = partition_ranges[partition].lower_key;
    if (left.empty() && right.empty())
      throw std::invalid_argument("inner partition boundary cannot be empty on both sides");
    if (left.empty()) left = right;
    if (right.empty()) right = left;
    normalize_boundary(&left);
    normalize_boundary(&right);
    const engine::FixedKey left_key = engine::FixedKey::From(left, fixed_key_size);
    const engine::FixedKey right_key = engine::FixedKey::From(right, fixed_key_size);
    if (left_key.Compare(right_key) != 0)
      throw std::invalid_argument("partition ranges must be contiguous half-open intervals");
  }
  for (uint32_t partition = 0; partition < partition_count; ++partition) {
    auto &range = partition_ranges[partition];
    normalize_boundary(&range.lower_key);
    normalize_boundary(&range.upper_key);
    if (IsInternalMaxKey(range.lower_key, fixed_key_size) ||
        IsInternalMaxKey(range.upper_key, fixed_key_size))
      throw std::invalid_argument("partition range boundary reserves internal max sentinel");
    if (partition + 1 != partition_count && range.upper_key.empty())
      throw std::invalid_argument("only final partition may have empty upper_key");
    if (!range.lower_key.empty() && !range.upper_key.empty()) {
      const engine::FixedKey lower =
          engine::FixedKey::From(range.lower_key, fixed_key_size);
      const engine::FixedKey upper =
          engine::FixedKey::From(range.upper_key, fixed_key_size);
      if (lower.Compare(upper) >= 0)
        throw std::invalid_argument("partition range must have lower_key < upper_key");
    }
  }
  if (cpu_affinity && vm_core_count_per_vm != 0 &&
      vm_core_count_per_vm < foreground_worker_count_per_vm + 1)
    throw std::invalid_argument(
        "cpu_affinity requires vm.core_count_per_vm >= foreground workers + demuxer");
  if (hw_cc_budget_mb == 0 || hw_cc_budget_mb > hwcc_size_mb ||
      owner_private_swcc_fraction <= 0.0 || owner_private_swcc_fraction >= 1.0 ||
      node_id >= vm_count ||
      transport_ring_total_mb == 0 || transport_ring_total_mb > hwcc_size_mb ||
      migration_policy != "Clock" || when_to_move_out != "OnDemand" ||
      scc_mechanism != "WriteThrough")
    throw std::invalid_argument("invalid allocator budget fractions");
  // §11.10: config may equal hwcc_size_mb (full physical). Validate only that
  // the configured ceiling exceeds EBR retiring and yields a non-zero per-VM
  // share; Open clamps further to remaining-after-static.
  {
    const uint64_t budget_bytes = hw_cc_budget_mb * 1024ULL * 1024ULL;
    if (budget_bytes <= star::CXL_EBR::max_ebr_retiring_memory)
      throw std::invalid_argument(
          "hw_cc_budget_mb must exceed CXL_EBR::max_ebr_retiring_memory");
    const uint64_t owner_dynamic =
        (budget_bytes - star::CXL_EBR::max_ebr_retiring_memory) / vm_count;
    if (owner_dynamic == 0)
      throw std::invalid_argument(
          "owner migration dynamic HWCC budget underflows to zero");
  }
  const auto &simulation = hardware_simulation;
  const auto is_power_of_two = [](uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
  };
  if (!is_power_of_two(simulation.fixed_latency.cache_line_bytes) ||
      !is_power_of_two(simulation.hwcc_access_count.cache_line_bytes) ||
      !is_power_of_two(simulation.remote_cache_invalidation.cache_line_bytes) ||
      simulation.hwcc_access_count.max_tags == 0 ||
      simulation.atomic_count.max_tags == 0 ||
      simulation.remote_cache_invalidation.max_tags == 0 ||
      simulation.hwcc_access_count.max_tags > latency_sim::kMaxBreakdownTags ||
      simulation.atomic_count.max_tags > latency_sim::kMaxBreakdownTags ||
      simulation.remote_cache_invalidation.max_tags > latency_sim::kMaxBreakdownTags ||
      simulation.remote_cache_invalidation.node_count == 0 ||
      simulation.remote_cache_invalidation.associativity == 0 ||
      simulation.remote_cache_invalidation.cache_instances_per_node == 0 ||
      simulation.remote_cache_invalidation.event_log_capacity == 0 ||
      simulation.remote_cache_invalidation.shared_sequencer_offset % 8 != 0)
    throw std::invalid_argument("invalid hardware simulation geometry");
  for (double value : {simulation.fixed_latency.swcc_fixed_ns_per_line,
                       simulation.fixed_latency.hwcc_fixed_ns_per_line}) {
    if (!std::isfinite(value) || value < 0.0)
      throw std::invalid_argument("fixed latency values must be finite and non-negative");
  }
  if (simulation.remote_cache_invalidation.replacement_policy != "lru" &&
      simulation.remote_cache_invalidation.replacement_policy != "lfu")
    throw std::invalid_argument("remote replacement_policy must be lru or lfu");
  if (simulation.remote_cache_invalidation.capacity_mode != "per_node" &&
      simulation.remote_cache_invalidation.capacity_mode != "total_equal_split" &&
      simulation.remote_cache_invalidation.capacity_mode != "explicit_by_node")
    throw std::invalid_argument("remote capacity_mode is invalid");
  if (simulation.remote_cache_invalidation.lfu_counter_bits == 0 ||
      simulation.remote_cache_invalidation.lfu_counter_bits > 63 ||
      simulation.remote_cache_invalidation.lfu_aging_interval_accesses == 0)
    throw std::invalid_argument("invalid remote LFU configuration");
  if (simulation.fixed_latency.enabled) {
    // This build has no independent merge worker: migration/EBR maintenance
    // runs inside foreground operations. Allowing foreground=false would
    // advertise enabled injection while recording and delaying no accesses.
    if (!simulation.fixed_latency.foreground_enabled)
      throw std::invalid_argument(
          "fixed_latency.enabled=true requires foreground_enabled=true");
    if (verbose)
      throw std::invalid_argument(
        "fixed_latency.enabled=true is incompatible with verbose=true");
    if (extra_check)
      throw std::invalid_argument(
        "fixed_latency.enabled=true is incompatible with extra_check=true");
#if !defined(TIGONKV_CMAKE_BUILD_TYPE)
    throw std::invalid_argument(
        "fixed_latency.enabled=true requires a known RelWithDebInfo build");
#else
    if (std::string_view(TIGONKV_CMAKE_BUILD_TYPE) != "RelWithDebInfo")
      throw std::invalid_argument(
          "fixed_latency.enabled=true is only supported in RelWithDebInfo builds");
#endif
  }
}

uint32_t Config::PartitionForKey(std::string_view key) const {
  const engine::FixedKey fixed_key = engine::FixedKey::From(key, fixed_key_size);
  const auto first_final = partition_ranges.end() - 1;
  const auto it = std::upper_bound(
      partition_ranges.begin(), first_final, fixed_key,
      [this](const engine::FixedKey &needle, const PartitionRange &range) {
        const engine::FixedKey boundary =
            engine::FixedKey::From(range.upper_key, fixed_key_size);
        return needle.Compare(boundary) < 0;
      });
  return static_cast<uint32_t>(it - partition_ranges.begin());
}


std::unique_ptr<KVStore> KVStore::Create(const Config &config, bool reset) {
  auto store = std::unique_ptr<KVStore>(new KVStore(config));
  store->Open(reset);
  return store;
}

KVStore::KVStore(const Config &config)
    : impl_(new Impl()), config_(config) {
  config_.Validate();
  latency_sim::GlobalLatencySimulator().Configure(config_.hardware_simulation);
  latency_sim::GlobalLatencySimulator().SetNodeId(config_.node_id);
}

KVStore::~KVStore() {
  Close();
}

void KVStore::Open(bool reset) {
  impl_->engine = engine::KVEngine::Open(config_, reset);
}

void KVStore::Close() {
  if (impl_ == nullptr || impl_->engine == nullptr) return;
  impl_->engine.reset();
}

uint32_t KVStore::StablePartitionForKey(std::string_view key) const {
  return config_.PartitionForKey(key);
}

uint32_t KVStore::OwnerForKey(std::string_view key) const {
  return StablePartitionForKey(key) % config_.vm_count;
}

void KVStore::ValidateKeyValue(std::string_view key, std::string_view value) const {
  if (key.size() != config_.fixed_key_size ||
      IsInternalMaxKey(key, config_.fixed_key_size) ||
      value.size() != config_.fixed_value_size)
    throw std::invalid_argument("key/value must match configured fixed size");
}

RuntimeStats &KVStore::ThreadRuntime() {
  return CurrentWorkerRuntime();
}

RuntimeStats &KVStore::CurrentWorkerRuntime() {
  if (impl_ == nullptr || impl_->engine == nullptr)
    throw std::runtime_error("foreground KV operation requires an open engine");
  return impl_->engine->CurrentWorkerRuntime();
}

Status KVStore::Put(std::string_view key, std::string_view value) {
  engine::mem_access::LatencyScope latency_scope(
      latency_sim::ScopeKind::kForeground);
  try { ValidateKeyValue(key, value); }
  catch (const std::exception &e) { return Status::Error(StatusCode::kInvalidArgument, e.what()); }
  RuntimeStats &runtime = ThreadRuntime();
  ++runtime.logical_ops;
  Status status = RunWithBusyRetry(this, &runtime, [&] {
    return impl_->engine->Put(key, value);
  });
  if (status.ok()) { ++runtime.commits; ++runtime.private_puts; }
  else ++runtime.aborts;
  return status;
}

GetResult KVStore::Get(std::string_view key) {
  engine::mem_access::LatencyScope latency_scope(
      latency_sim::ScopeKind::kForeground);
  if (key.size() != config_.fixed_key_size ||
      IsInternalMaxKey(key, config_.fixed_key_size))
    return {Status::Error(StatusCode::kInvalidArgument, "invalid key"), {}};
  RuntimeStats &runtime = ThreadRuntime();
  ++runtime.logical_ops;
  GetResult result;
  Status status = RunWithBusyRetry(this, &runtime, [&] {
    result = impl_->engine->Get(key);
    return result.status;
  });
  result.status = status;
  if (result.status.ok()) { ++runtime.commits; ++runtime.private_gets; }
  else if (result.status.code != StatusCode::kNotFound) ++runtime.aborts;
  return result;
}

Status KVStore::Delete(std::string_view key) {
  engine::mem_access::LatencyScope latency_scope(
      latency_sim::ScopeKind::kForeground);
  if (key.size() != config_.fixed_key_size ||
      IsInternalMaxKey(key, config_.fixed_key_size))
    return Status::Error(StatusCode::kInvalidArgument, "invalid key");
  RuntimeStats &runtime = ThreadRuntime();
  ++runtime.logical_ops;
  Status status = RunWithBusyRetry(this, &runtime, [&] {
    return impl_->engine->Delete(key);
  });
  if (status.ok()) { ++runtime.commits; ++runtime.private_deletes; }
  else if (status.code != StatusCode::kNotFound) ++runtime.aborts;
  return status;
}

ScanResult KVStore::Scan(std::string_view start_key, std::string_view end_key,
                         uint64_t limit) {
  engine::mem_access::LatencyScope latency_scope(
      latency_sim::ScopeKind::kForeground);
  if (start_key.size() != config_.fixed_key_size ||
      IsInternalMaxKey(start_key, config_.fixed_key_size) ||
      (!end_key.empty() && end_key.size() != config_.fixed_key_size))
    return {Status::Error(StatusCode::kInvalidArgument, "invalid scan range"), {}};
  // Routing and the tree both use FixedKey::Compare().  Preserve that exact
  // bytewise ordering at the facade instead of introducing a second
  // std::string comparison rule.  An empty/inverted half-open interval is a
  // valid empty scan, matching the engine's lower-bound implementation.
  if (!end_key.empty() &&
      engine::FixedKey::From(start_key, config_.fixed_key_size)
              .Compare(engine::FixedKey::From(end_key, config_.fixed_key_size)) >= 0)
    return {Status::Ok(), {}};
  RuntimeStats &runtime = ThreadRuntime();
  ++runtime.logical_ops;
  ++runtime.scan_ops;
  ScanResult result;
  Status status = RunWithBusyRetry(this, &runtime, [&] {
    result = impl_->engine->Scan(start_key, end_key, limit);
    return result.status;
  });
  result.status = status;
  if (result.status.ok()) {
    ++runtime.commits;
    runtime.scan_rows_returned += result.items.size();
  } else {
    ++runtime.aborts;
  }
  return result;
}

CasResult KVStore::CompareExchange(std::string_view key, std::string_view expected,
                                   std::string_view desired) {
  engine::mem_access::LatencyScope latency_scope(
      latency_sim::ScopeKind::kForeground);
  try { ValidateKeyValue(key, desired); }
  catch (const std::exception &e) { return {Status::Error(StatusCode::kInvalidArgument, e.what()), false}; }
  if (!expected.empty() && expected.size() != config_.fixed_value_size)
    return {Status::Error(StatusCode::kInvalidArgument,
                          "CAS expected value must be empty or fixed size"), false};
  RuntimeStats &runtime = ThreadRuntime();
  ++runtime.logical_ops;
  CasResult result;
  Status status = RunWithBusyRetry(this, &runtime, [&] {
    result = impl_->engine->CompareExchange(key, expected, desired);
    return result.status;
  });
  result.status = status;
  if (result.status.ok()) { ++runtime.commits; ++runtime.private_puts; }
  else if (result.status.code == StatusCode::kCompareFailed) ++runtime.aborts;
  else if (result.status.code != StatusCode::kNotFound) ++runtime.aborts;
  return result;
}

IncrementResult KVStore::Increment(std::string_view key, int64_t delta) {
  engine::mem_access::LatencyScope latency_scope(
      latency_sim::ScopeKind::kForeground);
  if (key.size() != config_.fixed_key_size ||
      IsInternalMaxKey(key, config_.fixed_key_size))
    return {Status::Error(StatusCode::kInvalidArgument, "invalid key"), 0};
  RuntimeStats &runtime = ThreadRuntime();
  ++runtime.logical_ops;
  IncrementResult result;
  Status status = RunWithBusyRetry(this, &runtime, [&] {
    result = impl_->engine->Increment(key, delta);
    return result.status;
  });
  result.status = status;
  if (result.status.ok()) { ++runtime.commits; ++runtime.private_puts; }
  else ++runtime.aborts;
  return result;
}

Status KVStore::PollTransport() {
  if (impl_ == nullptr || impl_->engine == nullptr)
    return Status::Error(StatusCode::kCorruption, "KVStore is closed");
  impl_->engine->PollTransport();
  return Status::Ok();
}

void KVStore::BindWorker(uint32_t worker_id) {
  if (impl_ == nullptr || impl_->engine == nullptr)
    throw std::runtime_error("BindWorker requires an open KVStore");
  if (worker_id >= config_.foreground_worker_count_per_vm)
    throw std::invalid_argument("BindWorker worker_id exceeds foreground worker count");
  impl_->engine->BindWorker(worker_id);
}

void KVStore::ReleaseWorker() {
  if (impl_ == nullptr || impl_->engine == nullptr)
    throw std::runtime_error("ReleaseWorker requires an open KVStore");
  impl_->engine->ReleaseWorker();
}

MemoryStats KVStore::Memory() const { return impl_->engine->Memory(); }

RuntimeStats KVStore::Runtime() const {
  if (impl_ == nullptr || impl_->engine == nullptr) return {};
  return impl_->engine->EngineRuntime();
}

std::string KVStore::DumpStats() const {
  const MemoryStats memory = Memory();
  const RuntimeStats runtime = Runtime();
  std::string out = "TIGONKV_MEMORY_STATS\n";
  out += "allocator_mode=" + memory.allocator_mode + "\n";
  out += "physical_region_split=" + std::to_string(memory.physical_region_split) + "\n";
  out += "total_pool_capacity_bytes=" + std::to_string(memory.total_pool_capacity_bytes) + "\n";
  out += "logical_hwcc_capacity_bytes=" + std::to_string(memory.logical_hwcc_capacity_bytes) + "\n";
  out += "logical_swcc_capacity_bytes=" + std::to_string(memory.logical_swcc_capacity_bytes) + "\n";
  out += "logical_hwcc_used_bytes=" + std::to_string(memory.logical_hwcc_used_bytes) + "\n";
  out += "physical_hwcc_used_bytes=" + std::to_string(memory.physical_hwcc_used_bytes) + "\n";
  out += "physical_swcc_used_bytes=" + std::to_string(memory.physical_swcc_used_bytes) + "\n";
  out += "owner_private_swcc_used_bytes=" + std::to_string(memory.owner_private_swcc_used_bytes) + "\n";
  out += "shared_payload_swcc_used_bytes=" + std::to_string(memory.shared_payload_swcc_used_bytes) + "\n";
  out += "allocator_hwcc_metadata_bytes=" + std::to_string(memory.allocator_hwcc_metadata_bytes) + "\n";
  out += "allocator_swcc_metadata_bytes=" + std::to_string(memory.allocator_swcc_metadata_bytes) + "\n";
  out += "allocator_shared_overhead_bytes=" + std::to_string(memory.allocator_shared_overhead_bytes) + "\n";
  out += "allocator_local_dram_bytes=" + std::to_string(memory.allocator_local_dram_bytes) + "\n";
  out += "unclassified_shared_bytes=" + std::to_string(memory.unclassified_shared_bytes) + "\n";
  out += "physical_hwcc_capacity_bytes=" +
         std::to_string(memory.physical_hwcc_capacity_bytes) + "\n";
  out += "owner_migration_dynamic_budget_bytes=" +
         std::to_string(memory.owner_migration_dynamic_budget_bytes) + "\n";
  out += "rss_kb=" + std::to_string(memory.rss_kb) + "\n";
  // §11.11: disclose foreground vs service threads; do not hide the demuxer.
  out += "foreground_worker_count_per_vm=" +
         std::to_string(config_.foreground_worker_count_per_vm) + "\n";
  out += "inbound_demuxer_threads=1\n";
  out += "kv_threads_per_vm=" +
         std::to_string(config_.foreground_worker_count_per_vm + 1) + "\n";
  out += "TIGONKV_RUNTIME_STATS\nnode=" + std::to_string(config_.node_id) + "\n";
  out += "logical_ops=" + std::to_string(runtime.logical_ops) + "\n";
  out += "commits=" + std::to_string(runtime.commits) + "\n";
  out += "aborts=" + std::to_string(runtime.aborts) + "\n";
  out += "retries=" + std::to_string(runtime.retries) + "\n";
  out += "private_gets=" + std::to_string(runtime.private_gets) + "\n";
  out += "private_puts=" + std::to_string(runtime.private_puts) + "\n";
  out += "private_deletes=" + std::to_string(runtime.private_deletes) + "\n";
  out += "private_swcc_flushes=" + std::to_string(runtime.private_swcc_flushes) + "\n";
  out += "shared_gets=" + std::to_string(runtime.shared_gets) + "\n";
  out += "shared_puts=" + std::to_string(runtime.shared_puts) + "\n";
  out += "shared_deletes=" + std::to_string(runtime.shared_deletes) + "\n";
  out += "shared_swcc_flushes=" + std::to_string(runtime.shared_swcc_flushes) + "\n";
  out += "migration_in=" + std::to_string(runtime.migration_in) + "\n";
  out += "migration_out=" + std::to_string(runtime.migration_out) + "\n";
  out += "network_tx_bytes=" + std::to_string(runtime.network_tx_bytes) + "\n";
  out += "network_rx_bytes=" + std::to_string(runtime.network_rx_bytes) + "\n";
  out += "scan_rows_returned=" + std::to_string(runtime.scan_rows_returned) + "\n";
  out += "scan_ops=" + std::to_string(runtime.scan_ops) + "\n";
  out += "scan_partition_probes=" +
         std::to_string(runtime.scan_partition_probes) + "\n";
  out += "scan_migrate_rpcs=" + std::to_string(runtime.scan_migrate_rpcs) + "\n";
  const latency_sim::Stats latency = latency_sim::GlobalLatencySimulator().SnapshotStats();
  const auto &simulation = latency_sim::GlobalLatencySimulator().config();
  out += "TIGONKV_HARDWARE_SIM_STATS\nfeatures=" +
      std::to_string(latency_sim::GlobalLatencySimulator().feature_mask()) +
      "\nfixed_latency_enabled=" +
      std::to_string(simulation.fixed_latency.enabled) +
      "\nhwcc_access_count_enabled=" +
      std::to_string(simulation.hwcc_access_count.enabled) +
      "\natomic_count_enabled=" +
      std::to_string(simulation.atomic_count.enabled) +
      "\nremote_cache_invalidation_enabled=" +
      std::to_string(simulation.remote_cache_invalidation.enabled) +
      "\nhwcc_read_ops=" + std::to_string(latency.hwcc_read_ops) +
      "\nhwcc_write_ops=" + std::to_string(latency.hwcc_write_ops) +
      "\nhwcc_read_lines=" + std::to_string(latency.hwcc_read_lines) +
      "\nhwcc_write_lines=" + std::to_string(latency.hwcc_write_lines) +
      "\nhwcc_read_bytes=" + std::to_string(latency.hwcc_read_bytes) +
      "\nhwcc_write_bytes=" + std::to_string(latency.hwcc_write_bytes) +
      "\nhwcc_atomic_ops=" + std::to_string(latency.hwcc_atomic_ops) +
      "\nhwcc_cas_attempts=" + std::to_string(latency.hwcc_cas_attempts) +
      "\nhwcc_cas_successes=" + std::to_string(latency.hwcc_cas_successes) +
      "\nhwcc_cas_failures=" + std::to_string(latency.hwcc_cas_failures) +
      "\nowner_private_swcc_atomic_ops=" +
      std::to_string(latency.owner_private_swcc_atomic_ops) +
      "\nlocal_dram_atomic_ops=" +
      std::to_string(latency.local_dram_atomic_ops) +
      "\nremote_dirty_handoffs=" +
      std::to_string(latency.remote_dirty_handoffs) +
      "\nremote_clean_copy_invalidations=" +
      std::to_string(latency.remote_clean_copy_invalidations) +
      "\nremote_write_transactions_causing_invalidation=" +
      std::to_string(latency.remote_write_transactions_causing_invalidation) +
      "\nremote_dirty_capacity_evictions=" +
      std::to_string(latency.remote_dirty_capacity_evictions) +
      "\nremote_swcc_explicit_handoffs=" +
      std::to_string(latency.remote_swcc_explicit_handoffs) +
      "\nremote_events=" + std::to_string(latency.remote_events) +
      "\nswcc_delayed_ns=" + std::to_string(latency.swcc_delayed_ns) +
      "\nhwcc_delayed_ns=" + std::to_string(latency.hwcc_delayed_ns) +
      "\ndelayed_ns=" + std::to_string(latency.TotalDelayedNs()) + "\n";
  return out;
}

}  // namespace tigonkv
