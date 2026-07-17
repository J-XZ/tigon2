#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: numa_placement_probe BACKING_FILE\n";
    return 2;
  }
  int fd = open(argv[1], O_RDONLY);
  if (fd < 0) { std::perror("open"); return 2; }
  struct stat st{};
  if (fstat(fd, &st) != 0 || st.st_size == 0) { std::cerr << "invalid backing file\n"; return 2; }
  void *mapping = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) { std::perror("mmap"); return 2; }
  const size_t pages = static_cast<size_t>(st.st_size) / 4096;
  std::cout << "TIGONKV_NUMA_STATS\n";
  std::cout << "node=host vm_numa=unknown shared_numa=unknown misplaced_pages=unknown mode=functional\n";
  std::cout << "sampled_pages=" << pages << " bytes=" << st.st_size << "\n";
  std::cout << "note=page placement is queried by the host orchestration; this probe does not move pages\n";
  munmap(mapping, static_cast<size_t>(st.st_size)); close(fd);
  return 0;
}
