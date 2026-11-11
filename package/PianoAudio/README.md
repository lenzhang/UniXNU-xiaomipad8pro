# PianoAudio

The speakers and microphones of piano for native macOS 27: four FourSemi FS19xx smart amplifiers on the I2C hub's SE0
(`i2c@980000`, 0x34-0x37) fed by the LPAIF secondary TDM interface (gpio121-124), DMICs on the LPASS VA macro, all streamed
through the audio DSP's AudioReach graph manager (GPR on the GLINK channel `adsp_apps`). PianoPower owns the DSP; PianoAudio is a
client of it (`package/include/PianoAdspContract.h`) and never starts, stops or restarts it or votes its rails.

- `piano-audio=1`: a read-only survey: the main TLMM's configuration and level of the fourteen audio pins (bus, the amplifiers'
  reset and interrupt lines, the TDM pins) and the apps SMMU's stream-match entries of the DSP's PCM streams 0x1001/0x1041 (global
  page only). Every MMIO access is a load.
- `piano-audio=2` (with `piano-power=2`): the DSP link: `adsp_apps` registered, PianoPower's health watched (off for the boot at the
  first sign of trouble), one `APM_CMD_GET_SPF_STATE`; nothing else is sent.
- `piano-audio=3`: also the macOS devices "Speaker" and "Mic" (IOAudio2Devices, DroidVMAudio's shape) on the null transport: their
  own clock, silence both ways, nothing reaches the hardware.

Host-tested, not run by any level yet: the FS19xx core, piano's AudioReach graphs with the one-command-at-a-time sequencer and its
reverse unwind, the period pump, the speaker-safety chain. Design, hardware facts, the ladder, the macOS side and the test plan:
`docs/drivers/audio.md`.

| File | What |
|---|---|
| `PianoAudio.cpp` | the IOService on IOPlatformExpertDevice; the boot arguments, the survey thread and its log lines, levels 2 and 3 |
| `AudioSurvey.h`, `AudioHw.h` | level 1: what it reads (no store in the file), the decoders and verdicts; the facts with their sources |
| `PianoGpr.{h,c}` | the GPR packet codec and the APM readiness probe (UniXNU-sheng's, renamed) |
| `PianoAudioReach.{h,c}` | piano's APM/PRM packets: the TDM speaker graphs, the VA capture graphs, PRM clocks, map/write/read, answers (from sheng's) |
| `AudioSequence.{h,c}` | one DSP command at a time with its answer checked, the reverse unwind; the speaker plan; the period pump |
| `AudioDspLink.{h,c}`, `AudioDsp.cpp` | level 2: the guard on PianoPower's health, the receive inbox, the readiness exchange; its kext glue |
| `Fs19xx.{h,c}` | the amplifiers: registers, probe, the `fs19xx.fsm` container (read at run time, never committed), the safety filter, the power sequences, status |
| `AudioSha256.{h,c}` | the stock preset's published size and digest |
| `AudioSafety.{h,c}` | the output chain (slots, DC blocker, gain <= 0 dB, fades, ceiling <= -1 dBFS), the test tone, the stream guard, the PCM ring |
| `AudioPcm.{h,c}`, `PianoAudioDevice.cpp` | the PCM transport interface and the null transport; the IOAudio2Device classes (from DroidVMAudio) |
| `tests/` | `audio_host.cpp`, `gpr_host.c`, `audioreach_host.c`, `sequence_host.c`, `fs19xx_host.c`, `pcm_host.c`, `dsplink_host.c` |

Build: `tooling/k27/build-native-piano-audio.sh` (all host tests, the objects, the import check, `check-audio2-family.py`).
PianoAudio is in `configs/k27-native-kexts.txt` after PianoPower (whose client functions it imports by name; its descriptor names
`com.unixnu.driver.PianoPower` as a library) and inert without `piano-audio`. No firmware, preset or topology is read by any level
yet. Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
