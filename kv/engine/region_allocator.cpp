#include "kv/engine/region_allocator.h"
#include "kv/engine/mem_access.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <new>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sstream>

#include "glog/logging.h"

namespace tigonkv::engine {
namespace {

bool RangeContains(uint64_t begin, uint64_t length, uint64_t offset,
                   uint64_t bytes);


uint64_t CheckedAtomicSubtract(std::atomic<uint64_t> *counter, uint64_t bytes,
                               const char *detail,
                               latency_sim::MemoryDomain domain) {
  uint64_t before = latency_sim::FixedLatencyAtomicLoad(
      *counter, std::memory_order_relaxed, domain);
  for (;;) {
    if (before < bytes) LOG(FATAL) << detail;
    if (latency_sim::FixedLatencyAtomicCompareExchangeWeak(
            *counter, before, before - bytes, std::memory_order_relaxed,
            std::memory_order_relaxed, domain))
      return before;
  }
}

latency_sim::MemoryDomain AtomicDomainFor(bool hwcc, bool /*shared_payload*/) {
  // Shared-payload allocator control is still owner-local metadata.  Mapping
  // it to the owner-private bucket records the executed operation without
  // pretending that SWCC has hardware coherence.
  return hwcc ? latency_sim::MemoryDomain::kHwcc
              : latency_sim::MemoryDomain::kOwnerPrivateSwcc;
}

template <typename T>
T FixedLatencyLoadFor(const std::atomic<T> &value, bool hwcc, bool shared_payload,
                 std::memory_order order) {
  return latency_sim::FixedLatencyAtomicLoad(
      value, order, AtomicDomainFor(hwcc, shared_payload));
}

template <typename T>
void FixedLatencyStoreFor(std::atomic<T> &value, T desired, bool hwcc,
                     bool shared_payload, std::memory_order order) {
  latency_sim::FixedLatencyAtomicStore(
      value, desired, order, AtomicDomainFor(hwcc, shared_payload));
}

template <typename T>
T FixedLatencyFetchAddFor(std::atomic<T> &value, T operand, bool hwcc,
                     bool shared_payload, std::memory_order order) {
  return latency_sim::FixedLatencyAtomicFetchAdd(
      value, operand, order, AtomicDomainFor(hwcc, shared_payload));
}

template <typename T>
bool FixedLatencyCasWeakFor(std::atomic<T> &value, T &expected, T desired,
                       bool hwcc, bool shared_payload,
                       std::memory_order success, std::memory_order failure) {
  return latency_sim::FixedLatencyAtomicCompareExchangeWeak(
      value, expected, desired, success, failure,
      AtomicDomainFor(hwcc, shared_payload));
}

#ifndef NDEBUG
const char *StartupAttachBarrierPath() {
  return std::getenv("TIGONKV_TEST_STARTUP_ATTACH_BARRIER");
}

void TouchBarrierFile(const std::string &path) {
  const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY, 0600);
  if (fd < 0) throw std::runtime_error("startup test barrier create failed");
  ::close(fd);
}

void WaitForBarrierFile(const std::string &path) {
  while (::access(path.c_str(), F_OK) != 0) _mm_pause();
}
#endif

void FlushForRemoteVisibility(const void *address, size_t bytes,
                              bool charge_swcc_flush = true) {
  if (charge_swcc_flush) mem_access::SwccInvalidate(address, bytes);
#if defined(__x86_64__) || defined(__i386__)
  const auto begin = reinterpret_cast<uintptr_t>(address) & ~(RegionAllocator::kAlignment - 1);
  const auto end = reinterpret_cast<uintptr_t>(address) + bytes;
  for (auto p = begin; p < end; p += RegionAllocator::kAlignment)
    _mm_clflush(reinterpret_cast<const void *>(p));
  _mm_sfence();
#else
  (void)address;
  (void)bytes;
  std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}

}  // namespace

uint64_t RegionAllocator::MetadataBytes() { return Align(sizeof(RegionAllocatorHeader)); }

uint64_t RegionAllocator::ClassBytes(uint32_t size_class) {
  if (size_class >= kAllocatorSizeClasses) return 0;
  uint64_t size = kAlignment;
  for (uint32_t i = 0; i < size_class; ++i) size = Align((size * 5 + 3) / 4);
  return size;
}

uint32_t RegionAllocator::SizeClass(uint64_t bytes) {
  for (uint32_t i = 0; i < kAllocatorSizeClasses; ++i)
    if (bytes <= ClassBytes(i)) return i;
  return kAllocatorSizeClasses;  // large allocation: aligned bump chunk
}

uint64_t RegionAllocator::AccountedBytes(uint64_t bytes) {
  if (bytes == 0) throw std::invalid_argument("zero-sized allocation");
  const uint64_t requested = Align(bytes + Align(sizeof(RegionFreeBlock)));
  const uint32_t size_class = SizeClass(requested);
  return size_class < kAllocatorSizeClasses ? ClassBytes(size_class)
                                            : requested;
}

RegionAllocator RegionAllocator::Initialize(void *region, uint64_t region_bytes,
                                            uint32_t shard_count,
                                            uint64_t reserved_prefix_bytes,
                                            bool control_is_hwcc, bool block_is_hwcc,
                                            bool block_is_shared_payload) {
  if (region == nullptr || shard_count == 0 || shard_count > kMaxAllocatorShards ||
      region_bytes <= MetadataBytes() ||
      reserved_prefix_bytes > region_bytes - MetadataBytes())
    throw std::invalid_argument("invalid allocator region");
  if (reinterpret_cast<uintptr_t>(region) % kAlignment != 0)
    throw std::invalid_argument("allocator region is not cacheline aligned");
  std::memset(region, 0, MetadataBytes());
  auto *header = new (region) RegionAllocatorHeader;
  header->shard_count = shard_count;
  header->region_bytes = region_bytes;
  header->metadata_bytes = MetadataBytes();
  header->reserved_prefix_bytes = reserved_prefix_bytes;
  const uint64_t payload_begin = header->metadata_bytes + reserved_prefix_bytes;
  const uint64_t payload = region_bytes - payload_begin;
  for (uint32_t shard = 0; shard < shard_count; ++shard) {
    auto &entry = header->shards[shard];
    entry.begin = payload_begin + (payload * shard) / shard_count;
    entry.end = payload_begin + (payload * (shard + 1)) / shard_count;
    entry.bump = entry.begin;
  }
  FlushForRemoteVisibility(header, MetadataBytes(), !control_is_hwcc);
  return RegionAllocator(region, region_bytes, header, control_is_hwcc, block_is_hwcc,
                         block_is_shared_payload);
}

RegionAllocator RegionAllocator::Attach(void *region, uint64_t region_bytes,
                                        bool control_is_hwcc, bool block_is_hwcc,
                                            bool block_is_shared_payload) {
  if (region == nullptr || region_bytes <= MetadataBytes())
    throw std::invalid_argument("invalid allocator attachment");
  if (reinterpret_cast<uintptr_t>(region) % kAlignment != 0)
    throw std::invalid_argument("allocator attachment is not cacheline aligned");
  auto *header = static_cast<RegionAllocatorHeader *>(region);
  if (header->magic != 0x5449474f4e414c4cULL || header->version != 4 ||
      header->region_bytes != region_bytes || header->metadata_bytes != MetadataBytes() ||
      header->reserved_prefix_bytes > region_bytes - MetadataBytes() ||
      header->shard_count == 0 || header->shard_count > kMaxAllocatorShards)
    throw std::runtime_error("allocator attachment validation failed");
  return RegionAllocator(region, region_bytes, header, control_is_hwcc, block_is_hwcc,
                         block_is_shared_payload);
}

RegionAllocator RegionAllocator::InitializeWithExternalHeader(
    void *region, uint64_t region_bytes, RegionAllocatorHeader *header,
    bool control_is_hwcc, bool block_is_hwcc,
                                            bool block_is_shared_payload) {
  if (region == nullptr || header == nullptr || region_bytes == 0)
    throw std::invalid_argument("invalid external allocator header");
  new (header) RegionAllocatorHeader;
  header->shard_count = 1;
  header->region_bytes = region_bytes;
  header->metadata_bytes = 0;
  header->reserved_prefix_bytes = 0;
  // RegionOffset zero is the persistent null sentinel.  Dynamic arenas keep
  // their allocator header in the owner-private control area, so reserve one
  // cache line here rather than ever placing a reusable free block at offset 0.
  header->shards[0].begin = kAlignment;
  header->shards[0].end = region_bytes;
  header->shards[0].bump = kAlignment;
  return RegionAllocator(region, region_bytes, header, control_is_hwcc, block_is_hwcc,
                         block_is_shared_payload);
}

RegionAllocator RegionAllocator::AttachWithExternalHeader(
    void *region, uint64_t region_bytes, RegionAllocatorHeader *header,
    bool control_is_hwcc, bool block_is_hwcc,
                                            bool block_is_shared_payload) {
  if (region == nullptr || header == nullptr || region_bytes == 0 ||
      header->magic != 0x5449474f4e414c4cULL || header->version != 4 ||
      header->region_bytes != region_bytes || header->metadata_bytes != 0 ||
      header->reserved_prefix_bytes != 0 || header->shard_count != 1 ||
      header->shards[0].begin != kAlignment ||
      header->shards[0].bump < kAlignment)
    throw std::runtime_error("external allocator attachment validation failed");
  return RegionAllocator(region, region_bytes, header, control_is_hwcc, block_is_hwcc,
                         block_is_shared_payload);
}

void RegionAllocator::RecordMetadataRead(const void *address,
                                         uint64_t bytes) const {
  if (control_is_hwcc_)
    mem_access::HwccRead(address, bytes);
  else
    mem_access::PrivateRead(address, bytes);
}

void RegionAllocator::RecordMetadataWrite(const void *address,
                                          uint64_t bytes) const {
  if (control_is_hwcc_)
    mem_access::HwccWrite(address, bytes);
  else
    mem_access::PrivateWrite(address, bytes);
}

void RegionAllocator::RecordBlockMetadataRead(const void *address,
                                              uint64_t bytes) const {
  if (block_is_hwcc_)
    mem_access::HwccRead(address, bytes);
  else if (block_is_shared_payload_)
    mem_access::SharedPayloadRead(address, bytes);
  else
    mem_access::PrivateRead(address, bytes);
}

void RegionAllocator::RecordBlockMetadataWrite(const void *address,
                                               uint64_t bytes) const {
  if (block_is_hwcc_)
    mem_access::HwccWrite(address, bytes);
  else if (block_is_shared_payload_)
    mem_access::SharedPayloadWrite(address, bytes);
  else
    mem_access::PrivateWrite(address, bytes);
}

void RegionAllocator::Lock(RegionAllocatorShard &shard) const {
  uint32_t expected = 0;
  auto cas = [&] {
    if (control_is_hwcc_)
      return mem_access::HwccAtomicCompareExchangeWeak(
          shard.lock, expected, 1u, std::memory_order_acquire,
          std::memory_order_relaxed);
    return mem_access::PrivateAtomicCompareExchangeWeak(
        shard.lock, expected, 1u, std::memory_order_acquire,
        std::memory_order_relaxed);
  };
  while (!cas()) {
    expected = 0;
    _mm_pause();
  }
}

void RegionAllocator::Unlock(RegionAllocatorShard &shard) const {
  if (control_is_hwcc_)
    mem_access::HwccAtomicStore(shard.lock, 0u, std::memory_order_release);
  else
    mem_access::PrivateAtomicStore(shard.lock, 0u, std::memory_order_release);
}

void RegionAllocator::AccountAllocate(uint64_t bytes, DomainCounter *counter) {
  if (control_is_hwcc_) {
    mem_access::HwccAtomicFetchAdd(header_->allocated_bytes, bytes,
                                   std::memory_order_relaxed);
    mem_access::HwccAtomicFetchAdd(header_->allocation_count, uint64_t{1},
                                   std::memory_order_relaxed);
  }
  const auto domain = control_is_hwcc_
                          ? latency_sim::MemoryDomain::kHwcc
                          : latency_sim::MemoryDomain::kOwnerPrivateSwcc;
  const uint64_t used = latency_sim::FixedLatencyAtomicFetchAdd(
                            counter->used_bytes, bytes,
                            std::memory_order_relaxed, domain) + bytes;
  uint64_t peak = latency_sim::FixedLatencyAtomicLoad(
      counter->peak_bytes, std::memory_order_relaxed, domain);
  while (peak < used) {
    if (latency_sim::FixedLatencyAtomicCompareExchangeWeak(
            counter->peak_bytes, peak, used, std::memory_order_relaxed,
            std::memory_order_relaxed, domain))
      break;
  }
}

void RegionAllocator::AccountFree(uint64_t bytes, DomainCounter *counter) {
  const auto domain = control_is_hwcc_
                          ? latency_sim::MemoryDomain::kHwcc
                          : latency_sim::MemoryDomain::kOwnerPrivateSwcc;
  CheckedAtomicSubtract(&counter->used_bytes, bytes,
                        "allocator domain accounting underflow", domain);
  if (control_is_hwcc_) {
    CheckedAtomicSubtract(&header_->allocated_bytes, bytes,
                          "allocator total accounting underflow",
                          latency_sim::MemoryDomain::kHwcc);
    mem_access::HwccAtomicFetchAdd(header_->free_count, uint64_t{1},
                                   std::memory_order_relaxed);
  }
}

void RegionAllocator::FlushAllocatedRanges() const {
  FlushForRemoteVisibility(header_, MetadataBytes(), !control_is_hwcc_);
  RecordMetadataRead(&header_->shard_count, sizeof(header_->shard_count));
  for (uint32_t shard = 0; shard < header_->shard_count; ++shard) {
    const auto &entry = header_->shards[shard];
    RecordMetadataRead(&entry.begin,
                       sizeof(entry.begin) + sizeof(entry.end) +
                           sizeof(entry.bump));
    const uint64_t bump = entry.bump;
    if (bump > entry.begin)
      FlushForRemoteVisibility(base_ + entry.begin, bump - entry.begin,
                               !control_is_hwcc_);
  }
}

void RegionAllocator::FlushOwnedRange(uint32_t owner_shard) const {
  RecordMetadataRead(&header_->shard_count, sizeof(header_->shard_count));
  if (owner_shard >= header_->shard_count)
    throw std::invalid_argument("flush owner shard outside allocator");
  const auto &entry = header_->shards[owner_shard];
  FlushForRemoteVisibility(&entry, sizeof(entry), !control_is_hwcc_);
  // The source loads bump/begin after invalidating the allocator shard. Keep
  // the simulated cache order identical so these reads cannot inherit a
  // pre-invalidation hit.
  RecordMetadataRead(&entry.begin,
                     sizeof(entry.begin) + sizeof(entry.end) +
                         sizeof(entry.bump));
  if (entry.bump > entry.begin)
    FlushForRemoteVisibility(base_ + entry.begin, entry.bump - entry.begin,
                             !control_is_hwcc_);
}

void *RegionAllocator::AllocateFromShard(uint64_t bytes, uint32_t size_class,
                                         uint32_t owner_shard) {
  auto &shard = header_->shards[owner_shard];
  Lock(shard);
  if (size_class < kAllocatorSizeClasses) {
    const RegionOffset head = FixedLatencyLoadFor(
        shard.free_heads[size_class], control_is_hwcc_,
        /*shared_payload=*/false, std::memory_order_relaxed);
    if (head != kNullOffset) {
      auto *block = static_cast<RegionFreeBlock *>(FromOffset(head));
      const RegionOffset next = FixedLatencyLoadFor(
          block->next, block_is_hwcc_, block_is_shared_payload_,
          std::memory_order_relaxed);
      FixedLatencyStoreFor(shard.free_heads[size_class], next,
                           control_is_hwcc_, /*shared_payload=*/false,
                           std::memory_order_relaxed);
      Unlock(shard);
      return block;
    }
  }
  const uint64_t allocation_bytes = size_class < kAllocatorSizeClasses ? ClassBytes(size_class) : bytes;
  RecordMetadataRead(&shard.end, sizeof(shard.end) + sizeof(shard.bump));
  const uint64_t begin = Align(shard.bump);
  if (begin > shard.end || allocation_bytes > shard.end - begin) {
    Unlock(shard);
    throw std::bad_alloc();
  }
  RecordMetadataWrite(&shard.bump, sizeof(shard.bump));
  shard.bump = begin + allocation_bytes;
  Unlock(shard);
  return base_ + begin;
}

void *RegionAllocator::Allocate(uint64_t bytes, AllocationDomain, DomainCounter *counter,
                                uint32_t owner_shard) {
  RecordMetadataRead(&header_->shard_count, sizeof(header_->shard_count));
  if (bytes == 0 || counter == nullptr || owner_shard >= header_->shard_count)
    throw std::invalid_argument("invalid allocation");
  const uint64_t requested = Align(bytes + Align(sizeof(RegionFreeBlock)));
  const uint32_t size_class = SizeClass(requested);
  const uint64_t header_bytes = Align(sizeof(RegionFreeBlock));
  void *result = AllocateFromShard(requested, size_class, owner_shard);
  if (size_class < kAllocatorSizeClasses) {
    auto *fresh_block = new (result) RegionFreeBlock;
    FixedLatencyStoreFor(fresh_block->next, kNullOffset, block_is_hwcc_,
                    block_is_shared_payload_, std::memory_order_relaxed);
    RecordBlockMetadataWrite(&fresh_block->size_class,
                        sizeof(fresh_block->size_class) +
                            sizeof(fresh_block->owner_shard));
    fresh_block->size_class = size_class;
    fresh_block->owner_shard = owner_shard;
  }
  AccountAllocate(size_class < kAllocatorSizeClasses ? ClassBytes(size_class) : requested, counter);
  return static_cast<std::byte *>(result) + header_bytes;
}

void RegionAllocator::FreeLocal(RegionOffset offset, uint32_t size_class, uint32_t owner_shard) {
  auto &shard = header_->shards[owner_shard];
  Lock(shard);
  auto *block = static_cast<RegionFreeBlock *>(FromOffset(offset));
  const RegionOffset head = FixedLatencyLoadFor(
      shard.free_heads[size_class], control_is_hwcc_,
      /*shared_payload=*/false, std::memory_order_relaxed);
  FixedLatencyStoreFor(block->next, head, block_is_hwcc_, block_is_shared_payload_,
                  std::memory_order_relaxed);
  FixedLatencyStoreFor(shard.free_heads[size_class], offset, control_is_hwcc_,
                       /*shared_payload=*/false, std::memory_order_release);
  Unlock(shard);
}

void RegionAllocator::Free(void *pointer, uint64_t bytes, AllocationDomain,
                           DomainCounter *counter, uint32_t owner_shard,
                           uint32_t current_shard) {
  RecordMetadataRead(&header_->shard_count, sizeof(header_->shard_count));
  if (!Contains(pointer) || bytes == 0 || counter == nullptr ||
      owner_shard >= header_->shard_count || current_shard >= header_->shard_count)
    throw std::invalid_argument("invalid free");
  const uint64_t requested = Align(bytes + Align(sizeof(RegionFreeBlock)));
  const uint32_t size_class = SizeClass(requested);
  if (size_class == kAllocatorSizeClasses)
    throw std::invalid_argument("large allocations are not individually reusable");
  auto *block = reinterpret_cast<RegionFreeBlock *>(
      static_cast<std::byte *>(pointer) - Align(sizeof(RegionFreeBlock)));
  RecordBlockMetadataRead(&block->size_class,
                     sizeof(block->size_class) + sizeof(block->owner_shard));
  if (block->owner_shard != owner_shard || block->size_class != size_class)
    throw std::runtime_error("allocator owner or size-class mismatch");
  const RegionOffset offset = ToOffset(block);
  if (owner_shard != current_shard)
    throw std::runtime_error("allocator free attempted by non-owner shard");
  FreeLocal(offset, size_class, owner_shard);
  AccountFree(ClassBytes(size_class), counter);
}

RegionOffset RegionAllocator::ToOffset(const void *pointer) const {
  const auto *p = static_cast<const std::byte *>(pointer);
  if (p == nullptr) return kNullOffset;
  if (p < base_ || p >= base_ + bytes_) throw std::invalid_argument("pointer outside region");
  return static_cast<RegionOffset>(p - base_);
}

void *RegionAllocator::FromOffset(RegionOffset offset) const {
  if (offset == kNullOffset) return nullptr;
  if (offset >= bytes_) throw std::invalid_argument("offset outside region");
  return base_ + offset;
}

bool RegionAllocator::Contains(const void *pointer) const {
  const auto *p = static_cast<const std::byte *>(pointer);
  if (p == nullptr) return false;
  RecordMetadataRead(&header_->metadata_bytes,
                     sizeof(header_->metadata_bytes) +
                         sizeof(header_->reserved_prefix_bytes));
  return p >= base_ + header_->metadata_bytes +
           header_->reserved_prefix_bytes + Align(sizeof(RegionFreeBlock)) &&
         p < base_ + bytes_;
}

bool DualRegionAllocator::IsHwccDomain(AllocationDomain domain) {
  switch (domain) {
    case AllocationDomain::kHwccIndex:
    case AllocationDomain::kHwccMetadata:
    case AllocationDomain::kHwccEbr:
    case AllocationDomain::kHwccLayout:
    case AllocationDomain::kTransport:
      return true;
    case AllocationDomain::kOwnerPrivateSwcc:
    case AllocationDomain::kSharedPayloadSwcc:
      return false;
    // Fixed accounting labels: allocator headers are initialized in place and
    // must never be requested through the dynamic allocation API.
    case AllocationDomain::kHwccAllocatorMetadata:
    case AllocationDomain::kSwccAllocatorMetadata:
    case AllocationDomain::kCount:
      throw std::invalid_argument("non-allocatable allocation domain");
  }
  throw std::invalid_argument("invalid allocation domain");
}

DualRegionAllocator DualRegionAllocator::Initialize(void *pool,
                                                    const DualRegionConfig &config) {
  if (pool == nullptr || config.total_pool_bytes == 0 || config.vm_count == 0 ||
      config.partition_count == 0 ||
      config.hwcc_size_bytes <= sizeof(DualRegionPersistentHeader) ||
      config.swcc_size_bytes <= sizeof(RegionAllocatorHeader) ||
      config.hwcc_offset_bytes + config.hwcc_size_bytes > config.total_pool_bytes ||
      config.swcc_offset_bytes + config.swcc_size_bytes > config.total_pool_bytes ||
      (config.hwcc_offset_bytes < config.swcc_offset_bytes + config.swcc_size_bytes &&
       config.swcc_offset_bytes < config.hwcc_offset_bytes + config.hwcc_size_bytes))
    throw std::invalid_argument("invalid dual-region configuration");
  if (!(config.owner_private_swcc_fraction > 0.0 &&
        config.owner_private_swcc_fraction < 1.0))
    throw std::invalid_argument("invalid owner-private SWCC fraction");
  auto *base = static_cast<std::byte *>(pool);
  if (reinterpret_cast<uintptr_t>(base) % RegionAllocator::kAlignment != 0)
    throw std::invalid_argument("dual-region pool is not cacheline aligned");
  auto *header = new (base + config.hwcc_offset_bytes) DualRegionPersistentHeader;
  // magic is the immutable-layout publication flag.  Keep it clear while
  // this VM fills the remaining fixed fields, so joining VMs cannot validate
  // a half-constructed default header.
  header->layout.config_hash = config.config_hash;
  header->layout.total_pool_bytes = config.total_pool_bytes;
  header->layout.hwcc_offset_bytes = config.hwcc_offset_bytes;
  header->layout.hwcc_size_bytes = config.hwcc_size_bytes;
  header->layout.swcc_offset_bytes = config.swcc_offset_bytes;
  header->layout.swcc_size_bytes = config.swcc_size_bytes;
  header->layout.vm_count = config.vm_count;
  header->layout.partition_count = config.partition_count;
  header->layout.fixed_key_size = config.fixed_key_size;
  header->layout.fixed_value_size = config.fixed_value_size;
  const uint64_t dual_header_bytes =
      (sizeof(*header) + RegionAllocator::kAlignment - 1) &
      ~(RegionAllocator::kAlignment - 1);
  if (config.hwcc_size_bytes < dual_header_bytes)
    throw std::invalid_argument("HWCC region is smaller than dual header");
  header->hwcc_allocator_offset = config.hwcc_offset_bytes + dual_header_bytes;
  header->hwcc_allocator_bytes = config.hwcc_size_bytes -
                                  (header->hwcc_allocator_offset - config.hwcc_offset_bytes);
  header->swcc_allocator_offset = config.swcc_offset_bytes;
  header->swcc_allocator_bytes = config.swcc_size_bytes;
  const uint64_t swcc_metadata_bytes =
      (sizeof(RegionAllocatorHeader) + RegionAllocator::kAlignment - 1) &
      ~(RegionAllocator::kAlignment - 1);
  const uint64_t swcc_payload_bytes = config.swcc_size_bytes - swcc_metadata_bytes;
  const uint64_t owner_private_total =
      (static_cast<uint64_t>(swcc_payload_bytes * config.owner_private_swcc_fraction) /
       RegionAllocator::kAlignment) * RegionAllocator::kAlignment;
  const uint64_t control_body =
      (sizeof(OwnerAllocatorControl) + RegionAllocator::kAlignment - 1) /
      RegionAllocator::kAlignment * RegionAllocator::kAlignment;
  const uint64_t external_header_bytes =
      (sizeof(RegionAllocatorHeader) + RegionAllocator::kAlignment - 1) /
      RegionAllocator::kAlignment * RegionAllocator::kAlignment;
  header->owner_controls_offset = swcc_metadata_bytes;
  header->owner_control_stride = control_body + 2 * external_header_bytes;
  if (config.vm_count > UINT64_MAX / header->owner_control_stride)
    throw std::invalid_argument("owner allocator control geometry overflows");
  header->owner_controls_bytes =
      static_cast<uint64_t>(config.vm_count) * header->owner_control_stride;
  if (owner_private_total <= header->owner_controls_bytes)
    throw std::invalid_argument("owner allocator controls exceed private SWCC");
  header->owner_private_arenas_offset =
      header->owner_controls_offset + header->owner_controls_bytes;
  const uint64_t arena_space = owner_private_total - header->owner_controls_bytes;
  header->owner_private_arena_stride =
      (arena_space / config.partition_count / RegionAllocator::kAlignment) *
      RegionAllocator::kAlignment;
  const uint64_t minimum_arena =
      ((sizeof(OwnerPrivateArenaHeader) + RegionAllocator::kAlignment - 1) /
       RegionAllocator::kAlignment * RegionAllocator::kAlignment) +
      2 * RegionAllocator::kAlignment;
  if (header->owner_private_arena_stride < minimum_arena)
    throw std::invalid_argument("owner-private arena is too small");
  header->owner_private_arenas_bytes =
      header->owner_private_arena_stride * config.partition_count;
  const uint64_t private_total_used =
      header->owner_controls_bytes + header->owner_private_arenas_bytes;
  if (private_total_used > owner_private_total)
    throw std::invalid_argument("owner-private geometry exceeds reserved SWCC");
  // Static transport/EBR allocations are made by VM0 before owner startup;
  // dynamic objects use owner-private control headers created in phase two.
  auto hwcc = RegionAllocator::Initialize(base + header->hwcc_allocator_offset,
                                          header->hwcc_allocator_bytes, 1,
                                          0,
                                          true, true);
  // The global SWCC header is initialized only in this VM's reset path.  It
  // is not retained as a process-wide resolver: owner controls and dynamic
  // arenas are reached through the immutable SWCC integer geometry below.
  (void)RegionAllocator::Initialize(
      base + header->swcc_allocator_offset, header->swcc_allocator_bytes, 1,
      header->owner_controls_bytes + header->owner_private_arenas_bytes, false, false);
  for (uint32_t partition = 0; partition < config.partition_count; ++partition) {
    // This is only an immutable offset publication.  The actual arena header
    // and allocator controls are constructed by its owner during phase two.
    header->layout.partitions[partition].private_arena =
        header->owner_private_arenas_offset +
        partition * header->owner_private_arena_stride;
  }
  auto set_fixed_domain = [&](AllocationDomain domain, uint64_t bytes) {
    auto &counter = header->layout.domains[static_cast<size_t>(domain)];
    mem_access::HwccAtomicStore(counter.used_bytes, bytes,
                                std::memory_order_relaxed);
    mem_access::HwccAtomicStore(counter.peak_bytes, bytes,
                                std::memory_order_relaxed);
  };
  set_fixed_domain(AllocationDomain::kHwccLayout, dual_header_bytes);
  set_fixed_domain(AllocationDomain::kHwccAllocatorMetadata,
                   hwcc.metadata_bytes());
  set_fixed_domain(AllocationDomain::kSwccAllocatorMetadata,
                   swcc_metadata_bytes);
  // Remain unpublished until transport, EBR, dynamic descriptors and the
  // immutable static bounds have all been written by KVEngine.
  FlushForRemoteVisibility(header, sizeof(*header), false);
  return DualRegionAllocator(base, config, header, hwcc);
}

void DualRegionAllocator::InitializeOwnerPrivateArenas(uint32_t node_id) {
  if (node_id >= header_->layout.vm_count)
    throw std::invalid_argument("owner-private initialization outside layout");
  const auto &descriptor = header_->layout.owner_dynamic_arenas[node_id];
  if (descriptor.hwcc_offset == kNullOffset || descriptor.hwcc_bytes == 0 ||
      descriptor.shared_swcc_offset == kNullOffset ||
      descriptor.shared_swcc_bytes == 0)
    throw std::runtime_error(
        "owner-private initialization observed unpublished dynamic descriptor");
  const uint64_t control_body =
      (sizeof(OwnerAllocatorControl) + RegionAllocator::kAlignment - 1) /
      RegionAllocator::kAlignment * RegionAllocator::kAlignment;
  const uint64_t header_bytes =
      (sizeof(RegionAllocatorHeader) + RegionAllocator::kAlignment - 1) /
      RegionAllocator::kAlignment * RegionAllocator::kAlignment;
  auto *control = static_cast<OwnerAllocatorControl *>(SwccFromOffset(
      header_->owner_controls_offset + node_id * header_->owner_control_stride));
  new (control) OwnerAllocatorControl;
  control->owner_shard = node_id;
  bound_owner_shard_ = node_id;
  auto *hwcc_header = reinterpret_cast<RegionAllocatorHeader *>(
      reinterpret_cast<std::byte *>(control) + control_body);
  auto *shared_header = reinterpret_cast<RegionAllocatorHeader *>(
      reinterpret_cast<std::byte *>(control) + control_body + header_bytes);
  new (hwcc_header) RegionAllocatorHeader;
  new (shared_header) RegionAllocatorHeader;
  control->dynamic_hwcc_allocator = SwccToOffset(hwcc_header);
  control->dynamic_shared_swcc_allocator = SwccToOffset(shared_header);
  dynamic_hwcc_[node_id] = std::make_unique<RegionAllocator>(
      RegionAllocator::InitializeWithExternalHeader(
          hwcc_.FromOffset(descriptor.hwcc_offset), descriptor.hwcc_bytes,
          hwcc_header, false, true));
  dynamic_shared_swcc_[node_id] = std::make_unique<RegionAllocator>(
      RegionAllocator::InitializeWithExternalHeader(
          SwccFromOffset(descriptor.shared_swcc_offset),
          descriptor.shared_swcc_bytes, shared_header, false, false, true));
  mem_access::PrivateWrite(control,
                           header_->owner_control_stride);
  for (uint32_t partition = 0; partition < header_->layout.partition_count;
       ++partition) {
    if (partition % header_->layout.vm_count != node_id) continue;
    auto *arena = static_cast<OwnerPrivateArenaHeader *>(SwccFromOffset(
        header_->owner_private_arenas_offset +
        partition * header_->owner_private_arena_stride));
    new (arena) OwnerPrivateArenaHeader;
    arena->partition_id = partition;
    arena->owner_shard = node_id;
    if (pthread_spin_init(&arena->clock.lock, PTHREAD_PROCESS_PRIVATE) != 0)
      throw std::runtime_error("owner-private Clock tracker lock init failed");
    arena->begin = header_->owner_private_arenas_offset +
                   partition * header_->owner_private_arena_stride +
                   ((sizeof(OwnerPrivateArenaHeader) + RegionAllocator::kAlignment - 1) &
                    ~(RegionAllocator::kAlignment - 1));
    arena->end = header_->owner_private_arenas_offset +
                 (partition + 1) * header_->owner_private_arena_stride;
    arena->bump = arena->begin;
    FlushForRemoteVisibility(arena,
                             (sizeof(OwnerPrivateArenaHeader) +
                              RegionAllocator::kAlignment - 1) &
                                 ~(RegionAllocator::kAlignment - 1));
    owner_private_arenas_[partition] = arena;
  }
}

void DualRegionAllocator::BindOwnerPrivateArenaHandles(uint32_t node_id) {
  if (node_id >= config_.vm_count)
    throw std::invalid_argument("allocator owner outside local configuration");
  // Layout compatibility was validated during Attach/Open.  This one-time
  // reconstruction deliberately touches only arenas owned by this VM.
  for (uint32_t partition = 0; partition < config_.partition_count;
       ++partition) {
    if (partition % config_.vm_count != node_id) continue;
    auto *arena = static_cast<OwnerPrivateArenaHeader *>(SwccFromOffset(
        header_->owner_private_arenas_offset +
        partition * header_->owner_private_arena_stride));
    mem_access::PrivateRead(arena, offsetof(OwnerPrivateArenaHeader, owner_shard));
    if (arena->magic != 0x5449474f4e41524eULL || arena->version != 3 ||
        arena->partition_id != partition || arena->owner_shard != node_id)
      throw std::runtime_error("owner-private arena attachment validation failed");
    owner_private_arenas_[partition] = arena;
  }
}

void DualRegionAllocator::BindOwnerPrivateAllocators(uint32_t node_id) {
  if (node_id >= header_->layout.vm_count)
    throw std::invalid_argument("allocator owner outside layout");
  BindOwnerPrivateArenaHandles(node_id);
  bound_owner_shard_ = node_id;
  const auto &descriptor = header_->layout.owner_dynamic_arenas[node_id];
  auto *control = OwnerControl(node_id);
  if (descriptor.hwcc_offset == kNullOffset ||
      descriptor.shared_swcc_offset == kNullOffset ||
      control->dynamic_hwcc_allocator == kNullOffset ||
      control->dynamic_shared_swcc_allocator == kNullOffset)
    throw std::runtime_error("owner-private allocator controls are not initialized");
  auto *hwcc_header = static_cast<RegionAllocatorHeader *>(
      SwccFromOffset(control->dynamic_hwcc_allocator));
  auto *shared_header = static_cast<RegionAllocatorHeader *>(
      SwccFromOffset(control->dynamic_shared_swcc_allocator));
  dynamic_hwcc_[node_id] = std::make_unique<RegionAllocator>(
      RegionAllocator::AttachWithExternalHeader(
          hwcc_.FromOffset(descriptor.hwcc_offset), descriptor.hwcc_bytes,
          hwcc_header, false, true));
  dynamic_shared_swcc_[node_id] = std::make_unique<RegionAllocator>(
      RegionAllocator::AttachWithExternalHeader(
          SwccFromOffset(descriptor.shared_swcc_offset),
          descriptor.shared_swcc_bytes, shared_header, false, false, true));
}

void DualRegionAllocator::FinalizeStaticHwccLayout() {
  if (static_hwcc_finalized_)
    throw std::logic_error("dynamic allocator ranges finalized more than once");
  const auto *static_header = reinterpret_cast<const RegionAllocatorHeader *>(
      pool_ + header_->hwcc_allocator_offset);
  mem_access::HwccRead(static_header, sizeof(*static_header));
  const auto &static_shard = static_header->shards[0];
  const uint64_t start = (static_shard.bump + RegionAllocator::kAlignment - 1) &
                         ~(RegionAllocator::kAlignment - 1);
  if (start >= hwcc_.capacity())
    throw std::runtime_error("no dynamic HWCC space after static allocation");
  const uint64_t hwcc_remaining = hwcc_.capacity() - start;
  const uint64_t shared_start = header_->owner_private_arenas_offset +
                                header_->owner_private_arenas_bytes;
  if (shared_start >= config_.swcc_size_bytes)
    throw std::runtime_error("no shared SWCC payload space after private arenas");
  const uint64_t shared_remaining = config_.swcc_size_bytes - shared_start;
  const auto align_up = [](uint64_t value) {
    return (value + RegionAllocator::kAlignment - 1) &
           ~(RegionAllocator::kAlignment - 1);
  };
  const auto align_down = [](uint64_t value) {
    return value & ~(RegionAllocator::kAlignment - 1);
  };
  for (uint32_t owner = 0; owner < header_->layout.vm_count; ++owner) {
    auto &entry = header_->layout.owner_dynamic_arenas[owner];
    mem_access::HwccWrite(&entry, sizeof(entry));
    const uint64_t hwcc_begin = align_up(
        start + hwcc_remaining * owner / header_->layout.vm_count);
    const uint64_t hwcc_end = align_down(
        start + hwcc_remaining * (owner + 1) / header_->layout.vm_count);
    const uint64_t shared_begin = align_up(
        shared_start + shared_remaining * owner / header_->layout.vm_count);
    const uint64_t shared_end = align_down(
        shared_start + shared_remaining * (owner + 1) / header_->layout.vm_count);
    entry.hwcc_offset = hwcc_begin;
    entry.hwcc_bytes = hwcc_end - hwcc_begin;
    entry.shared_swcc_offset = shared_begin;
    entry.shared_swcc_bytes = shared_end - shared_begin;
    if (entry.hwcc_bytes == 0 || entry.shared_swcc_bytes == 0)
      throw std::runtime_error("dynamic owner arena has zero capacity");
  }
  FlushForRemoteVisibility(header_->layout.owner_dynamic_arenas.data(),
                           sizeof(header_->layout.owner_dynamic_arenas), false);
  static_hwcc_finalized_ = true;
}

void DualRegionAllocator::PublishStaticHwccLayout() {
  if (!static_hwcc_finalized_)
    throw std::logic_error("static HWCC layout must be finalized before publication");
  if (mem_access::HwccAtomicLoad(header_->layout.magic,
                                 std::memory_order_acquire) != 0)
    throw std::logic_error("static HWCC layout published more than once");
  for (uint32_t owner = 0; owner < header_->layout.vm_count; ++owner) {
    const auto &descriptor = header_->layout.owner_dynamic_arenas[owner];
    if (descriptor.hwcc_offset == kNullOffset || descriptor.hwcc_bytes == 0 ||
        descriptor.shared_swcc_offset == kNullOffset ||
        descriptor.shared_swcc_bytes == 0)
      throw std::runtime_error("static HWCC layout has incomplete owner descriptor");
  }
  if ((header_->transport_bytes != 0 &&
       header_->transport_offset + header_->transport_bytes >
           header_->hwcc_allocator_bytes) ||
      (header_->ebr_bytes != 0 &&
       header_->ebr_offset + header_->ebr_bytes > header_->hwcc_allocator_bytes))
    throw std::runtime_error("static HWCC layout has invalid transport/EBR bounds");
#ifndef NDEBUG
  if (const char *barrier = StartupAttachBarrierPath(); barrier != nullptr) {
    const std::string marker = barrier;
    const std::string release = marker + ".release";
    WaitForBarrierFile(marker);
    TouchBarrierFile(release);
  }
#endif
  // This is the sole immutable-layout publication point.  Every field above,
  // including transport/EBR allocations made by KVEngine, is visible before
  // the release-store of magic.
  mem_access::HwccAtomicStore(header_->layout.magic, kSharedLayoutMagic,
                              std::memory_order_release);
  FlushForRemoteVisibility(&header_->layout, sizeof(header_->layout), false);
}

DualRegionAllocator DualRegionAllocator::Attach(void *pool,
                                                const DualRegionConfig &config) {
  if (pool == nullptr) throw std::invalid_argument("null dual-region pool");
  auto *base = static_cast<std::byte *>(pool);
  auto *header = reinterpret_cast<DualRegionPersistentHeader *>(base + config.hwcc_offset_bytes);
#ifndef NDEBUG
  if (const char *barrier = StartupAttachBarrierPath(); barrier != nullptr) {
    const std::string marker = barrier;
    const std::string release = marker + ".release";
    TouchBarrierFile(marker);
    WaitForBarrierFile(release);
  }
#endif
  // A joining VM can map the backing before VM0 has published the immutable
  // layout.  Wait for that HWCC publication once, then validate it below.
  // Retrying Attach from the KV facade races the same phase and can expire
  // while VM0 is still constructing the static transport/EBR prefix.
  for (;;) {
    if (mem_access::HwccAtomicLoad(header->layout.magic,
                                   std::memory_order_acquire) ==
        kSharedLayoutMagic)
      break;
    _mm_pause();
  }
  // A joining VM must attach while the layout is still Initializing so it can
  // construct its own private arena/root.  Ready is intentionally checked by
  // KVEngine only after all owner bits are published.
  const uint64_t dual_header_bytes =
      (sizeof(*header) + RegionAllocator::kAlignment - 1) &
      ~(RegionAllocator::kAlignment - 1);
  if (config.hwcc_size_bytes < dual_header_bytes)
    throw std::runtime_error("dual-region HWCC size is smaller than dual header");
  if (header->hwcc_allocator_offset !=
          config.hwcc_offset_bytes + dual_header_bytes ||
      header->hwcc_allocator_bytes !=
          config.hwcc_size_bytes - dual_header_bytes)
    throw std::runtime_error("dual-region HWCC allocator bounds are invalid");
  const bool compatible =
      mem_access::HwccAtomicLoad(header->layout.magic,
                                 std::memory_order_acquire) ==
          kSharedLayoutMagic &&
      header->layout.layout_version == kSharedLayoutVersion &&
      header->layout.config_hash == config.config_hash &&
      header->layout.total_pool_bytes == config.total_pool_bytes &&
      header->layout.vm_count == config.vm_count &&
      header->layout.partition_count == config.partition_count &&
      header->layout.hwcc_offset_bytes == config.hwcc_offset_bytes &&
      header->layout.hwcc_size_bytes == config.hwcc_size_bytes &&
      header->layout.swcc_offset_bytes == config.swcc_offset_bytes &&
      header->layout.swcc_size_bytes == config.swcc_size_bytes &&
      header->layout.fixed_key_size == config.fixed_key_size &&
      header->layout.fixed_value_size == config.fixed_value_size &&
      header->owner_private_arena_stride != 0;
  if (!compatible) {
    std::ostringstream detail;
    detail << "dual-region layout attachment validation failed"
           << " version=" << header->layout.layout_version
           << " hash=" << header->layout.config_hash
           << " expected_hash=" << config.config_hash
           << " vm_count=" << header->layout.vm_count
           << " expected_vm_count=" << config.vm_count
           << " partition_count=" << header->layout.partition_count
           << " expected_partition_count=" << config.partition_count;
    throw std::runtime_error(detail.str());
  }
  auto hwcc = RegionAllocator::Attach(base + header->hwcc_allocator_offset,
                                      header->hwcc_allocator_bytes, true, true);
  // Attach never reads the non-coherent global SWCC allocator header.  The
  // owner-private controls are validated only after this VM binds its slot.
  DualRegionAllocator attached(base, config, header, hwcc);
  attached.static_hwcc_finalized_ = true;
  return attached;
}

void DualRegionAllocator::PublishOwnerInitialized(uint32_t node_id) {
  if (node_id >= header_->layout.vm_count || node_id >= 64)
    throw std::invalid_argument("owner-ready node outside layout");
  if (mem_access::HwccAtomicLoad(header_->layout.state,
                                 std::memory_order_acquire) !=
      static_cast<uint32_t>(LayoutState::kInitializing))
    throw std::logic_error("owner initialized after layout became ready");
  // The owner, and only the owner, makes its private SWCC initialization
  // visible before advertising the corresponding startup bit.
  FlushOwnedRanges(node_id);
  const uint64_t bit = 1ULL << node_id;
  const uint64_t previous = mem_access::HwccAtomicFetchOr(
      header_->layout.owner_init_ready_bitmap, bit,
      std::memory_order_release);
  if ((previous & bit) != 0)
    throw std::logic_error("owner initialization published more than once");
  FlushForRemoteVisibility(&header_->layout.owner_init_ready_bitmap,
                           sizeof(header_->layout.owner_init_ready_bitmap), false);
}

void DualRegionAllocator::WaitForOwnersAndPublishReady() {
  if (header_->layout.vm_count == 0 || header_->layout.vm_count > 64)
    throw std::logic_error("invalid owner-ready bitmap width");
  const uint64_t expected = header_->layout.vm_count == 64
                                ? ~uint64_t{0}
                                : ((uint64_t{1} << header_->layout.vm_count) - 1);
  for (;;) {
    if (mem_access::HwccAtomicLoad(header_->layout.owner_init_ready_bitmap,
                                   std::memory_order_acquire) == expected)
      break;
    _mm_pause();
  }
  PublishReady();
}

void DualRegionAllocator::WaitUntilReady() const {
  for (;;) {
    if (mem_access::HwccAtomicLoad(header_->layout.state,
                                   std::memory_order_acquire) ==
        static_cast<uint32_t>(LayoutState::kReady))
      return;
    _mm_pause();
  }
}

void DualRegionAllocator::PublishReady() {
  const uint32_t state = mem_access::HwccAtomicLoad(
      header_->layout.state, std::memory_order_acquire);
  if (state != static_cast<uint32_t>(LayoutState::kInitializing)) {
    throw std::logic_error(
        "dual-region layout ready published with state=" +
        std::to_string(state));
  }
  const uint64_t expected = header_->layout.vm_count == 64
                                ? ~uint64_t{0}
                                : ((uint64_t{1} << header_->layout.vm_count) - 1);
  if (mem_access::HwccAtomicLoad(header_->layout.owner_init_ready_bitmap,
                                 std::memory_order_acquire) != expected)
    throw std::logic_error("layout ready before every owner initialized");
  // Each owner flushed its own private arena before setting its bit. VM0 only
  // performs the HWCC release; it must not walk another VM's private SWCC.
  mem_access::HwccAtomicStore(
      header_->layout.state, static_cast<uint32_t>(LayoutState::kReady),
      std::memory_order_release);
  FlushForRemoteVisibility(&header_->layout.state, sizeof(header_->layout.state),
                           false);
}

void *DualRegionAllocator::Allocate(uint64_t bytes, AllocationDomain domain,
                                    uint32_t owner_shard) {
  if (domain == AllocationDomain::kOwnerPrivateSwcc)
    throw std::invalid_argument(
        "owner-private allocation requires partition-specific arena");
  if (owner_shard >= header_->layout.vm_count)
    throw std::invalid_argument("allocation owner outside layout");
  if (domain == AllocationDomain::kHwccIndex ||
      domain == AllocationDomain::kHwccMetadata) {
    auto *allocator = dynamic_hwcc_[owner_shard].get();
    if (allocator == nullptr)
      throw std::runtime_error("dynamic HWCC allocator is not initialized by owner");
    auto *control = OwnerControl(owner_shard);
    void *result = allocator->Allocate(bytes, domain, &control->dynamic_hwcc, 0);
    mem_access::PrivateAtomicFetchAdd(control->total_hw_cc_usage, bytes,
                                      std::memory_order_relaxed);
    return result;
  }
  if (domain == AllocationDomain::kSharedPayloadSwcc) {
    auto *allocator = dynamic_shared_swcc_[owner_shard].get();
    if (allocator == nullptr)
      throw std::runtime_error("shared SWCC allocator is not initialized by owner");
    auto *control = OwnerControl(owner_shard);
    return allocator->Allocate(bytes, domain, &control->shared_payload, 0);
  }
  if (domain != AllocationDomain::kTransport && domain != AllocationDomain::kHwccEbr)
    throw std::invalid_argument("invalid static allocation domain");
  if (static_hwcc_finalized_)
    throw std::logic_error("static HWCC allocation after dynamic ranges published");
  DomainCounter &counter = header_->layout.domains[static_cast<size_t>(domain)];
  void *result = hwcc_.Allocate(bytes, domain, &counter, 0);
  const RegionOffset offset = hwcc_.ToOffset(result);
  const uint64_t accounted = RegionAllocator::AccountedBytes(bytes);
  uint64_t *begin = domain == AllocationDomain::kTransport
                        ? &header_->transport_offset : &header_->ebr_offset;
  uint64_t *length = domain == AllocationDomain::kTransport
                         ? &header_->transport_bytes : &header_->ebr_bytes;
  mem_access::HwccWrite(begin, sizeof(*begin) + sizeof(*length));
  if (*length == 0) *begin = offset;
  if (offset < *begin) *begin = offset;
  const uint64_t end = offset + accounted;
  const uint64_t current_end = *begin + *length;
  if (end < offset || end > current_end) *length = end - *begin;
  return result;
}

OwnerPrivateArenaHeader *DualRegionAllocator::Arena(uint32_t partition_id) const {
  if (partition_id >= owner_private_arenas_.size())
    throw std::invalid_argument("owner-private arena partition outside layout");
  auto *arena = owner_private_arenas_[partition_id];
  if (arena == nullptr)
    throw std::runtime_error("owner-private arena is not bound by this VM");
  return arena;
}

OwnerAllocatorControl *DualRegionAllocator::OwnerControl(
    uint32_t owner_shard) const {
  if (owner_shard >= header_->layout.vm_count)
    throw std::runtime_error("owner allocator control is outside layout");
  if (bound_owner_shard_ == UINT32_MAX || owner_shard != bound_owner_shard_)
    throw std::runtime_error("owner allocator control is not locally bound");
  const RegionOffset offset = header_->owner_controls_offset +
      static_cast<uint64_t>(owner_shard) * header_->owner_control_stride;
  auto *control = static_cast<OwnerAllocatorControl *>(SwccFromOffset(offset));
  mem_access::PrivateRead(control, sizeof(*control));
  if (control->magic != 0x5449474f4e4f574eULL || control->version != 1 ||
      control->owner_shard != owner_shard)
    throw std::runtime_error("owner allocator control validation failed");
  return control;
}

void *DualRegionAllocator::AllocateOwnerPrivate(uint64_t bytes, uint32_t partition_id,
                                                uint32_t owner_shard) {
  if (bytes == 0) throw std::invalid_argument("zero-sized private allocation");
  auto *arena = Arena(partition_id);
  mem_access::PrivateRead(&arena->owner_shard, sizeof(arena->owner_shard));
  if (arena->owner_shard != owner_shard)
    throw std::runtime_error("private arena allocation from non-owner shard");
  uint32_t expected = 0;
  while (!mem_access::PrivateAtomicCompareExchangeWeak(
      arena->lock, expected, uint32_t{1}, std::memory_order_acquire,
      std::memory_order_relaxed)) {
    expected = 0;
    _mm_pause();
  }
  const uint64_t aligned = (bytes + RegionAllocator::kAlignment - 1) &
                           ~(RegionAllocator::kAlignment - 1);
  const uint64_t requested = aligned + RegionAllocator::kAlignment;
  if (requested > UINT32_MAX) {
    mem_access::PrivateAtomicStore(arena->lock, uint32_t{0},
                                   std::memory_order_release);
    throw std::bad_alloc();
  }
  RegionOffset previous = kNullOffset;
  RegionOffset current = mem_access::PrivateAtomicLoad(
      arena->free_head, std::memory_order_relaxed);
  while (current != kNullOffset) {
    auto *block = static_cast<RegionFreeBlock *>(SwccFromOffset(current));
    mem_access::PrivateRead(&block->size_class, sizeof(block->size_class));
    const uint32_t block_bytes = block->size_class;
    if (block_bytes >= requested) {
      const RegionOffset next = mem_access::PrivateAtomicLoad(
          block->next, std::memory_order_relaxed);
      if (previous == kNullOffset) {
        mem_access::PrivateAtomicStore(arena->free_head, next,
                                       std::memory_order_relaxed);
      } else {
        auto *previous_block =
            static_cast<RegionFreeBlock *>(SwccFromOffset(previous));
        mem_access::PrivateAtomicStore(previous_block->next, next,
                                       std::memory_order_relaxed);
      }
      mem_access::PrivateWrite(&block->owner_shard,
                               sizeof(block->owner_shard));
      block->owner_shard = owner_shard;
      mem_access::PrivateAtomicStore(arena->lock, uint32_t{0},
                                     std::memory_order_release);
      mem_access::PrivateAtomicFetchAdd(
          arena->allocated_bytes, static_cast<uint64_t>(block_bytes),
          std::memory_order_relaxed);
      return static_cast<std::byte *>(static_cast<void *>(block)) + RegionAllocator::kAlignment;
    }
    previous = current;
    current = mem_access::PrivateAtomicLoad(block->next,
                                            std::memory_order_relaxed);
  }
  mem_access::PrivateRead(&arena->bump, sizeof(arena->bump));
  mem_access::PrivateRead(&arena->end, sizeof(arena->end));
  const uint64_t begin = (arena->bump + RegionAllocator::kAlignment - 1) &
                         ~(RegionAllocator::kAlignment - 1);
  if (begin > arena->end || requested > arena->end - begin) {
    mem_access::PrivateAtomicStore(arena->lock, uint32_t{0},
                                   std::memory_order_release);
    throw std::bad_alloc();
  }
  mem_access::PrivateWrite(&arena->bump, sizeof(arena->bump));
  arena->bump = begin + requested;
  void *block_address = SwccFromOffset(begin);
  auto *fresh_block = new (block_address) RegionFreeBlock;
  mem_access::PrivateAtomicStore(fresh_block->next, kNullOffset,
                                 std::memory_order_relaxed);
  mem_access::PrivateWrite(&fresh_block->size_class,
                           sizeof(fresh_block->size_class) +
                               sizeof(fresh_block->owner_shard));
  auto *block = fresh_block;
  block->size_class = static_cast<uint32_t>(requested);
  block->owner_shard = owner_shard;
  mem_access::PrivateAtomicFetchAdd(arena->allocated_bytes, requested,
                                    std::memory_order_relaxed);
  mem_access::PrivateAtomicStore(arena->lock, uint32_t{0},
                                 std::memory_order_release);
  return static_cast<std::byte *>(SwccFromOffset(begin)) + RegionAllocator::kAlignment;
}

void DualRegionAllocator::FreeOwnerPrivate(void *pointer, uint64_t bytes,
                                           uint32_t partition_id,
                                           uint32_t owner_shard) {
  if (pointer == nullptr || bytes == 0) throw std::invalid_argument("invalid private free");
  auto *arena = Arena(partition_id);
  mem_access::PrivateRead(&arena->owner_shard, sizeof(arena->owner_shard));
  if (arena->owner_shard != owner_shard || !IsInOwnerPrivateArena(pointer, partition_id))
    throw std::runtime_error("private arena free from non-owner or wrong arena");
  auto *block = reinterpret_cast<RegionFreeBlock *>(
      static_cast<std::byte *>(pointer) - RegionAllocator::kAlignment);
  const uint64_t requested = ((bytes + RegionAllocator::kAlignment - 1) &
                              ~(RegionAllocator::kAlignment - 1)) + RegionAllocator::kAlignment;
  mem_access::PrivateRead(&block->size_class,
                          sizeof(block->size_class) +
                              sizeof(block->owner_shard));
  const uint32_t block_bytes = block->size_class;
  if (block->owner_shard != owner_shard || block_bytes < requested)
    throw std::runtime_error("private arena free size or owner mismatch");
  uint32_t expected = 0;
  while (!mem_access::PrivateAtomicCompareExchangeWeak(
      arena->lock, expected, uint32_t{1}, std::memory_order_acquire,
      std::memory_order_relaxed)) {
    expected = 0;
  }
  const RegionOffset head = mem_access::PrivateAtomicLoad(
      arena->free_head, std::memory_order_relaxed);
  mem_access::PrivateAtomicStore(block->next, head,
                                 std::memory_order_relaxed);
  mem_access::PrivateAtomicStore(arena->free_head, SwccToOffset(block),
                                 std::memory_order_release);
  mem_access::PrivateAtomicStore(arena->lock, uint32_t{0},
                                 std::memory_order_release);
  CheckedAtomicSubtract(&arena->allocated_bytes, block_bytes,
                        "owner-private allocator accounting underflow",
                        latency_sim::MemoryDomain::kOwnerPrivateSwcc);
}

void DualRegionAllocator::Free(void *pointer, uint64_t bytes, AllocationDomain domain,
                               uint32_t owner_shard, uint32_t current_shard) {
  if (owner_shard != current_shard)
    throw std::runtime_error("cross-owner free is forbidden");
  if (domain == AllocationDomain::kHwccIndex ||
      domain == AllocationDomain::kHwccMetadata) {
    auto *allocator = dynamic_hwcc_[owner_shard].get();
    if (allocator == nullptr || !allocator->Contains(pointer))
      throw std::runtime_error("dynamic HWCC free outside owner arena");
    auto *control = OwnerControl(owner_shard);
    allocator->Free(pointer, bytes, domain, &control->dynamic_hwcc, 0, 0);
    CheckedAtomicSubtract(&control->total_hw_cc_usage, bytes,
                          "owner Clock policy counter underflow",
                          latency_sim::MemoryDomain::kOwnerPrivateSwcc);
    return;
  }
  if (domain == AllocationDomain::kSharedPayloadSwcc) {
    auto *allocator = dynamic_shared_swcc_[owner_shard].get();
    if (allocator == nullptr || !allocator->Contains(pointer))
      throw std::runtime_error("shared SWCC free outside owner arena");
    auto *control = OwnerControl(owner_shard);
    allocator->Free(pointer, bytes, domain, &control->shared_payload, 0, 0);
    return;
  }
  if (!hwcc_.Contains(pointer)) throw std::runtime_error("static HWCC free outside HWCC region");
  hwcc_.Free(pointer, bytes, domain,
             &header_->layout.domains[static_cast<size_t>(domain)], 0, 0);
}

void DualRegionAllocator::Retire(uint32_t owner_shard, uint32_t queue_partition,
                                 uint32_t worker_id, uint32_t epoch,
                                 void *pointer, uint64_t bytes,
                                 AllocationDomain domain,
                                 uint32_t private_partition) {
  if (pointer == nullptr || bytes == 0 || worker_id >= kOwnerPrivateEbrWorkers ||
      epoch >= kOwnerPrivateEbrEpochs)
    throw std::invalid_argument("invalid owner-private EBR retire record");
  auto *control = OwnerControl(owner_shard);
  auto *arena = Arena(queue_partition);
  mem_access::PrivateRead(&arena->owner_shard, sizeof(arena->owner_shard));
  if (arena->owner_shard != owner_shard)
    throw std::runtime_error("EBR retire queue belongs to another owner");
  auto *record = static_cast<OwnerPrivateRetireRecord *>(
      AllocateOwnerPrivate(sizeof(OwnerPrivateRetireRecord), queue_partition,
                           owner_shard));
  new (record) OwnerPrivateRetireRecord;
  mem_access::PrivateWrite(record, sizeof(OwnerPrivateRetireRecord));
  record->bytes = bytes;
  record->domain = domain;
  record->private_partition = private_partition;
  record->record_partition = queue_partition;
  record->object_offset =
      private_partition != UINT32_MAX
          ? SwccToOffset(pointer)
          : (domain == AllocationDomain::kSharedPayloadSwcc
                 ? EncodeSharedPayloadOffset(pointer, owner_shard)
                 : (IsHwccDomain(domain) ? hwcc_.ToOffset(pointer)
                                          : SwccToOffset(pointer)));
  auto &head = control->retire_heads[worker_id][epoch];
  RegionOffset previous = mem_access::PrivateAtomicLoad(
      head, std::memory_order_relaxed);
  do {
    record->next = previous;
    mem_access::PrivateWrite(&record->next, sizeof(record->next));
  } while (!mem_access::PrivateAtomicCompareExchangeWeak(
      head, previous, SwccToOffset(record), std::memory_order_release,
      std::memory_order_relaxed));
  mem_access::PrivateAtomicFetchAdd(control->retire_counts[worker_id][epoch],
                                    uint64_t{1}, std::memory_order_relaxed);
}

uint64_t DualRegionAllocator::RetireCount(uint32_t owner_shard,
                                          uint32_t worker_id,
                                          uint32_t epoch) const {
  if (worker_id >= kOwnerPrivateEbrWorkers || epoch >= kOwnerPrivateEbrEpochs)
    throw std::invalid_argument("EBR retire count outside worker/epoch range");
  auto *control = OwnerControl(owner_shard);
  return mem_access::PrivateAtomicLoad(
      control->retire_counts[worker_id][epoch], std::memory_order_acquire);
}

std::vector<OwnerPrivateRetireRecord> DualRegionAllocator::TakeRetired(
    uint32_t owner_shard, uint32_t worker_id, uint32_t epoch) {
  if (worker_id >= kOwnerPrivateEbrWorkers || epoch >= kOwnerPrivateEbrEpochs)
    throw std::invalid_argument("EBR reclaim outside worker/epoch range");
  std::vector<OwnerPrivateRetireRecord> retired;
  auto *control = OwnerControl(owner_shard);
  RegionOffset offset = mem_access::PrivateAtomicExchange(
      control->retire_heads[worker_id][epoch], kNullOffset,
      std::memory_order_acq_rel);
  while (offset != kNullOffset) {
    auto *record = static_cast<OwnerPrivateRetireRecord *>(SwccFromOffset(offset));
    mem_access::PrivateRead(record, sizeof(OwnerPrivateRetireRecord));
    const OwnerPrivateRetireRecord copy = *record;
    const RegionOffset next = record->next;
    retired.push_back(copy);
    if (copy.record_partition >= header_->layout.partition_count)
      throw std::runtime_error("owner-private EBR record has invalid queue partition");
    FreeOwnerPrivate(record, sizeof(OwnerPrivateRetireRecord),
                     copy.record_partition, owner_shard);
    offset = next;
  }
  const uint64_t removed = retired.size();
  if (removed != 0) {
    CheckedAtomicSubtract(&control->retire_counts[worker_id][epoch], removed,
                          "owner-private EBR retire accounting underflow",
                          latency_sim::MemoryDomain::kOwnerPrivateSwcc);
  }
  return retired;
}

bool DualRegionAllocator::IsHwccAddress(const void *pointer) const {
  return hwcc_.Contains(pointer);
}

bool DualRegionAllocator::IsSwccAddress(const void *pointer) const {
  const auto *p = static_cast<const std::byte *>(pointer);
  return p != nullptr && p >= pool_ + config_.swcc_offset_bytes &&
         p < pool_ + config_.swcc_offset_bytes + config_.swcc_size_bytes;
}

RegionOffset DualRegionAllocator::ToOwnerPrivateOffset(
    const void *pointer, uint32_t partition_id) const {
  if (!IsInOwnerPrivateArena(pointer, partition_id))
    throw std::runtime_error("owner-private offset outside partition arena");
  return SwccToOffset(pointer);
}

uint64_t DualRegionAllocator::EncodeSharedPayloadOffset(
    const void *pointer, uint32_t owner_shard) const {
  if (owner_shard >= header_->layout.vm_count)
    throw std::invalid_argument("shared payload owner outside layout");
  const auto &descriptor = header_->layout.owner_dynamic_arenas[owner_shard];
  const RegionOffset offset = SwccToOffset(pointer);
  if (!RangeContains(descriptor.shared_swcc_offset,
                     descriptor.shared_swcc_bytes, offset, 1))
    throw std::runtime_error("shared payload pointer outside owner arena");
  return config_.swcc_offset_bytes + offset;
}

RegionOffset DualRegionAllocator::ToTransportOffset(const void *pointer) const {
  const RegionOffset offset = hwcc_.ToOffset(pointer);
  if (!RangeContains(header_->transport_offset, header_->transport_bytes,
                     offset, 1))
    throw std::runtime_error("transport pointer outside static domain");
  return offset;
}

RegionOffset DualRegionAllocator::ToDynamicHwccOffset(
    const void *pointer, uint32_t owner_shard) const {
  const RegionOffset offset = hwcc_.ToOffset(pointer);
  if (owner_shard >= header_->layout.vm_count)
    throw std::invalid_argument("dynamic HWCC owner outside layout");
  const auto &descriptor = header_->layout.owner_dynamic_arenas[owner_shard];
  if (!RangeContains(descriptor.hwcc_offset, descriptor.hwcc_bytes, offset, 1))
    throw std::runtime_error("dynamic HWCC pointer outside owner arena");
  return offset;
}

bool DualRegionAllocator::IsInOwnerPrivateArena(const void *pointer,
                                                uint32_t partition_id) const {
  const auto *arena = Arena(partition_id);
  const auto *p = static_cast<const std::byte *>(pointer);
  mem_access::PrivateRead(&arena->begin,
                          sizeof(arena->begin) + sizeof(arena->end));
  return p >= pool_ + config_.swcc_offset_bytes + arena->begin &&
         p < pool_ + config_.swcc_offset_bytes + arena->end;
}

void *DualRegionAllocator::SwccFromOffset(RegionOffset offset,
                                          uint64_t bytes) const {
  if (offset == kNullOffset || offset > config_.swcc_size_bytes ||
      bytes == 0 || bytes > config_.swcc_size_bytes - offset)
    throw std::runtime_error("SWCC offset outside mapped region");
  return pool_ + config_.swcc_offset_bytes + offset;
}

RegionOffset DualRegionAllocator::SwccToOffset(const void *pointer,
                                               uint64_t bytes) const {
  const auto *p = static_cast<const std::byte *>(pointer);
  if (p == nullptr || bytes == 0 ||
      p < pool_ + config_.swcc_offset_bytes ||
      p > pool_ + config_.swcc_offset_bytes + config_.swcc_size_bytes - bytes)
    throw std::invalid_argument("SWCC pointer outside mapped region");
  return static_cast<RegionOffset>(p - (pool_ + config_.swcc_offset_bytes));
}

namespace {
bool RangeContains(uint64_t begin, uint64_t length, uint64_t offset,
                   uint64_t bytes) {
  return bytes != 0 && begin <= offset && offset - begin <= length &&
         bytes <= length - (offset - begin) && (offset % RegionAllocator::kAlignment) == 0;
}
}

void *DualRegionAllocator::ResolveOwnerPrivate(RegionOffset offset,
                                               uint64_t bytes,
                                               uint32_t partition_id,
                                               uint32_t expected_owner) const {
  if (partition_id >= header_->layout.partition_count ||
      partition_id % header_->layout.vm_count != expected_owner ||
      expected_owner != bound_owner_shard_)
    throw std::runtime_error("owner-private resolver owner/partition mismatch");
  const uint64_t begin = header_->owner_private_arenas_offset +
      static_cast<uint64_t>(partition_id) * header_->owner_private_arena_stride;
  if (!RangeContains(begin +
                         ((sizeof(OwnerPrivateArenaHeader) + RegionAllocator::kAlignment - 1) /
                          RegionAllocator::kAlignment * RegionAllocator::kAlignment),
                     header_->owner_private_arena_stride -
                         ((sizeof(OwnerPrivateArenaHeader) + RegionAllocator::kAlignment - 1) /
                          RegionAllocator::kAlignment * RegionAllocator::kAlignment),
                     offset, bytes))
    throw std::runtime_error("owner-private resolver rejected offset");
  return SwccFromOffset(offset, bytes);
}

void *DualRegionAllocator::ResolveDynamicHwcc(RegionOffset offset,
                                              uint64_t bytes,
                                              uint32_t expected_owner) const {
  if (expected_owner >= header_->layout.vm_count)
    throw std::runtime_error("dynamic HWCC resolver owner outside layout");
  const auto &descriptor = header_->layout.owner_dynamic_arenas[expected_owner];
  if (!RangeContains(descriptor.hwcc_offset, descriptor.hwcc_bytes, offset, bytes)) {
    std::ostringstream detail;
    detail << "dynamic HWCC resolver rejected offset=" << offset
           << " bytes=" << bytes << " begin=" << descriptor.hwcc_offset
           << " length=" << descriptor.hwcc_bytes << " owner=" << expected_owner;
    throw std::runtime_error(detail.str());
  }
  return hwcc_.FromOffset(offset);
}

void *DualRegionAllocator::ResolveSharedPayload(uint64_t whole_pool_offset,
                                                uint64_t bytes) const {
  const auto contains_owner = [&](uint32_t owner) {
    const auto &descriptor = header_->layout.owner_dynamic_arenas[owner];
    return RangeContains(config_.swcc_offset_bytes + descriptor.shared_swcc_offset,
                         descriptor.shared_swcc_bytes, whole_pool_offset, bytes);
  };
  uint32_t matches = 0;
  for (uint32_t owner = 0; owner < header_->layout.vm_count; ++owner) {
    if (contains_owner(owner)) ++matches;
  }
  // The original SCC atomic word carries only this whole-pool offset; it has
  // no allocation-owner field.  Validate that it denotes exactly one
  // immutable shared-payload arena rather than using the current VM owner as
  // a routing hint.
  if (matches != 1) {
    std::ostringstream detail;
    detail << "shared payload resolver rejected whole-pool offset="
           << whole_pool_offset << " bytes=" << bytes
           << " matching_shared_arenas=" << matches;
    throw std::runtime_error(detail.str());
  }
  return SwccFromOffset(whole_pool_offset - config_.swcc_offset_bytes, bytes);
}

void *DualRegionAllocator::ResolveTransport(RegionOffset offset,
                                            uint64_t bytes) const {
  if (!RangeContains(header_->transport_offset, header_->transport_bytes,
                     offset, bytes))
    throw std::runtime_error("transport resolver rejected offset");
  return hwcc_.FromOffset(offset);
}

void *DualRegionAllocator::ResolveEbr(RegionOffset offset, uint64_t bytes) const {
  if (!RangeContains(header_->ebr_offset, header_->ebr_bytes, offset, bytes))
    throw std::runtime_error("EBR resolver rejected offset");
  return hwcc_.FromOffset(offset);
}

RegionOffset DualRegionAllocator::OwnerPrivateArenaOffset(uint32_t partition_id) const {
  // This address calculation is safe for every VM because its inputs are
  // layout metadata in HWCC.  Do not call Arena() here: that validates the
  // SWCC header and would make a non-owner attach read another owner's
  // non-coherent private arena merely to construct a shared-tree handle.
  mem_access::HwccRead(&header_->layout.partition_count,
                       sizeof(header_->layout.partition_count));
  if (partition_id >= header_->layout.partition_count)
    throw std::invalid_argument("owner-private arena partition outside layout");
  mem_access::HwccRead(&header_->owner_private_arenas_offset,
                       sizeof(header_->owner_private_arenas_offset));
  mem_access::HwccRead(&header_->owner_private_arena_stride,
                       sizeof(header_->owner_private_arena_stride));
  return header_->owner_private_arenas_offset +
         partition_id * header_->owner_private_arena_stride;
}

uint64_t DualRegionAllocator::SharedPayloadCapacityBytes(
    uint32_t owner_shard) const {
  if (owner_shard >= header_->layout.vm_count)
    throw std::invalid_argument("shared payload owner outside layout");
  mem_access::HwccRead(&header_->layout.owner_dynamic_arenas[owner_shard],
                       sizeof(OwnerDynamicArenaDescriptor));
  return header_->layout.owner_dynamic_arenas[owner_shard].shared_swcc_bytes;
}

uint64_t DualRegionAllocator::OwnerPrivateUsedBytes(uint32_t owner_shard) const {
  if (owner_shard >= header_->layout.vm_count)
    throw std::invalid_argument("private usage owner outside layout");
  uint64_t used = 0;
  for (uint32_t partition = 0; partition < header_->layout.partition_count;
       ++partition) {
    if (partition % header_->layout.vm_count != owner_shard) continue;
    auto *arena = Arena(partition);
    used += mem_access::PrivateAtomicLoad(arena->allocated_bytes,
                                          std::memory_order_relaxed);
  }
  return used;
}

uint64_t DualRegionAllocator::DynamicHwccUsedBytes(uint32_t owner_shard) const {
  auto *control = OwnerControl(owner_shard);
  return mem_access::PrivateAtomicLoad(control->dynamic_hwcc.used_bytes,
                                      std::memory_order_relaxed);
}

uint64_t DualRegionAllocator::PolicyHwccUsedBytes(uint32_t owner_shard) const {
  auto *control = OwnerControl(owner_shard);
  return mem_access::PrivateAtomicLoad(control->total_hw_cc_usage,
                                      std::memory_order_relaxed);
}

uint64_t DualRegionAllocator::SharedPayloadUsedBytes(uint32_t owner_shard) const {
  auto *control = OwnerControl(owner_shard);
  return mem_access::PrivateAtomicLoad(control->shared_payload.used_bytes,
                                      std::memory_order_relaxed);
}

uint64_t DualRegionAllocator::ReadStaticDomainUsedBytes(
    AllocationDomain domain) const {
  switch (domain) {
    case AllocationDomain::kHwccLayout:
    case AllocationDomain::kHwccAllocatorMetadata:
    case AllocationDomain::kTransport:
    case AllocationDomain::kHwccEbr:
    case AllocationDomain::kSwccAllocatorMetadata:
      break;
    default:
      throw std::invalid_argument(
          "ReadStaticDomainUsedBytes rejects dynamic allocation domains");
  }
  auto *counter = &header_->layout.domains[static_cast<size_t>(domain)].used_bytes;
  return mem_access::HwccAtomicLoad(*counter, std::memory_order_relaxed);
}

void DualRegionAllocator::FlushOwnedRanges(uint32_t node_id) {
  mem_access::HwccRead(&header_->layout.vm_count,
                       sizeof(header_->layout.vm_count) +
                           sizeof(header_->layout.partition_count));
  if (node_id >= header_->layout.vm_count) {
    throw std::invalid_argument("flush owner outside layout");
  }
  for (uint32_t partition = 0; partition < header_->layout.partition_count; ++partition) {
    if (partition % header_->layout.vm_count != node_id) continue;
    auto *arena = Arena(partition);
    const RegionOffset arena_offset = SwccToOffset(arena);
    mem_access::PrivateRead(&arena->bump, sizeof(arena->bump));
    if (arena->bump > arena_offset)
      FlushForRemoteVisibility(arena, arena->bump - arena_offset);
  }
}

DualRegionMappedPool DualRegionMappedPool::Open(const std::string &path,
                                                const DualRegionConfig &config,
                                                bool reset) {
  if (path.empty() || config.total_pool_bytes == 0)
    throw std::invalid_argument("invalid dual-region backing-file request");
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0660);
  if (fd < 0) throw std::runtime_error("open dual-region backing file failed");
  try {
    struct stat st {};
    if (fstat(fd, &st) != 0) throw std::runtime_error("stat dual-region backing file failed");
    const bool is_character_device = S_ISCHR(st.st_mode);
    if (!is_character_device) {
      if (flock(fd, LOCK_EX) != 0) throw std::runtime_error("lock dual-region backing file failed");
      if (reset) {
        if (ftruncate(fd, static_cast<off_t>(config.total_pool_bytes)) != 0)
          throw std::runtime_error("resize dual-region backing file failed");
      } else if (st.st_size != static_cast<off_t>(config.total_pool_bytes)) {
        throw std::runtime_error("dual-region backing file size mismatch");
      }
    }
    void *base = mmap(nullptr, config.total_pool_bytes, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) throw std::runtime_error("map dual-region backing file failed");
    // Pool init writes/reads the mapped HWCC/SWCC regions through the wrapped
    // accessors.  Under the always-active model those need the two ranges
    // registered and an executing scope, so register them (0/0 zero-nanosecond
    // model) and run the init inside a scope.  The engine startup re-applies
    // the real three-field policy after clearing the registrations.  No-op in
    // a compile-off build.
    std::unique_ptr<DualRegionAllocator> allocator;
#if !defined(LATENCY_SIM_COMPILE_OFF)
    {
      auto &simulator = latency_sim::GlobalLatencySimulator();
      simulator.ClearPoolRegistrations();
      simulator.RegisterPool(
          latency_sim::MemoryDomain::kHwcc,
          static_cast<const std::byte *>(base) + config.hwcc_offset_bytes,
          config.hwcc_size_bytes);
      simulator.RegisterPool(
          latency_sim::MemoryDomain::kSwcc,
          static_cast<const std::byte *>(base) + config.swcc_offset_bytes,
          config.swcc_size_bytes);
      latency_sim::FixedLatencyConfig zero;
      zero.cache_line_bytes = 64;
      zero.swcc_fixed_ns_per_line = 0.0;
      zero.hwcc_fixed_ns_per_line = 0.0;
      simulator.Configure(zero);
      latency_sim::ScopeGuard pool_init_scope(
          latency_sim::ExecutionClass::kBackground);
#endif
    try {
      if (reset) {
        // Guest workflows set TIGONKV_DEVICE_BACKING_ZEROED=1 after the host
        // cxl_pool_initer already zeroed the ivshmem backing.  Skipping the
        // guest-side 32GiB memset avoids multi-minute stalls before stage=opened.
        const char *prezeroed = std::getenv("TIGONKV_DEVICE_BACKING_ZEROED");
        if (prezeroed == nullptr) prezeroed = std::getenv("CXLKV_DEVICE_BACKING_ZEROED");
        const bool skip_memset =
            prezeroed != nullptr && prezeroed[0] == '1' && prezeroed[1] == '\0';
        if (!skip_memset) std::memset(base, 0, config.total_pool_bytes);
        allocator = std::make_unique<DualRegionAllocator>(
            DualRegionAllocator::Initialize(base, config));
      } else {
        allocator = std::make_unique<DualRegionAllocator>(
            DualRegionAllocator::Attach(base, config));
      }
    } catch (...) {
      munmap(base, config.total_pool_bytes);
      throw;
    }
#if !defined(LATENCY_SIM_COMPILE_OFF)
    }  // pool-init scope and 0/0 registration end
#endif
    if (!is_character_device) flock(fd, LOCK_UN);
    return DualRegionMappedPool(fd, base, config.total_pool_bytes, std::move(allocator));
  } catch (...) {
    flock(fd, LOCK_UN);
    ::close(fd);
    throw;
  }
}

DualRegionMappedPool::~DualRegionMappedPool() { Close(); }

DualRegionMappedPool::DualRegionMappedPool(DualRegionMappedPool &&other) noexcept
    : fd_(other.fd_), base_(other.base_), bytes_(other.bytes_),
      allocator_(std::move(other.allocator_)) {
  other.fd_ = -1;
  other.base_ = nullptr;
  other.bytes_ = 0;
}

DualRegionMappedPool &DualRegionMappedPool::operator=(DualRegionMappedPool &&other) noexcept {
  if (this == &other) return *this;
  Close();
  fd_ = other.fd_;
  base_ = other.base_;
  bytes_ = other.bytes_;
  allocator_ = std::move(other.allocator_);
  other.fd_ = -1;
  other.base_ = nullptr;
  other.bytes_ = 0;
  return *this;
}

void DualRegionMappedPool::Close() noexcept {
  allocator_.reset();
  if (base_ != nullptr) munmap(base_, bytes_);
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  base_ = nullptr;
  bytes_ = 0;
}

}  // namespace tigonkv::engine
