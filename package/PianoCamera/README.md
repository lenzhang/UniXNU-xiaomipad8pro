# PianoCamera

Piano's two cameras for native macOS 27: the rear Samsung **S5KJN1** (CCI0 master 1, 0x10) and the front OmniVision **OV32D40**
(QUP2 SE1's pins gpio4/5, 0x10), SM8750 CAMSS. Backlog B11. Levels 1-3 built and host-tested. On the tablet (2026-11-12): level 1
passed (c1); level 2 read the rear's chip ID 0x38e1 over CCI0 at 19.2 and 24 MHz (c2r, c2r24); the front's MCLK4 did not run (c2f:
GCC's camera XO and TITAN_TOP now on for it); level 3 reached stream-on and aborted in its power-down at an SMMU word the hypervisor
refuses (c3rp, c3r: the release now keeps such words, runs first, and the report is logged before the power-down).
`docs/drivers/camera.md` §8.4 has the results, §8.1 the next images.

- `piano-camera=1`: a read-only survey (`CameraSurvey.h`): the GCC camera branches, CAMCC's GDSCs, branches, clock sources and PLLs
  (only while GCC clocks CAMCC's AHB), the MCLK controller (likewise), CCI0 and the CSID/TFE versions (only for a block that is powered
  and clocked), QUP2 SE1's GENI words (only while clocked), the ten camera pins, the eight camera/CSIPHY LDOs over the SPMI observer
  and the five voted ones' cmd-db addresses, the SMMU groups of stream 0x1c00..0x1c1f; a summary (`piano-camera-survey`) and level
  2's verdict per sensor. Every MMIO access is a load; the only store is the SPMI observer's read command.
- `piano-camera=2`: the survey, then each sensor (`CameraPower.h`): its bus first (rear: TITAN_TOP, CCI0's clock from the XO,
  its pins, the controller reset and timed for 400 kHz; front, or the rear with `piano-camera-rear-bus=1`: software I2C), the reset
  line low, its rails over RPMh under PianoA830's DRV-2 lock, each read back over SPMI at its exact voltage, the 2.8 V supply's enable
  pin, the MCLK (19.2 MHz from the XO; the rear at 24 MHz from the MCLK PLL with `piano-camera-mclk24=1`), the reset released, the
  chip ID (0x38e1 at 0x0000; 0x5632 at 0x300a), then everything taken back in reverse order, exactly what was changed (the rails'
  enables only where this level added them). Stores only to its allowlist; no sensor register is written.
  Since c1: CAMCC's domains (MXC, MMCX) are voted through the shared helper (`package/include/PianoMmVotes.h`) before its first
  access, and a hardware-gated GCC branch counts as on once enabled (Linux's rule). Since c2f: GCC's camera XO on for every path,
  TITAN_TOP (the sensors' DT power domain) for either sensor (`piano-camera-camss=0`: the minimal path), Linux's shared-RCG enable
  for the MCLK even at the XO.
- `piano-camera=3`: RAW10 frames from one sensor (`CameraStream.h`): level 2's power-up (TITAN_TOP for either bus, the rear's MCLK
  at 24 MHz), MM1 (shared votes), GCC's camera HF AXI, CAMCC PLL0 at 1200 MHz and the stream's clocks (400/200 MHz), the SMMU route of
  stream 0x1c00 (the boot chain's bypass, else a translating window of ours mapping only the buffer: `CameraSmmu.h`,
  `CameraSmmuWindow.h`), a physically contiguous buffer with a canary guard, CSID-lite0 reset, TFE-lite0's write master 0 in frame
  mode, CSID-lite0's RX and RDI0 (RAW10, payload only), the CSIPHY with its lane table from `data/camera/csiphy-v2.4.tbl`, the
  sensor's tables (and the rear's colour bars), a bounded number of frames polled (BUF_DONE, SOF/EOF; per-frame times, stale
  BUF_DONEs apart, the CSID/CSIPHY status), stop, one frame's statistics (with the first rows' bytes), the whole report logged, then
  everything down with the SMMU window first (while the camera NoC is voted and clocked; a found word this SMMU refuses is not
  written back). Rear 4080x3072 (S5KJN1), front 3264x1836 (OV32D40).
- Levels 4-5 (a software ISP to NV12, a macOS camera device for apps) are designed in `docs/drivers/camera.md` §4, not built.

| File | What |
|---|---|
| `PianoCamera.cpp` | the IOService on IOPlatformExpertDevice: the thread, pages mapped on demand, the SPMI observer, cmd-db, the RPMh DRV-2 transaction under the lock word, the report, the properties, the power-tree join |
| `CameraHw.h` | the facts, each with its source |
| `CameraSurvey.h` | portable: level 1's loads, its gates, the decoders, level 2's refusals |
| `CameraPower.h` | portable: level 2 (the allowlist, the undo record, the sensors' sequences, the chip ID, the power-down) over an ops class |
| `CciMaster.h` | portable: CCI0 master 1 (reset, timing, queued writes and register reads, NACK/timeout recovery), polled |
| `CameraStream.h` | portable: level 3 (its allowlist, the undo record, PLL0 and the clocks, the route, the buffer, CSID/TFE/CSIPHY, the sensor's tables, the stream, the statistics, the power-down) |
| `CameraSmmu.h` | portable: stream 0x1c00's route (bypass, our window, refused) and the window's release |
| `CameraSmmuWindow.h` | portable: the translating window (PianoWifi's `WifiSmmuWindow.h`, copied) |
| `CameraRpmh.h` | portable: one RPMh transaction on apps DRV-2 (PianoBluetooth's `BtRpmh.h`; commands with full addresses for the shared votes) |
| `BitI2c.h` | portable: the software I2C master (PianoKeyboard's) |
| `CameraPlan.h` | portable: `piano-camera-plan`, several test images' sequences on one boot (their arguments, the parser, the rule to go on) |
| `tests/camera_host.cpp` | host tests against a board model (gates, CCI0 and bit-level sensors, PMIC/RPMh, timing; level 3: PLL0, the CAMSS blocks, the SMMU and its walk, memory, frames written by the model's write master; the tablet's SMMU rules and the teardown's order), failures at every step, the as-found check after every power-down, the tables' header |

Build: `tooling/k27/build-native-piano-camera.sh` (the tables' check against `data/camera/MANIFEST`, their header into `out/`, the
host tests, a store check of the survey headers, the objects, the camera thread's stack bound (`tooling/k27/check-kext-stack.py`, at
most 6 KiB of the kext's own frames: the thread has XNU's 16 KiB; the sequence's objects live in static storage), the import check). PianoCamera is in `configs/k27-native-kexts.txt`
after PianoA830 (its lock words) and inert without `piano-camera`. Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
