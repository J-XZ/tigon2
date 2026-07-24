#include "kv/engine/kv_types_layout.h"

#include <cassert>

int main() {
  using namespace tigonkv::engine;
  const FixedKey alpha = FixedKey::From("alpha", 8);
  const FixedKey beta = FixedKey::From("beta", 8);
  assert(alpha.Compare(alpha) == 0);
  assert(FixedKeyLess{}(alpha, beta));
  assert(kNullOffset == 0);

  SharedLayoutHeader header;
  header.config_hash = 7;
  header.total_pool_bytes = 4096;
  header.vm_count = 2;
  header.partition_count = 16;
  assert(!header.IsCompatible(7, 4096, 2, 16));
  header.state.store(static_cast<uint32_t>(LayoutState::kClean),
                     std::memory_order_release);
  assert(header.IsCompatible(7, 4096, 2, 16));
  assert(!header.IsCompatible(8, 4096, 2, 16));
  return 0;
}
