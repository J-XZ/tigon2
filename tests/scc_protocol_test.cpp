#include "tests/scc_test_backend.h"

#include <cassert>
#include <cstring>

int main() {
  using tigonkv::test::NonCoherentSwccTestBackend;
  NonCoherentSwccTestBackend backend(128, 3);
  char out[4] = {};
  const char one[] = "one", two[] = "two";

  backend.Read(1, 0, out, 3);                         assert(std::memcmp(out, "\0\0\0", 3) == 0); // 1
  backend.Write(0, 0, one, 3);                         // 2
  backend.Read(1, 0, out, 3);                         assert(std::memcmp(out, one, 3) != 0);          // 3
  assert(!backend.Publish(0, 0, 3));                   // 4: no write-back
  backend.WriteBack(0, 0, 3);                          assert(backend.flushes() == 1);                  // 5
  assert(backend.Publish(0, 0, 3));                    assert(backend.IsPublished(0));                  // 6
  backend.Read(1, 0, out, 3);                         assert(std::memcmp(out, one, 3) != 0);          // 7: no invalidate
  backend.Invalidate(1, 0, 3);                         backend.Read(1, 0, out, 3);                      // 8
  assert(std::memcmp(out, one, 3) == 0);               assert(backend.invalidates() == 1);              // 9
  backend.Invalidate(1, 0, 3);                         assert(backend.invalidates() == 2);              // 10
  backend.Write(0, 0, two, 3);                         assert(!backend.Publish(0, 0, 3));               // 11
  backend.WriteBack(0, 0, 3);                          backend.Read(2, 0, out, 3);                      // 12
  assert(std::memcmp(out, two, 3) != 0);               backend.Invalidate(2, 0, 3);                     // 13
  backend.Read(2, 0, out, 3);                          assert(std::memcmp(out, two, 3) == 0);           // 14
  return 0;
}
