#pragma once

#include "kv/kv_store.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace tigonkv::engine {

// A single MPSCRingBuffer entry is 2048 bytes.  This fixed wire record stays
// below its usable payload and carries no process virtual addresses.
enum class KvMessageType : uint8_t {
  kPut = 1,
  kGet = 2,
  kDelete = 3,
  kIncrement = 4,
  kCasPrepare = 5,
  kCasCommit = 6,
  kResponse = 7,
  // Owner-only move_row_in (TwoPLPasha DATA_MIGRATION_*). No value payload;
  // requester retries CXL Shared after a successful ack.
  kMigrate = 11,
  // Owner moves a complete range prefix plus adjacency boundaries into CXL
  // without returning values; response carries the CXL range certificate.
  kScanMigrate = 12,
};

struct KvMessage {
  // Default type is an illegal wire value so zeroed/uninitialized frames fail
  // validation instead of looking like a live Get (§12.4).
  KvMessageType type = static_cast<KvMessageType>(0);
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
static_assert(offsetof(KvMessage, value) == 68,
              "wire header size must stay offsetof(value); key stays fixed 32B");

// Variable-length wire: send header + actual value bytes only (§11.4).
// Key remains fixed 32B inside the header; value padding is omitted on the wire.
inline constexpr size_t WireHeaderBytes() { return offsetof(KvMessage, value); }

inline size_t WireSize(const KvMessage &message) {
  if (message.value_size > message.value.size())
    throw std::invalid_argument("KV message value_size exceeds capacity");
  return WireHeaderBytes() + message.value_size;
}

inline bool ValidWireFrame(size_t received, const KvMessage &message) {
  if (received < WireHeaderBytes() || received > sizeof(KvMessage)) return false;
  if (message.key_size > message.key.size()) return false;
  if (message.value_size > message.value.size()) return false;
  return received == WireHeaderBytes() + message.value_size;
}

inline KvMessage MakeRequest(KvMessageType type, uint32_t source, uint32_t destination,
                             uint64_t request_id, std::string_view key,
                             std::string_view value = {}) {
  if (key.size() > KvMessage{}.key.size() || value.size() > KvMessage{}.value.size())
    throw std::invalid_argument("KV message field exceeds fixed transport contract");
  KvMessage message{};
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

inline KvMessage MakeResponse(uint32_t source, uint32_t destination, uint64_t request_id,
                              StatusCode status = StatusCode::kOk,
                              std::string_view value = {}) {
  KvMessage message = MakeRequest(KvMessageType::kResponse, source, destination,
                                  request_id, {}, value);
  message.status = static_cast<uint32_t>(status);
  return message;
}

}  // namespace tigonkv::engine
