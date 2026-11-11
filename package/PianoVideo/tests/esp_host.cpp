// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoVideo's ESP read (VideoFirmware.h espRead over FatVolume.c) against FAT32 images that
// tooling/k27/build-native-piano-video.sh makes with mtools the way tooling/persist/build-esp.py formats the ESP (4096-byte
// sectors, one per cluster): the file at VideoHw.h's kEspFirmwarePath is found and read back byte for byte; an image without it,
// a directory in its place, an empty file and a volume that is not FAT are refused with their codes.
//   esp_host OK.img EXPECTED.bin MISSING.img DIRECTORY.img EMPTY.img NOTFAT.img
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "VideoFirmware.h"

using namespace pianovideo;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static std::vector<uint8_t> slurp(const char *path)
{
  std::vector<uint8_t> b;
  FILE *f = std::fopen(path, "rb");
  if (!f) return b;
  uint8_t chunk[65536];
  for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) b.insert(b.end(), chunk, chunk + n);
  std::fclose(f);
  return b;
}
static int imageRead(void *context, uint64_t offset, void *to, size_t length)
{
  const std::vector<uint8_t> *img = static_cast<const std::vector<uint8_t> *>(context);
  if (offset > img->size() || length > img->size() - offset) return -1;
  std::memcpy(to, img->data() + offset, length);
  return 0;
}
static int readFrom(const char *path, std::vector<uint8_t> *out)
{
  const std::vector<uint8_t> img = slurp(path);
  static FatVolume v;
  uint8_t *bytes = nullptr;
  uint64_t size = 0;
  const int rc = espRead(
      &v, img.size(), imageRead, const_cast<std::vector<uint8_t> *>(&img), [](uint64_t n) { return static_cast<uint8_t *>(std::malloc(n)); },
      [](uint8_t *p, uint64_t) { std::free(p); }, &bytes, &size);
  if (!rc && out) out->assign(bytes, bytes + size);
  if (!rc) std::free(bytes);
  return rc;
}

int main(int argc, char **argv)
{
  if (argc != 7) {
    std::printf("usage: esp_host OK.img EXPECTED.bin MISSING.img DIRECTORY.img EMPTY.img NOTFAT.img\n");
    return 2;
  }
  std::vector<uint8_t> got;
  const std::vector<uint8_t> want = slurp(argv[2]);
  CHECK(!want.empty() && readFrom(argv[1], &got) == kEspOk && got == want);
  CHECK(readFrom(argv[3], nullptr) == kEspNotFound);
  CHECK(readFrom(argv[4], nullptr) == kEspNotFound);
  CHECK(readFrom(argv[5], nullptr) == kEspSize);
  CHECK(readFrom(argv[6], nullptr) == kEspNotFat);
  if (failures) {
    std::printf("PianoVideo ESP host tests: %d failed\n", failures);
    return 1;
  }
  std::printf("PianoVideo ESP host tests: %s found and read back (%zu bytes) on a FAT32 volume of 4096-byte sectors; missing, "
              "directory, empty and not-FAT refused\n",
              kEspFirmwarePath, want.size());
  return 0;
}
