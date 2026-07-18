#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>

int main(int argc, char **argv) {
  if (argc != 2 && argc != 3) {
    std::cerr << "usage: numa_placement_probe BACKING_FILE [EXPECTED_NUMA_NODE]\n";
    return 2;
  }
  int fd = open(argv[1], O_RDONLY);
  if (fd < 0) { std::perror("open"); return 2; }
  struct stat st{};
  if (fstat(fd, &st) != 0 || st.st_size == 0) { std::cerr << "invalid backing file\n"; return 2; }
  void *mapping = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) { std::perror("mmap"); return 2; }
  const size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  const size_t pages = (static_cast<size_t>(st.st_size) + page_size - 1) / page_size;
  const int expected_node = argc == 3 ? std::stoi(argv[2]) : -1;
  const char *mode = expected_node >= 0 ? "performance" : "functional";
  std::vector<void *> addresses(pages);
  std::vector<int> nodes(pages, -1);
  // move_pages reports -1 for non-resident pages without faulting them in.
  // Read one byte through each page first; this is read-only and does not
  // change NUMA policy, but makes the placement query cover the backing that
  // the experiment actually maps.
  volatile unsigned char sample = 0;
  for (size_t i = 0; i < pages; ++i) {
    addresses[i] = static_cast<char *>(mapping) + i * page_size;
    sample ^= *static_cast<const unsigned char *>(addresses[i]);
  }
  (void)sample;
  const long query = syscall(SYS_move_pages, 0, pages, addresses.data(), nullptr,
                             nodes.data(), 0);
  const int query_errno = query == 0 ? 0 : errno;
  int exit_code = 0;
  std::cout << "TIGONKV_NUMA_STATS\n";
  if (query == 0) {
    size_t resident = 0, misplaced = 0;
    int dominant_node = -1;
    std::vector<size_t> counts(256, 0);
    for (int node : nodes) {
      if (node < 0) continue;
      ++resident;
      if (static_cast<size_t>(node) >= counts.size()) counts.resize(static_cast<size_t>(node) + 1);
      ++counts[static_cast<size_t>(node)];
      if (expected_node >= 0 && node != expected_node) ++misplaced;
    }
    for (size_t node = 0; node < counts.size(); ++node)
      if (dominant_node < 0 || counts[node] > counts[static_cast<size_t>(dominant_node)])
        dominant_node = static_cast<int>(node);
    std::cout << "node=host vm_numa=unknown shared_numa=" << dominant_node
              << " misplaced_pages=" << (expected_node >= 0 ? std::to_string(misplaced) : "unknown")
              << " mode=" << mode << " resident_pages=" << resident << "\n";
    for (size_t node = 0; node < counts.size(); ++node)
      if (counts[node] != 0) std::cout << "shared_numa_node_" << node << "_pages=" << counts[node] << "\n";
    if (expected_node >= 0 && (resident == 0 || misplaced != 0)) exit_code = 3;
  } else {
    std::cout << "node=host vm_numa=unknown shared_numa=unknown misplaced_pages=unknown mode=" << mode
              << " query_errno=" << query_errno << " query_error=" << std::strerror(query_errno) << "\n";
    if (expected_node >= 0) exit_code = 3;
  }
  std::cout << "sampled_pages=" << pages << " bytes=" << st.st_size << "\n";
  std::cout << "note=read-only move_pages query; this probe does not move pages or change policy\n";
  munmap(mapping, static_cast<size_t>(st.st_size)); close(fd);
  return exit_code;
}
