// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoAudio's log line ("PianoAudio: ..."), shared by the kext's files (PianoAudio.cpp defines it), and the calls between them.
#pragma once
#include <stddef.h>
#include <stdint.h>

extern "C" void piano_audio_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// AudioDsp.cpp: level 2 (the DSP link) from the GPR port set `ports` (AudioDspLink.h audio_ports; try_ports: walk the others while
// nobody answers); 0 when the APM answered ready.
extern "C" int piano_audio_dsp_link(uint32_t wait_s, uint32_t settle_ms, int ports, int try_ports, char *summary, size_t size);

// AudioDsp.cpp: levels 4-7 (AudioSession.h) after level 2, or level 5 alone without the DSP (dsp 0); then, from piano-audio=8,
// levels 8-10 (AudioLive.h) for the rest of the boot when levels 4-6 passed. The arguments as parsed.
struct PianoAudioLevelArgs {
  uint32_t level;                     // 4..10
  int dsp;                            // level 2 reached "APM ready" in this boot
  uint32_t lpassRead, presetAny, mic, silenceMs;
  uint32_t tone, toneMask, toneMs, gapMs, toneGain;
  uint32_t i2cSwap, i2cHalfUs, smmu;
  uint32_t liveGain, liveCeilingDb, liveIdleMs, micGain;   // levels 8-10 (0: the level's default)
};
// Runs until the levels are done (levels 4-7 bounded; levels 8-10 for the rest of the boot, until PianoPower's health stops
// them); the summary for the log and ioreg. 0 when every level that ran passed.
int piano_audio_levels(const PianoAudioLevelArgs *args, char *summary, size_t size);
// AudioDsp.cpp: the dsp transport's shared state (levels 8-10), made once before the devices are published; volume_percent is the
// master volume of level 9 (piano-audio-volume). 0, or -1 (no memory: the devices then use the null transport).
extern "C" int piano_audio_live_prepare(uint32_t volume_percent);
// PianoAudio.cpp: a property on the PianoAudio service (the levels' summaries, levels 8-10's state while they run).
extern "C" void piano_audio_set_property(const char *key, const char *value);
