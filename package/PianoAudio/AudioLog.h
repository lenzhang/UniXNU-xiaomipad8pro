// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoAudio's log line ("PianoAudio: ..."), shared by the kext's files (PianoAudio.cpp defines it).
#pragma once
#include <stddef.h>
#include <stdint.h>

extern "C" void piano_audio_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// AudioDsp.cpp: level 2 (the DSP link); 0 when the APM answered ready.
extern "C" int piano_audio_dsp_link(uint32_t wait_s, uint32_t settle_ms, char *summary, size_t size);
