// Parses a code map with FEX's own CodeMap::ParseCodeMap and prints one line per file,
// in the same format as `rlaot.py dump`, so the two can be diffed.
// Also prints XXH3_64 of argv[2..] (FEX's vendored xxhash) to cross-check rlaot's file ids.
#include <FEXCore/Core/CodeCache.h>
#include <xxhash.h>
#include <cstdio>
#include <cstring>
#include <fstream>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s FILE.codemap [string...]\n", argv[0]);
    return 2;
  }
  std::ifstream File(argv[1], std::ios_base::binary);
  auto Parsed = FEXCore::CodeMap::ParseCodeMap(File);
  for (auto& [Id, C] : Parsed) {
    char Bits[3] = " -";
    if (C.ExecutableBitness) {
      std::snprintf(Bits, sizeof(Bits), "%d", *C.ExecutableBitness);
    }
    std::printf("%016llx %2s %7zu blocks  %s\n", (unsigned long long)Id, Bits, C.Blocks.size(), C.Filename.c_str());
  }
  for (int i = 2; i < argc; i++) {
    std::printf("xxh3 %s %llu\n", argv[i], (unsigned long long)XXH3_64bits(argv[i], std::strlen(argv[i])));
  }
  return 0;
}
