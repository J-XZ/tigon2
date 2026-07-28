#include "kv/engine/kv_types_layout.h"
#include "kv/engine/region_allocator.h"
#include "kv/engine/kv_messages.h"

#include <cassert>
#include <cstring>
#include <new>
#include <string>

int main() {
  using namespace tigonkv::engine;
  // Frozen architecture contract constants (PLAN.md §14.1).
  assert(kSingleTableId == 0);
  assert(kMaxFixedKeyBytes == 32);
  assert(kMaxPartitions >= 16);
  assert(kSharedLayoutVersion == 21);
  assert(sizeof(PartitionDirectoryEntry) == 64);
  assert(sizeof(PrivateValueStruct) == sizeof(RegionOffset));
  assert(alignof(PrivateMetadataLocal) == 64);
  // §11.4 WireSize: header = offsetof(value); value bytes only on the wire.
  assert(WireHeaderBytes() == offsetof(KvMessage, value));
  {
    KvMessage empty = MakeRequest(KvMessageType::kMigrate, 0, 1, 1, "k");
    assert(empty.value_size == 0);
    assert(WireSize(empty) == WireHeaderBytes());
    assert(ValidWireFrame(WireSize(empty), empty));
    KvMessage small = MakeRequest(KvMessageType::kPut, 0, 1, 2, "k",
                                  std::string(32, 'v'));
    assert(WireSize(small) == WireHeaderBytes() + 32);
    assert(ValidWireFrame(WireSize(small), small));
    assert(!ValidWireFrame(WireHeaderBytes(), small));  // truncated value
    KvMessage forged = small;
    forged.value_size = 0;
    assert(!ValidWireFrame(WireSize(small), forged));  // trailing / forged size
    KvMessage maxv = MakeRequest(KvMessageType::kPut, 0, 1, 3, "k",
                                 std::string(1024, 'x'));
    // Max wire is header+1024 value bytes. sizeof(KvMessage) may include
    // trailing alignment padding (1092 wire vs 1096 object on this ABI).
    assert(WireSize(maxv) == WireHeaderBytes() + maxv.value.size());
    assert(WireSize(maxv) <= sizeof(KvMessage));
    assert(ValidWireFrame(WireSize(maxv), maxv));
    assert(!ValidWireFrame(sizeof(KvMessage), maxv));  // padded recv != wire
  }
  // §5.1 ScanMigrate codec round-trip and reject unknown flags / wrong size.
  {
    const std::string max_key(32, static_cast<char>(0xff));
    const auto req = EncodeScanMigrateRequest(
        7, kScanMigrateFlagCursorDuplicate, 17, max_key);
    assert(req.size() == kScanMigrateRequestBytes);
    uint32_t pid = 0, flags = 0;
    uint64_t limit = 0;
    std::string decoded_max;
    assert(DecodeScanMigrateRequest(req, &pid, &flags, &limit, &decoded_max));
    assert(pid == 7 && flags == kScanMigrateFlagCursorDuplicate && limit == 17);
    assert(decoded_max == max_key);
    assert(!DecodeScanMigrateRequest(req + "x", &pid, &flags, &limit,
                                     &decoded_max));
    const auto bad_flags = EncodeScanMigrateRequest(0, 2u, 1, max_key);
    assert(!DecodeScanMigrateRequest(bad_flags, &pid, &flags, &limit,
                                     &decoded_max));
    const auto resp = EncodeScanMigrateResponse(7, true, true);
    assert(resp.size() == kScanMigrateResponseBytes);
    bool exhausted = false;
    bool no_pred = false;
    assert(DecodeScanMigrateResponse(resp, &pid, &exhausted, &no_pred));
    assert(pid == 7 && exhausted && no_pred);
    assert(DecodeScanMigrateResponse(EncodeScanMigrateResponse(3, false, false),
                                     &pid, &exhausted, &no_pred));
    assert(pid == 3 && !exhausted && !no_pred);
  }
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
