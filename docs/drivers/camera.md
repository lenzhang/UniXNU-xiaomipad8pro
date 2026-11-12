# Cameras (B11): PianoCamera, the level ladder

Status, 2026-11-12: **levels 1-3 built and host-tested** (`package/PianoCamera`, in the kext list, inert without `piano-camera`).
Level 1 **ran on the tablet** (c1, §8.4: no hang, 0 stores; GCC's camera and MCLK-controller AHB branches off and hardware-gated, so
neither clock controller was readable; no SMMU group for stream 0x1c00); levels 2 and 3 adjusted to it and **not run yet**. Levels
4-5 designed (§4.4-4.5). The sensors' register tables and the CSIPHY's lane table are in the repository as data (`data/camera`,
§11); the multimedia votes are shared with PianoVideo (`package/include/PianoMmVotes.h`, §4.3.1). Backlog item B11
(`../BACKLOG-hardware.md`). Template: `README.md`.

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
| Clock controllers | CAMCC `ade0000` (0x20000; domains MMCX + MXC at low_svs; GCC's camera AHB `0x26004`), cambistmclkcc `1760000` (0x6000; MMCX + MX; GCC `0xa0004`). Linux keeps GCC `0x26004`, `0x26034` (camera XO), `0xa0004` on from GCC's probe (CLK_ENABLE only), and CAMCC's DRV AHB/XO, GDSC clock and sleep clock (`0x113c4/0x113c0/0x1137c/0x11398`) on from CAMCC's. **At XNU (c1)**: GCC `0x26004` = 0x88000002, `0x26034` = 0x88000000, `0xa0004` = 0x88000002: all three off (CLK_ENABLE clear), both AHBs with HW_CTL (hardware gating: CLK_OFF reads set while idle even when enabled, as the display's 0x88000003); HF/SF AXI 0x8a200000 off; neither controller readable at level 1 | DT, msm; tablet | verified (source); GCC state verified (c1); CAMCC/MCLK-CC: needs-boot (L2) |
| GDSCs | TITAN_TOP `+0x1134c` (parent of all; CFG completion bits, RETAIN_FF), TFE0/1/2 `+0x11004/0x11084/0x110ec`; Linux's set-up: wait values 2/8/2, HW control and SW override off; on: un-collapse, POWER_UP_COMPLETE within 2 ms, 1 µs, RETAIN_FF | msm (`gdsc.c`) | verified (source) |
| CAMCC PLL0 | Taycan ELU, 19.2 MHz × 62.5 = 1200 MHz (L 0x3e, alpha 0x8000); even /2 = 600, odd /3 = 400 MHz: the CSIPHY/CSID/TFE-lite sources (400 MHz = main /3), CAMNOC RT AXI 200 (even /3), slow AHB 80 (even /7.5). Every CAMCC source's selector 0 is the XO (19.2 MHz), the reset state | msm | verified (source) |
| MCLKs | cambistmclkcc PLL0 Rivian ELU 960 MHz (L 0x32, CONFIG_CTL 0x12000000/0x00890263/0x1af04237); MCLK n RCG `+0x4000` + 0x1c·n, branch `+0x4018` + 0x1c·n; table 12 / 19.2 (XO) / 24 / 68.57 MHz; 24 MHz = PLL0 / 10 × 1/4 (source 3, CFG 0x2313, M 1, N reg 0xfc, D reg 0xfb). KDTS: rear MCLK1 24 MHz (gpio90 `cam_mclk`, 4 mA), front MCLK4 19.2 MHz (gpio93 `cam_aon_mclk4`, 2 mA) | msm, KDTS | verified (source) |
| CCI | only **CCI0** `ac7b000`, SPI 426, master 1 (KDTS 1 MHz Fast-mode Plus with the CCI clock at 37.5 MHz from PLL0), SDA gpio115, SCL gpio116 (`cci_i2c_*`, 4 mA, no bias); registers per master 0x100·m, queues 0x300 + 0x200·m + 0x100·n, IRQ 0xc00..0xc0c; commands SET_PARAM 1, REPORT 8, WRITE 9, READ 10; queues of 64/16 words, writes ≤ 11 bytes, reads ≤ 12; timing sets for a 19.2 MHz CCI clock (100/400 kHz) and a 37.5 MHz one (100/400/1000 kHz) | DT, msm | verified (source) |
| Rear S5KJN1 | CCI0 m1 **0x10**; CSIPHY1, 4 lanes (D-PHY), 700 MHz link (1.4 Gbit/s a lane); reset gpio6 active low; vddd L1I 1.096 V (`ldoi1`), vdda 2.8 V fixed (gpio175 enable, 100 µs), vddio L6B 1.8 V (`ldob6`), afvdd L7B 2.8 V (`ldob7`); on: vddd, 1–2 ms, vdda, vddio, afvdd, MCLK, reset release, 10–15 ms; off: reset, MCLK, afvdd, vddio, vdda, 1–2 ms, vddd; ID 0x0000 = 0x38e1; modes 4080×3072@30 (HTS 4352, VTS 4288), 8160×6144@10; RAW10, mounted rotated 180° (Ubuntu sets vflip: BGGR); controls exposure 0x0202, gain 0x0204 (32 = 1×, 32..2048), frame length 0x0340, orientation 0x0101, test pattern 0x0600 (2 = colour bars), stream 0x0100; flash = PianoTorch's lamp | DT, msm | verified (source) |
| Front OV32D40 | **not on CCI**: QUP2 SE1's pins (`i2c@884000`, SDA gpio4, SCL gpio5, `qup2_se1`, 2 mA, no bias; Ubuntu ran GENI I2C at 1 MHz) **0x10**; CSIPHY4, 4 lanes, 842.4 MHz; reset gpio7 active low; vddio L10B 1.8 V (`ldob10`), dvdd L2G 1.2 V (`ldog2`), avdd 2.8 V fixed (gpio176); on: reset asserted, supplies, 5–6 ms, MCLK, 10–12 ms, reset release, 20–25 ms; ID 0x300a = 0x5632 (five reads, 10–12 ms apart); modes 3264×2448@30, 3264×1836@30, 6528×4896@10; SBGGR10; controls exposure 0x3500 (24 bits), gain 0x3508 (256 = 1×, 288..15872), frame length 0x380e | DT, msm | verified (source) |
| Rails | the five sensor LDOs supply only the cameras in the DT (L6B/L7B also the disabled VCM); RPMh VRM (+0 mV, +4 enable, +8 mode, 7 = HPM, the DT's initial mode); PMIC letters are SPMI SIDs (b 1, f 5, g 6, i 8); an LDO n at page 0xc0 + n or 0x40 + n − 1 (decided by TYPE 0x04). CSIPHY rails L3G 1.2 V (also the UFS/DSI PHY PLL: never voted), L1F 0.88 V (CSIPHY0/1/3/5), L3I 0.88 V (CSIPHY2/4), both always-on. **At XNU (c1)**: every PPID at 0xc0 + n (L1I 0x8c1, L6B 0x1c6, L7B 0x1c7, L10B 0x1ca, L2G 0x6c2, L3G 0x6c3, L1F 0x5c1, L3I 0x8c3), owner EE 4; the five sensor LDOs disabled (L1I 944 mV, L7B 1800 mV: the votes set the DT's values); L3G 1200, L1F 880, L3I 880 mV enabled and ready; all eight cmd-db addresses found | DT, `PianoWcnRails.h`; tablet | verified (c1) |
| Pins | none of the ten is in TLMM's reserved ranges (36–39, 48–51, 74); gpio4–7 and 175/176 have the egpio bit (Linux sets EGPIO_ENABLE when it selects a function); sleep states pull down, reset/enable output low. **At XNU (c1)**: all ten GPIO inputs with pull-down at 2 mA (cfg 0x1; the egpio ones 0x801: present, not claimed by apps), none driven. eGPIO (pinctrl-msm.c's set_mux, behaviour): an eGPIO-capable pin with EGPIO_ENABLE clear is the other owner's (the `egpio` function); selecting any other function, GPIO included, sets EGPIO_ENABLE (the apps TLMM claims it), which is how Ubuntu's camera pinctrl states take gpio4-7/175/176. Levels 2-3 do the same whenever they configure such a pin and give it back with the configuration as found at the power-down (host-tested: gpio7) | DT, msm; tablet | verified (c1) |
| SMMU | `iommus = <apps_smmu 0x1c00 0x0>`: one stream for every camera write master; Linux's DMA mask 32 bits; the write master's address register holds address >> 8 (40-bit reach). **At XNU (c1)**: SCR0 0x002d0406 (USFCFG set: an unmatched stream faults), IDR0 0x4c017e7f, IDR1 0x60000053: 127 groups (122 free), 83 banks; **no group covers 0x1c00..0x1c1f** (level 3 takes its own window) | DT, msm; tablet | verified (c1) |
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
| L1 | 1 | read-only survey: CAMSS/CAMCC GDSCs and clocks, CCI0, the sensors' rails/pins/MCLKs as the boot chain left them, the SMMU streams; level 2's verdict | none (the SPMI observer's read command) | no hang, `0 stores`, the lines of §8.2 | **built**, host-tested; **passed on the tablet** (c1, §8.4) |
| L2 | 2 | one sensor at a time: its bus, rails (RPMh under the shared lock, read back over SPMI), supply enable and reset GPIOs, MCLK; the chip ID over its bus (both sensors); powered down exactly as found | allowlisted camera registers only (§4.2); CAMCC's domains through the shared votes on the CAMCC path | 0x38e1 and 0x5632, `power down complete` for both | **built**, host-tested; adjusted after c1 (§4.2) |
| L3 | 3 | RAW10 frames from one sensor through CSIPHY → CSID-lite0 → RDI0 → TFE-lite0's write master 0 into memory, no ISP; a bounded number of frames, one frame's statistics (the rear's colour bars by default, or a scene) | + CAMCC PLL0 and the stream's clocks, MM1 (shared helper), GCC's camera HF AXI, our SMMU window for stream 0x1c00, CSID-lite0, TFE-lite0's write master 0, the CSIPHY's lane table, the sensor's tables | the bars' known band pattern; a scene's per-channel means move with the light; the guard intact; `power down complete` | **built**, host-tested (§4.3) |
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
CSIPHY LDOs enabled, the pins in their sleep states, no SMMU group for 0x1c00. **Found (c1, §8.4):** all of it but the first two:
GCC's three branches are off (the AHBs hardware-gated), so CAMCC and the MCLK controller stay unread at level 1 (a load needs their
AHB enabled: a store, which is level 2's step 201).

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

**After c1 (2026-11-12).** The survey found GCC's camera AHB, camera XO and MCLK-controller AHB off (0x88000002, 0x88000000,
0x88000002; both AHBs hardware-gated), so neither clock controller had ever been read under XNU. Two changes follow, host-tested
against that state (`testTabletC1`): (1) step 201 counts a **hardware-gated GCC branch as on once enabled** (Linux's rule:
`clk-branch.c` skips the halt check in hardware gating mode; such a branch reads CLK_OFF set while idle, as the display's 0x88000003
and the UFS AHB do), in the survey's gates too (`gccClockOn`); before this, step 201 waited for CLK_OFF to clear and would have
failed on the tablet (the c2 images built before this commit stop there, harmlessly: their power-down turns the branches off
again); (2) on the CAMCC path (the rear on CCI0, or level 3) step 201 first **votes CAMCC's power domains** through the shared
helper (§4.3.1: MXC low_svs; MMCX is the boot display's, at NOM_L1: no vote of ours, the 0x140 floor declared), as Linux has them
up before it touches CAMCC; the power-down withdraws them last. `piano-camera-votes=0` leaves them out (the strict model then
stalls on CAMCC: the reason for the change). The L1 verdict reads `pending: ... step 201 turns them on` instead of a refusal.

**Deviations from Linux, on purpose.** CCI0 and the rear's MCLK run from the XO at L2 (no CAMCC PLL); 400 kHz instead of 1 MHz. The
rear sensor's EXTCLK range is not in any source: Linux refuses anything but 24 MHz because its mode's PLL words assume it. Reading
the ID at 19.2 MHz is *inferred* to work (sensors of this class take 6–27 MHz); a rear `idRead` failure with the front passing is
retried with `piano-camera-mclk24=1` (level 3 uses 24 MHz by default). The front's GENI engine is not used: the software I2C needs
no QUP clocks or GENI firmware (L1 reports SE1's protocol for later).

**Allowlist** (`Session::writable`; a store elsewhere is refused and fails the step): GCC `0x26004/0x26034/0xa0004`; CAMCC TITAN_TOP's
GDSCR, the four critical branches, the top AHB and CCI0 branches, the slow AHB source's CMD/CFG, CCI0's source CMD/CFG/M/N/D; the
MCLK controller's MCLK1/MCLK4 sources and branches and PLL0's registers; the ten camera pins' CFG/IO; CCI0's registers. Never: any
QUP vote word, any other GDSC, the CSIPHY/CSID/TFE blocks, the SMMU, any other pin.

### 4.3 L3: RAW frames into memory (built, host-tested; not run)

`CameraStream.h` (`Stream`, over level 2's ops) for one sensor: `piano-camera=3` (the rear by default, `piano-camera-sensor=2` the
front). Specification: `camss.c` `*_res_8750`, `camss-csiphy-3ph-1-0.c`, `camss-csid-980.c`, `camss-vfe-gen4.c`, `camcc-sm8750.c`,
`clk-alpha-pll.c`, the interconnect's `sm8750.c` (facts only; `CameraHw.h` "level 3" has each with its source). The path: rear
CSIPHY1 → CSID-lite0 → TFE-lite0's RDI0 (write master 0); front CSIPHY4 → the same (one sensor at a time). TFE-lite has no GDSC of
its own (TITAN_TOP only) and fewer clocks than a full TFE (whose RDI0 is write master 23); a full TFE is the fallback.

| Step | What (bounds in brackets) |
|---|---|
| 300 | level 2's power-up and chip ID (§4.2) with TITAN_TOP and the camera AHB for either bus (`camss`), CAMCC's domains voted at 201, the rear's MCLK at Linux's 24 MHz (`piano-camera-mclk24` defaults to 1 here); **refused** when TITAN_TOP was already on (someone else's CAMSS) |
| 301 | BCM MM1 on (the camera NoC's `qnm_camnoc_hf`), through the shared votes (§4.3.1) |
| 302 | GCC's camera HF AXI branch: CLK_ENABLE where clear (no CLK_OFF wait: Linux's halt check is skipped for it) |
| 303 | CAMCC PLL0 (Taycan ELU): when not running, Linux's configuration (L 0x3e with CAL_L 0x44, alpha 0x8000: 1200 MHz; CONFIG_CTL 0x19660387/0x098060a0/0xb416cb20; USER_CTL 0x8400: even /2, odd /3; USER_CTL_U 2; OUTCTRL off, standby, RESET_N); then RESET_N, RUN, LOCK_DET [1.5 ms], the three outputs, OUTCTRL. A PLL0 found running must be at L 0x3e (else refused); one in FSM vote mode is refused |
| 304 | the sources, each switched with its root forced on [0.5 ms each wait]: `cphy_rx`, the PHY's timer, `ife_lite`, `ife_lite_csid` at 400 MHz (PLL0 main / 3), `camnoc_rt_axi` at 200 MHz (PLL0 even / 3); the AHBs stay on the XO (Linux gives them no rate). Then eleven branches: top fast AHB, the CSIPHY and its timer, CAMNOC RT/NRT AXI, CAMNOC RT IFE-lite, IFE-lite AHB, IFE-lite, IFE-lite CSID, IFE-lite CPHY RX, QDSS debug XO [CLK_OFF clear within 500 µs; NRT AXI is voted/hardware-gated: not waited on] |
| 305 | CSID-lite0's and TFE-lite0's HW_VERSION (logged) |
| 306 | the SMMU route of stream 0x1c00 (`CameraSmmu.h` planRoute, loads only): **bypass** when one group covers it and bypasses (S2CR type 1, or a bank with translation off and VA64: PianoDwc3's rule); **our window** when no group covers it (c1's case) or its own exact group faults: a free group and a free bank (`CameraSmmuWindow.h`, PianoWifi's IOVA window: stage 1, a 32-bit input space, the buffer's 2 MiB blocks at IOVA 0x20000000, nothing else mapped); **refused** for several groups, a masked group that does not bypass, a bank with translation on (someone's tables), no free group or bank |
| 307 | the frame buffer: one physically contiguous range, 2 MiB aligned, below 1 TiB (the write master's reach), of the mode's 16-byte-aligned stride × height plus a guard of at least 256 KiB, rounded to 2 MiB (16 MiB for the rear's 4080×3072, 8 MiB for the front's 3264×1836); the frame zeroed, the guard filled with 0xa5 (a canary), cleaned to the PoC; for the window: its two table pages built and checked by a software walk (first, last, one past), cleaned, the bank programmed whole before the one store that points the stream at it |
| 308 | CSID-lite0 reset: TOP mask/clear RST_DONE + BUF_DONE, BUF_DONE mask/clear RDI0, IRQ_CMD, RST_CFG 0x11, RST_CMD 0x5 [RST_DONE within 500 ms] |
| 309 | TFE-lite0's bus: CGC override 0x7ffffff, test bus off; write master 0: FRAME_INCR stride × height >> 8, IMAGE_CFG_0/2 0xffff, packer 0, IRQ subsample 0/1, MMU prefetch 1/0xffffffff, CFG = enable + frame mode, IMAGE_ADDR = the bus address >> 8 (Linux's order: the VFE before the CSID) |
| 310 | CSID-lite0: RX_CFG0 = 3 \| 0x3210 << 4 \| (PHY + 1) << 20 (0x00232103 rear, 0x00532103 front), RX_CFG1 = ECC correction; RDI0 CFG0 = RAW10 (0x2b), payload only (0xf), VC 0, timestamps, then enable; CFG1 = MIPI packing; RX interrupt mask all, RDI0 mask SOF/EOF/RUP_DONE/errors (polled: no interrupt is attached); the register updates as the driver's loop (AUP, RUP, twice: the address latched); RDI0 start |
| 311 | the CSIPHY: reset (CTRL0 = 1, 8 ms, 0), version (CTRL6 = SHOW_REV_ID, STATUS12..15), CTRL5 = 0xd5 (clock + data lanes 0-3), CTRL6 = 1, CTRL7 = 2, CTRL0 = 0; the 103-entry lane table from `data/camera/csiphy-v2.4.tbl` (87 written, five with 209 µs after them, the five settle entries take **29** = (85 ns + 6 × UI/2) / 2.5 ns − 6 for both sensors' link rates at a 400 MHz timer; the 16 skew-calibration entries not written, as the driver); CTRL11..21 = 0 (interrupts masked) |
| 312 | the sensor: its `start`, `init` and mode tables (rear `4080x3072`: 315 writes over CCI0, the CCI's own register stores counted, not logged; front `3264x1836`: 866 over the software I2C), the rear's controls (orientation 0 (native), exposure 0x0202 = the mode's 3840 lines, analogue gain 0x0204 = `piano-camera-gain` (64: 2×), frame length 0x0340 = 4288, test pattern 0x0600 = 2 colour bars unless `piano-camera-pattern=0`), then `on` (stream on, last) |
| 320 | the stream: CSID-lite0's TOP/BUF_DONE/RX/RDI0 status polled every 100 µs (cleared when set), SOF/EOF/RUP_DONE/error bits counted; each BUF_DONE re-arms the write master with the same buffer (IMAGE_ADDR, AUP); `piano-camera-frames` (2) frames [the first within 1 s, each next within 200 ms, all within `piano-camera-timeout-ms` (3000)]; the CSIPHY's status words 0-10 read |
| 330 | stop: write master 0 off first (read back: nothing more reaches memory), RDI0 stopped and disabled, the PHY's CTRL5/CTRL6 = 0, the sensor's `off` table; late BUF_DONEs counted for 100 ms |
| 331 | the statistics (the buffer invalidated from the caches first): per Bayer position (row/column parity) min/mean/max and an 8-bin histogram (128 a bin), the eight vertical bands' means per position (the colour bars' step pattern), the written extent and the stride it implies (packed 5100 bytes or padded 5104 for the rear: the open question 7, settled by the first frame), the guard (intact, or the first changed byte), the first 32 bytes |
| 340 | down in reverse, only what level 3 changed: the branches (reverse), the sources back to what they were (before PLL0 stops), PLL0 (OUTCTRL off, outputs off, standby; its configuration stays, as Linux's probe leaves it), GCC's HF AXI, MM1; the window released (the stream made to fault first, the group's SMR/S2CR, the bank's SCTLR, every other word it wrote, CBAR/CBA2R as found, our faults cleared after they are logged, the TLBs invalidated); level 2's power-down (sensor, bus, TITAN_TOP collapsed: the CSID/TFE/CSIPHY registers with it, MCLK, rails, pins, GCC, CAMCC's domains); the buffer freed only when the write master read back stopped and its IFE-lite clock stopped and the window is released (else kept and said so) |

**Allowlist** (`Stream::writable`; level 2's own for the sensor, its bus and power): GCC's camera HF AXI; CAMCC PLL0 (+0..+0x30),
the five sources' CMD/CFG (the sensor's PHY timer only), the eleven branches (the sensor's CSIPHY and timer only); the sensor's
CSIPHY block (0x2000); CSID-lite0 (0xa00); TFE-lite0's CGC override and test bus and write master 0 (0x100); the SMMU words of the
route's own group (SMR, S2CR), bank (its page, CBAR, CBA2R) and the global TLB invalidate and sync, only for our window. Never: the
other CSIPHY, CSID or TFE, any other write master, any GDSC but level 2's TITAN_TOP, any other SMMU word, a bypass route's group.
The build greps CameraStream.h for stores outside `put()`/`putQuiet()` and the SMMU headers for any store but their Write.

**Memory safety.** On c1's tablet the route is our window: the write master can reach only the buffer's 16 MiB (anything else
faults in the SMMU, logged from the bank's FSR/FAR before the release), the guard catches an overflow inside it, and the buffer is
not freed while the write master could still run. With a boot-chain bypass (not the tablet's case) the write master sees physical
memory: frame mode writes what the sensor sends, so only the mode's table, one latched address per frame and the write master
stopped first bound it; the guard reports an overflow after the fact.

**Not done here, on purpose:** MM0, SH0/MC0/ACV (DDR) and CN0 are not voted (the display's MM0 from the boot and PianoA830's DDR at
its host level hold them above a bounded RAW stream's 470 MB/s; Linux's hf_mnoc vote is 472/926 MB/s), the CSID's IRQ line is not
attached (polled), the front has no test pattern (its driver's 0x4308 is unverified), no exposure control for the front (its
tables' defaults), the frame is analysed in the kernel and not exported (L4's user client carries frames). `piano-camera-stop=3xx`
stops after any step with the full power-down.

#### 4.3.1 The shared multimedia votes (`package/include/PianoMmVotes.h`)

MXC, MMCX and BCM MM1 are voted from apps DRV-2 by PianoVideo (V1-V3) and PianoCamera (levels 2-3); one RPMh vote per resource per
DRV replaces that DRV's earlier one, so without coordination one driver's release (PianoVideo's MXC 0 and MM1 off at its
power-off) drops the other's need. The helper (a C header, like `PianoWcnRails.h`; words in PianoA830: `piano_mm_votes_owner` and
`piano_mm_votes[32]`) keeps per client (video, camera; display and GPU reserved) the need of each resource (a rail's vlvl; MM1's
BCM word), the highest MMCX floor any client declared (the boot display's NOM_L1 0x140: PianoVideo's `piano-video-mmcx-floor`,
PianoCamera's `piano-camera-mmcx-floor`), and what DRV-2 was last told. An update (under its own lock, 1 ms steps, at most 5 s;
then the DRV-2 lock inside the client's send) replaces the client's needs for the resources it names and sends, in one
transaction, each resource whose vote changed: a rail's first hlvl reaching max(floor, every need); MM1 valid while any client needs
it (x summed, y the maximum), commit only when none; a resource nobody of ours ever needed is not voted only to say "nothing". A
failed transaction leaves the words as they were. With one client the commands are exactly that client's old ones (host-tested
against PianoVideo's previous rules over every sequence of up to six operations). PianoA830 votes CX, MX, DDR and CN0 itself, none
of these three, and only defines the words. Host tests: `package/include/tests/mm_votes_host.cpp` (one and two clients, floors,
failures, the lock bound, a lost lock, two threads, the cmd-db reader) and PianoVideo's `testSharedVotes`.

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

L1/L2: PianoA830 (its DRV-2 and GCC lock words, the shared votes' words), PianoPower's `SpmiObserver.h`, PianoKeyboard's `CmdDb.h`,
`package/include/PianoMmVotes.h` (§4.3.1). L3 (all in place): the shared votes (MXC, MM1; with PianoVideo), an SMMU route for 0x1c00
(the boot chain's bypass or our window: PianoWifi's `WifiSmmuWindow.h`, copied), the CSIPHY table as data (`data/camera`). L4: the A830 Metal path for 30 fps on the rear (optional). L5: C5 (the extension
installs on our boot), PianoTorch (the photo flash, B12, built).

## 6. Boot args

| Argument | Default | Meaning |
|---|---|---|
| `piano-camera` | 0 | 1 survey (read only); 2 survey + power and chip ID of each sensor; 3 survey + RAW10 frames from one sensor (§4.3); 4..5 reserved (run as 3 until built) |
| `piano-camera-sensor` | 3 | 1 rear, 2 front, 3 both (rear first); level 3: 2 the front, else the rear |
| `piano-camera-rear-bus` | 0 | 1: the rear through software I2C on gpio115/116 instead of CCI0 (no CAMCC write at all) |
| `piano-camera-mclk24` | 0 (level 3: 1) | 1: the rear's MCLK at 24 MHz from the MCLK PLL (else 19.2 MHz from the XO) |
| `piano-camera-cci-khz` | 400 | 100 or 400 (CCI0 clocked from the XO) |
| `piano-camera-i2c-half-us` | 5 | the software I2C's half bit, 2..50 µs |
| `piano-camera-pull` | 0 | 1: the pads' pull-ups on the software I2C lines |
| `piano-camera-blind` | 0 | 1: vote a rail whose PMIC state could not be read |
| `piano-camera-hold` | 0 | keep the sensor powered this many ms after its ID (0..10000) |
| `piano-camera-stop` | 0 | power down after this step (201..230; level 3: 300..331, below 300 stops level 2's part) |
| `piano-camera-cycles` | 1 | repeat level 2 (1..20) |
| `piano-camera-delay` | 0 | wait first (s) |
| `piano-camera-cc`, `-mclkcc`, `-blocks`, `-qup`, `-spmi`, `-smmu` | 1 | 0 leaves that part of the survey out (`-cc=0` also refuses the rear on CCI0) |
| `piano-camera-votes` | 1 | 0: level 2 votes no CAMCC domain (§4.2 after c1); level 3 always votes |
| `piano-camera-mmcx-floor` | 0x140 | the MMCX floor this client declares to the shared votes (the boot display's NOM_L1; never lower than another client's) |
| `piano-camera-frames` | 2 | level 3: frames to collect (1..8) |
| `piano-camera-pattern` | 1 | level 3: the rear's colour bars (0x0600 = 2); 0: the scene |
| `piano-camera-timeout-ms` | 3000 | level 3: the stream's hard bound from stream-on (500..10000) |
| `piano-camera-gain` | 64 | level 3: the rear's analogue gain code (32 = 1×; 32..2048) |

## 7. Kernel collection

`package/PianoCamera` + `tooling/k27/build-native-piano-camera.sh`: the tables' tool self-test, `data/camera` against its MANIFEST,
the tables' header into `out/native-kexts/PianoCamera/gen`, the host tests (`tests/camera_host.cpp`: a board model whose CAMCC,
MCLK controller, CCI0 and CAMSS blocks stall a load or store without their AHB/GDSC/clock; branches, RCGs, TITAN_TOP, the MCLK PLL;
TLMM with open-drain lines pulled up only by the sensor's vddio; PMIC LDOs and VRM votes; CCI0 executing its queues; bit-level
sensors that answer only when powered, clocked and out of reset long enough; checks: the survey stores nothing and respects every
gate; each sensor and bus finds its ID with the drivers' order and waits; every register, pin and rail enable is as found after every
power-down; failures at each step (missing sensor, wrong ID, a rail at another voltage, a failed vote, TITAN_TOP stuck, a stuck
bus, a PLL that never locks, a stop step), refusals that store nothing, the allowlist, CCI packing and NACK recovery, the tables'
header against MANIFEST (the PHY table's 103 entries too); c1's tablet state (`testTabletC1`); level 3 against CAMCC PLL0, the
stream's clock sources, the CAMSS blocks (reset by TITAN_TOP's collapse), the SMMU with a page-table walk, memory and a write master
that writes the model's frames only when the sensor streams, the PHY's table and settle count are right, CSID-lite0 is configured
for that PHY and the write master is armed: the colour bars band by band, padded and unpadded lines, a scene, the front, a boot-chain
bypass, a refused route, failures at each step, every stop step, overflows into the guard and past the buffer, the allowlist,
everything as found after each), `package/include/tests/run.sh` (the shared votes), store checks (the survey headers store nothing;
CameraPower.h and CameraStream.h store only through their allowlisted `put()`/`putQuiet()`; the SMMU headers only through their
Write), the objects (with the generated tables' header), the import check (PianoA830's lock and vote words). `PianoCamera` is in
`configs/k27-native-kexts.txt` after PianoA830.

## 8. Hardware test plan (RAM boots)

### 8.1 The images

`tooling/k27/build-camera-test-images.sh` (the GPU profile of the other test images: PianoA830, the Turnip stage, `vmm-present 0`,
the USB console, `piano-reset-after` as the backstop, the Gunyah watchdog behind it; `CAM_PROFILE=plain` for a KC built without the
GPU path):

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh   # the KC with PianoCamera
tooling/k27/build-camera-test-images.sh                              # out/ram-cam-<name>/ for c1 c1x c2f c2r c2rg c2r24 c3rp c3r c3rs c3f
python3 tooling/native/usb-console.py --capture 1000 &               # then each image: tooling/native/native-loop.sh out/ram-cam-c1/boot-piano-macos-ram-A-bundled.img
grep -a "PianoCamera" logs/usb-console/<newest>-port0.txt
```

| Image | Arguments (besides the profile's) | Unattended | What it settles |
|---|---|---|---|
| `c1` | `piano-camera=1 piano-camera-delay=20` | **yes** | the survey: CAMCC/MCLK readable at the boot rails, the rails' PPIDs, the pins, the SMMU (**passed**, §8.4) |
| `c1x` | `piano-camera=1 piano-camera-cc=0 piano-camera-mclkcc=0 piano-camera-delay=20` | **yes** | only if `c1` hangs after its `gcc:` line: the rest of the survey without the clock controllers |
| `c1b` | `piano-camera=2 piano-camera-sensor=1 piano-camera-stop=203 piano-camera-delay=20` | **yes** (c1 passed) | c1's re-test: level 2's steps 201-203 only (CAMCC's domains voted, GCC's three camera branches enabled), the `step 202: the controllers ...` lines (CAMCC's GDSCs, branches, sources, PLLs; the MCLK controller; TITAN_TOP's state) that c1 could not read, then everything as found: no rail, pin, CAMCC or CCI store |
| `c2f` | `piano-camera=2 piano-camera-sensor=2 piano-camera-delay=20` | **yes**, after `c1` passed | the front: rails, software I2C, 0x5632; no CAMCC write |
| `c2r` | `piano-camera=2 piano-camera-sensor=1 piano-camera-delay=20` | **yes**, after `c1` passed | the rear over CCI0 (TITAN_TOP, XO clocks), 0x38e1 |
| `c2rg` | `piano-camera=2 piano-camera-sensor=1 piano-camera-rear-bus=1 piano-camera-delay=20` | **yes** | the rear over software I2C (separates CCI0 from the sensor when `c2r` fails) |
| `c2r24` | `piano-camera=2 piano-camera-sensor=1 piano-camera-mclk24=1 piano-camera-delay=20` | **yes** | the rear with Linux's 24 MHz MCLK (the MCLK PLL) |
| (later) | `piano-camera=2 piano-camera-cycles=20` | yes | 20 power cycles of both, no leak |
| `c3rp` | `piano-camera=3 piano-camera-sensor=1 piano-camera-stop=312 piano-camera-delay=20` | **yes**, after `c2r24` passed | level 3 prepared to the sensor's tables, stopped before stream-on: PLL0's lock, the clocks, the SMMU window, CSID-lite0's reset, the versions, the CSIPHY; then the full power-down |
| `c3r` | `piano-camera=3 piano-camera-sensor=1 piano-camera-delay=20` | **yes**, after `c3rp` passed | the rear's colour bars: two frames, the band pattern, the stride, the guard |
| `c3rs` | `piano-camera=3 piano-camera-sensor=1 piano-camera-pattern=0 piano-camera-frames=4 piano-camera-delay=20` | **yes**, after `c3r` | the rear's scene: four frames, the means with the room light |
| `c3f` | `piano-camera=3 piano-camera-sensor=2 piano-camera-delay=20` | **yes**, after `c2f` and `c3r` | the front's scene over CSIPHY4 |

Each runs `piano-reset-after=300`. The order is `c1` → (`c1x`) → `c1b` → `c2f` → `c2r` → `c2rg`/`c2r24` as needed → `c3rp` → `c3r` →
`c3rs` → `c3f`. **Rebuild the c2 images from this commit on**: images built before it wait at step 201 for CLK_OFF on the
hardware-gated AHB branches c1 found and stop there (harmlessly: `FAILED at step 201: a GCC branch did not run`, power down
complete).

**Why level 2 is safe unattended** (and what could go wrong). What it changes: five LDO votes that, in the boot DT, supply nothing
but the cameras (L6B/L7B also the disabled VCM), at the DT's exact voltages, each read back (any other voltage: off at once); ten
TLMM pins that only the cameras use (none reserved by the secure world); GCC's three camera branches, only if found off; CAMCC's
TITAN_TOP and the CCI's clocks from the XO (rear on CCI0 only); one MCLK branch; CCI0; on the rear's CCI0 path, MXC at low_svs through
the shared votes (raised for under a second, then withdrawn; nothing else natively votes it) and the MMCX floor declared (no MMCX
vote of its own). It writes no sensor register, sends no MMCX, CX or L3G vote, starts no PLL by default, touches no partition and
not the ADSP. The sensors are powered for well under a second, in Linux's order, and everything is taken back. What could go wrong:
(1) a load or store that stalls the bus (a clock controller whose rail is down): c1 could not read the controllers (their GCC AHB
was off), so c2's step 202 is their first load, made only after the AHB is enabled and (rear) MXC voted, as Linux has them; a stall hangs the boot until
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
**c1b** (and the start of every c2/c3 run since c1): `step 201: CAMCC's domains (MXC, MMCX: shared votes); GCC: camera AHB, camera XO,
MCLK controller AHB (on where off)`, `votes: CAMCC's domains up: ok; mxc hlvl . (vlvl 0x40, ...); 1 command(s) sent`, the three GCC
writes (`... (GCC camera AHB on)`: CLK_ENABLE only), `step 202: the controllers with GCC's branches enabled (the survey again):` with
`gcc: camera ahb 88000003 (on (hardware-gated, idle)) ...`, the `camcc gdsc/branches/rcgs/plls`, `mclk-cc` and `mclk0..7` lines
(the first CAMCC readout under XNU: record them), the CAMSS blocks and CCI0 `not read (TITAN_TOP GDSC off)`; then `step 203`,
`FAILED at step 203: stopped (piano-camera-stop)` (or a refusal: report it), the power-down (the branches' CLK_ENABLE cleared,
`votes: CAMCC's domains withdrawn: ok`) and `power down complete`. A hang right after `step 202`: CAMCC is not readable even so
(report the last lines; c2r would hang the same way).

**c2r**: the same shape with `step 205: the bus: TITAN_TOP, CCI0's clocks from the XO, its pins, the controller`, the TITAN_TOP and
CCI writes, `rear: CCI0 HW_VERSION ........`, three rails, `rear S5KJN1: chip ID 0x38e1 (expected 0x38e1): OK over CCI0 master 1`.

Failure lines: `REFUSED: <why>; nothing written for it` (report the survey); `FAILED at step 205: TITAN_TOP did not power up` (its
GDSCR/CFG in the `last value`); `FAILED at step 213: a rail read back at another voltage` (the rail line shows it; the rail is off
again); `FAILED at step 230: the chip ID read failed: no ack` (rear: try `c2r24`, then `c2rg`; front: `piano-camera-pull=1`, then a
longer `piano-camera-i2c-half-us`); `unexpected chip ID` (another part: report the ID); `power down INCOMPLETE` (report the warnings;
the next boot clears the votes). A hang after a `step` line: report it (its number is in `piano_camera_stage`).

**c3r** (after level 1's lines and level 2's steps 201-230 for the rear with `MCLK1 24 MHz`; `..` = values to record):
```
PianoCamera: level 3: the rear sensor, frames=2 pattern=1 timeout=3000 ms gain=64 mclk24=1
PianoCamera: stage 5: level 3: RAW10 frames through CSIPHY, CSID-lite0 and TFE-lite0's RDI0 into memory
PianoCamera: step 300: the sensor: level 2's power-up and chip ID (TITAN_TOP, CAMCC's domains, the rear's MCLK at 24 MHz)
PianoCamera: votes: mxc.lvl 0x..... (.. levels, top vlvl 0x...), mmcx.lvl 0x..... (.. levels), MM1 0x.....
PianoCamera: rsc: shared votes: 1 command(s) (0x.....=0x.) through TCS . completed after . ms
PianoCamera: votes: CAMCC's domains up: ok; mxc hlvl . (vlvl 0x40, others 0), mmcx hlvl . (vlvl 0x140, floor 0x140, others 0); 1 command(s) sent (MXC only: MMCX stays the boot display's)
PianoCamera: step 201 ... step 230 (level 2's lines, §8.2)
PianoCamera: step 301: MM1 on (the camera NoC; the shared multimedia votes)
PianoCamera: votes: the camera NoC (MM1) on: ok; MM1 60000001 (other clients do not need it); 1 command(s) sent
PianoCamera: step 302 ... 305 (write lines: GCC HF AXI, PLL0's configuration, RUN, outputs, OUTCTRL, the RCG switches, the branches)
PianoCamera: step 306: the SMMU route of stream 0x1c00 (as the boot chain left it, else our window)
PianoCamera: step 307: ...; dma: frame buffer 16777216 bytes at 0x.........; the window's tables at 0x.........
PianoCamera: write 0x15...... = ........ (SMMU: the window)   (the bank, then S2CR, SMR, TLBIALLNSNH, sync)
PianoCamera: step 308 ... 311; csiphy1: version ........; the lane table: 87 entries written (settle 29 for 700000000 Hz), 16 skipped ...
PianoCamera: step 312: the S5KJN1's tables (start, init, mode 4080x3072), its controls, stream on
PianoCamera: step 320: the stream: BUF_DONE polled, the write master re-armed after each frame
PianoCamera: step 330 ... 331 ... 340
PianoCamera: level 3: rear S5KJN1 4080x3072 RAW10: OK
PianoCamera: route: our translating window (a free group and bank of ours); bus 0x20000000 -> pa 0x........., 16777216 bytes (frame 15679488 = 5104 x 3072 + a guard)
PianoCamera: versions: csid-lite0 ........ tfe-lite0 ........ csiphy1 ........; settle 29; lane table 87 written, 16 skipped; sensor 317 table writes, 5 controls (pattern 1, gain 64)
PianoCamera: stream: 2 frame(s) (BUF_DONE) in ..... us: the first after ..... us, the last interval ~33000 us; SOF . EOF . RUP_DONE .; late 0; ... polls
PianoCamera: irq seen: top ........ buf-done 00000002 csi2-rx ........ rdi0 ........ (error bits 00000000)
PianoCamera: frame: written 15667200 bytes (15667200 unpadded, 15679488 padded lines), stride 5100; guard intact
PianoCamera: frame: r0c0 min .. mean .. max ..; histogram (128 a bin) ...   (four lines)
PianoCamera: frame: eight vertical bands, mean r0c0/r0c1/r1c0/r1c1: b0 ../../../.. ... b7 ../../../..   (the colour bars: eight distinct steps)
PianoCamera: level 3: N writes logged, M quiet (...), 0 refused, 0 off-side warnings; power down complete (everything as found); buffer freed
PianoCamera: stage 6: done (level 3)
```
Failure lines: `FAILED at step 303: ... no LOCK_DET within 1.5 ms` (PLL0: its MODE in `last value`; MXC's vote line above),
`FAILED at step 306: refused: ...` (the SMMU's state: report the survey's smmu lines), `FAILED at step 308: CSID-lite0 did not
reset`, `FAILED at step 320: no frame arrived` (report the `irq seen` and `csiphy1 status` lines: a CSI-2 RX error says the PHY saw
the lanes; nothing at all says the sensor did not stream or the PHY's settle/lane table; next: the scene `c3rs` with
`piano-camera-pattern=0`, then a longer `piano-camera-timeout-ms`), `guard DAMAGED` (the write master wrote past the frame: report
the written extent and the smmu fault line), `power down INCOMPLETE` / `buffer KEPT` (report the warnings). A hang after a `step 3xx`
line: report it (`piano_camera_stage` has the number).

### 8.3 Level 3 unattended?

**Yes, in the order of §8.1** (`c3rp` only after `c2r24` passed, then `c3r`, `c3rs`, `c3f`), with the residual risks below; no
step can damage hardware or leave persistent state. What it changes beyond level 2: CAMCC's PLL0 and the stream's clocks (camera
only, all taken back; PLL0's configuration words stay, as Linux's probe leaves them), GCC's camera HF AXI branch (if found off),
CSID-lite0, TFE-lite0's write master 0 and one CSIPHY (reset with TITAN_TOP's collapse), the sensor's own registers (its drivers'
tables, gone with its power), BCM MM1 on for the stream's duration, MXC at low_svs (as level 2), one free stream-match group and
one free context bank of the apps SMMU (every word restored as found after the stream: the stream made to fault first), and 16 MiB
of contiguous memory. It does not vote MMCX (the boot display's vote stays exactly), touches no other CAMSS block, no partition, not
the ADSP, the display, the torch or the battery. The stream is bounded (two frames by default, 3 s at most from stream-on, every wait
bounded), then everything is taken down in reverse.

Memory: on the tablet the route is our window (c1: no group for 0x1c00, unmatched streams fault): the write master can reach only
the buffer, so a wrong frame size or address faults in the SMMU (logged) instead of reaching XNU's memory; the guard reports an
overflow inside the buffer; the buffer is not freed unless the write master read back stopped and its clock stopped. Residual
risks: (1) a CAMSS register that stalls the bus because a clock or domain this level does not know of is needed (the model knows
Linux's clock lists; the hardware may want more): a hang until `piano-reset-after`/the Gunyah watchdog, a RAM boot leaves nothing;
`c3rp` meets every register access but the stream's first, so it goes first; (2) the SMMU group or bank the plan takes free is in
use by someone it cannot see (the same plan as PianoWifi's level 5 window and PianoVideo's bank; PianoWifi's ran on this tablet):
the symptom would be another device's DMA faults for the stream's few seconds; (3) MXC's DRV-2 vote goes to 0 at the end (as
PianoVideo's power-off does): nothing else natively votes it; (4) a sensor that streams in a way its tables do not describe: the
window bounds it. The first streaming run (`c3r`) is the first time anything natively drives CAMSS: attended is not required, but
the webcam view of the panel is worth keeping (the display should not change at all).

### 8.4 Results

**c1, 2026-11-12** (queue run 11:21-11:27, image 016d85c229315fe4; `logs/usb-console/20261112-112113-port0.txt`): no hang, `done: 166
MMIO loads, 0 stores`, 32 observer reads, 0 map failures. GCC: camera AHB 0x88000002 (off, hardware-gated), XO 0x88000000 (off),
MCLK-controller AHB 0x88000002 (off, hardware-gated), HF/SF AXI 0x8a200000 (off), QMIP 0x88000002; so CAMCC, the MCLK controller,
CCI0 and every CAMSS block "not read (GCC camera AHB off)". QUP2's vote 0x00f1009f, SE1 not clocked. Pins: all ten GPIO inputs,
pull-down, 2 mA (cfg 0x1; the egpio ones 0x801, not claimed by apps), none driven. Rails: every PPID at 0xc0 + n (owner EE 4),
the five sensor LDOs disabled, L3G 1200 / L1F 880 / L3I 880 mV enabled and ready, all cmd-db addresses found. SMMU: SCR0 0x002d0406,
IDR0 0x4c017e7f, IDR1 0x60000053: 127 groups (122 free), 83 banks, no group for 0x1c00..0x1c1f. What changed in response: §4.2
"After c1" (the hardware-gated rule; CAMCC's domains voted before its first access) and level 3's route (our window; §4.3 step 306).

## 9. Fallback

Without `piano-camera` nothing is mapped. A refusal writes nothing for that sensor. Any failure runs the full power-down. A
system sleep during a sequence waits for its power-down (the kext joins the power tree at level 2).

## 10. Open questions

1. CAMCC and the MCLK controller readable at the boot rails (MMCX on for the display; MXC unknown)? → `c1`: not readable at all
   (their GCC AHB off); level 2 enables the AHB and, for CAMCC, votes MXC first → `c2r` (its step 202) answers it.
2. The rails' PPIDs (both page rules), their state at XNU time, the pins' states → `c1`: **answered** (§8.4).
3. Does the rear answer at a 19.2 MHz MCLK? → `c2r` / `c2r24`.
4. The front bus's pull-ups (the sensor's vddio, or always-on) → `c2f` (else `piano-camera-pull=1`).
5. Stream 0x1c00's SMMU entry at XNU time; bypass allowed? → `c1`: **no entry** (unmatched streams fault): L3 takes its window.
6. The shared multimedia votes (MXC, MMCX, MM BCMs) between camera, video and display → **built** (§4.3.1); whether MXC at low_svs
   suffices for PLL0 at 1200 MHz → `c3rp` (step 303).
7. Frame mode's line padding for 4080-pixel lines → L3's first frame (`c3r`: `frame: written ..., stride ...`).
10. Are Linux's clock lists all the CAMSS blocks need (no stall on a CSID-lite0/TFE-lite0/CSIPHY access)? → `c3rp`.
11. The CSID-lite 980's RDI interrupt bits (SOF 12, EOF 9: inferred from the 680's layout at the same offset) → `c3r`'s `irq seen`
    line (two frames: two SOF/EOF pairs).
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
