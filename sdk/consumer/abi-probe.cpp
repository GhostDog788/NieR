// Verifies the extracted C++ and libarchive runtime closure before LLVM builds.
#include <archive.h>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>
#ifndef SELA_EXPECT_POINTER_BYTES
#error Define the expected native pointer size
#endif
int main() {
  static_assert(sizeof(void *) == SELA_EXPECT_POINTER_BYTES);
  auto value = std::make_unique<std::vector<std::uint64_t>>(3, 7);
  if (value->at(2) != 7 || archive_version_number() < 3007000) return 1;
  std::cout << "Native ABI " << sizeof(void *) * 8 << " / C++ / libarchive: OK\n";
}
