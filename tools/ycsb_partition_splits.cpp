#include "tools/e2e_trace_format.h"

#include <algorithm>
#include <array>
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
    if ((argc != 9 && argc != 11) || std::string_view(argv[1]) != "--trace-dir" ||
        std::string_view(argv[3]) != "--config" ||
        std::string_view(argv[5]) != "--workers" ||
        std::string_view(argv[7]) != "--fixed-key-size" ||
        (argc == 11 && std::string_view(argv[9]) != "--sample-stride"))
      throw std::runtime_error(
          "usage: ycsb_partition_splits --trace-dir DIR --config FILE "
          "--workers N --fixed-key-size N [--sample-stride N]");
    const std::string trace_dir = argv[2];
    const std::string config_path = argv[4];
    const uint64_t workers = ParseUnsigned(argv[6], "workers");
    const uint32_t fixed_key_size = static_cast<uint32_t>(
        ParseUnsigned(argv[8], "fixed-key-size"));
    constexpr uint64_t kDefaultSampleStride = 64;
    const uint64_t sample_stride = argc == 11
        ? ParseUnsigned(argv[10], "sample-stride")
        : kDefaultSampleStride;
    if (workers != 16 || fixed_key_size == 0 || fixed_key_size > 32)
      throw std::runtime_error("formal partition split requires 16 workers and 1..32-byte keys");
    if (sample_stride == 0)
      throw std::runtime_error("sample-stride must be positive");

    std::vector<std::string> samples;
    std::vector<std::string> complete_keys;
    uint64_t load_key_digest = 1469598103934665603ULL;
    std::array<std::string, 4> representative_keys;
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
        const std::string key = tigonkv::e2e_trace::FixedTraceKey(
            operation.raw_key, fixed_key_size);
        load_key_digest = Fnv1a(key, load_key_digest);
        complete_keys.push_back(key);
        if ((valid_puts++ % sample_stride) == 0) samples.push_back(key);
      }
    }
    const tigonkv::e2e_trace::FixedTraceKeyLess fixed_less;
    std::sort(samples.begin(), samples.end(), fixed_less);
    samples.erase(std::unique(samples.begin(), samples.end(),
                              [&](const std::string &left, const std::string &right) {
                                return !fixed_less(left, right) && !fixed_less(right, left);
                              }),
                  samples.end());
    if (samples.size() < 4)
      throw std::runtime_error("insufficient distinct load PUT samples for four ranges");
    std::vector<std::string> boundaries;
    for (size_t partition = 1; partition < 4; ++partition)
      boundaries.push_back(samples[(samples.size() * partition) / 4]);
    if (!(fixed_less(boundaries[0], boundaries[1]) &&
          fixed_less(boundaries[1], boundaries[2])))
      throw std::runtime_error("sample quantiles do not define four non-empty ranges");
    ReplacePartitioning(config_path, boundaries);

    std::array<uint64_t, 4> complete_counts{};
    for (const auto &key : complete_keys) {
      const auto partition = std::upper_bound(
          boundaries.begin(), boundaries.end(), key,
          [&](const std::string &needle, const std::string &boundary) {
            return fixed_less(needle, boundary);
          });
      ++complete_counts[static_cast<size_t>(partition - boundaries.begin())];
      const size_t partition_id =
          static_cast<size_t>(partition - boundaries.begin());
      if (representative_keys[partition_id].empty() ||
          fixed_less(key, representative_keys[partition_id]))
        representative_keys[partition_id] = key;
    }
    const auto [min_count, max_count] =
        std::minmax_element(complete_counts.begin(), complete_counts.end());
    if (*min_count == 0 || *max_count * 4 > *min_count * 5)
      throw std::runtime_error("sampled partition splits exceed 1.25 full-load imbalance");

    uint64_t digest = 1469598103934665603ULL;
    for (const auto &boundary : boundaries) digest = Fnv1a(boundary, digest);
    std::cout << "YCSB_PARTITION_SPLITS workers=16 sample_stride=" << sample_stride
              << " samples="
              << samples.size() << " partition_count=4 split_digest=" << digest
              << " load_key_digest=" << load_key_digest << "\n";
    for (size_t partition = 0; partition < 4; ++partition) {
      if (representative_keys[partition].empty())
        throw std::runtime_error("full load partition has no representative key");
      const auto sampled_begin = partition == 0 ? samples.begin() :
          std::lower_bound(samples.begin(), samples.end(), boundaries[partition - 1], fixed_less);
      const auto sampled_end = partition == 3 ? samples.end() :
          std::lower_bound(samples.begin(), samples.end(), boundaries[partition], fixed_less);
      std::cout << "YCSB_PARTITION_SPLIT partition=" << partition
                << " sampled_keys=" << (sampled_end - sampled_begin)
                << " load_keys=" << complete_counts[partition]
                << " owner=" << partition
                << " representative_key_hex=";
      for (unsigned char byte : representative_keys[partition]) {
        static constexpr char kHex[] = "0123456789abcdef";
        std::cout << kHex[byte >> 4] << kHex[byte & 0xf];
      }
      std::cout << "\n";
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "ycsb_partition_splits: " << error.what() << "\n";
    return 2;
  }
}
