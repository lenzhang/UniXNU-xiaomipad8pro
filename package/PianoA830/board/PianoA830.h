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
#ifdef __cplusplus
}
#endif

enum : uint32_t {
  PianoA830SurveyInputs = 1u << 0,     // board windows and interrupts, the embedded firmware against its pins (no hardware)
  PianoA830SurveyGcc = 1u << 1,        // GCC's GPU clocks and votes (always-on block)
  PianoA830SurveyGpuCc = 1u << 2,      // GPU_CC: PLL0, RCGs, branches, CX GDSC (only when GCC's GPU config AHB clock is enabled)
  PianoA830SurveySmmu = 1u << 3,       // the GPU SMMU's global space, stream-match table, the banks in use (CX on + SMMU clock)
  PianoA830SurveyGmu = 1u << 4,        // GMU CM3/HFI/AO registers, TCM version words, RSCC, CX_MISC (CX on + AHB/GMU clocks)
  PianoA830SurveyDrv2 = 1u << 5,       // the apps RSC DRV-2 the driver votes through: version, TCS 0..2 state
  PianoA830SurveyZap = 1u << 6,        // the ZAP carve-out's first words (last: a TZ-protected region may abort the read)
  PianoA830SurveyGx = 1u << 7,         // GX GDSC state (gx_clkctl, opt-in: not in the default mask)
  PianoA830SurveyDefault = 0x7f,
};
