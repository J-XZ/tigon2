#pragma once

#include "kv/kv_store.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace tigonkv::engine {

// A single MPSCRingBuffer entry is 2048 bytes.  This fixed wire record stays
// below its usable payload and carries no process virtual addresses.
enum class KvMessageType : uint8_t { kPut = 1, kGet = 2, kDelete = 3, kResponse = 4 };

struct KvMessage {
  KvMessageType type = KvMessageType::kGet;
  uint8_t reserved[3]{};
  uint32_t source_node = 0;
  uint32_t destination_node = 0;
  uint64_t request_id = 0;
  uint32_t status = static_cast<uint32_t>(StatusCode::kOk);
  uint32_t key_size = 0;
  uint32_t value_size = 0;
  std::array<char, 32> key{};
  std::array<char, 1024> value{};
};
static_assert(sizeof(KvMessage) < 2039, "KV message must fit one transport entry");

inline KvMessage MakeRequest(KvMessageType type, uint32_t source, uint32_t destination,
                             uint64_t request_id, std::string_view key,
                             std::string_view value = {}) {
  if (key.size() > KvMessage{}.key.size() || value.size() > KvMessage{}.value.size())
    throw std::invalid_argument("KV message field exceeds fixed transport contract");
  KvMessage message;
  message.type = type;
  message.source_node = source;
  message.destination_node = destination;
  message.request_id = request_id;
  message.key_size = static_cast<uint32_t>(key.size());
  message.value_size = static_cast<uint32_t>(value.size());
  std::memcpy(message.key.data(), key.data(), key.size());
  std::memcpy(message.value.data(), value.data(), value.size());
  return message;
}

}  // namespace tigonkv::engine
