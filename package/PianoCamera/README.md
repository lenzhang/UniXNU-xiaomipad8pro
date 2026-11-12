# PianoCamera

Piano's two cameras for native macOS 27: the rear Samsung **S5KJN1** (CCI0 master 1, 0x10) and the front OmniVision **OV32D40**
(QUP2 SE1's pins gpio4/5, 0x10), SM8750 CAMSS. Backlog B11. Levels 1 and 2 built and host-tested; **not run on the tablet yet**.

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
- Levels 3-5 (raw frames through CSIPHY/CSID/RDI, a software ISP to NV12, a macOS camera device for apps) are designed in
  `docs/drivers/camera.md` §4, not built. The sensors' register tables they use are in `data/camera` (data, with provenance).

| File | What |
|---|---|
| `PianoCamera.cpp` | the IOService on IOPlatformExpertDevice: the thread, pages mapped on demand, the SPMI observer, cmd-db, the RPMh DRV-2 transaction under the lock word, the report, the properties, the power-tree join |
| `CameraHw.h` | the facts, each with its source |
| `CameraSurvey.h` | portable: level 1's loads, its gates, the decoders, level 2's refusals |
| `CameraPower.h` | portable: level 2 (the allowlist, the undo record, the sensors' sequences, the chip ID, the power-down) over an ops class |
| `CciMaster.h` | portable: CCI0 master 1 (reset, timing, queued writes and register reads, NACK/timeout recovery), polled |
| `CameraRpmh.h` | portable: one VRM transaction on apps DRV-2 (PianoBluetooth's `BtRpmh.h`) |
| `BitI2c.h` | portable: the software I2C master (PianoKeyboard's) |
| `tests/camera_host.cpp` | host tests against a board model (gates, CCI0 and bit-level sensors, PMIC/RPMh, timing), failures at every step, the as-found check after every power-down, the tables' header |

Build: `tooling/k27/build-native-piano-camera.sh` (the tables' check against `data/camera/MANIFEST`, their header into `out/`, the
host tests, a store check of the survey headers, the objects, the import check). PianoCamera is in `configs/k27-native-kexts.txt`
after PianoA830 (its lock words) and inert without `piano-camera`. Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
