# Cameras (B11): PianoCamera, the level ladder

Status, 2026-11-12: **levels 1 and 2 built and host-tested** (`package/PianoCamera`, in the kext list, inert without
`piano-camera`); **not run on the tablet**. Levels 3-5 designed (§4.3-4.5). The sensors' register tables are in the repository
as data (`data/camera`, §11). Backlog item B11 (`../BACKLOG-hardware.md`). Template: `README.md`.

## 1. Purpose and gate

Ubuntu v0.3.0 brought both cameras up (rear S5KJN1SQ03, fixed focus; front OV32D40), v0.10.0 fixed auto-exposure; its daemon
publishes 1920×1440 NV12 at 30 fps to every app through v4l2loopback, and takes photos with the rear torch. Natively: FaceTime,
Photo Booth, QuickTime and browsers should see "Rear Camera" and "Front Camera". No login milestone depends on it; the owner made it
a high priority on 2026-11-12. Every level is inert below its boot argument `piano-camera=N`.

## 2. Hardware facts

Sources: the boot DT ("DT", `out/inputs/piano-boot.dtb`); the Ubuntu piano kernel piano/dev 7f8d6bed (GPL, facts only; fetched
with pinned hashes by `tooling/camera/fetch-ubuntu-camera-sources.sh`): `sm8750-xiaomi-piano-camera.dtsi` ("KDTS"), `camss*.c`,
`s5kjn1.c`, `ov32d40.c`, `i2c-qcom-cci.c`, `camcc-sm8750.c`, `cambistmclkcc-sm8750.c`, `gcc-sm8750.c`, `clk-alpha-pll.c`, `gdsc.c`,
`pinctrl-sm8750.c` ("msm"); the Ubuntu device repo `device/camera` (MIT, behaviour). `package/PianoCamera/CameraHw.h` has the same
facts as code, each with its source.

| Item | Value | Source | State |
|---|---|---|---|
| CAMSS | `isp@ad27000`: CSID0/1/2 `ad27000/ad2a000/ad2d000` (0x2b00), CSID-lite0/1 `ad6d000/ad72000` (0xa00), CSIPHY0..5 `ada9000` + 0x2000·n, TFE0/1/2 `ac86000/ac96000/aca6000` (0x10000), TFE-lite0/1 `ad6e000/ad73000` (0x3000); SPIs CSID0 601, CSID1 603, CSID-lite0 605, CSIPHY1 478, CSIPHY4 122, TFE0 433, TFE-lite0 606 (edge); HW_VERSION at +0 of CSID and TFE (31:28 generation, 27:16 revision, 15:0 step) | DT, msm | verified (source); values: needs-boot (L1) |
| Clock controllers | CAMCC `ade0000` (0x20000; domains MMCX + MXC at low_svs; GCC's camera AHB `0x26004`), cambistmclkcc `1760000` (0x6000; MMCX + MX; GCC `0xa0004`). Linux keeps GCC `0x26004`, `0x26034` (camera XO), `0xa0004` on from GCC's probe, and CAMCC's DRV AHB/XO, GDSC clock and sleep clock (`0x113c4/0x113c0/0x1137c/0x11398`) on from CAMCC's | DT, msm | verified (source); state at XNU: L1 |
| GDSCs | TITAN_TOP `+0x1134c` (parent of all; CFG completion bits, RETAIN_FF), TFE0/1/2 `+0x11004/0x11084/0x110ec`; Linux's set-up: wait values 2/8/2, HW control and SW override off; on: un-collapse, POWER_UP_COMPLETE within 2 ms, 1 µs, RETAIN_FF | msm (`gdsc.c`) | verified (source) |
| CAMCC PLL0 | Taycan ELU, 19.2 MHz × 62.5 = 1200 MHz (L 0x3e, alpha 0x8000); even /2 = 600, odd /3 = 400 MHz: the CSIPHY/CSID/TFE-lite sources (400 MHz = main /3), CAMNOC RT AXI 200 (even /3), slow AHB 80 (even /7.5). Every CAMCC source's selector 0 is the XO (19.2 MHz), the reset state | msm | verified (source) |
| MCLKs | cambistmclkcc PLL0 Rivian ELU 960 MHz (L 0x32, CONFIG_CTL 0x12000000/0x00890263/0x1af04237); MCLK n RCG `+0x4000` + 0x1c·n, branch `+0x4018` + 0x1c·n; table 12 / 19.2 (XO) / 24 / 68.57 MHz; 24 MHz = PLL0 / 10 × 1/4 (source 3, CFG 0x2313, M 1, N reg 0xfc, D reg 0xfb). KDTS: rear MCLK1 24 MHz (gpio90 `cam_mclk`, 4 mA), front MCLK4 19.2 MHz (gpio93 `cam_aon_mclk4`, 2 mA) | msm, KDTS | verified (source) |
| CCI | only **CCI0** `ac7b000`, SPI 426, master 1 (KDTS 1 MHz Fast-mode Plus with the CCI clock at 37.5 MHz from PLL0), SDA gpio115, SCL gpio116 (`cci_i2c_*`, 4 mA, no bias); registers per master 0x100·m, queues 0x300 + 0x200·m + 0x100·n, IRQ 0xc00..0xc0c; commands SET_PARAM 1, REPORT 8, WRITE 9, READ 10; queues of 64/16 words, writes ≤ 11 bytes, reads ≤ 12; timing sets for a 19.2 MHz CCI clock (100/400 kHz) and a 37.5 MHz one (100/400/1000 kHz) | DT, msm | verified (source) |
| Rear S5KJN1 | CCI0 m1 **0x10**; CSIPHY1, 4 lanes (D-PHY), 700 MHz link (1.4 Gbit/s a lane); reset gpio6 active low; vddd L1I 1.096 V (`ldoi1`), vdda 2.8 V fixed (gpio175 enable, 100 µs), vddio L6B 1.8 V (`ldob6`), afvdd L7B 2.8 V (`ldob7`); on: vddd, 1–2 ms, vdda, vddio, afvdd, MCLK, reset release, 10–15 ms; off: reset, MCLK, afvdd, vddio, vdda, 1–2 ms, vddd; ID 0x0000 = 0x38e1; modes 4080×3072@30 (HTS 4352, VTS 4288), 8160×6144@10; RAW10, mounted rotated 180° (Ubuntu sets vflip: BGGR); controls exposure 0x0202, gain 0x0204 (32 = 1×, 32..2048), frame length 0x0340, orientation 0x0101, test pattern 0x0600 (2 = colour bars), stream 0x0100; flash = PianoTorch's lamp | DT, msm | verified (source) |
| Front OV32D40 | **not on CCI**: QUP2 SE1's pins (`i2c@884000`, SDA gpio4, SCL gpio5, `qup2_se1`, 2 mA, no bias; Ubuntu ran GENI I2C at 1 MHz) **0x10**; CSIPHY4, 4 lanes, 842.4 MHz; reset gpio7 active low; vddio L10B 1.8 V (`ldob10`), dvdd L2G 1.2 V (`ldog2`), avdd 2.8 V fixed (gpio176); on: reset asserted, supplies, 5–6 ms, MCLK, 10–12 ms, reset release, 20–25 ms; ID 0x300a = 0x5632 (five reads, 10–12 ms apart); modes 3264×2448@30, 3264×1836@30, 6528×4896@10; SBGGR10; controls exposure 0x3500 (24 bits), gain 0x3508 (256 = 1×, 288..15872), frame length 0x380e | DT, msm | verified (source) |
| Rails | the five sensor LDOs supply only the cameras in the DT (L6B/L7B also the disabled VCM); RPMh VRM (+0 mV, +4 enable, +8 mode, 7 = HPM, the DT's initial mode); PMIC letters are SPMI SIDs (b 1, f 5, g 6, i 8); an LDO n at page 0xc0 + n or 0x40 + n − 1 (decided by TYPE 0x04). CSIPHY rails L3G 1.2 V (also the UFS/DSI PHY PLL: never voted), L1F 0.88 V (CSIPHY0/1/3/5), L3I 0.88 V (CSIPHY2/4), both always-on | DT, `PianoWcnRails.h` | verified (source); PPIDs: L1 |
| Pins | none of the ten is in TLMM's reserved ranges (36–39, 48–51, 74); gpio4–7 and 175/176 have the egpio bit (Linux sets EGPIO_ENABLE when it selects a function); sleep states pull down, reset/enable output low | DT, msm | verified (source); state at XNU: L1 |
| SMMU | `iommus = <apps_smmu 0x1c00 0x0>`: one stream for every camera write master; Linux's DMA mask 32 bits; the write master's address register holds address >> 8 (40-bit reach) | DT, msm | verified (source); entries at XNU: L1 |
| Firmware | **none** on this path (no `request_firmware`; no ICP/CDM users); `camera_mem` carve-outs exist but are unused | msm, device `source.json` | verified (source) |
| Focus | GT9764 at CCI0 m1 0x0c, **disabled** ("register protocol unverified"): fixed focus, as in Ubuntu | DT | verified |

## 3. Ubuntu as the specification

**The pixel path, not raw.** Ubuntu's daemon (`piano-camerad`) routes sensor → CSIPHY → CSID → IPP → **TFE 980 pixel pipeline** →
NV12 (rear csiphy1/csid0/tfe0, front csiphy4/csid1/tfe1): demux, black level 64, white-balance gains, demosaic, GTM, a 64-entry gamma,
RGB→YCbCr BT.601 full range with the colour matrix folded in, a downscaler, NV12 write masters (FD_Y = WM7, FD_C = WM8); the raw
RDI path (WM 23..27) stays in the driver, never exercised on piano. The TFE register layouts were taken from Qualcomm's CamX settings
code (the driver says so). 3A is the daemon's (MIT): AE target mean luma 112 (lowered by highlights), every 3 frames, exposure lines
first, then analogue gain (rear: the raw code, 32 = 1×; front 256 = 1×), then ISP digital gain (≤ 8×); bounded start-up acquisition
(10 updates, steps up to 2×), then ±6 % steps with a 0.25 filter, re-acquisition on a lasting error (0.35 in log luma); grey-world AWB
on mid-tones (start gains R 1.8, B 1.6). Photos: torch on, drain the queue, wait for AE, capture, torch off (an explicit off: closing
V4L2 does not turn the torch off).

**The bring-up order** (`camss-*.c`, `s5kjn1.c`, `ov32d40.c`): CCI0 is up from its probe (clocks, pins, reset, timing) before any
sensor powers; a sensor's runtime resume is its power-on sequence (§2); streaming is VFE → CSID → CSIPHY → sensor last.

## 4. Design: the ladder

| Level | `piano-camera=` | What it does | Writes | Pass | State |
|---|---|---|---|---|---|
| L1 | 1 | read-only survey: CAMSS/CAMCC GDSCs and clocks, CCI0, the sensors' rails/pins/MCLKs as the boot chain left them, the SMMU streams; level 2's verdict | none (the SPMI observer's read command) | no hang, `0 stores`, the lines of §8.2 | **built**, host-tested |
| L2 | 2 | one sensor at a time: its bus, rails (RPMh under the shared lock, read back over SPMI), supply enable and reset GPIOs, MCLK; the chip ID over its bus (both sensors); powered down exactly as found | allowlisted camera registers only (§4.2) | 0x38e1 and 0x5632, `power down complete` for both | **built**, host-tested |
| L3 | 3 | RAW10 frames from one sensor through CSIPHY → CSID → RDI0 → a TFE-lite write master into memory, no ISP; one frame dumped, checked by its statistics (test pattern first, then a scene) | + CAMCC PLL0 and the camera clocks, MXC/MMCX/bus votes (shared helper), an SMMU entry for stream 0x1c00, the sensor's tables | the bars' known statistics; a scene's per-channel means move with the light | designed (§4.3) |
| L4 | 4 | a software ISP: black level, white balance, demosaic, colour matrix, gamma, simple AE → NV12 at 1280×960 / 1920×1440 | + the sensor's exposure/gain registers | a usable image; AE converges within 1 s | designed (§4.4) |
| L5 | 5 | a macOS camera device for apps: the kext's user client + a CoreMediaIO camera extension | + the user client's ring | Photo Booth/FaceTime/QuickTime/Safari list and use both cameras | designed (§4.5); C5 test first |

### 4.1 L1: the survey (built)

`CameraSurvey.h` loads, in this order: GCC's camera branches (AHB, XO, the MCLK controller's AHB, HF/SF AXI, QMIP) and QUP2's vote
word with SE1's branches; **CAMCC only while GCC clocks its AHB** (GDSCs with their CFG words, 21 branches, 11 clock sources with
their CMD/CFG and the CCI source's M/N, the seven PLLs' MODE/OPMODE/L); the MCLK controller likewise (PLL0, MCLK0..7 source,
divider, M/N/D, branch, the sleep branch); CCI0's version, IRQ, master 1 timing and queue counts, and each CSID/TFE version **only
when TITAN_TOP is on, the top AHB runs and the block's own clock (and GDSC) is on** (an unclocked CAMSS register can stall the
bus; the CSIPHY's version needs a store and is not read); QUP2 SE1's GENI firmware word only while its four branches run; the ten
camera pins (configuration and level, egpio ownership); the eight LDOs over the SPMI observer (both page rules, TYPE, STATUS1,
VSET, MODE, EN_CTL) and the five voted ones' cmd-db addresses; the SMMU's groups covering streams 0x1c00..0x1c1f with their S2CR,
CBAR and the bank's SCTLR. Then a summary (`piano-camera-survey`) and level 2's verdict for each sensor (`sensorRefusal`, §4.2).
Each part can be left out: `piano-camera-cc=0`, `-mclkcc=0`, `-blocks=0`, `-qup=0`, `-spmi=0`, `-smmu=0`.

Expected at XNU time (inferred: the boot chain does not use the cameras): GCC's three branches on, CAMCC readable with every GDSC
collapsed and its branches off, every CAMSS block "not read (TITAN_TOP GDSC off)", the MCLKs off, the camera LDOs disabled, the
CSIPHY LDOs enabled, the pins in their sleep states, no SMMU group for 0x1c00.

### 4.2 L2: power one sensor, read its chip ID, power it down (built)

`CameraPower.h` (`Session`) runs each sensor in turn (`piano-camera-sensor=1` rear, 2 front, 3 both, rear first):

1. **201** GCC's camera AHB, camera XO and the MCLK controller's AHB on where found off (under PianoA830's GCC lock word); **202** the
   survey again (the controllers are readable now); **203** the refusals: the MCLK controller unread, the sensor's MCLK branch already
   on, its reset or supply enable driven high (someone else uses it), a rail without a cmd-db address, unreadable (unless
   `piano-camera-blind=1`) or found enabled at another voltage; for the rear on CCI, CAMCC unread or CCI0 running with queued words.
   A refusal stores nothing.
2. **205 the bus first**, as Linux has CCI0 up from its probe. Rear: CAMCC's four critical branches, TITAN_TOP (Linux's set-up,
   un-collapse, CFG completion within 2 ms, RETAIN_FF), the slow AHB and CCI0 sources on **the XO** (their reset state; switched
   only if found elsewhere, the root forced on for the switch), the top AHB and CCI0 branches, gpio115/116 to `cci_i2c_*`, CCI0's IRQ
   mask, reset (RST_DONE within 100 ms) and master 1's timing for **400 kHz at a 19.2 MHz CCI clock** (`piano-camera-cci-khz=100`:
   100 kHz). Front (and the rear with `piano-camera-rear-bus=1`): the two pins as GPIO, output value 0, released: a **software I2C**
   (`BitI2c.h`, PianoKeyboard's, ~80–100 kHz; `piano-camera-i2c-half-us`), open drain, the board's pull-ups (`piano-camera-pull=1`
   adds the pads').
3. **210** reset low (asserted), the supply enable low, the MCLK pin to its function. **211–213** the rails in the driver's order
   with its waits: each one VRM transaction (mode HPM, the DT's exact voltage, enable) on apps DRV-2 under `sm8750_rpmh_drv2_owner`,
   1 ms, then read back over SPMI: enabled, ready, within 8 mV of the DT value, else the step fails. A rail found already on at its
   voltage is used and neither voted nor withdrawn. The fixed 2.8 V supply by its GPIO (100 µs).
4. **214** the MCLK: 19.2 MHz from the XO (the RCG's reset state; the front's rate; the rear's at L2, see below), its branch.
   `piano-camera-mclk24=1`: the rear's 24 MHz from the MCLK PLL (configured only when not running: Linux's CONFIG_CTL words, L 0x32,
   standby, RESET_N | BYPASSNL; then RUN, LOCK_DET within 1.5 ms, outputs, OUTCTRL), the RCG to PLL0 / 10 with M/N 1/4.
   **215** reset released; 15 ms (rear) / 25 ms (front).
5. **230** the chip ID: the register address written, two bytes read (CCI0: queue 0 write + REPORT, queue 1 READ, RD_DONE; the
   software I2C: write, repeated START, read). Rear 0x0000 = 0x38e1 (three tries, 10 ms apart), front 0x300a = 0x5632 (five, 12 ms).
   No sensor register is written. `piano-camera-hold=MS` keeps it powered that long.
6. **240** power down, always (after success, a failure, a refusal (then only GCC's branches, if step 201 turned any on), or
   `piano-camera-stop=STEP`): reset low, the bus
   quiet (CCI0's IRQ mask as found, its branches and sources as found, TITAN_TOP collapsed and its word as found, the critical
   branches off if they were), the MCLK branch off, its source as found, the PLL to standby, the rails' enables withdrawn in reverse
   order (only those this level added; the mode and set point votes stay, as `PianoWcnRails.h` does), the supply enable low, every pin
   back to its found configuration and level, GCC's branches off if they were found off. `power down complete` means the undo record
   is empty.

**Deviations from Linux, on purpose.** CCI0 and the rear's MCLK run from the XO at L2 (no CAMCC PLL, so no MXC vote, which
PianoVideo also votes, §4.3); 400 kHz instead of 1 MHz. The rear sensor's EXTCLK range is not in any source: Linux refuses anything
but 24 MHz because its mode's PLL words assume it. Reading the ID at 19.2 MHz is *inferred* to work (sensors of this class take
6–27 MHz); a rear `idRead` failure with the front passing is retried with `piano-camera-mclk24=1`. The front's GENI engine is not
used: the software I2C needs no QUP clocks or GENI firmware (L1 reports SE1's protocol for later).

**Allowlist** (`Session::writable`; a store elsewhere is refused and fails the step): GCC `0x26004/0x26034/0xa0004`; CAMCC TITAN_TOP's
GDSCR, the four critical branches, the top AHB and CCI0 branches, the slow AHB source's CMD/CFG, CCI0's source CMD/CFG/M/N/D; the
MCLK controller's MCLK1/MCLK4 sources and branches and PLL0's registers; the ten camera pins' CFG/IO; CCI0's registers. Never: any
QUP vote word, any other GDSC, the CSIPHY/CSID/TFE blocks, the SMMU, any other pin.

### 4.3 L3: RAW frames into memory (designed)

From the specification (`camss.c` `*_res_8750`, `camss-csiphy-3ph-1-0.c`, `camss-csid-980.c`, `camss-vfe-gen4.c`; facts only):

- **Path.** Rear: CSIPHY1 → CSID-lite0 → TFE-lite0 RDI0 (write master 0, at TFE-lite + 0x700; bus base +0x200); front: CSIPHY4 →
  CSID-lite0 (one sensor at a time) → the same. Any CSID takes any PHY (CSID RX_CFG0's PHY field); CSID n feeds VFE n by index (no
  output-select register). TFE-lite has no GDSC of its own (TITAN_TOP only) and needs fewer clocks than a full TFE (whose RDI0 is
  write master 23 at TFE + 0x1e00); a full TFE is the fallback.
- **Clocks.** CAMCC PLL0 at Linux's configuration (1200 MHz); `cphy_rx` 400 MHz (one source for every CSIPHY and the CSID RX clocks),
  CSIPHY timer 400 MHz, CSID 400 MHz, IFE-lite 400 (266.67 enough for RDI), CAMNOC RT AXI 200 MHz, GCC camera HF AXI, the top/fast
  AHBs at their table rates. **Votes:** MXC at low_svs (CAMCC's PLLs), MMCX at the display's floor (never lower), the camera's
  interconnect (Linux: ahb 150/300 MB/s, hf_mnoc 472/926 MB/s for 4096×3072@30). MXC is also PianoVideo's, MMCX and the MM BCMs the
  display's: one DRV-2 vote per resource replaces the last, so **step 0 of L3 is a shared multimedia-vote helper**
  (`package/include/PianoMmVotes.h`, words in PianoA830 like `PianoWcnRails.h`: per client the level it needs, the vote = the maximum
  with the display floor) that PianoVideo adopts in the same change.
- **CSIPHY** (2-phase v2.3/2.4 D-PHY; common block at +0x1000, status at +0x1138): reset (CTRL0 = 1, 5–8 ms, 0); lanes CTRL5 = 0x80 |
  the data lanes' bits = 0xd5 for 4 lanes, CTRL6 = 0x01, CTRL7 = 0x02, CTRL0 = 0; then the **103-entry lane table** (4 data lanes ×
  21 at +0x000/0x400/0x800/0xc00 and the clock lane's 19 at +0xe00; five entries carry a 209 µs delay; Linux skips its 16 skew-
  calibration entries) with the **settle count** (85 ns + 6 UI) / 2.5 ns − 6 = **29** for both sensors (400 MHz timer); interrupts
  masked (CTRL11..21 = 0). The table is a vendor-tuned PHY table, not a sensor's; by the owner's decision of 2026-11-12 it is carried
  as data: `data/camera/csiphy-v2.4.tbl` (the same tool, format rules, provenance note and MANIFEST as the sensors' tables). Stop:
  CTRL5 = CTRL6 = 0.
- **CSID-lite** (RX_CFG0/1 at +0x200/0x204; RDI n CFG0/CTRL/CFG1 at +0x500/0x504/0x510 + 0x100 n; combined RUP/AUP at +0x18; IRQ
  top/buf-done/RX/RDI status): reset (RST_CFG, RST_CMD, RST_DONE), RX_CFG0 = lanes − 1 | lane map 0x3210 << 4 | (PHY + 1) << 20
  (0x00232103 for CSIPHY1, 0x00532103 for CSIPHY4), RX_CFG1 = ECC correction; RDI0: data type 0x2b (RAW10), VC 0, decode "payload
  only" (0xf), MIPI packing (RAW10 stays packed: 5 bytes per 4 pixels), enable; register update; start. A completed frame is the
  **CSID's BUF_DONE** (the TFE's interrupt does nothing); polled first, the SPI later.
- **Write master** (frame mode, CFG = EN | MODE): IMAGE_ADDR = address >> 8 (256-byte aligned, 40-bit reach), FRAME_INCR =
  stride × height >> 8 (0xef40 for 4080×3072 at 5104-byte lines, 0x9867 for 3264×2448 at 4080), IMAGE_CFG_0/2 0xffff, packer 0,
  IRQ subsample period 0 / pattern 1, prefetch on. Open: 4080 RAW10 pixels are 5100 bytes, not a multiple of 16; frame mode may
  pack lines unpadded (*inferred*): the first dump settles it (else line mode with an explicit stride).
- **Memory and SMMU.** A ring of four frame buffers (RAW10 packed: 15 MiB each for the rear's 4080×3072, 10 MiB for the front's 3264×2448), physically
  contiguous (IOBufferMemoryDescriptor with a physical mask) or scattered behind a context bank. Stream 0x1c00: **first try** a bypass
  group (the S2CR type PianoDwc3's `SmmuStream.h` uses; the write master's 40-bit addresses then reach XNU's RAM above 4 GiB
  directly), **else** a stage-1 bank of our own mapping the ring at a 32-bit IOVA (PianoVideo's `VideoSmmu.h` technique). L1's SMMU
  lines decide. Writes are not coherent: the CPU invalidates a buffer before reading it.
- **Sensor.** The tables in `data/camera` (§11): rear `start`, `init`, `mode 4080x3072`, then our controls (vflip for BGGR, exposure,
  gain 2×, the default frame length), `on`; front `start`, `init`, `mode 3264x2448`, controls, `on`. Over CCI0 each write is one queue
  load (address + value ≤ 11 bytes); ~320 writes for the rear, ~880 for the front (a software I2C at 100 kHz: ~0.4 s, fine). Off:
  the `off` table, then L2's power-down.
- **The check.** First the rear's **test pattern** (0x0600 = 2, colour bars): one frame dumped (its first 64 KiB and per-band
  statistics logged: the mean of each Bayer channel in eight vertical bands, which for colour bars is a known step pattern), then the
  pattern off and a scene (the means move with the room light; `piano-camera-frames=N` frames counted, CSID error bits zero). The
  frame itself over the USB console (`pianousb` file pull) or into the RAM image's dump area for the Mac.
- **Order** (Linux's): power (L2's) → clocks and votes → TFE-lite set-up → CSID reset → CSIPHY reset; start: write master (two
  addresses) → CSID RX, RDI, register updates, start → CSIPHY lanes and table → sensor tables and stream on last. Stop: write master
  off → CSIPHY off → sensor off → power down.

Effort: 2–3 weeks offline (the helper and PianoVideo's change, the three blocks' plans with host tests against models, the tables'
second file), 3–6 boots.

### 4.4 L4: the software ISP (designed)

Where: in user space (the camera extension of L5, or first a test tool `tooling/native/piano-camera-dump` run over the USB root shell),
never in the kernel. Input: one RAW10 MIPI-packed frame from the ring with its exposure, gain and Bayer order. Steps, per output pixel
of a **half-resolution quad** (each 2×2 Bayer quad → one RGB pixel: 2040×1536 from the rear's 4080×3072, 1632×1224 from the front's
3264×2448): unpack 10 bits; black level 64 (Ubuntu's); white balance (grey world on mid-tones, Ubuntu's start gains R 1.8, B 1.6,
smoothed); a 3×3 colour matrix (identity first; Ubuntu's daemon carries a per-sensor matrix in its V4L2 control, MIT, reusable); gamma
(sRGB or Ubuntu's 64-entry curve); BT.601 full-range Y'CbCr; a bilinear scale to **1280×960** (default) or 1920×1440; NV12 (`420f`)
into an IOSurface-backed CVPixelBuffer. The rear is made upright as Ubuntu's daemon has it (the sensor's vflip, which also makes it BGGR); its left-right sense is checked on the first scene and, if needed, mirrored in the scale step.
AE (Ubuntu's algorithm, MIT): the mean luma of a sparse sample, target 112 lowered by highlights, an update every 3 frames: exposure
lines first (up to the frame length minus the margin), then analogue gain, then digital gain ≤ 8× in the ISP; bounded acquisition,
then filtered ±6 % steps; through the user client (L5), which writes the sensor's exposure/gain registers between frames (CCI0 queue
writes from the kext's frame-done path).

Cost: ~3.1 M quads a frame for the rear at 30 fps is ~94 M quads/s: NEON (vImage for the scale) on one P-core is marginal, so the
rear runs at 15 fps on NEON and at 30 on the A830 through a Metal compute kernel once the GPU path is up (the front's 2 M quads at
30 fps fit NEON). A first version can drop to 1/4-resolution quads (1020×768) to stay simple.

### 4.5 L5: a macOS camera device (designed; C5 first)

Research of 2026-11-12 (public documentation and developer reports, sources at the end of this section; no Apple binary read):

- **The only supported system-wide path on macOS 12.3 through 27 is a CoreMediaIO camera extension** (`CMIOExtensionProvider`,
  `Device`, `Stream`; `CMIOExtensionProvider.startService`), a system extension inside an app in /Applications
  (`Contents/Library/SystemExtensions/<id>.systemextension`, file name = bundle id), activated with `OSSystemExtensionRequest` and
  approved by an administrator (Settings › General › Login Items & Extensions › Camera Extensions on 15+). The 26.5 and 27.2 SDKs'
  CoreMediaIO headers are identical (documented). **DAL plug-ins are dead** (deprecated in 12.3, disabled from 14.1; no app loads
  third-party ones). Apple's built-in camera stack (the ISP firmware kexts and their daemon) has no public interface.
- **Packaging.** Host `/Applications/PianoCamera.app` (background, `LSUIElement`) with `com.apple.developer.system-extension.install`
  and an app group; the extension: Info.plist `CMIOExtension` → `CMIOExtensionMachServiceName` = `<app group>.<suffix>` (activation
  fails otherwise, reported), `NSSystemExtensionUsageDescription`, entitlements `com.apple.security.app-sandbox` (mandatory), the same
  app group, and `com.apple.security.temporary-exception.iokit-user-client-class` = `PianoCameraUserClient` to open our kext's user
  client from the sandbox (reported fix for `deny iokit-open-user-client`). It runs as `_cmiodalassistants` (uid 262) under a camera
  sandbox: the user client must not require an administrator (it checks an entitlement of ours with `IOTaskHasEntitlement` instead).
- **Signing on our boot.** Our images stage ad-hoc signed code into the Data seed with its CDHashes in the static trust cache
  (`stage-soft-metal.py`), and the ADT sets `amfi-only-platform-code = 1` (`configs/k27-adt.toml`): trust-cached code counts as
  platform code, so a restricted entitlement needs no provisioning profile (*inferred*). sysextd accepts an empty team id (its
  `systemextensionsctl` usage says so, documented); `systemextensionsctl developer on` (skips the /Applications and version checks)
  needs SIP off, which our ADT's `lp-sip0`/`lp-sip1` bits set. Unknowns: whether sysextd activates an ad-hoc **camera** extension on
  27 (reported to work self-signed on 26.2 with SIP partly off, and ad-hoc for a DriverKit extension on 27); every rebuild is a new
  CDHash, so a new trust cache and a reboot: keep the extension thin and stable. A free personal team cannot get the capability
  (reported); a paid account is the fallback route (Apple Development certificate + profile, still trust-cached).
- **Kext ↔ extension.** One IOService per sensor (property `location` = front/back) with a user client: open/close (open powers the
  sensor up through L2's sequence and starts L3's stream; close stops and powers down: nothing stays on), formats, memory type 0 = the
  ring (N RAW slots, each with a sequence number, the start-of-frame `mach_absolute_time`, exposure, gain, Bayer order, stride),
  mapped with `IOConnectMapMemory64`; frame-ready as an async notification (no polling); AE controls (exposure, gain, frame length);
  the torch through PianoTorch's `PianoTorchSet`.
- **The device.** Names "Front Camera" / "Rear Camera", model "Xiaomi Pad 8 Pro", transport built-in, fixed device UUIDs; formats
  `420v`/`420f` and BGRA (QuickTime wanted BGRA listed) at 1280×960 and 1920×1440 (4:3, the sensors' aspect; 1280×720 cropped for
  apps that ask), frame durations 1/30..1/15 s, host-time clock; samples sent with `send(_:discontinuity:hostTimeInNanoseconds:)`
  from an IOSurface-backed pool. There is no position property: Front/Back come from the names only (`AVCaptureDevice.position` stays
  unspecified).
- **C5, before any camera code: a test-pattern extension** (no kernel): the trust-cached host app runs with the install
  entitlement; `systemextensionsctl developer on` succeeds; sysextd's log accepts the empty-team extension and its category; the
  approval appears and `systemextensionsctl list` shows it activated; the job `CMIOExtension.<id>` runs in `user/262` without sandbox
  denials; a dummy user client opens with the exception entitlement and is refused without it; Photo Booth, QuickTime, FaceTime,
  Safari/Chrome `getUserMedia` see it (420v vs BGRA); stop and idle exit; upgrade and `systemextensionsctl uninstall`.
- **Fallbacks** if sysextd refuses: a self-signed identity (still trust-cached); a paid account's profile; a virtual UVC device through
  `IOUSBHostControllerInterface` from a root daemon so Apple's own UVC extension carries it (a managed entitlement, bulk only, shown
  as an external USB camera); VideoDriverKit (a DriverKit beta with Apple's host) bridged to the kext; nothing else is supported.
- Sources: Apple, "Creating a camera extension with Core Media I/O" (developer.apple.com/documentation/coremediaio) and WWDC22
  session 10022; Apple, "Debugging and testing system extensions" (DriverKit documentation); Apple support 108387 (DAL plug-ins off
  from 14.1); OBS Studio's `plugins/mac-virtualcam` (its camera extension's entitlements and Info.plist); Apple developer forums
  threads 706184, 716181, 725481, 795835, 823200; the SDKs' CoreMediaIO headers (26.5 and 27.2). Reports, not tested here.

## 5. Dependencies

L1/L2: PianoA830 (its DRV-2 and GCC lock words), PianoPower's `SpmiObserver.h`, PianoKeyboard's `CmdDb.h`. L3: a shared
multimedia-vote helper (MXC, MMCX, the MM BCMs; with PianoVideo and the display), an SMMU route for 0x1c00 (PianoDwc3's bypass or
PianoVideo's bank), the CSIPHY table as data. L4: the A830 Metal path for 30 fps on the rear (optional). L5: C5 (the extension
installs on our boot), PianoTorch (the photo flash, B12, built).

## 6. Boot args

| Argument | Default | Meaning |
|---|---|---|
| `piano-camera` | 0 | 1 survey (read only); 2 survey + power and chip ID of each sensor; 3..5 reserved (run as 2 until built) |
| `piano-camera-sensor` | 3 | 1 rear, 2 front, 3 both (rear first) |
| `piano-camera-rear-bus` | 0 | 1: the rear through software I2C on gpio115/116 instead of CCI0 (no CAMCC write at all) |
| `piano-camera-mclk24` | 0 | 1: the rear's MCLK at 24 MHz from the MCLK PLL (else 19.2 MHz from the XO) |
| `piano-camera-cci-khz` | 400 | 100 or 400 (CCI0 clocked from the XO) |
| `piano-camera-i2c-half-us` | 5 | the software I2C's half bit, 2..50 µs |
| `piano-camera-pull` | 0 | 1: the pads' pull-ups on the software I2C lines |
| `piano-camera-blind` | 0 | 1: vote a rail whose PMIC state could not be read |
| `piano-camera-hold` | 0 | keep the sensor powered this many ms after its ID (0..10000) |
| `piano-camera-stop` | 0 | power down after this step (201..230) |
| `piano-camera-cycles` | 1 | repeat level 2 (1..20) |
| `piano-camera-delay` | 0 | wait first (s) |
| `piano-camera-cc`, `-mclkcc`, `-blocks`, `-qup`, `-spmi`, `-smmu` | 1 | 0 leaves that part of the survey out (`-cc=0` also refuses the rear on CCI0) |

## 7. Kernel collection

`package/PianoCamera` + `tooling/k27/build-native-piano-camera.sh`: the tables' tool self-test, `data/camera` against its MANIFEST,
the tables' header into `out/native-kexts/PianoCamera/gen`, the host tests (`tests/camera_host.cpp`: a board model whose CAMCC,
MCLK controller, CCI0 and CAMSS blocks stall a load or store without their AHB/GDSC/clock; branches, RCGs, TITAN_TOP, the MCLK PLL;
TLMM with open-drain lines pulled up only by the sensor's vddio; PMIC LDOs and VRM votes; CCI0 executing its queues; bit-level
sensors that answer only when powered, clocked and out of reset long enough; checks: the survey stores nothing and respects every
gate; each sensor and bus finds its ID with the drivers' order and waits; every register, pin and rail enable is as found after every
power-down; failures at each step (missing sensor, wrong ID, a rail at another voltage, a failed vote, TITAN_TOP stuck, a stuck
bus, a PLL that never locks, a stop step), refusals that store nothing, the allowlist, CCI packing and NACK recovery, the tables'
header against MANIFEST), a store check of the survey headers, a check that CameraPower.h stores only through its allowlisted
`put()`, the objects, the import check. `PianoCamera` is in `configs/k27-native-kexts.txt` after PianoA830.

## 8. Hardware test plan (RAM boots)

### 8.1 The images

`tooling/k27/build-camera-test-images.sh` (the GPU profile of the other test images: PianoA830, the Turnip stage, `vmm-present 0`,
the USB console, `piano-reset-after` as the backstop, the Gunyah watchdog behind it; `CAM_PROFILE=plain` for a KC built without the
GPU path):

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh   # the KC with PianoCamera
tooling/k27/build-camera-test-images.sh                              # out/ram-cam-<name>/ for c1 c1x c2f c2r c2rg c2r24
python3 tooling/native/usb-console.py --capture 1000 &               # then each image: tooling/native/native-loop.sh out/ram-cam-c1/boot-piano-macos-ram-A-bundled.img
grep -a "PianoCamera" logs/usb-console/<newest>-port0.txt
```

| Image | Arguments (besides the profile's) | Unattended | What it settles |
|---|---|---|---|
| `c1` | `piano-camera=1 piano-camera-delay=20` | **yes** | the survey: CAMCC/MCLK readable at the boot rails, the rails' PPIDs, the pins, the SMMU |
| `c1x` | `piano-camera=1 piano-camera-cc=0 piano-camera-mclkcc=0 piano-camera-delay=20` | **yes** | only if `c1` hangs after its `gcc:` line: the rest of the survey without the clock controllers |
| `c2f` | `piano-camera=2 piano-camera-sensor=2 piano-camera-delay=20` | **yes**, after `c1` passed | the front: rails, software I2C, 0x5632; no CAMCC write |
| `c2r` | `piano-camera=2 piano-camera-sensor=1 piano-camera-delay=20` | **yes**, after `c1` passed | the rear over CCI0 (TITAN_TOP, XO clocks), 0x38e1 |
| `c2rg` | `piano-camera=2 piano-camera-sensor=1 piano-camera-rear-bus=1 piano-camera-delay=20` | **yes** | the rear over software I2C (separates CCI0 from the sensor when `c2r` fails) |
| `c2r24` | `piano-camera=2 piano-camera-sensor=1 piano-camera-mclk24=1 piano-camera-delay=20` | **yes** | the rear with Linux's 24 MHz MCLK (the MCLK PLL) |
| (later) | `piano-camera=2 piano-camera-cycles=20` | yes | 20 power cycles of both, no leak |

Each runs `piano-reset-after=300`. The order is `c1` → (`c1x`) → `c2f` → `c2r` → `c2rg`/`c2r24` as needed.

**Why level 2 is safe unattended** (and what could go wrong). What it changes: five LDO votes that, in the boot DT, supply nothing
but the cameras (L6B/L7B also the disabled VCM), at the DT's exact voltages, each read back (any other voltage: off at once); ten
TLMM pins that only the cameras use (none reserved by the secure world); GCC's three camera branches, only if found off; CAMCC's
TITAN_TOP and the CCI's clocks from the XO (rear on CCI0 only); one MCLK branch; CCI0. It writes no sensor register, votes no shared
rail (MMCX, MXC, CX, L3G are not touched), starts no PLL by default, touches no partition and not the ADSP. The sensors are powered
for well under a second, in Linux's order, and everything is taken back. What could go wrong: (1) a load or store that stalls the
bus (a clock controller whose rail is down): `c1` makes the same loads first and is a prerequisite; a stall hangs the boot until
`piano-reset-after`/the Gunyah watchdog (32 s after the last pet) resets the tablet, a RAM boot leaves nothing behind; (2) a rail
that does not come up or comes up at another voltage: refused or powered down by the read-back, logged; (3) the rear not answering at
19.2 MHz: `idRead`, powered down; `c2r24` answers the question; (4) a sensor or bus wiring other than the DT says: no ACK, powered
down; (5) a power-down step that fails: `power down INCOMPLETE` with its warning count; every RPMh vote of ours is dropped by the next
reset at the latest. None of these can damage the sensors (the drivers' own voltages and order) or anything else (nothing shared is
written). The torch, the display and the battery are not involved.

### 8.2 Expected lines

**c1** (`..` = values to record):
```
PianoCamera: start: piano-camera=1 sensor=3 rear-bus=cci mclk24=0 cci=400 kHz ... delay=20 s; survey cc=1 mclkcc=1 blocks=1 qup=1 spmi=1 smmu=1
PianoCamera: level 1: loads only, nothing written (the SPMI observer's read command is the only store)
PianoCamera: stage 1: the survey (loads only; the SPMI observer for the PMIC)
PianoCamera: spmi: arbiter 0x70020000, .. APIDs
PianoCamera: gcc: camera ahb 00000001 (on) xo .. (on) mclk-cc ahb .. (on) hf-axi .. sf-axi ..; qmip .. .. ..
PianoCamera: gcc: QUP2 vote .. (s1 .); m-ahb .. s-ahb .. core .. core2x .. s1 ..; s1 rcg ..: SE1 not clocked|clocked
PianoCamera: camcc gdsc: titan-top ../.. (off), tfe0 ../.. (off), ...
PianoCamera: camcc branches: drv-ahb .. drv-xo .. gdsc-clk .. sleep .. top-ahb 80000000 ... cci0 80000000 ...
PianoCamera: camcc rcgs: slow-ahb 80000000/00000000 src 0 root-off, ... cci0 ../.. src 0 ...
PianoCamera: camcc plls: pll0 mode .. op 0 L 0 (off), ...
PianoCamera: mclk-cc: pll0 mode .. op 0 L .. (off); sleep branch ..
PianoCamera: mclk1: rcg 80000000/00000000 ... (source 0, root off, 19200000 Hz from the XO) branch 80000000 (off) <- rear (24 MHz in Linux)
PianoCamera: camss csid0: not read (TITAN_TOP GDSC off)      (likewise csid1 .. tfe-lite1)
PianoCamera: cci0: not read (TITAN_TOP GDSC off)
PianoCamera: pin cci-sda gpio115: cfg .. (function ., pull down, . mA, input) io .. (in . out .)   (and the nine others)
PianoCamera: rail rear vddd vreg_l1i_1p1 (ldoi1): PPID 0x8c1|0x840 ... type 04 ... 1096 mV ... en 00 (disabled); cmd-db at 0x.....
PianoCamera: rail csiphy 1p2 vreg_l3g_1p2 (ldog3): ... en 80 (ENABLED); ... read only, never voted
PianoCamera: smmu: scr0 .. idr0 .. idr1 ..: .. groups (.. free), .. banks; 0 group(s) cover streams 0x1c00-0x1c1f
PianoCamera: summary: gcc cam-ahb on mclk-ahb on; camcc read titan off; mclk-cc read mclk1 off mclk4 off; cci0 gated; rails 8/8 read, 3 on; smmu 0 group(s)
PianoCamera: level 2 verdict, rear: ready (its rails, pins and MCLK are as level 2 expects)
PianoCamera: level 2 verdict, front: ready (...)
PianoCamera: stage 2: done (level 1)
PianoCamera: done: N MMIO loads, 0 stores
```
**c2f**:
```
PianoCamera: stage 3: level 2: power each sensor, read its chip ID, power it down
PianoCamera: cycle 1: front OV32D40
PianoCamera: step 201: GCC: MCLK controller AHB (on where off)  ...  step 202, step 203
PianoCamera: step 205: the bus: software I2C pins (GPIO, released)
PianoCamera: write 0xf104004 = 00000000 (I2C SDA: GPIO, open drain (released))  ...
PianoCamera: step 210: pins: reset low (asserted), supply enable low, the MCLK pin's function
PianoCamera: step 213: vddio: L10B 1800 mV, dvdd: L2G 1200 mV (RPMh), read back; avdd: gpio176 high
PianoCamera: rsc: ldob10 (0x.....): mode 7, mV 1800, enable 1 through TCS 2 completed after 0 ms
PianoCamera: rail front vddio (ldob10): status 80 1800 mV mode 07 en 80
PianoCamera: step 214: MCLK4 19.2 MHz (XO)    step 215: reset released    step 230: chip ID at 0x300a (expect 0x5632)
PianoCamera: step 240: power down (reverse order, only what this level changed)
PianoCamera: front OV32D40: chip ID 0x5632 (expected 0x5632): OK over software I2C gpio4/5 after 1 try; MCLK 19200000 Hz
PianoCamera: front rail front vddio (ldob10): found .. mV en 00; on 1800 mV en 80 status 80; off en 00
PianoCamera: front: N writes logged, M software I2C line changes, 0 refused, 0 off-side warnings; power down complete (everything as found)
PianoCamera: done: rear 0/0, front 1/1 (chip ID and a clean power-down); ...
```
**c2r**: the same shape with `step 205: the bus: TITAN_TOP, CCI0's clocks from the XO, its pins, the controller`, the TITAN_TOP and
CCI writes, `rear: CCI0 HW_VERSION ........`, three rails, `rear S5KJN1: chip ID 0x38e1 (expected 0x38e1): OK over CCI0 master 1`.

Failure lines: `REFUSED: <why>; nothing written for it` (report the survey); `FAILED at step 205: TITAN_TOP did not power up` (its
GDSCR/CFG in the `last value`); `FAILED at step 213: a rail read back at another voltage` (the rail line shows it; the rail is off
again); `FAILED at step 230: the chip ID read failed: no ack` (rear: try `c2r24`, then `c2rg`; front: `piano-camera-pull=1`, then a
longer `piano-camera-i2c-half-us`); `unexpected chip ID` (another part: report the ID); `power down INCOMPLETE` (report the warnings;
the next boot clears the votes). A hang after a `step` line: report it (its number is in `piano_camera_stage`).

## 9. Fallback

Without `piano-camera` nothing is mapped. A refusal writes nothing for that sensor. Any failure runs the full power-down. A
system sleep during a sequence waits for its power-down (the kext joins the power tree at level 2).

## 10. Open questions

1. CAMCC and the MCLK controller readable at the boot rails (MMCX on for the display; MXC unknown)? → `c1`.
2. The rails' PPIDs (both page rules), their state at XNU time, the pins' states → `c1`.
3. Does the rear answer at a 19.2 MHz MCLK? → `c2r` / `c2r24`.
4. The front bus's pull-ups (the sensor's vddio, or always-on) → `c2f` (else `piano-camera-pull=1`).
5. Stream 0x1c00's SMMU entry at XNU time; bypass allowed? → `c1`, then L3.
6. The shared multimedia votes (MXC, MMCX, MM BCMs) between camera, video and display → L3 step 0.
7. Frame mode's line padding for 4080-pixel lines → L3's first dump.
8. Does sysextd activate an ad-hoc, trust-cached camera extension on 27 (C5)? → before L5.
9. QUP2 SE1's GENI firmware (protocol 3?) for a hardware I2C later → `c1`'s `qup2 se1` line (only when clocked).

## 11. Licence and the tables (owner decision, 2026-11-12)

The camss, sensor, CCI, clock, pinctrl drivers and the piano DT (GPL-2.0) are the specification only (addresses, fields, sequences,
timings); no code, comment or structure is copied; PianoCamera is BSD-2-Clause-Patent (`package/PianoCamera/UPSTREAM`). **The
sensors' register tables** exist only in those drivers. Owner's decision (2026-11-12): they are hardware data and are kept in this
repository as data: `data/camera/s5kjn1.tbl` and `ov32d40.tbl`, extracted by our own tool (`tooling/camera/sensor-tables.py`)
from the pinned driver files into our own format, pinned by `data/camera/MANIFEST` (file SHA-256 and every section's write count),
with the provenance (project, driver, commit, file hash) in `data/camera/README.md`; the build checks them and generates the C header
the kext reads. The CSIPHY lane table L3 needs is a PHY tuning table, not a sensor's; the owner's second decision of 2026-11-12 (PHY
tuning tables from the Linux drivers as data in their own directory, with a provenance note, nothing rewritten for licence reasons)
put it in `data/camera/csiphy-v2.4.tbl`, extracted by the same tool from `camss-csiphy-3ph-1-0.c` at the same pin. Per-unit data (this tablet's lens shading or OTP, if any is ever
read) is not committed. Ubuntu's TFE pixel-path programming, itself derived from Qualcomm's CamX settings code, is not used. The
camera extension (L5) uses only public SDK headers (CoreMediaIO, CoreVideo, IOKit, SystemExtensions); no Apple binary is read, no
vendor blob.
