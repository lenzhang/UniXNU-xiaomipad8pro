// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoA830: the Adreno830 (A830) kernel GPU driver as a k27 native kext on piano. The pieces of package/PianoA830/board:
//   PianoA830.cpp        the IOKit side: the starter (inert unless piano-gpu=N), the macOS AGXAccelerator, the log hooks
//   PianoA830Board.cpp   Adreno830Board from the generated DT constants and the embedded firmware, the boot arguments, adreno830_start
//   PianoA830Survey.cpp  piano-gpu=1: the read-only survey of power, clocks, SMMU, GMU, DRV-2 and the ZAP carve-out
#pragma once
#include <IOKit/IOTypes.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
// One console line "PianoA830: <text>" (IOLog; thread context).
void piano_a830_note(const char *format, ...) __attribute__((format(printf, 1, 2)));
// Fill the board, parse the boot arguments and run adreno830_start once (level 1: S0 only; 2: S1..S10, piano-gpu-stop honoured).
IOReturn piano_a830_board_start(unsigned level);
// The read-only survey of piano-gpu=1 (mask: PianoA830Survey* bits, piano-gpu-survey). Thread context; writes nothing.
void piano_a830_survey(uint32_t mask);
// adreno830_start's outcome as console lines (status block, the last failure, stage times).
void piano_a830_report(IOReturn result);
// The GPU SMMU's route of every GPU-side stream ID (read only, the GPU must be on); returns how many reach memory untranslated.
unsigned piano_a830_smmu_routes(const char *when);
#ifdef __cplusplus
}
#endif

// The write-combined BO pool's default (piano-gpu-pool=<MiB> wins; S0 keeps it in 64..2048, S3 halves it until a reservation succeeds):
// a quarter of the RAM in 256 MiB steps, 512..2048 MiB. piano's XNU reports 8192 MiB (max_mem), about 13300 MiB with the 13 GiB window
// (configs/k27-adt.toml): 2048 either way; the cap is S0's. XNU maps no block of 2 GiB (NativeAdrenoContiguousMaxBytes, 2 GiB less a page),
// so S3 never asks for the 2048 and starts at 1024, the size every boot since 2026-11-12 got: asking had cost 2 GiB of pages k27 gave back
// gobbled (the 13 GiB boot's vm_page_unwire panic, patches/k27/0065; with 8 GiB the phantom wired pages of 17:27). The login screen alone used 486
// MiB of the 13T board's 512 (6 clients, WindowServer 446 MiB); the first desktop (run attend4, 24 clients) used 936 of a 1024 MiB pool
// and 701 MiB of own BOs outside it, scattered: 3 ms a BO and 90-130 ms a surface-sized one of page attribute changes, and a range of
// the GPU table per page, until the table's ranges ran out (docs/drivers/gpu.md §8, "The BO pool").
#ifdef __cplusplus
constexpr uint32_t piano_a830_pool_default(uint64_t ram_bytes)
{
  const uint64_t mib = (ram_bytes >> 20) / 4 / 256 * 256;
  return mib < 512 ? 512u : mib > 2048 ? 2048u : uint32_t(mib);
}
static_assert(piano_a830_pool_default(13ull << 30) == 2048 && piano_a830_pool_default(8ull << 30) == 2048 &&
              piano_a830_pool_default(6ull << 30) == 1536 && piano_a830_pool_default(5ull << 30) == 1280 &&
              piano_a830_pool_default(4ull << 30) == 1024 && piano_a830_pool_default(2ull << 30) == 512 &&
              piano_a830_pool_default(0) == 512 && piano_a830_pool_default(64ull << 30) == 2048,
              "the pool default's steps");
#endif

enum : uint32_t {
  PianoA830SurveyInputs = 1u << 0,    // board windows and interrupts, the embedded firmware against its pins (no hardware)
  PianoA830SurveyGcc = 1u << 1,        // GCC's GPU clocks and votes (always-on block)
  PianoA830SurveyGpuCc = 1u << 2,      // GPU_CC: PLL0, RCGs, branches, CX GDSC (only when GCC's GPU config AHB clock is enabled)
  PianoA830SurveySmmu = 1u << 3,       // the GPU SMMU's global space, stream-match table, the banks in use (CX on + SMMU clock)
  PianoA830SurveyGmu = 1u << 4,        // GMU CM3/HFI/AO registers, TCM version words, RSCC, CX_MISC (CX on + AHB/GMU clocks)
  PianoA830SurveyDrv2 = 1u << 5,       // the apps RSC DRV-2 the driver votes through: version, TCS 0..2 state
  PianoA830SurveyZap = 1u << 6,        // the ZAP carve-out's first words (last: a TZ-protected region may abort the read)
  PianoA830SurveyGx = 1u << 7,         // GX GDSC state (gx_clkctl, opt-in: not in the default mask)
  PianoA830SurveyDefault = 0x7f,
};
