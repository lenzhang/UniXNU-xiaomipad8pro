// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoVideo V2 (piano-video=3): the firmware. Portable (host tests in tests/bringup_host.cpp with a synthetic signed-image layout
// and a secure-world model; tooling/k27/build-native-piano-video.sh also parses the owner's own file when it is on this Mac).
//
// Where the file comes from (docs/drivers/video.md section 12.2): the owner's vpu35_4v.mbn through the shared run-time firmware
// loader (package/include/PianoFirmware.h, docs/kc-space.md §6) against the pin PianoVideo/vpu35_4v.mbn the build writes: from a
// RAM image's addon or the installed system's ESP (EFI/UniXNU/firmware/PianoVideo/vpu35_4v.mbn), staged by the build into the
// ignored out/firmware/PianoVideo/ and placed by build-ram-images.py / build-esp.py; never in this repository. Not from odm_a: that
// is a logical partition inside "super" (LP metadata) holding an EROFS image, which nothing native reads.
//
// The sequence (Linux iris_firmware.c iris_fw_load and drivers/soc/qcom/mdt_loader.c qcom_mdt_load as the specification):
//   30 the file (the owner's fwRead)          31 the pin once more (the loader checked it): 2 409 160 bytes and its SHA-256
//   32 the image: an ELF with a hash segment, every loadable segment inside the 8 MiB carveout (relocatable: the span; fixed: the
//      addresses)                              33 the metadata (headers + hash segment) in contiguous memory, cleaned to the PoC
//   then, holding the kernel-wide secure-call lock once (package/include/PianoScmLock.h, tag VIDE) across all of it:
//   34 PAS init_image(9, metadata)  35 PAS mem_setup(9, 0x9c300000, span) (relocatable images)  36 the segments into the carveout
//   (write-combined mapping, 8-byte stores, the memory-only tails zeroed)  37 PAS auth_and_reset(9)  38 MP_VIDEO_VAR (0x42000c08)
//   with the content-protection ranges of iris_platform_vpu3x.c tz_cp_config_vpu3.
//   A failure after init_image is followed by PAS shutdown(9) (Linux does so after MP_VIDEO_VAR fails; we also do it after a
//   failed mem_setup, segment load or authentication, so the next try starts from a clean PAS state). The lock is given back before
//   this returns, on every path.
//   39 (unload) PAS shutdown(9), a secure call of its own.
// The Hw adds: int fwRead(const uint8_t **bytes, uint64_t *size) (0 or an errno); void fwRelease(); void sha256(const uint8_t *,
// uint64_t, uint8_t out[32]); uint8_t *fwMetadata(uint64_t bytes, uint64_t *phys); void fwMetadataFree(); volatile uint8_t
// *carveoutMap(uint64_t pa, uint64_t bytes); void carveoutUnmap(); void cleanToPoc(const void *, uint64_t); void barrier() (dsb sy);
// void scmHold(bool take) (the secure-call lock: one hold of this kext);
// int scmPasInit(uint64_t metaPhys); int scmPasMemSetup(uint64_t pa, uint64_t bytes); int scmPasAuth(); int scmMpVideoVar(uint32_t,
// uint32_t, uint32_t, uint32_t); int scmPasShutdown(); int scmRemoteState(uint32_t state) (each 0 when x0 and the result are 0;
// the Hw logs every call before and after it).
#pragma once
#include <stdint.h>
#include "QcomMdt.h"
#include "VideoOps.h"

namespace pianovideo {

struct FwState {
  bool initDone = false;   // init_image accepted: a failure from here on is followed by PAS shutdown
  bool loaded = false;     // auth_and_reset done: PAS 9 runs the core's firmware (until PAS shutdown)
  QcomMdtImage mdt;
  uint64_t size = 0, metaBytes = 0, span = 0, stored = 0;
  unsigned loadable = 0;
  int pinOk = 0;           // 1 matched, -1 refused
  uint8_t sha[32] = {};
  int failedStep = 0, error = kOk;
};

struct MemFile {
  const uint8_t *bytes;
  uint64_t size;
};
inline int pianoVideoMemRead(void *context, uint64_t offset, void *to, size_t length)
{
  const MemFile *f = static_cast<const MemFile *>(context);
  if (offset > f->size || length > f->size - offset) return -1;
  uint8_t *out = static_cast<uint8_t *>(to);
  for (size_t i = 0; i < length; ++i) out[i] = f->bytes[offset + i];
  return 0;
}

inline bool samePin(const uint8_t a[32], const uint8_t b[32])
{
  unsigned diff = 0;
  for (unsigned i = 0; i < 32; ++i) diff |= unsigned(a[i] ^ b[i]);
  return !diff;
}

// The image's place in the carveout: every loadable segment fits (relocatable images by their span from the lowest address).
inline const char *imageVerdict(const QcomMdtImage &m, unsigned *loadable)
{
  *loadable = 0;
  if (m.relocatable && qcom_mdt_span(&m) > kCarveoutBytes) return "the image's span is larger than the 8 MiB carveout";
  for (unsigned i = 0; i < m.count; ++i) {
    if (!m.segment[i].load) continue;
    uint64_t at = 0;
    if (qcom_mdt_place(&m, i, kCarveout, kCarveoutBytes, &at)) return "a segment lies outside the carveout or the file";
    ++*loadable;
  }
  return *loadable ? nullptr : "no loadable segment";
}

// 8-byte aligned stores (bytes at the ends), no memcpy: the carveout is a write-combined device-side mapping.
inline void storeOut(volatile uint8_t *to, const uint8_t *from, uint64_t n)
{
  while (n && (uintptr_t(to) & 7)) {
    *to++ = *from++;
    --n;
  }
  for (; n >= 8; n -= 8, to += 8, from += 8) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= uint64_t(from[i]) << (8 * i);
    *reinterpret_cast<volatile uint64_t *>(to) = v;
  }
  while (n--) *to++ = *from++;
}
inline void zeroOut(volatile uint8_t *to, uint64_t n)
{
  while (n && (uintptr_t(to) & 7)) {
    *to++ = 0;
    --n;
  }
  for (; n >= 8; n -= 8, to += 8) *reinterpret_cast<volatile uint64_t *>(to) = 0;
  while (n--) *to++ = 0;
}

template <class O>
class Firmware {
 public:
  Firmware(O &ops, FwState &st) : ops(ops), st(st) {}
  O &ops;
  FwState &st;
  int stopAt = 0;
  // The pin (the kernel never changes it; the host tests point it at their synthetic image's own size and SHA-256).
  uint64_t pinBytes = kFirmwareBytes;
  const uint8_t *pinSha = kFirmwareSha256;

  int fail(int stepNo, int error)
  {
    st.failedStep = stepNo;
    st.error = error;
    return error;
  }

  // 30-38. The core must be powered (V1's on) before this runs (Linux powers it before iris_fw_load).
  int load()
  {
    auto &hw = ops.hw;
    st.error = kOk;
    ops.hw.stage(kStepFwFile, "the firmware file (the boot's addon or the ESP)");
    const uint8_t *bytes = nullptr;
    uint64_t size = 0;
    const int rc = hw.fwRead(&bytes, &size);
    if (rc || !bytes) {
      hw.note("firmware: %s not loaded (error %d): stage the owner's copy (build-native-piano-video.sh, PIANO_VIDEO_FW) before "
              "building the RAM image or the ESP", kFirmwarePinPath, rc);
      return fail(kStepFwFile, kErrFwFile);
    }
    st.size = size;
    int result = check(bytes, size);
    if (!result) result = pas(bytes, size);
    hw.fwMetadataFree();
    hw.fwRelease();
    return result;
  }

  int check(const uint8_t *bytes, uint64_t size)
  {
    auto &hw = ops.hw;
    hw.stage(kStepFwPin, "the pin: size and SHA-256");
    hw.sha256(bytes, size, st.sha);
    const bool match = size == pinBytes && samePin(st.sha, pinSha);
    st.pinOk = match ? 1 : -1;
    hw.note("firmware: %llu bytes, sha256 %02x%02x%02x%02x%02x%02x%02x%02x...: %s", (unsigned long long)size, st.sha[0], st.sha[1],
            st.sha[2], st.sha[3], st.sha[4], st.sha[5], st.sha[6], st.sha[7],
            match ? "the pinned vpu35_4v.mbn (OS3.0.303.0)" : "NOT the pinned file (2409160 bytes, sha256 95c5882f...): refused");
    if (!match) return fail(kStepFwPin, kErrFwPin);
    if (stopAt == kStepFwPin) return fail(kStepFwPin, kErrStopped);
    hw.stage(kStepFwParse, "the image's headers");
    MemFile f = {bytes, size};
    const int parsed = qcom_mdt_parse(&st.mdt, size, pianoVideoMemRead, &f);
    const char *why = parsed ? "the ELF or program headers do not parse" : imageVerdict(st.mdt, &st.loadable);
    st.span = parsed ? 0 : qcom_mdt_span(&st.mdt);
    st.metaBytes = parsed ? 0 : qcom_mdt_metadata_size(&st.mdt);
    hw.note("firmware: ELF%u, %u program headers, %u loadable, hash at %d, %s, span %#llx at %#llx, metadata %llu bytes%s%s",
            st.mdt.elf64 ? 64 : 32, st.mdt.count, st.loadable, st.mdt.hash, st.mdt.relocatable ? "relocatable" : "fixed",
            (unsigned long long)st.span, (unsigned long long)st.mdt.min_address, (unsigned long long)st.metaBytes, why ? ": " : "",
            why ? why : "");
    if (why) return fail(kStepFwParse, kErrFwImage);
    return kOk;
  }

  int pas(const uint8_t *bytes, uint64_t size)
  {
    auto &hw = ops.hw;
    hw.stage(kStepFwMeta, "the metadata in contiguous memory");
    uint64_t phys = 0;
    uint8_t *meta = hw.fwMetadata(st.metaBytes, &phys);
    MemFile f = {bytes, size};
    if (!meta || !phys || (phys & 0xfff)) return fail(kStepFwMeta, kErrNoMemory);
    if (qcom_mdt_metadata(&st.mdt, pianoVideoMemRead, &f, meta, st.metaBytes)) return fail(kStepFwMeta, kErrFwImage);
    hw.cleanToPoc(meta, st.metaBytes);
    if (stopAt == kStepFwMeta) return fail(kStepFwMeta, kErrStopped);
    hw.scmHold(true);
    int rc = held(bytes, size, phys);
    if (rc && st.initDone) {
      hw.note("firmware: %s at step %d: PAS shutdown(9) so the next load starts clean", errorName(rc), st.failedStep);
      hw.scmPasShutdown();
      st.initDone = st.loaded = false;
    }
    hw.scmHold(false);
    return rc;
  }

  int held(const uint8_t *bytes, uint64_t size, uint64_t phys)
  {
    auto &hw = ops.hw;
    hw.stage(kStepPasInit, "PAS init_image(9)");
    if (hw.scmPasInit(phys)) return fail(kStepPasInit, kErrScm);
    st.initDone = true;
    if (stopAt == kStepPasInit) return fail(kStepPasInit, kErrStopped);
    uint64_t region = kCarveoutBytes;
    if (st.mdt.relocatable) {
      hw.stage(kStepPasMemSetup, "PAS mem_setup(9, the carveout, the span)");
      if (hw.scmPasMemSetup(kCarveout, st.span)) return fail(kStepPasMemSetup, kErrScm);
      region = st.span;
    }
    hw.stage(kStepPasSegments, "the segments into the carveout");
    volatile uint8_t *to = hw.carveoutMap(kCarveout, region);
    if (!to) return fail(kStepPasSegments, kErrNoMemory);
    st.stored = 0;
    for (unsigned i = 0; i < st.mdt.count; ++i) {
      const QcomMdtSegment &s = st.mdt.segment[i];
      uint64_t at = 0;
      if (!s.load || qcom_mdt_place(&st.mdt, i, kCarveout, region, &at)) continue;
      if (s.file_size) storeOut(to + at, bytes + s.file_offset, s.file_size);
      if (s.memory_size > s.file_size) zeroOut(to + at + s.file_size, s.memory_size - s.file_size);
      st.stored += s.file_size;
    }
    (void)size;
    hw.barrier();
    hw.carveoutUnmap();
    hw.note("firmware: %u segments, %llu bytes stored at %#llx (+ %#llx mapped write-combined), unmapped", st.loadable,
            (unsigned long long)st.stored, (unsigned long long)kCarveout, (unsigned long long)region);
    if (stopAt == kStepPasSegments) return fail(kStepPasSegments, kErrStopped);
    hw.stage(kStepPasAuth, "PAS auth_and_reset(9)");
    if (hw.scmPasAuth()) return fail(kStepPasAuth, kErrScm);
    st.loaded = true;
    if (stopAt == kStepPasAuth) return fail(kStepPasAuth, kErrStopped);
    hw.stage(kStepMpVideoVar, "MP_VIDEO_VAR: the content-protection ranges");
    if (hw.scmMpVideoVar(kCpStart, kCpSize, kCpNonPixelStart, kCpNonPixelSize)) return fail(kStepMpVideoVar, kErrScm);
    return kOk;
  }

  // 39: PAS shutdown(9). The core must still be powered (Linux unloads before it powers off).
  int unload()
  {
    if (!st.loaded && !st.initDone) return kOk;
    ops.hw.stage(kStepPasShutdown, "PAS shutdown(9)");
    const int rc = ops.hw.scmPasShutdown();
    st.loaded = st.initDone = false;
    return rc ? kErrScm : kOk;
  }
};

}  // namespace pianovideo
