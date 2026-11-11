// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoAudio's log line ("PianoAudio: ..."), shared by the kext's files (PianoAudio.cpp defines it), and the calls between them.
#pragma once
#include <stddef.h>
#include <stdint.h>

extern "C" void piano_audio_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// AudioDsp.cpp: level 2 (the DSP link) from the GPR port set `ports` (AudioDspLink.h audio_ports; try_ports: walk the others while
// nobody answers); 0 when the APM answered ready.
extern "C" int piano_audio_dsp_link(uint32_t wait_s, uint32_t settle_ms, int ports, int try_ports, char *summary, size_t size);

// AudioDsp.cpp: levels 4-7 (AudioSession.h) after level 2, or level 5 alone without the DSP (dsp 0). The arguments as parsed.
struct PianoAudioLevelArgs {
  uint32_t level;                     // 4..7
  int dsp;                            // level 2 reached "APM ready" in this boot
  uint32_t lpassRead, presetAny, mic, silenceMs;
  uint32_t tone, toneMask, toneMs, gapMs, toneGain;
  uint32_t i2cSwap, i2cHalfUs, smmu;
};
// Runs until the levels are done (bounded); the summary for the log and ioreg. 0 when every level that ran passed.
int piano_audio_levels(const PianoAudioLevelArgs *args, char *summary, size_t size);
