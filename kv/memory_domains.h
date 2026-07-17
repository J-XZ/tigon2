#pragma once

#include <cstdint>

namespace tigonkv {

enum class AccessClass {
  kProcessLocalDram,
  kOwnerPrivateSwcc,
  kSharedHwcc,
  kSharedSwccPayload,
  kPublishedImmutableSwcc,
};

enum class PoolDomain {
  kHwccIndex,
  kHwccMetadata,
  kHwccEbr,
  kHwccLayout,
  kOwnerPrivateSwcc,
  kSharedPayloadSwcc,
  kTransport,
};

struct PersistentPtr {
  uint64_t offset = 0;
  uint32_t domain = 0;
  uint32_t reserved = 0;
};

}  // namespace tigonkv
