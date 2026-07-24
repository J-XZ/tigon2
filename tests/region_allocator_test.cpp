#include "kv/engine/region_allocator.h"

#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace tigonkv::engine;

constexpr size_t kBytes = 4 * 1024 * 1024;

struct Mapping {
  explicit Mapping(bool shared) {
    char path[] = "/tmp/tigonkv-region-XXXXXX";
    fd = mkstemp(path);
    assert(fd >= 0);
    name = path;
    assert(ftruncate(fd, kBytes) == 0);
    base = mmap(nullptr, kBytes, PROT_READ | PROT_WRITE,
                shared ? MAP_SHARED : MAP_PRIVATE, fd, 0);
    assert(base != MAP_FAILED);
    std::memset(base, 0, kBytes);
  }
  ~Mapping() {
    if (base != MAP_FAILED) munmap(base, kBytes);
    if (fd >= 0) close(fd);
    if (!name.empty()) unlink(name.c_str());
  }
  int fd = -1;
  void *base = MAP_FAILED;
  std::string name;
};

void TestAttachReuseAndAccounting() {
  Mapping mapping(true);
  auto allocator = RegionAllocator::Initialize(mapping.base, kBytes, 2);
  DomainCounter index;
  DomainCounter payload;
  void *first = allocator.Allocate(1, AllocationDomain::kHwccIndex, &index, 0);
  void *second = allocator.Allocate(80, AllocationDomain::kSharedPayloadSwcc, &payload, 1);
  assert(reinterpret_cast<uintptr_t>(first) % RegionAllocator::kAlignment == 0);
  assert(reinterpret_cast<uintptr_t>(second) % RegionAllocator::kAlignment == 0);
  assert(index.used_bytes.load() == 64);
  assert(payload.used_bytes.load() == 128);
  const RegionOffset offset = allocator.ToOffset(second);
  auto attached = RegionAllocator::Attach(mapping.base, kBytes);
  assert(attached.FromOffset(offset) == second);
  allocator.Free(first, 1, AllocationDomain::kHwccIndex, &index, 0, 0);
  assert(index.used_bytes.load() == 0);
  void *reused = allocator.Allocate(1, AllocationDomain::kHwccIndex, &index, 0);
  assert(reused == first);
  allocator.Free(reused, 1, AllocationDomain::kHwccIndex, &index, 0, 0);
  allocator.Free(second, 80, AllocationDomain::kSharedPayloadSwcc, &payload, 1, 1);
  assert(index.used_bytes.load() == 0 && payload.used_bytes.load() == 0);
  assert(index.peak_bytes.load() == 64 && payload.peak_bytes.load() == 128);
}

void TestCrossProcessRemoteFree() {
  Mapping mapping(true);
  auto allocator = RegionAllocator::Initialize(mapping.base, kBytes, 2);
  void *counter_map = mmap(nullptr, sizeof(DomainCounter), PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  assert(counter_map != MAP_FAILED);
  auto *counter = new (counter_map) DomainCounter;
  void *block = allocator.Allocate(100, AllocationDomain::kHwccMetadata, counter, 0);
  const RegionOffset offset = allocator.ToOffset(block);
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    void *child_base = mmap(nullptr, kBytes, PROT_READ | PROT_WRITE, MAP_SHARED, mapping.fd, 0);
    if (child_base == MAP_FAILED) _exit(2);
    try {
      auto child_allocator = RegionAllocator::Attach(child_base, kBytes);
      child_allocator.Free(child_allocator.FromOffset(offset), 100,
                           AllocationDomain::kHwccMetadata, counter, 0, 1);
      munmap(child_base, kBytes);
      _exit(0);
    } catch (...) {
      _exit(3);
    }
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  // The owner harvests remote frees before its next allocation.
  void *reused = allocator.Allocate(100, AllocationDomain::kHwccMetadata, counter, 0);
  assert(reused == block);
  allocator.Free(reused, 100, AllocationDomain::kHwccMetadata, counter, 0, 0);
  assert(counter->used_bytes.load() == 0);
  munmap(counter_map, sizeof(DomainCounter));
}

void TestConcurrencyAndBounds() {
  Mapping mapping(true);
  auto allocator = RegionAllocator::Initialize(mapping.base, kBytes, 2);
  DomainCounter counter;
  std::vector<std::thread> threads;
  for (int worker = 0; worker < 8; ++worker) {
    threads.emplace_back([&] {
      for (int i = 0; i < 500; ++i) {
        const uint64_t bytes = 1 + (i % 700);
        void *block = allocator.Allocate(bytes, AllocationDomain::kOwnerPrivateSwcc,
                                         &counter, 1);
        allocator.Free(block, bytes, AllocationDomain::kOwnerPrivateSwcc,
                       &counter, 1, 1);
      }
    });
  }
  for (auto &thread : threads) thread.join();
  assert(counter.used_bytes.load() == 0);
  bool oom = false;
  try {
    for (;;) allocator.Allocate(65536, AllocationDomain::kSharedPayloadSwcc, &counter, 0);
  } catch (const std::bad_alloc &) {
    oom = true;
  }
  assert(oom);
}

void TestInvalidAttachment() {
  Mapping mapping(true);
  auto allocator = RegionAllocator::Initialize(mapping.base, kBytes, 2);
  (void)allocator;
  bool rejected = false;
  try {
    auto bad = RegionAllocator::Attach(mapping.base, kBytes / 2);
    (void)bad;
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  assert(rejected);
}

}  // namespace

int main() {
  TestAttachReuseAndAccounting();
  TestCrossProcessRemoteFree();
  TestConcurrencyAndBounds();
  TestInvalidAttachment();
  return 0;
}
