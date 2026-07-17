#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 3) { std::cerr << "usage: cxl_pool_initer PATH SIZE_MB\n"; return 2; }
  const uint64_t bytes = std::stoull(argv[2]) * 1024ULL * 1024ULL;
  int fd = open(argv[1], O_RDWR | O_CREAT, 0660);
  if (fd < 0) { std::cerr << "open: " << std::strerror(errno) << "\n"; return 2; }
  if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) { std::cerr << "ftruncate: " << std::strerror(errno) << "\n"; return 2; }
  void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) { std::cerr << "mmap failed\n"; return 2; }
  std::memset(p, 0, bytes);
  if (msync(p, bytes, MS_SYNC) != 0) { std::cerr << "msync failed\n"; return 2; }
  munmap(p, bytes); close(fd);
  std::cout << "initialized backing bytes=" << bytes << " (ordinary host DRAM)\n";
  return 0;
}
