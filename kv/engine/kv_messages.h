#pragma once

#include "kv/kv_store.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tigonkv::engine {

// A single MPSCRingBuffer entry is 2048 bytes.  This fixed wire record stays
// below its usable payload and carries no process virtual addresses.
enum class KvMessageType : uint8_t {
  kPut = 1,
  kGet = 2,
  kDelete = 3,
  // Values 4 and 5 were current-only forwarded Increment/CAS messages.
  // They remain intentionally unused so the original wire slots are not
  // repurposed; receiving either is malformed.
  // Former two-phase CAS commit; reserved / malformed if received.
  kCasCommitReserved = 6,
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

// Single-partition ScanMigrate value payloads. Key carries min_key; payload
// keeps the original min/max/limit scan contract by carrying inclusive max.
constexpr uint32_t kScanMigrateFlagCursorDuplicate = 1u;
constexpr uint32_t kScanMigrateKnownFlags = kScanMigrateFlagCursorDuplicate;
constexpr size_t kScanMigrateRequestBytes = 48;  // partition_id + flags + limit + max
constexpr size_t kScanMigrateResponseBytes = 6;  // partition_id + exhausted + no_pred

inline void AppendLe32(std::string *out, uint32_t value) {
  const char bytes[4] = {
      static_cast<char>(value & 0xff),
      static_cast<char>((value >> 8) & 0xff),
      static_cast<char>((value >> 16) & 0xff),
      static_cast<char>((value >> 24) & 0xff)};
  out->append(bytes, 4);
}

inline void AppendLe64(std::string *out, uint64_t value) {
  AppendLe32(out, static_cast<uint32_t>(value & 0xffffffffu));
  AppendLe32(out, static_cast<uint32_t>((value >> 32) & 0xffffffffu));
}

inline bool ConsumeLe32(std::string_view *in, uint32_t *value) {
  if (in == nullptr || value == nullptr || in->size() < 4) return false;
  const auto *p = reinterpret_cast<const unsigned char *>(in->data());
  *value = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
  in->remove_prefix(4);
  return true;
}

inline bool ConsumeLe64(std::string_view *in, uint64_t *value) {
  uint32_t lo = 0;
  uint32_t hi = 0;
  if (!ConsumeLe32(in, &lo) || !ConsumeLe32(in, &hi)) return false;
  *value = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
  return true;
}

inline std::string EncodeScanMigrateRequest(uint32_t partition_id, uint32_t flags,
                                            uint64_t output_limit,
                                            std::string_view inclusive_max) {
  if (inclusive_max.size() != KvMessage{}.key.size())
    throw std::invalid_argument("scan migrate max key must be fixed size");
  std::string out;
  out.reserve(kScanMigrateRequestBytes);
  AppendLe32(&out, partition_id);
  AppendLe32(&out, flags);
  AppendLe64(&out, output_limit);
  out.append(inclusive_max.data(), inclusive_max.size());
  return out;
}

inline bool DecodeScanMigrateRequest(std::string_view value, uint32_t *partition_id,
                                     uint32_t *flags, uint64_t *output_limit,
                                     std::string *inclusive_max) {
  if (partition_id == nullptr || flags == nullptr || output_limit == nullptr ||
      inclusive_max == nullptr)
    return false;
  if (value.size() != kScanMigrateRequestBytes) return false;
  if (!ConsumeLe32(&value, partition_id) || !ConsumeLe32(&value, flags) ||
      !ConsumeLe64(&value, output_limit) || value.size() != KvMessage{}.key.size())
    return false;
  inclusive_max->assign(value.data(), value.size());
  if ((*flags & ~kScanMigrateKnownFlags) != 0) return false;
  return true;
}

inline std::string EncodeScanMigrateResponse(uint32_t partition_id, bool exhausted,
                                             bool no_predecessor = false) {
  std::string out;
  out.reserve(kScanMigrateResponseBytes);
  AppendLe32(&out, partition_id);
  out.push_back(exhausted ? '\1' : '\0');
  out.push_back(no_predecessor ? '\1' : '\0');
  return out;
}

inline bool DecodeScanMigrateResponse(std::string_view value, uint32_t *partition_id,
                                      bool *exhausted, bool *no_predecessor = nullptr) {
  if (partition_id == nullptr || exhausted == nullptr) return false;
  if (value.size() != kScanMigrateResponseBytes) return false;
  if (!ConsumeLe32(&value, partition_id) || value.size() != 2) return false;
  *exhausted = value[0] != 0;
  if (no_predecessor != nullptr) *no_predecessor = value[1] != 0;
  return true;
}

}  // namespace tigonkv::engine
