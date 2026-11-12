# PianoAudio

The speakers and microphones of piano for native macOS 27: four FourSemi FS19xx smart amplifiers on the I2C hub's SE0
(`i2c@980000`, 0x34-0x37) fed by the LPAIF secondary TDM interface (gpio121-124), DMICs on the LPASS VA macro, all streamed
through the audio DSP's AudioReach graph manager (GPR on the GLINK channel `adsp_apps`). PianoPower owns the DSP; PianoAudio is a
client of it (`package/include/PianoAdspContract.h`) and never starts, stops or restarts it or votes its rails.

- `piano-audio=1`: a read-only survey: the main TLMM's configuration and level of the fourteen audio pins (bus, the amplifiers'
  reset and interrupt lines, the TDM pins) and the apps SMMU's stream-match entries of the DSP's PCM streams 0x1001/0x1041 (global
  page only). Every MMIO access is a load.
- `piano-audio=2` (with `piano-power=2`): the DSP link: `adsp_apps` registered, PianoPower's health watched (off for the boot at the
  first sign of trouble), one `APM_CMD_GET_SPF_STATE` from the GPR port set of `piano-audio-ports` (Ubuntu's by default); nothing
  else is sent.
- `piano-audio=3`: also the macOS devices "Speaker" and "Mic" (IOAudio2Devices, the VM-era audio kext's shape) on the null transport: their
  own clock, silence both ways, nothing reaches the hardware.
- `piano-audio=4`: also the LPASS clocks, the DSP's resources: the TDM bit clock, the VA macro's votes and mclk requested and
  released through PRM; the VA macro's identity and the LPI DMIC pins loaded while they hold.
- `piano-audio=5`: also the amplifiers: the bit-banged bus, resets released, DEVID x 4, the owner's stock preset through the shared
  firmware loader's pin `PianoAudio/fs19xx.fsm` (a RAM image's addon, or the ESP's `EFI/UniXNU/firmware/PianoAudio/fs19xx.fsm`; then
  `build-esp.py --audio-preset`'s `EFI/UniXNU/piano-fs19xx.fsm`), configured powered down and muted, held in reset at the end. Runs
  alone without the DSP.
- `piano-audio=6`: also the speaker graphs with a silent stream (the amplifiers stay down) and the microphone graphs opened and
  closed: the TDM pins, the SMMU bypass of the PCM streams, the buffers in the audio slice of `package/include/PianoDspMemory.h`.
- `piano-audio=7` with `piano-audio-tone=1`: also the tone walk, one speaker at a time (1 kHz, -20 dBFS, 600 ms each, gain 160, a 5 s
  watchdog; attended). Without `piano-audio-tone=1` nothing is powered up below level 8.
- `piano-audio=8`: after levels 4-6 (7 with the tone), for the rest of the boot: the macOS Speaker on the DSP (the dsp transport: the
  speaker plan at the first sound, the safety chain, down after `piano-audio-idle` ms of silence), gain 160, full scale at -20 dBFS.
- `piano-audio=9`: also the daily level (`piano-audio-gain` up to 192, `piano-audio-ceiling` up to -1 dBFS, the kernel's master volume
  and mute); `piano-audio=10`: also the Mic (the capture plan with the VA macro's DMIC path).

Every DSP command goes out one at a time with its answer checked, the first error undoes what was done in reverse, and any change in
PianoPower's health stops everything without another DSP command, the amplifiers muted, powered down and held in reset at once.
On the tablet so far (11-12): levels 1-3. Design, hardware facts, the ladder, the macOS side and the test plan (§8.8 the batched
tablet test list, `tooling/k27/build-audio-test-images.sh` its images): `docs/drivers/audio.md`.

| File | What |
|---|---|
| `PianoAudio.cpp` | the IOService on IOPlatformExpertDevice; the boot arguments, the survey thread and its log lines, the level dispatch |
| `AudioSurvey.h`, `AudioHw.h` | level 1: what it reads (no store in the file), the decoders and verdicts; the facts with their sources |
| `PianoGpr.{h,c}` | the GPR packet codec and the APM readiness probe (UniXNU-sheng's, renamed) |
| `PianoAudioReach.{h,c}` | piano's APM/PRM packets: the TDM speaker graphs, the VA capture graphs, PRM clocks, map/write/read, answers (from sheng's) |
| `AudioSequence.{h,c}` | one DSP command at a time with its answer checked, the reverse unwind; the speaker, PRM and microphone plans; the period pump |
| `AudioDspLink.{h,c}`, `AudioDsp.cpp` | level 2: the guard on PianoPower's health, the receive inbox, the readiness exchange, the GPR port sets; the levels loop |
| `AudioSession.{h,c}` | levels 4-7 as one state machine over the pieces (the rules of `audio.md` §10.1-10.2 as code); `keep` hands the hardware to levels 8-10 |
| `AudioLive.{h,c}` | levels 8-10: the devices' rings and flags, the speaker and mic paths over two sequencers, one command at a time, the drift controller, the master volume |
| `AudioVa.{h,c}` | level 10: the VA macro's DMIC path and the LPI DMIC pins as register values |
| `Fs19xx.{h,c}` | the amplifiers: registers, probe, the `fs19xx.fsm` container (read at run time, never committed), the safety filter, the power sequences, status |
| `AudioAmps.{h,c}` | the four amplifiers as a group: reset, all or none, the preset's verdict, the test gain, the way down |
| `AudioPreset.{h,c}`, `FatVolume.{h,c}` | the preset read from the ESP (FatVolume: PianoPower's, copied) |
| `AudioSha256.{h,c}` | the stock preset's published size and digest |
| `AudioSafety.{h,c}` | the output chain (slots, DC blocker, gain <= 0 dB, fades, ceiling <= -1 dBFS), the test tone, the walk, the stream guard, the PCM ring |
| `AudioPins.h`, `AudioBoard.{h,cpp}` | the pin values (TDM, reset lines, the bus) and the LPASS loads; the kext's hardware for levels 4-6 |
| `BitI2c.h`, `SmmuStream.h` | the software I2C master (PianoKeyboard's) and the SMMU bypass of a stream (PianoDwc3's), copied |
| `AudioPcm.{h,c}`, `PianoAudioDevice.cpp` | the PCM transport interface and the null transport (the dsp transport is in `AudioDsp.cpp`); the IOAudio2Device classes (provenance: `UPSTREAM`) |
| `tests/` | `audio_host.cpp`, `gpr_host.c`, `audioreach_host.c`, `sequence_host.c`, `fs19xx_host.c`, `amps_host.c`, `pcm_host.c`, `dsplink_host.c`, `session_host.c`, `live_host.c`, `va_host.c`, `preset_host.c`; `synthetic_fsm.h` (a made-up preset container) |

Build: `tooling/k27/build-native-piano-audio.sh` (all host tests, the copies checked against their originals, the objects, the import
check, `check-audio2-family.py`). PianoAudio is in `configs/k27-native-kexts.txt` after PianoPower and NativeUfs (whose functions it
imports by name; its descriptor names both as libraries) and inert without `piano-audio`. No firmware, preset or topology is in the
repository; the build pins the stock preset (size and SHA-256) and stages the owner's own copy in the ignored `out/firmware/`. Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
