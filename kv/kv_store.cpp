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
// Match e2e_trace_runner: Busy is contention, retry at the logical op boundary
// so YCSB and API callers share one contract with the engine's inner budgets.
constexpr int kStoreBusyRetryBudget = 64;
thread_local KVStore *TlsRuntimeOwner = nullptr;
thread_local RuntimeStats *TlsRuntimeStats = nullptr;

bool IsInternalMaxKey(std::string_view key) {
  return !key.empty() && std::all_of(key.begin(), key.end(),
                                     [](char byte) {
                                       return static_cast<unsigned char>(byte) == 0xff;
                                     });
}

template <typename Op>
Status RunWithBusyRetry(KVStore *store, Op &&op) {
  Status status;
  for (int attempt = 0; attempt < kStoreBusyRetryBudget; ++attempt) {
    status = op();
    if (status.code != StatusCode::kBusy) return status;
    if (store != nullptr) store->PollTransport();
    std::this_thread::yield();
  }
  return status;
}

void AddRuntimeStats(RuntimeStats *total, const RuntimeStats &part) {
  total->logical_ops += part.logical_ops;
  total->commits += part.commits;
  total->aborts += part.aborts;
  total->retries += part.retries;
  total->private_gets += part.private_gets;
  total->private_puts += part.private_puts;
  total->private_deletes += part.private_deletes;
  total->private_swcc_flushes += part.private_swcc_flushes;
  total->shared_gets += part.shared_gets;
  total->shared_puts += part.shared_puts;
  total->shared_deletes += part.shared_deletes;
  total->shared_swcc_flushes += part.shared_swcc_flushes;
  total->migration_in += part.migration_in;
  total->migration_out += part.migration_out;
  total->network_tx_bytes += part.network_tx_bytes;
  total->network_rx_bytes += part.network_rx_bytes;
  total->scan_rows_returned += part.scan_rows_returned;
  total->scan_ops += part.scan_ops;
  total->scan_partition_probes += part.scan_partition_probes;
  total->scan_migrate_rpcs += part.scan_migrate_rpcs;
  total->scan_owner_rows_movein_attempted +=
      part.scan_owner_rows_movein_attempted;
  total->deferred_queue_peak =
      std::max(total->deferred_queue_peak, part.deferred_queue_peak);
  total->abandoned_responses += part.abandoned_responses;
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

  const auto &tigon = RequireUniqueMember(root, "tigon_kv", "$");
  if (tigon.kind != JsonValueKind::kObject)
    throw std::invalid_argument("config field must be object: tigon_kv");
  if (CountJsonKey(text, "tigon_kv") != 1)
    throw std::invalid_argument("tigon_kv must appear exactly once");
  const auto tigon_members =
      ParseJsonObjectMembers(text, tigon.value_begin);

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
  const auto latency_members =
      ParseJsonObjectMembers(text, latency.value_begin);

  static const std::unordered_set<std::string> allowed = {
      "enabled", "foreground_enabled", "merge_enabled", "stats_enabled",
      "cache_line_bytes", "swcc_read_ns_per_line",
      "swcc_write_ns_per_line", "swcc_flush_ns_per_line",
      "hwcc_read_ns_per_line", "hwcc_write_ns_per_line",
      "hwcc_atomic_load_ns", "hwcc_atomic_store_ns",
      "hwcc_atomic_rmw_ns", "cache_model", "cache_hits_enabled",
      "cache_capacity_lines", "cache_associativity",
      "cache_fixed_hit_rate", "cache_hit_extra_ns"};
  for (const auto &member : latency_members) {
    if (!allowed.count(member.key))
      throw std::invalid_argument(
          "unknown tigon_kv.latency_inject field: " + member.key);
  }
  for (const auto &name : allowed) {
    if (CountJsonKey(text, name) != 1)
      throw std::invalid_argument(
          "latency field must appear exactly once and only under "
          "tigon_kv.latency_inject: " +
          name);
  }

  const auto field = [&](std::string_view name) -> const JsonMemberSpan & {
    return RequireUniqueMember(latency_members, name,
                               "tigon_kv.latency_inject");
  };
  config->latency_enabled =
      ParseStrictBool(text, field("enabled"),
                      "tigon_kv.latency_inject.enabled");
  config->latency_foreground_enabled =
      ParseStrictBool(text, field("foreground_enabled"),
                      "tigon_kv.latency_inject.foreground_enabled");
  config->latency_merge_enabled =
      ParseStrictBool(text, field("merge_enabled"),
                      "tigon_kv.latency_inject.merge_enabled");
  config->latency_stats_enabled =
      ParseStrictBool(text, field("stats_enabled"),
                      "tigon_kv.latency_inject.stats_enabled");
  config->latency_cache_line_bytes =
      ParseStrictUint64(text, field("cache_line_bytes"),
                        "tigon_kv.latency_inject.cache_line_bytes");
  config->swcc_read_ns =
      ParseStrictDouble(text, field("swcc_read_ns_per_line"),
                        "tigon_kv.latency_inject.swcc_read_ns_per_line");
  config->swcc_write_ns =
      ParseStrictDouble(text, field("swcc_write_ns_per_line"),
                        "tigon_kv.latency_inject.swcc_write_ns_per_line");
  config->swcc_flush_ns =
      ParseStrictDouble(text, field("swcc_flush_ns_per_line"),
                        "tigon_kv.latency_inject.swcc_flush_ns_per_line");
  config->hwcc_read_ns =
      ParseStrictDouble(text, field("hwcc_read_ns_per_line"),
                        "tigon_kv.latency_inject.hwcc_read_ns_per_line");
  config->hwcc_write_ns =
      ParseStrictDouble(text, field("hwcc_write_ns_per_line"),
                        "tigon_kv.latency_inject.hwcc_write_ns_per_line");
  config->hwcc_atomic_load_ns =
      ParseStrictDouble(text, field("hwcc_atomic_load_ns"),
                        "tigon_kv.latency_inject.hwcc_atomic_load_ns");
  config->hwcc_atomic_store_ns =
      ParseStrictDouble(text, field("hwcc_atomic_store_ns"),
                        "tigon_kv.latency_inject.hwcc_atomic_store_ns");
  config->hwcc_atomic_rmw_ns =
      ParseStrictDouble(text, field("hwcc_atomic_rmw_ns"),
                        "tigon_kv.latency_inject.hwcc_atomic_rmw_ns");
  config->latency_cache_model =
      ParseStrictString(text, field("cache_model"),
                        "tigon_kv.latency_inject.cache_model");
  config->latency_cache_hits_enabled =
      ParseStrictBool(text, field("cache_hits_enabled"),
                      "tigon_kv.latency_inject.cache_hits_enabled");
  config->latency_cache_capacity_lines =
      ParseStrictUint64(text, field("cache_capacity_lines"),
                        "tigon_kv.latency_inject.cache_capacity_lines");
  config->latency_cache_associativity =
      ParseStrictUint64(text, field("cache_associativity"),
                        "tigon_kv.latency_inject.cache_associativity");
  config->latency_cache_fixed_hit_rate =
      ParseStrictDouble(text, field("cache_fixed_hit_rate"),
                        "tigon_kv.latency_inject.cache_fixed_hit_rate");
  config->latency_cache_hit_extra_ns =
      ParseStrictDouble(text, field("cache_hit_extra_ns"),
                        "tigon_kv.latency_inject.cache_hit_extra_ns");
  config->hwcc_atomic_ns =
      std::max({config->hwcc_atomic_load_ns, config->hwcc_atomic_store_ns,
                config->hwcc_atomic_rmw_ns});
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
  static const std::unordered_set<std::string> allowed = {"strategy", "ranges"};
  for (const auto &member : partitioning_members) {
    if (!allowed.count(member.key))
      throw std::invalid_argument(
          "unknown tigon_kv.partitioning field: " + member.key);
  }
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
    static const std::unordered_set<std::string> range_allowed = {
        "lower_key", "upper_key"};
    for (const auto &member : members) {
      if (!range_allowed.count(member.key))
        throw std::invalid_argument(
            "unknown tigon_kv.partitioning.ranges field: " + member.key);
    }
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
      "latency_inject", "enabled", "foreground_enabled", "merge_enabled",
      "stats_enabled", "cache_line_bytes", "swcc_read_ns_per_line",
      "swcc_write_ns_per_line", "swcc_flush_ns_per_line", "hwcc_read_ns_per_line",
      "hwcc_write_ns_per_line", "hwcc_atomic_load_ns", "hwcc_atomic_store_ns",
      "hwcc_atomic_rmw_ns", "cache_model", "cache_hits_enabled",
      "cache_capacity_lines", "cache_associativity", "cache_fixed_hit_rate",
      "cache_hit_extra_ns"};
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
  if (size_mb == 0 || hwcc_size_mb > 1024 || hwcc_size_mb > size_mb ||
      swcc_size_mb > size_mb || hwcc_offset_mb + hwcc_size_mb > size_mb ||
      swcc_offset_mb + swcc_size_mb > size_mb || fixed_value_size == 0 ||
      (hwcc_offset_mb < swcc_offset_mb + swcc_size_mb &&
       swcc_offset_mb < hwcc_offset_mb + hwcc_size_mb))
    throw std::invalid_argument("invalid shared-memory capacity or HWCC budget");
  if (vm_count == 0 || partition_count < vm_count ||
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
    const engine::FixedKey key = engine::FixedKey::From(*boundary, fixed_key_size);
    boundary->assign(key.bytes, fixed_key_size);
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
    if (IsInternalMaxKey(range.lower_key) || IsInternalMaxKey(range.upper_key))
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
  if (latency_cache_model != "none" && latency_cache_model != "fixed_hit_rate" &&
      latency_cache_model != "per_thread_lru")
    throw std::invalid_argument("unknown latency cache model");
  if (latency_cache_fixed_hit_rate < 0.0 || latency_cache_fixed_hit_rate > 1.0)
    throw std::invalid_argument("latency cache hit rate must be in [0,1]");
  if (latency_cache_line_bytes != 64 || latency_cache_capacity_lines == 0 ||
      latency_cache_associativity == 0)
    throw std::invalid_argument("invalid latency cache geometry");
  if (!std::isfinite(swcc_read_ns) || swcc_read_ns < 0 ||
      !std::isfinite(swcc_write_ns) || swcc_write_ns < 0 ||
      !std::isfinite(swcc_flush_ns) || swcc_flush_ns < 0 ||
      !std::isfinite(hwcc_read_ns) || hwcc_read_ns < 0 ||
      !std::isfinite(hwcc_write_ns) || hwcc_write_ns < 0 ||
      !std::isfinite(hwcc_atomic_load_ns) || hwcc_atomic_load_ns < 0 ||
      !std::isfinite(hwcc_atomic_store_ns) || hwcc_atomic_store_ns < 0 ||
      !std::isfinite(hwcc_atomic_rmw_ns) || hwcc_atomic_rmw_ns < 0 ||
      !std::isfinite(latency_cache_hit_extra_ns) ||
      latency_cache_hit_extra_ns < 0)
    throw std::invalid_argument("latency values must be finite and non-negative");
  if (latency_enabled) {
    // This build has no independent merge worker: migration/EBR maintenance
    // runs inside foreground operations. Allowing foreground=false would
    // advertise enabled injection while recording and delaying no accesses.
    if (!latency_foreground_enabled)
      throw std::invalid_argument(
          "latency_inject.enabled=true requires foreground_enabled=true");
    if (verbose)
      throw std::invalid_argument(
          "latency_inject.enabled=true is incompatible with verbose=true");
    if (extra_check)
      throw std::invalid_argument(
          "latency_inject.enabled=true is incompatible with extra_check=true");
#if !defined(TIGONKV_CMAKE_BUILD_TYPE)
    throw std::invalid_argument(
        "latency_inject.enabled=true requires a known RelWithDebInfo build");
#else
    if (std::string_view(TIGONKV_CMAKE_BUILD_TYPE) != "RelWithDebInfo")
      throw std::invalid_argument(
          "latency_inject.enabled=true is only supported in RelWithDebInfo builds");
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
    : impl_(new Impl()), config_(config),
      worker_runtime_(config.foreground_worker_count_per_vm) {
  config_.Validate();
  latency_sim::Config latency;
  latency.enabled = config_.latency_enabled;
  latency.foreground_enabled = config_.latency_foreground_enabled;
  latency.merge_enabled = config_.latency_merge_enabled;
  latency.stats_enabled = config_.latency_stats_enabled;
  latency.cache_line_bytes = config_.latency_cache_line_bytes;
  latency.swcc_read_ns_per_line = config_.swcc_read_ns;
  latency.swcc_write_ns_per_line = config_.swcc_write_ns;
  latency.swcc_flush_ns_per_line = config_.swcc_flush_ns;
  latency.hwcc_read_ns_per_line = config_.hwcc_read_ns;
  latency.hwcc_write_ns_per_line = config_.hwcc_write_ns;
  latency.hwcc_atomic_load_ns = config_.hwcc_atomic_load_ns;
  latency.hwcc_atomic_store_ns = config_.hwcc_atomic_store_ns;
  latency.hwcc_atomic_rmw_ns = config_.hwcc_atomic_rmw_ns;
  latency.cache_model = latency_sim::ParseCacheModel(config_.latency_cache_model);
  latency.cache_hits_enabled = config_.latency_cache_hits_enabled;
  latency.cache_capacity_lines = config_.latency_cache_capacity_lines;
  latency.cache_associativity = config_.latency_cache_associativity;
  latency.cache_fixed_hit_rate = config_.latency_cache_fixed_hit_rate;
  latency.cache_hit_extra_ns = config_.latency_cache_hit_extra_ns;
  latency_sim::GlobalLatencySimulator().Configure(latency);
}

KVStore::~KVStore() {
  if (TlsRuntimeOwner == this) {
    TlsRuntimeOwner = nullptr;
    TlsRuntimeStats = nullptr;
  }
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
  if (key.size() != config_.fixed_key_size || IsInternalMaxKey(key) ||
      value.size() != config_.fixed_value_size)
    throw std::invalid_argument("key/value must match configured fixed size");
}

RuntimeStats &KVStore::ThreadRuntime() {
  if (TlsRuntimeOwner == this && TlsRuntimeStats != nullptr) return *TlsRuntimeStats;
  // Single-threaded API users are not required to call BindWorker. Multi-worker
  // harnesses bind every worker and therefore never share this fallback slot.
  return unbound_runtime_.stats;
}

Status KVStore::Put(std::string_view key, std::string_view value) {
  engine::mem_access::LatencyScope latency_scope(
      latency_sim::ScopeKind::kForeground);
  impl_->engine->PollTransport();
  try { ValidateKeyValue(key, value); }
  catch (const std::exception &e) { return Status::Error(StatusCode::kInvalidArgument, e.what()); }
  RuntimeStats &runtime = ThreadRuntime();
  ++runtime.logical_ops;
  Status status = RunWithBusyRetry(this, [&] {
    return impl_->engine->Put(key, value);
  });
  if (status.ok()) { ++runtime.commits; ++runtime.private_puts; }
  else ++runtime.aborts;
  return status;
}

GetResult KVStore::Get(std::string_view key) {
  engine::mem_access::LatencyScope latency_scope(
      latency_sim::ScopeKind::kForeground);
  impl_->engine->PollTransport();
  if (key.size() != config_.fixed_key_size || IsInternalMaxKey(key))
    return {Status::Error(StatusCode::kInvalidArgument, "invalid key"), {}};
  RuntimeStats &runtime = ThreadRuntime();
  ++runtime.logical_ops;
  GetResult result;
  Status status = RunWithBusyRetry(this, [&] {
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
  impl_->engine->PollTransport();
  if (key.size() != config_.fixed_key_size || IsInternalMaxKey(key))
    return Status::Error(StatusCode::kInvalidArgument, "invalid key");
  RuntimeStats &runtime = ThreadRuntime();
  ++runtime.logical_ops;
  Status status = RunWithBusyRetry(this, [&] {
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
  impl_->engine->PollTransport();
  if (start_key.size() != config_.fixed_key_size || IsInternalMaxKey(start_key) ||
      (!end_key.empty() && end_key.size() != config_.fixed_key_size))
    return {Status::Error(StatusCode::kInvalidArgument, "invalid scan range"), {}};
  if (!end_key.empty() && !IsInternalMaxKey(end_key) && start_key >= end_key)
    return {Status::Error(StatusCode::kInvalidArgument,
                          "scan range is empty or inverted"), {}};
  RuntimeStats &runtime = ThreadRuntime();
  ++runtime.logical_ops;
  ++runtime.scan_ops;
  ScanResult result;
  Status status = RunWithBusyRetry(this, [&] {
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
  impl_->engine->PollTransport();
  try { ValidateKeyValue(key, desired); }
  catch (const std::exception &e) { return {Status::Error(StatusCode::kInvalidArgument, e.what()), false}; }
  if (!expected.empty() && expected.size() != config_.fixed_value_size)
    return {Status::Error(StatusCode::kInvalidArgument,
                          "CAS expected value must be empty or fixed size"), false};
  RuntimeStats &runtime = ThreadRuntime();
  ++runtime.logical_ops;
  CasResult result;
  Status status = RunWithBusyRetry(this, [&] {
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
  impl_->engine->PollTransport();
  if (key.size() != config_.fixed_key_size || IsInternalMaxKey(key))
    return {Status::Error(StatusCode::kInvalidArgument, "invalid key"), 0};
  RuntimeStats &runtime = ThreadRuntime();
  ++runtime.logical_ops;
  IncrementResult result;
  Status status = RunWithBusyRetry(this, [&] {
    result = impl_->engine->Increment(key, delta);
    return result.status;
  });
  result.status = status;
  if (result.status.ok()) { ++runtime.commits; ++runtime.private_puts; }
  else ++runtime.aborts;
  return result;
}

Status KVStore::PollTransport() {
  engine::mem_access::LatencyScope latency_scope(
      latency_sim::ScopeKind::kForeground);
  if (impl_ == nullptr || impl_->engine == nullptr)
    return Status::Error(StatusCode::kCorruption, "KVStore is closed");
  impl_->engine->PollTransport();
  return Status::Ok();
}

void KVStore::BindWorker(uint32_t worker_id) {
  if (impl_ == nullptr || impl_->engine == nullptr)
    throw std::runtime_error("BindWorker requires an open KVStore");
  if (worker_id >= worker_runtime_.size())
    throw std::invalid_argument("BindWorker worker_id exceeds foreground worker count");
  impl_->engine->BindWorker(worker_id);
  TlsRuntimeOwner = this;
  TlsRuntimeStats = &worker_runtime_[worker_id].stats;
}

void KVStore::ReleaseWorker() {
  if (impl_ == nullptr || impl_->engine == nullptr)
    throw std::runtime_error("ReleaseWorker requires an open KVStore");
  impl_->engine->ReleaseWorker();
  if (TlsRuntimeOwner == this) {
    TlsRuntimeOwner = nullptr;
    TlsRuntimeStats = nullptr;
  }
}

MemoryStats KVStore::Memory() const { return impl_->engine->Memory(); }

RuntimeStats KVStore::Runtime() const {
  RuntimeStats stats;
  AddRuntimeStats(&stats, unbound_runtime_.stats);
  for (const WorkerRuntime &worker : worker_runtime_)
    AddRuntimeStats(&stats, worker.stats);
  if (impl_ != nullptr && impl_->engine != nullptr) {
    const RuntimeStats engine = impl_->engine->EngineRuntime();
    stats.shared_gets += engine.shared_gets;
    stats.shared_puts += engine.shared_puts;
    stats.shared_deletes += engine.shared_deletes;
    stats.shared_swcc_flushes += engine.shared_swcc_flushes;
    stats.migration_in += engine.migration_in;
    stats.migration_out += engine.migration_out;
    stats.abandoned_responses += engine.abandoned_responses;
    stats.scan_partition_probes += engine.scan_partition_probes;
    stats.scan_migrate_rpcs += engine.scan_migrate_rpcs;
    stats.scan_owner_rows_movein_attempted +=
        engine.scan_owner_rows_movein_attempted;
    stats.deferred_queue_peak =
        std::max(stats.deferred_queue_peak, engine.deferred_queue_peak);
    stats.network_tx_bytes = engine.network_tx_bytes;
    stats.network_rx_bytes = engine.network_rx_bytes;
  }
  return stats;
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
  out += "retired_pending_bytes=" + std::to_string(memory.retired_pending_bytes) + "\n";
  out += "reclaimed_total_bytes=" + std::to_string(memory.reclaimed_total_bytes) + "\n";
  out += "active_shared_rows=" + std::to_string(memory.active_shared_rows) + "\n";
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
  out += "scan_owner_rows_movein_attempted=" +
         std::to_string(runtime.scan_owner_rows_movein_attempted) + "\n";
  out += "deferred_queue_peak=" +
         std::to_string(runtime.deferred_queue_peak) + "\n";
  out += "abandoned_responses=" + std::to_string(runtime.abandoned_responses) + "\n";
  const latency_sim::Stats latency = latency_sim::GlobalLatencySimulator().SnapshotStats();
  const auto ratio = [](uint64_t hits, uint64_t misses) {
    const uint64_t total = hits + misses;
    return total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
  };
  out += "TIGONKV_LATENCY_STATS\nswcc_raw=" +
      std::to_string(latency.swcc_raw_line_accesses) +
      "\nhwcc_raw=" + std::to_string(latency.hwcc_raw_line_accesses) +
      "\nswcc_hits=" + std::to_string(latency.swcc_cache_hits) +
      "\nhwcc_hits=" + std::to_string(latency.hwcc_cache_hits) +
      "\nswcc_misses=" + std::to_string(latency.swcc_cache_misses) +
      "\nhwcc_misses=" + std::to_string(latency.hwcc_cache_misses) +
      "\nswcc_hit_ratio=" + std::to_string(ratio(latency.swcc_cache_hits, latency.swcc_cache_misses)) +
      "\nhwcc_hit_ratio=" + std::to_string(ratio(latency.hwcc_cache_hits, latency.hwcc_cache_misses)) +
      "\nswcc_delayed_ns=" + std::to_string(latency.swcc_delayed_ns) +
      "\nhwcc_delayed_ns=" + std::to_string(latency.hwcc_delayed_ns) +
      "\ndelayed_ns=" + std::to_string(latency.TotalDelayedNs()) +
      "\ncache_model=" + latency_sim::CacheModelName(latency_sim::GlobalLatencySimulator().config().cache_model) +
      "\ncache_hits_enabled=" + std::to_string(latency_sim::GlobalLatencySimulator().config().cache_hits_enabled) + "\n";
  return out;
}

}  // namespace tigonkv
