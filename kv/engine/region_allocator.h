#pragma once

#include "kv/engine/kv_types_layout.h"
#include "kv/engine/mem_access.h"

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace tigonkv::engine {

// All allocator metadata is part of the mapped region.  The fixed upper bound
// keeps the on-region format inspectable and avoids a process-local directory.
constexpr uint32_t kAllocatorSizeClasses = 32;
constexpr uint32_t kOwnerPrivateEbrWorkers = 5;
constexpr uint32_t kOwnerPrivateEbrEpochs = 3;

struct alignas(64) RegionFreeBlock {
  std::atomic<RegionOffset> next{kNullOffset};
  uint32_t size_class = 0;
  uint32_t owner_shard = 0;
};

struct alignas(64) RegionAllocatorShard {
  std::atomic<uint32_t> lock{0};
  uint64_t begin = 0;
  uint64_t end = 0;
  uint64_t bump = 0;  // only protected by lock
  std::atomic<RegionOffset> free_heads[kAllocatorSizeClasses]{};
};

// Persistent per-region allocator metadata. It is safe to attach from a
// different mapping address because all links are offsets from base_.
struct alignas(64) RegionAllocatorHeader {
  uint64_t magic = 0x5449474f4e414c4cULL;  // TIGONALL
  uint32_t version = 4;
  uint32_t shard_count = 0;
  uint64_t region_bytes = 0;
  uint64_t metadata_bytes = 0;
  uint64_t reserved_prefix_bytes = 0;
  std::atomic<uint64_t> allocated_bytes{0};
  std::atomic<uint64_t> allocation_count{0};
  std::atomic<uint64_t> free_count{0};
  RegionAllocatorShard shards[kMaxAllocatorShards]{};
};

class RegionAllocator {
 public:
  static constexpr uint64_t kAlignment = 64;
  static uint64_t AccountedBytes(uint64_t bytes);

  static RegionAllocator Initialize(void *region, uint64_t region_bytes,
                                    uint32_t shard_count,
                                    uint64_t reserved_prefix_bytes = 0,
                                    bool control_is_hwcc = false,
                                    bool block_is_hwcc = false,
                                    bool block_is_shared_payload = false);
  static RegionAllocator Attach(void *region, uint64_t region_bytes,
                                bool control_is_hwcc = false,
                                bool block_is_hwcc = false,
                                bool block_is_shared_payload = false);
  static RegionAllocator InitializeWithExternalHeader(
      void *region, uint64_t region_bytes, RegionAllocatorHeader *header,
      bool control_is_hwcc = false, bool block_is_hwcc = false,
      bool block_is_shared_payload = false);
  static RegionAllocator AttachWithExternalHeader(
      void *region, uint64_t region_bytes, RegionAllocatorHeader *header,
      bool control_is_hwcc = false, bool block_is_hwcc = false,
      bool block_is_shared_payload = false);

  // Hot path: owner shard size-class freelist / bump under a short spin lock.
  // Every allocation is reclaimed by its owner. Cross-owner free is a protocol
  // error: remote nodes never access allocator control in owner-private SWCC.
  void *Allocate(uint64_t bytes, AllocationDomain domain, DomainCounter *counter,
                 uint32_t owner_shard = 0);
  void Free(void *pointer, uint64_t bytes, AllocationDomain domain,
            DomainCounter *counter, uint32_t owner_shard,
            uint32_t current_shard);

  RegionOffset ToOffset(const void *pointer) const;
  void *FromOffset(RegionOffset offset) const;
  bool Contains(const void *pointer) const;
  uint64_t capacity() const { return bytes_; }
  uint64_t metadata_bytes() const { return header_->metadata_bytes; }
  uint64_t allocated() const {
    const auto domain = control_is_hwcc_
                            ? latency_sim::AtomicDomain::kHwcc
                            : latency_sim::AtomicDomain::kOwnerPrivateSwcc;
    return latency_sim::FixedLatencyAtomicLoad(header_->allocated_bytes,
                                          std::memory_order_acquire, domain);
  }
  uint32_t shard_count() const { return header_->shard_count; }
  // Flush allocator metadata plus each shard's allocated high-water range.
  // This deliberately never sweeps an entire region.
  void FlushAllocatedRanges() const;
  void FlushOwnedRange(uint32_t owner_shard) const;

 private:
  RegionAllocator(void *base, uint64_t bytes, RegionAllocatorHeader *header,
                  bool control_is_hwcc, bool block_is_hwcc,
                  bool block_is_shared_payload)
      : base_(static_cast<std::byte *>(base)), bytes_(bytes), header_(header),
        control_is_hwcc_(control_is_hwcc), block_is_hwcc_(block_is_hwcc),
        block_is_shared_payload_(block_is_shared_payload) {}
  static uint64_t Align(uint64_t bytes) {
    if (bytes > UINT64_MAX - (kAlignment - 1)) throw std::bad_alloc();
    return (bytes + kAlignment - 1) & ~(kAlignment - 1);
  }
  static uint32_t SizeClass(uint64_t bytes);
  static uint64_t ClassBytes(uint32_t size_class);
  static uint64_t MetadataBytes();
  void Lock(RegionAllocatorShard &shard) const;
  void Unlock(RegionAllocatorShard &shard) const;
  void *AllocateFromShard(uint64_t bytes, uint32_t size_class,
                          uint32_t owner_shard);
  void FreeLocal(RegionOffset offset, uint32_t size_class, uint32_t owner_shard);
  void AccountAllocate(uint64_t bytes, DomainCounter *counter);
  void AccountFree(uint64_t bytes, DomainCounter *counter);
  void RecordMetadataRead(const void *address, uint64_t bytes) const;
  void RecordMetadataWrite(const void *address, uint64_t bytes) const;
  void RecordBlockMetadataRead(const void *address, uint64_t bytes) const;
  void RecordBlockMetadataWrite(const void *address, uint64_t bytes) const;

  std::byte *base_;
  uint64_t bytes_;
  RegionAllocatorHeader *header_;
  bool control_is_hwcc_;
  bool block_is_hwcc_;
  bool block_is_shared_payload_;
};

// The pool is mapped once, but allocations are physically constrained to one
// of the two configured intervals.  This is deliberately a concrete routing
// object, not a callback layer: hot callers select a domain and use an inline
// switch to reach the correct RegionAllocator.
struct DualRegionConfig {
  uint64_t total_pool_bytes = 0;
  uint64_t hwcc_offset_bytes = 0;
  uint64_t hwcc_size_bytes = 0;
  uint64_t swcc_offset_bytes = 0;
  uint64_t swcc_size_bytes = 0;
  uint64_t config_hash = 0;
  uint32_t vm_count = 0;
  uint32_t partition_count = 0;
  uint32_t fixed_key_size = 0;
  uint32_t fixed_value_size = 0;
  double owner_private_swcc_fraction = 0.35;
};

// A fixed SWCC subrange assigned to one partition.  It is persistent and
// intentionally carries no process virtual address.  Owner-only allocation is
// a lock-protected bump path; reclamation is added through EBR in M4.
struct alignas(64) OwnerPrivateArenaHeader {
  uint64_t magic = 0x5449474f4e41524eULL;  // TIGONARN
  uint32_t version = 3;
  uint32_t partition_id = 0;
  uint32_t owner_shard = 0;
  uint32_t reserved = 0;
  uint64_t begin = 0;
  uint64_t end = 0;
  uint64_t bump = 0;
  std::atomic<uint32_t> lock{0};
  std::atomic<uint64_t> allocated_bytes{0};
  std::atomic<RegionOffset> free_head{kNullOffset};
  // Original-compute-node state: the private B+tree root and Clock control
  // words belong with this owner's non-coherent SWCC arena, never in the
  // globally coherent partition directory.
  std::atomic<RegionOffset> private_root{kNullOffset};
  OwnerPrivateClockTrackerControl clock{};
};

// One fixed owner slot precedes the partition arenas.  It is the only
// persistent home for an owner's dynamic allocator headers, accounting, and
// EBR queues; partition arenas contain partition-local roots/Clock state only.
struct alignas(64) OwnerAllocatorControl {
  uint64_t magic = 0x5449474f4e4f574eULL;  // TIGONOWN
  uint32_t version = 1;
  uint32_t owner_shard = 0;
  RegionOffset dynamic_hwcc_allocator = kNullOffset;
  RegionOffset dynamic_shared_swcc_allocator = kNullOffset;
  std::atomic<uint64_t> total_hw_cc_usage{0};
  // Original EBR keeps one retire list per worker/epoch.  The lists are
  // owner-private SWCC offsets so their allocator metadata never becomes a
  // cross-VM synchronization object.
  std::atomic<RegionOffset>
      retire_heads[kOwnerPrivateEbrWorkers][kOwnerPrivateEbrEpochs]{};
  std::atomic<uint64_t>
      retire_counts[kOwnerPrivateEbrWorkers][kOwnerPrivateEbrEpochs]{};
  DomainCounter dynamic_hwcc{};
  DomainCounter shared_payload{};
};

struct OwnerPrivateRetireRecord {
  RegionOffset object_offset = kNullOffset;
  uint64_t bytes = 0;
  AllocationDomain domain = AllocationDomain::kHwccIndex;
  uint32_t private_partition = UINT32_MAX;
  uint32_t record_partition = UINT32_MAX;
  RegionOffset next = kNullOffset;
};

struct alignas(64) DualRegionPersistentHeader {
  SharedLayoutHeader layout{};
  uint64_t hwcc_allocator_offset = 0;
  uint64_t hwcc_allocator_bytes = 0;
  uint64_t swcc_allocator_offset = 0;
  uint64_t swcc_allocator_bytes = 0;
  uint64_t owner_private_arenas_offset = 0;
  uint64_t owner_private_arenas_bytes = 0;
  uint64_t owner_private_arena_stride = 0;
  uint64_t owner_controls_offset = 0;
  uint64_t owner_controls_bytes = 0;
  uint64_t owner_control_stride = 0;
  uint64_t transport_offset = kNullOffset;
  uint64_t transport_bytes = 0;
  uint64_t ebr_offset = kNullOffset;
  uint64_t ebr_bytes = 0;
};

class DualRegionAllocator {
 public:
  static DualRegionAllocator Initialize(void *pool, const DualRegionConfig &config);
  static DualRegionAllocator Attach(void *pool, const DualRegionConfig &config);
  // Construct only this VM's owner-private arena controls.  Static layout
  // records offsets/bounds in HWCC, but no VM may construct another owner's
  // SWCC header.
  void InitializeOwnerPrivateArenas(uint32_t node_id);
  // Rebuild only this process's non-owning dynamic allocator handles after
  // its owner-private controls were already initialized and published.
  void BindOwnerPrivateAllocators(uint32_t node_id);
  void FinalizeStaticHwccLayout();
  void PublishStaticHwccLayout();
  // Startup has exactly one cross-VM state machine.  Each VM initializes only
  // its own SWCC arena and roots, publishes its bit, then VM0 releases Ready.
  // These are startup-only operations, never a checkpoint/recovery protocol.
  void PublishOwnerInitialized(uint32_t node_id);
  void WaitForOwnersAndPublishReady();
  void WaitUntilReady() const;
  // Internal final release after every owner bit is visible.
  void PublishReady();

  void *Allocate(uint64_t bytes, AllocationDomain domain, uint32_t owner_shard);
  void *AllocateOwnerPrivate(uint64_t bytes, uint32_t partition_id,
                             uint32_t owner_shard);
  void FreeOwnerPrivate(void *pointer, uint64_t bytes, uint32_t partition_id,
                        uint32_t owner_shard);
  void Free(void *pointer, uint64_t bytes, AllocationDomain domain,
            uint32_t owner_shard, uint32_t current_shard);
  void Retire(uint32_t owner_shard, uint32_t queue_partition,
              uint32_t worker_id, uint32_t epoch, void *pointer,
              uint64_t bytes, AllocationDomain domain,
              uint32_t private_partition);
  uint64_t RetireCount(uint32_t owner_shard, uint32_t worker_id,
                       uint32_t epoch) const;
  std::vector<OwnerPrivateRetireRecord> TakeRetired(
      uint32_t owner_shard, uint32_t worker_id, uint32_t epoch);
  bool IsHwccAddress(const void *pointer) const;
  bool IsSwccAddress(const void *pointer) const;
  RegionOffset ToOwnerPrivateOffset(const void *pointer,
                                    uint32_t partition_id) const;
  uint64_t EncodeSharedPayloadOffset(const void *pointer,
                                     uint32_t owner_shard) const;
  RegionOffset ToTransportOffset(const void *pointer) const;
  RegionOffset ToDynamicHwccOffset(const void *pointer,
                                   uint32_t owner_shard) const;
  bool IsInOwnerPrivateArena(const void *pointer, uint32_t partition_id) const;
  void *ResolveOwnerPrivate(RegionOffset offset, uint64_t bytes,
                            uint32_t partition_id,
                            uint32_t expected_owner) const;
  void *ResolveDynamicHwcc(RegionOffset offset, uint64_t bytes,
                           uint32_t expected_owner) const;
  void *ResolveSharedPayload(uint64_t whole_pool_offset,
                             uint64_t bytes) const;
  void *ResolveTransport(RegionOffset offset, uint64_t bytes) const;
  void *ResolveEbr(RegionOffset offset, uint64_t bytes) const;
  RegionOffset OwnerPrivateArenaOffset(uint32_t partition_id) const;
  uint64_t SharedPayloadCapacityBytes(uint32_t owner_shard) const;
  uint64_t OwnerPrivateUsedBytes(uint32_t owner_shard) const;
  uint64_t DynamicHwccUsedBytes(uint32_t owner_shard) const;
  uint64_t PolicyHwccUsedBytes(uint32_t owner_shard) const;
  uint64_t SharedPayloadUsedBytes(uint32_t owner_shard) const;
  // Static HWCC/SWCC domain counters live in the layout header. Reporting and
  // Open() clamp must record each load; do not return the atomic pointer.
  uint64_t ReadStaticDomainUsedBytes(AllocationDomain domain) const;
  // Explicit test/teardown flush only. It is not a distributed checkpoint or
  // a substitute for SCC publication.
  void FlushOwnedRanges(uint32_t node_id);
  const SharedLayoutHeader &layout() const { return header_->layout; }
  SharedLayoutHeader &layout() { return header_->layout; }
  const RegionAllocator &hwcc() const { return hwcc_; }

 private:
  DualRegionAllocator(std::byte *pool, const DualRegionConfig &config,
                      DualRegionPersistentHeader *header, RegionAllocator hwcc)
      : pool_(pool), config_(config), header_(header), hwcc_(hwcc) {}
  static bool IsHwccDomain(AllocationDomain domain);
  void BindOwnerPrivateArenaHandles(uint32_t node_id);
  OwnerAllocatorControl *OwnerControl(uint32_t owner_shard) const;
  OwnerPrivateArenaHeader *Arena(uint32_t partition_id) const;
  void *SwccFromOffset(RegionOffset offset, uint64_t bytes = 1) const;
  RegionOffset SwccToOffset(const void *pointer, uint64_t bytes = 1) const;
  std::byte *pool_;
  DualRegionConfig config_;
  DualRegionPersistentHeader *header_;
  RegionAllocator hwcc_;
  // Process-local, non-owning handles rebuilt only for this VM's private
  // arenas.  They never enter the mapped layout and keep Allocate/Free off
  // the HWCC descriptor path.
  std::array<OwnerPrivateArenaHeader *, kMaxPartitions>
      owner_private_arenas_{};
  std::array<std::unique_ptr<RegionAllocator>, kMaxAllocatorShards>
      dynamic_hwcc_{};
  std::array<std::unique_ptr<RegionAllocator>, kMaxAllocatorShards>
      dynamic_shared_swcc_{};
  bool static_hwcc_finalized_ = false;
  uint32_t bound_owner_shard_ = UINT32_MAX;
};

// Owns one MAP_SHARED backing-file mapping. Initialization is serialized with
// the backing file lock; normal attach never rewrites allocator metadata.
class DualRegionMappedPool {
 public:
  static DualRegionMappedPool Open(const std::string &path,
                                   const DualRegionConfig &config, bool reset);
  ~DualRegionMappedPool();
  DualRegionMappedPool(DualRegionMappedPool &&other) noexcept;
  DualRegionMappedPool &operator=(DualRegionMappedPool &&other) noexcept;
  DualRegionMappedPool(const DualRegionMappedPool &) = delete;
  DualRegionMappedPool &operator=(const DualRegionMappedPool &) = delete;

  DualRegionAllocator &allocator() { return *allocator_; }
  const DualRegionAllocator &allocator() const { return *allocator_; }
  void *base() const { return base_; }
  uint64_t bytes() const { return bytes_; }

 private:
  DualRegionMappedPool(int fd, void *base, uint64_t bytes,
                       std::unique_ptr<DualRegionAllocator> allocator)
      : fd_(fd), base_(base), bytes_(bytes), allocator_(std::move(allocator)) {}
  void Close() noexcept;
  int fd_ = -1;
  void *base_ = nullptr;
  uint64_t bytes_ = 0;
  std::unique_ptr<DualRegionAllocator> allocator_;
};

}  // namespace tigonkv::engine
