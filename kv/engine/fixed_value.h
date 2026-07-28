#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace tigonkv::engine {

inline bool DecodeCanonicalFixedDecimal(std::string_view encoded, int64_t *value) {
  const size_t nul = encoded.find('\0');
  const std::string_view decimal = encoded.substr(0, nul);
  if (decimal.empty()) return false;
  for (size_t i = nul; i < encoded.size(); ++i)
    if (encoded[i] != '\0') return false;
  int64_t parsed = 0;
  const auto result = std::from_chars(decimal.data(), decimal.data() + decimal.size(),
                                      parsed);
  if (result.ec != std::errc{} || result.ptr != decimal.data() + decimal.size())
    return false;
  char canonical[32];
  const auto formatted = std::to_chars(canonical, canonical + sizeof(canonical), parsed);
  if (formatted.ec != std::errc{} ||
      std::string_view(canonical, formatted.ptr - canonical) != decimal)
    return false;
  *value = parsed;
  return true;
}

inline bool EncodeCanonicalFixedDecimal(int64_t value, uint32_t fixed_value_size,
                                        std::string *encoded) {
  encoded->assign(fixed_value_size, '\0');
  const auto result = std::to_chars(encoded->data(),
                                    encoded->data() + encoded->size(), value);
  return result.ec == std::errc{};
}

}  // namespace tigonkv::engine
