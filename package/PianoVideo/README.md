# PianoVideo

piano's video core for native macOS 27: the SM8750 Iris (VPU 3.5) that decodes H.264, HEVC and VP9 and encodes H.264 and HEVC in
hardware. **Level 1 only**: a read-only survey of the core's power and clock state, so that power-up, the firmware load through PAS
and the HFI bring-up (levels 2-4 of `docs/drivers/video.md`) start from measured facts.

- `piano-video=1`: GCC's four video branches first; VIDEO_CC only with GCC's video AHB on (PLL0, the core clock's RCG and
  dividers with the rate and the OPP it matches, both GDSCs, every branch); the core's controller registers (version,
  CTRL_INIT/CTRL_STATUS, queue table, UC region, interrupt, LPI and power-status words) only with the controller GDSC powered and
  clocked, the codec's idle words only with the codec domain on too; the apps SMMU's entries for streams 0x1940..0x194f, the free
  stream-match groups and the banks in use. Every MMIO access is a load; no store at all; the write-to-clear and doorbell
  registers are never loaded. `piano-video-carveout=1` also loads the first words of the firmware carveout.

Design, hardware facts, the VideoToolbox verdict, the ladder and the test plan: `docs/drivers/video.md`.

| File | What |
|---|---|
| `PianoVideo.cpp` | the IOService on IOPlatformExpertDevice; the survey thread, its log lines and the `piano-video-survey` property |
| `VideoSurvey.h` | portable: what level 1 reads (no store in the file), the gate, the clock and status decoders, the SMMU scan, the verdicts |
| `VideoHw.h` | the facts, each with its source, including those later levels need (PAS, SCM, HFI) |
| `tests/video_host.cpp` | host tests against register models (the gate, the allowlist of loads, decoders, verdicts, SMMU) |

Build: `tooling/k27/build-native-piano-video.sh` (host tests, the objects, the import check). PianoVideo is in
`configs/k27-native-kexts.txt` and inert without `piano-video`. Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
