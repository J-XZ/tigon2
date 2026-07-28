#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tigonkv::e2e_trace {

struct Operation {
  std::string_view name;
  std::string_view raw_key;
  uint64_t length = 0;
};

inline uint64_t ParseDecimal(std::string_view text, size_t *position,
                             std::string_view label) {
  const size_t begin = *position;
  uint64_t value = 0;
  while (*position < text.size() && text[*position] >= '0' &&
         text[*position] <= '9') {
    value = value * 10 + static_cast<uint64_t>(text[*position] - '0');
    ++*position;
  }
  if (begin == *position)
    throw std::runtime_error("missing " + std::string(label));
  return value;
}

inline bool ParseLine(std::string_view line, std::string_view trace,
                      uint64_t line_number, Operation *operation) {
  if (line.empty() || line.front() == '#') return false;
  size_t position = 0;
  const auto skip_space = [&] {
    while (position < line.size() &&
           (line[position] == ' ' || line[position] == '\t'))
      ++position;
  };
  skip_space();
  const size_t op_begin = position;
  while (position < line.size() && line[position] != ' ' &&
         line[position] != '\t')
    ++position;
  if (op_begin == position)
    throw std::runtime_error(std::string(trace) + ": malformed line " +
                             std::to_string(line_number));
  operation->name = line.substr(op_begin, position - op_begin);
  skip_space();
  const uint64_t key_length = ParseDecimal(line, &position, "key length");
  if (position >= line.size() ||
      (line[position] != ' ' && line[position] != '\t'))
    throw std::runtime_error(std::string(trace) + ": missing LEN separator at line " +
                             std::to_string(line_number));
  skip_space();
  operation->length = ParseDecimal(line, &position, "operation length");
  skip_space();
  if (key_length > line.size() - position)
    throw std::runtime_error(std::string(trace) + ": key length mismatch at line " +
                             std::to_string(line_number));
  operation->raw_key = line.substr(position, static_cast<size_t>(key_length));
  for (size_t tail = position + static_cast<size_t>(key_length);
       tail < line.size(); ++tail) {
    if (line[tail] != ' ' && line[tail] != '\t')
      throw std::runtime_error(std::string(trace) +
                               ": trailing bytes after key at line " +
                               std::to_string(line_number));
  }
  return true;
}

inline std::string FixedTraceKey(std::string_view key, uint32_t fixed_key_size) {
  if (key.size() > fixed_key_size)
    throw std::runtime_error("trace key exceeds fixed_key_size: size=" +
                             std::to_string(key.size()) +
                             " fixed_key_size=" +
                             std::to_string(fixed_key_size));
  std::string fixed(key);
  fixed.resize(static_cast<size_t>(fixed_key_size), ' ');
  return fixed;
}

inline int CompareFixedTraceKey(std::string_view left, std::string_view right) {
  if (left.size() != right.size())
    throw std::invalid_argument("fixed trace key width mismatch");
  return std::memcmp(left.data(), right.data(), left.size());
}

struct FixedTraceKeyLess {
  bool operator()(std::string_view left, std::string_view right) const {
    return CompareFixedTraceKey(left, right) < 0;
  }
};

}  // namespace tigonkv::e2e_trace
