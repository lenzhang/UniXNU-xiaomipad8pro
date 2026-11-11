# PianoVideo

piano's video core for native macOS 27: the SM8750 Iris (VPU 3.5) that decodes H.264, HEVC and VP9 and encodes H.264 and HEVC in
hardware. **Levels 1-4** (the ladder's V0-V3 of `docs/drivers/video.md`), built and host-tested; none has run on the tablet yet.

- `piano-video=1`: a read-only survey: GCC's four video branches first; VIDEO_CC only with GCC's video AHB on (PLL0, the core
  clock's RCG and dividers with the rate and the OPP it matches, both GDSCs, every branch); the core's controller registers
  (version, CTRL_INIT/CTRL_STATUS, queue table, UC region, interrupt, LPI and power-status words) only with the controller GDSC
  powered and clocked, the codec's idle words only with the codec domain on too; the apps SMMU's entries for streams
  0x1940..0x194f, the free stream-match groups and the banks in use. Every MMIO access is a load; the write-to-clear and doorbell
  registers are never loaded. `piano-video-carveout=1` also loads the first words of the firmware carveout.
- `piano-video=2` (V1): the survey, then power cycles at the lowest operating point (240 MHz): MXC svs, MMCX at the boot display's
  floor and BCM MM1 by RPMh under PianoA830's DRV-2 lock, GCC's video branches, VIDEO_CC's PLL0 at 720 MHz, the controller then
  the codec GDSC and clocks, the readout with the gate open, off in Linux's order (`piano-video-cycles`, `-hold-ms`).
- `piano-video=3` (V2): + the owner's `vpu35_4v.mbn` (pinned size and SHA-256) from the RAM image's addon or the ESP, PAS 9 and MP_VIDEO_VAR under one
  hold of the kernel-wide secure-call lock, PAS shutdown before the power-off.
- `piano-video=4` (V3): + the streams' own SMMU bank and the UC region, the boot words and the HFI handshake (SYS_INIT,
  IMAGE_VERSION, the version string), collapse 1.5 s after the last activity with the firmware kept, resume/collapse cycles
  (`piano-video-resumes`), `piano-video-unload=1` to end with PAS shutdown and the bank released.

Levels 2-4 write registers only through `VideoBringup.h`: every (address, value) pair is logged the first time it is written,
every wait is bounded, any failure powers everything down, and nothing is written below `piano-video=2`. Design, decisions, the
batched tablet test list with its expected lines and the risks: `docs/drivers/video.md` (section 12 for levels 2-4).

| File | What |
|---|---|
| `PianoVideo.cpp` | the IOService on IOPlatformExpertDevice; the survey thread and its log lines; the kernel's Hw for levels 2-4 (MMIO pages on demand, cmd-db, the DRV-2 word, the firmware through the shared loader (`piano_fw_load`), SHA-256, the metadata buffer, the carveout and UC-region mappings, the secure calls); the `piano-video-survey` and `piano-video-bringup` properties |
| `VideoSurvey.h` | portable: what level 1 reads (no store in the file), the gate, the clock and status decoders, the SMMU scan, the verdicts |
| `VideoOps.h` | portable: the one access path of levels 2-4 (the write log, regmap-style read-modify-writes, bounded polls), steps and errors |
| `VideoRpmh.h` | portable: cmd-db lookups, corners, one ACTIVE-TCS transaction on apps DRV-2, the video's MXC/MMCX/MM1 votes |
| `VideoPower.h` | portable: V1's power-on and power-off in Linux's VPU 3.5 order at 240 MHz; the readout's verdict |
| `VideoFirmware.h` | portable: V2's pin, the image's place in the carveout, PAS 9 + MP_VIDEO_VAR under the secure-call lock, PAS shutdown |
| `VideoSmmu.h` | portable: V3's SMMU plan, our table for the UC region, the bank and routes, the teardown |
| `VideoHfi.h` | portable: V3's queue table and queues, HFI gen2 packets and answers, the boot words |
| `VideoBringup.h` | portable: levels 2-4 as one sequence (V1, V2, V3, the idle collapse, resume, unload, failures) |
| `VideoHw.h` | the facts, each with its source |
| `FatVolume.c/h`, `QcomMdt.c/h`, `Sm8750Scm.c/h` | copies of PianoPower's (the build checks them byte for byte; Sm8750Scm is MIT); FatVolume serves the shared loader's ESP source |
| `tests/video_host.cpp` | level 1's host tests against register models (the gate, the allowlist of loads, decoders, verdicts, SMMU) |
| `tests/bringup_host.cpp` | levels 2-4 against one SoC model (VIDEO_CC, the core, DRV-2, cmd-db, the secure world, the SMMU, a firmware reached through our table), every wait made to fail, every secure call refused, the stop argument at every step |

Build: `tooling/k27/build-native-piano-video.sh` (the secure-call lock's tests, the shared firmware loader's tests, the copies'
check, the two host tests, the pin, the objects with the loader's `package/common` files, the import check). PianoVideo is in
`configs/k27-native-kexts.txt` after PianoA830 (its lock words) and NativeUfs (the loader's ESP source), and inert without
`piano-video`. The firmware (`vpu35_4v.mbn`, the owner's copy, never committed) is pinned as `PianoVideo/vpu35_4v.mbn`
(docs/kc-space.md §6): the build stages it into the ignored `out/firmware/PianoVideo/`, `build-ram-images.py` puts it into a RAM
image's addon and `build-esp.py` onto the ESP; level 3 loads it at run time. Sources and
licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent; the MIT notice for Sm8750Scm).
