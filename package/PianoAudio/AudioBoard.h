// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoAudio levels 4-6, the kext side: the hardware AudioSession.c drives through AudioSessionOps (AudioBoard.cpp): the amplifiers'
// bit-banged bus on gpio64/65 and their reset lines, the TDM pins, the apps SMMU's entries for the DSP's PCM streams, the DMA buffers
// in PianoAudio's slice of the DSP-addressable memory, the VA macro and LPI pin loads, and the preset read from the ESP.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "AudioSession.h"

namespace pianoaudio {

struct BoardArgs {
  uint32_t level;       // the highest level that runs: the bus and the preset from 5, the SMMU and the DMA slice from 6
  uint32_t i2cSwap;     // 0: SDA on gpio64 (the DT's lane order), 1: on gpio65, 2: gpio64, and gpio65 once when no amplifier answers
  uint32_t i2cHalfUs;   // the software bus's half bit time (5: about 80 kHz)
  uint32_t smmu;        // 0: the SMMU is left alone (the streams must bypass already)
  uint32_t micGain;     // level 10: the VA decimators' gain (AudioVa.h: 0..124, 84 = 0 dB; 0 means the default 124)
};

// Fills the hardware half of ops (what the level needs; the rest stays null and the session reports it).
void boardSetup(AudioSessionOps &ops, const BoardArgs &args);
// After the session finished: the bus pins back as found, the DMA slice zeroed and unmapped, every mapping released. The reset lines
// stay driven low (the amplifiers in hardware shutdown) and the SMMU entries stay (bypass: harmless).
void boardTeardown();
// One line for ioreg: the bus, the preset, the SMMU, the slice.
void boardSummary(char *line, size_t size);

}  // namespace pianoaudio
