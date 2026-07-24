#include "kv/engine/kv_types_layout.h"

#include <cassert>
#include <cstring>
#include <new>

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
  assert(header.partitions[3].private_root == kNullOffset);
  header.partitions[3].private_root = 64;
  header.partitions[3].shared_root = 128;
  header.partitions[3].migration_in_seq.store(1, std::memory_order_release);
  assert(header.partitions[3].private_root == 64);
  assert(header.partitions[3].migration_in_seq.load(std::memory_order_acquire) == 1);

  alignas(PrivateRow) std::byte storage[sizeof(PrivateRow) + 16]{};
  auto *row = new (storage) PrivateRow;
  row->key_len = 3;
  row->value_len = 5;
  row->version = 9;
  std::memcpy(row->kv, "keyvalue", 8);
  assert(std::memcmp(row->kv, "keyvalue", 8) == 0);
  assert(row->migrated_smeta_off == kNullOffset && row->is_migrated == 0);
  return 0;
}
