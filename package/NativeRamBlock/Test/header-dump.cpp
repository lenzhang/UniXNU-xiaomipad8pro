// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host helper for tooling/persist/test-data-store.py: block 0 of a store window as PersistOverlay.h writes it, on stdout (4096 bytes),
// so the tooling's reader (tooling/persist/common.py store_header) is checked against the driver's own encoder.
//   header-dump PAGES SEED_HEX64 GENERATION FLAGS FLATTENED
#include <cstdio>
#include <cstdlib>
#include "../PersistOverlay.h"
int main(int argc, char **argv) {
  if (argc != 6 || strlen(argv[2]) != 64) { fprintf(stderr, "usage: header-dump PAGES SEED_HEX64 GENERATION FLAGS FLATTENED\n"); return 2; }
  uint8_t seed[32], block[persist::kBlock];
  for (int i = 0; i < 32; ++i) { char byte[3] = {argv[2][2 * i], argv[2][2 * i + 1], 0}; seed[i] = (uint8_t) strtoul(byte, nullptr, 16); }
  persist::encodeHeader(block, strtoull(argv[1], nullptr, 0), seed, strtoull(argv[3], nullptr, 0), (uint32_t) strtoul(argv[4], nullptr, 0),
                        strtoull(argv[5], nullptr, 0));
  fwrite(block, 1, sizeof block, stdout);
  return 0;
}
