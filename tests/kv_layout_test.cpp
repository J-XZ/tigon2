#include "kv/engine/kv_types_layout.h"
#include "kv/engine/region_allocator.h"
#include "common/Encoder.h"
#include "common/Message.h"
#include "common/MessagePiece.h"

#include <cassert>
#include <cstring>
#include <new>
#include <string>

int main() {
  using namespace tigonkv::engine;
  // Frozen architecture contract constants.
  assert(kSingleTableId == 0);
  assert(kMaxFixedKeyBytes == 32);
  assert(kMaxPartitions >= 16);
  assert(kSharedLayoutVersion == 21);
  assert(sizeof(PartitionDirectoryEntry) == 64);
  assert(sizeof(PrivateValueStruct) == sizeof(RegionOffset));
  assert(alignof(PrivateMetadataLocal) == 64);
  // Transport uses the original Message/MessagePiece framing. The largest KV
  // request is a 32B key + 1024B fixed value + original transaction/key slot.
  {
    star::Message message;
    message.set_source_node_id(0);
    message.set_dest_node_id(1);
    message.set_worker_id(3);
    const uint32_t piece_bytes = star::MessagePiece::get_header_size() + 32 + 1024 +
        sizeof(uint64_t) + sizeof(uint32_t);
    const std::string key(32, 'k');
    const std::string value(1024, 'v');
    star::Encoder encoder(message.data);
    encoder << star::MessagePiece::construct_message_piece_header(
        1, piece_bytes, kSingleTableId, 0);
    encoder.write_n_bytes(key.data(), key.size());
    encoder.write_n_bytes(value.data(), value.size());
    encoder << uint64_t{1} << uint32_t{0};
    message.flush();
    assert(message.check_size() && message.check_deadbeef());
    assert(message.get_message_count() == 1);
    assert(message.get_message_length() == star::Message::get_prefix_size() + piece_bytes);
    assert(message.get_message_length() <= 2048 - 9);
  }
  const FixedKey alpha = FixedKey::From("alpha", 8);
  const FixedKey beta = FixedKey::From("beta", 8);
  assert(alpha.Compare(alpha) == 0);
  assert(FixedKeyLess{}(alpha, beta));
  assert(kNullOffset == 0);

  SharedLayoutHeader header;
  header.magic.store(kSharedLayoutMagic, std::memory_order_release);
  header.config_hash = 7;
  header.total_pool_bytes = 4096;
  header.vm_count = 2;
  header.partition_count = 16;
  assert(!header.IsCompatible(7, 4096, 2, 16));
  header.state.store(static_cast<uint32_t>(LayoutState::kReady),
                     std::memory_order_release);
  assert(header.IsCompatible(7, 4096, 2, 16));
  assert(!header.IsCompatible(8, 4096, 2, 16));
  OwnerPrivateArenaHeader arena;
  assert(arena.private_root == kNullOffset);
  arena.private_root = 64;
  header.partitions[3].shared_root.store(128, std::memory_order_release);
  header.partitions[3].migration_in_seq.store(1, std::memory_order_release);
  assert(arena.private_root == 64);
  assert(header.partitions[3].shared_root.load(std::memory_order_acquire) == 128);
  assert(header.partitions[3].migration_in_seq.load(std::memory_order_acquire) == 1);

  alignas(PrivateValueStruct) std::byte value_storage[
      sizeof(PrivateValueStruct) + 16]{};
  auto *private_value = new (value_storage) PrivateValueStruct;
  std::memcpy(private_value->data, "value", 6);
  assert(std::memcmp(private_value->data, "value", 6) == 0);
  alignas(PrivateMetadataLocal) std::byte metadata_storage[
      sizeof(PrivateMetadataLocal)]{};
  auto *metadata = new (metadata_storage) PrivateMetadataLocal;
  assert(metadata->migrated_smeta_off == kNullOffset && !metadata->is_migrated);
  pthread_spin_destroy(&metadata->latch);
  return 0;
}
