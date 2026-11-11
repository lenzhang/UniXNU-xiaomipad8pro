# PianoAudio

The speakers and microphones of piano for native macOS 27: four FourSemi FS19xx smart amplifiers on the I2C hub's SE0
(`i2c@980000`, 0x34-0x37) fed by the LPAIF secondary TDM interface (gpio121-124), DMICs on the LPASS VA macro, all streamed
through the audio DSP's AudioReach graph manager (GPR on the GLINK channel `adsp_apps`). **Level 1 only**: a read-only survey of
what the boot stages leave of that path, so that level 2 (the DSP link and the amplifiers' bus probe) starts from measured facts.

- `piano-audio=1`: the main TLMM's configuration and level of the fourteen audio pins (bus, the amplifiers' reset and interrupt
  lines, the TDM pins) and the apps SMMU's stream-match entries of the DSP's PCM streams 0x1001/0x1041 (global page only). Every
  MMIO access is a load; no store at all. Not read: the amplifiers, the I2C hub's engine, any LPASS block, the DSP.

Design, hardware facts, the bring-up ladder, the macOS side and the test plan: `docs/drivers/audio.md`.

| File | What |
|---|---|
| `PianoAudio.cpp` | the IOService on IOPlatformExpertDevice; the survey thread, its log lines and the `piano-audio-survey` property |
| `AudioSurvey.h` | portable: what level 1 reads (no store in the file), the pin decode, the reset/bus/TDM summary and verdict, the SMMU scan |
| `AudioHw.h` | the facts, each with its source, including the blocks level 1 does not read |
| `tests/audio_host.cpp` | host tests against register models (what is and is never loaded, tables, decoders, verdicts, SMMU) |

Build: `tooling/k27/build-native-piano-audio.sh` (host tests, the objects, the import check). PianoAudio is in
`configs/k27-native-kexts.txt` and inert without `piano-audio`. No firmware, preset or topology is read at this level. Sources and
licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
