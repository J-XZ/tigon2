#include "tools/e2e_trace_format.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

uint64_t ParseUnsigned(std::string_view text, std::string_view label) {
  size_t position = 0;
  const uint64_t value = tigonkv::e2e_trace::ParseDecimal(text, &position, label);
  if (position != text.size())
    throw std::runtime_error("invalid " + std::string(label));
  return value;
}

std::string JsonEscape(std::string_view value) {
  std::string escaped;
  for (unsigned char byte : value) {
    if (byte == '\\' || byte == '"') escaped += '\\';
    if (byte == '\\' || byte == '"') escaped += static_cast<char>(byte);
    else if (byte >= 0x20) escaped += static_cast<char>(byte);
    else {
      static constexpr char kHex[] = "0123456789abcdef";
      escaped += "\\u00";
      escaped += kHex[byte >> 4];
      escaped += kHex[byte & 0xf];
    }
  }
  return escaped;
}

size_t MatchArrayEnd(const std::string &text, size_t open) {
  size_t depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (size_t i = open; i < text.size(); ++i) {
    const char c = text[i];
    if (in_string) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') in_string = false;
      continue;
    }
    if (c == '"') in_string = true;
    else if (c == '[') ++depth;
    else if (c == ']' && --depth == 0) return i;
  }
  throw std::runtime_error("unterminated partition ranges array");
}

uint64_t Fnv1a(std::string_view value, uint64_t hash = 1469598103934665603ULL) {
  for (unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

void ReplacePartitioning(const std::string &config_path,
                         const std::vector<std::string> &boundaries) {
  std::ifstream input(config_path);
  if (!input) throw std::runtime_error("cannot open config: " + config_path);
  std::string text((std::istreambuf_iterator<char>(input)),
                   std::istreambuf_iterator<char>());
  const size_t tigon = text.find("\"tigon_kv\"");
  const size_t partition_count = text.find("\"partition_count\"", tigon);
  const size_t count_colon = text.find(':', partition_count);
  const size_t count_begin = text.find_first_of("0123456789", count_colon + 1);
  const size_t count_end = text.find_first_not_of("0123456789", count_begin);
  if (tigon == std::string::npos || partition_count == std::string::npos ||
      count_colon == std::string::npos || count_begin == std::string::npos)
    throw std::runtime_error("cannot locate tigon_kv.partition_count");
  text.replace(count_begin, count_end - count_begin, "4");

  const size_t partitioning = text.find("\"partitioning\"", tigon);
  const size_t ranges = text.find("\"ranges\"", partitioning);
  const size_t array_begin = text.find('[', ranges);
  if (partitioning == std::string::npos || ranges == std::string::npos ||
      array_begin == std::string::npos)
    throw std::runtime_error("cannot locate tigon_kv.partitioning.ranges");
  const size_t array_end = MatchArrayEnd(text, array_begin);
  std::string replacement = "[\n";
  for (size_t partition = 0; partition < 4; ++partition) {
    const std::string_view lower = partition == 0 ? std::string_view{} :
                                                   std::string_view(boundaries[partition - 1]);
    const std::string_view upper = partition == 3 ? std::string_view{} :
                                                   std::string_view(boundaries[partition]);
    replacement += "        {\"lower_key\": \"" + JsonEscape(lower) +
                   "\", \"upper_key\": \"" + JsonEscape(upper) + "\"}";
    replacement += partition == 3 ? "\n      " : ",\n";
  }
  replacement += "]";
  text.replace(array_begin, array_end - array_begin + 1, replacement);
  std::ofstream output(config_path, std::ios::trunc);
  if (!output) throw std::runtime_error("cannot write config: " + config_path);
  output << text;
}

}  // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 9 || std::string_view(argv[1]) != "--trace-dir" ||
        std::string_view(argv[3]) != "--config" ||
        std::string_view(argv[5]) != "--workers" ||
        std::string_view(argv[7]) != "--fixed-key-size")
      throw std::runtime_error(
          "usage: ycsb_partition_splits --trace-dir DIR --config FILE "
          "--workers N --fixed-key-size N");
    const std::string trace_dir = argv[2];
    const std::string config_path = argv[4];
    const uint64_t workers = ParseUnsigned(argv[6], "workers");
    const uint32_t fixed_key_size = static_cast<uint32_t>(
        ParseUnsigned(argv[8], "fixed-key-size"));
    if (workers != 16 || fixed_key_size == 0 || fixed_key_size > 32)
      throw std::runtime_error("formal partition split requires 16 workers and 1..32-byte keys");

    std::vector<std::string> samples;
    for (uint64_t worker = 0; worker < workers; ++worker) {
      const std::string path = trace_dir + "/worker" + std::to_string(worker) + ".txt";
      std::ifstream input(path);
      if (!input) throw std::runtime_error("cannot open load trace: " + path);
      std::string line;
      uint64_t line_number = 0;
      uint64_t valid_puts = 0;
      while (std::getline(input, line)) {
        ++line_number;
        tigonkv::e2e_trace::Operation operation;
        if (!tigonkv::e2e_trace::ParseLine(line, path, line_number, &operation) ||
            operation.name != "PUT")
          continue;
        if ((valid_puts++ % 64) == 0)
          samples.push_back(tigonkv::e2e_trace::FixedTraceKey(
              operation.raw_key, fixed_key_size));
      }
    }
    std::sort(samples.begin(), samples.end());
    samples.erase(std::unique(samples.begin(), samples.end()), samples.end());
    if (samples.size() < 4)
      throw std::runtime_error("insufficient distinct load PUT samples for four ranges");
    std::vector<std::string> boundaries;
    for (size_t partition = 1; partition < 4; ++partition)
      boundaries.push_back(samples[(samples.size() * partition) / 4]);
    if (!(boundaries[0] < boundaries[1] && boundaries[1] < boundaries[2]))
      throw std::runtime_error("sample quantiles do not define four non-empty ranges");
    ReplacePartitioning(config_path, boundaries);

    uint64_t digest = 1469598103934665603ULL;
    for (const auto &boundary : boundaries) digest = Fnv1a(boundary, digest);
    std::cout << "YCSB_PARTITION_SPLITS workers=16 sample_stride=64 samples="
              << samples.size() << " partition_count=4 digest=" << digest << "\n";
    for (size_t partition = 0; partition < 4; ++partition) {
      const auto begin = partition == 0 ? samples.begin() :
                                          std::lower_bound(samples.begin(), samples.end(), boundaries[partition - 1]);
      const auto end = partition == 3 ? samples.end() :
                                        std::lower_bound(samples.begin(), samples.end(), boundaries[partition]);
      std::cout << "YCSB_PARTITION_SPLIT partition=" << partition
                << " sampled_keys=" << (end - begin) << "\n";
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "ycsb_partition_splits: " << error.what() << "\n";
    return 2;
  }
}
