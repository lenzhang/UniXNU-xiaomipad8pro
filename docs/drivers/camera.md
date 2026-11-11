# Cameras (B11): design

Status, 2026-11-11: **design only, offline.** Nothing built. Backlog item B11 (`../BACKLOG-hardware.md`). Template: `README.md`.

## 1. Purpose and gate

Ubuntu v0.3.0 brought both cameras up (rear S5KJN1SQ03, fixed focus; front OV32D40), v0.10.0 fixed auto-exposure; its daemon
publishes 1920×1440 NV12 at 30 fps to every app through v4l2loopback, and takes photos with the rear torch. Natively: FaceTime,
Photo Booth, QuickTime and browsers should see "Rear Camera" and "Front Camera". No login milestone; a late daily-use item.

## 2. Hardware facts

Sources: the boot DT ("DT"); the Ubuntu piano kernel piano/dev 7f8d6bed (GPL, facts only): `sm8750-xiaomi-piano-camera.dtsi`, the
piano dts, `camss*.c` (SM8750 resources, CSIPHY 3-phase/2-phase, CSID 980, VFE/TFE gen4), `s5kjn1.c`, `ov32d40.c`, `dw9768.c`,
`i2c-qcom-cci.c`, `camcc-sm8750.c`, `cambistmclkcc-sm8750.c` ("msm"); the Ubuntu device repo `device/camera` (MIT, behaviour).

| Item | Value | Source | State |
|---|---|---|---|
| CAMSS | `isp@ad27000`: CSID0/1/2 `ad27000/ad2a000/ad2d000` (0x2b00), CSID-lite0/1 `ad6d000/ad72000`, CSIPHY0..5 `ada9000` + 0x2000·n, TFE0/1/2 `ac86000/ac96000/aca6000` (0x10000), TFE-lite0/1 `ad6e000/ad73000`, TPG0..2 `ad8b000`..; SPIs: CSID0 601, CSID1 603, CSIPHY1 478, CSIPHY4 122, TFE0 433, TFE1 436 (all edge); enabled in the boot DT | DT, msm | verified |
| Power, clocks | GDSCs TITAN_TOP (camcc +0x1134c, parent of all), TFE0/1/2 (+0x11004/0x11084/0x110ec); camcc `ade0000` (MMCX + MXC), MCLKs in **cambistmclkcc `1760000`** (MMCX + MX; PLL0 L 0x32; rates 12/19.2/24/68.57 MHz); CSIPHY 400/480 MHz, CSID 400/480, TFE 360..833 MHz, CCI 37.5 MHz; GCC camera HF/SF AXI | DT, msm | verified (source); state at XNU: needs-boot |
| Interconnect | ahb (APPSS → CAMERA_CFG), hf_mnoc / sf_mnoc → EBI1; Linux votes ahb 150/300 MB/s and hf 472/926 MB/s (4096×3072 30 fps) | DT, msm | verified (source) |
| SMMU | `iommus = <apps_smmu 0x1c00 0x0>`: one stream for every camera write master | DT | verified |
| Firmware | **none** on this path (no `request_firmware`; no ICP/CDM users); `camera_mem` carve-outs exist but are unused | msm, device `source.json` | verified (source) |
| CCI | only **CCI0** `ac7b000`, SPI 426, master 1 at 1 MHz (Fast-mode Plus), SDA gpio115, SCL gpio116; registers: per master 0x100·m, queues 0x300 + 0x200·m + 0x100·n, IRQ 0xc00..0xc0c | DT, msm | verified |
| Rear S5KJN1 | CCI0 m1 **0x10**; CSIPHY1, 4 lanes (D-PHY), 700 MHz link (1.4 Gbit/s a lane); MCLK1 **24 MHz** (gpio90); reset gpio6 active low; vddd L1I 1.096 V, vdda 2.8 V fixed (gpio175 enable), vddio L6B 1.8 V, afvdd L7B 2.8 V; on: vddd, 1–2 ms, vdda, vddio, afvdd, MCLK, reset release, 10–15 ms; ID 0x0000 = 0x38e1; modes 4080×3072@30 (HTS 4352, VTS 4288), 8160×6144@10; RAW10 (SGRBG, the flips change the order); mounted rotated 180°; flash = PianoTorch's lamp | DT, msm | verified (source) |
| Front OV32D40 | **not on CCI**: QUP2 SE1 I2C (`i2c@884000`, gpio4/5, 1 MHz) **0x10**; CSIPHY4, 4 lanes, 842.4 MHz; MCLK4 **19.2 MHz** (gpio93); reset gpio7 active low; vddio L10B 1.8 V, dvdd L2G 1.2 V, avdd 2.8 V fixed (gpio176); on: regulators, 5–6 ms, MCLK, 10–12 ms, reset release, 20–25 ms; ID 0x300a = 0x5632; modes 3264×2448@30, 3264×1836@30, 6528×4896@10, SBGGR10 | DT, msm | verified (source) |
| CSIPHY rails | L3G 1.2 V (all), L1F 0.88 V (CSIPHY0/1/3/5, always on), L3I 0.88 V (CSIPHY2/4, always on in the boot DT) | DT | verified |
| Focus | GT9764 at CCI0 m1 0x0c, **disabled** ("register protocol unverified"): fixed focus, as in Ubuntu | DT | verified |

## 3. Ubuntu as the specification

**The pixel path, not raw.** Ubuntu's daemon (`piano-camerad`) routes sensor → CSIPHY → CSID → IPP → **TFE 980 pixel pipeline** →
NV12 (rear csiphy1/csid0/tfe0, front csiphy4/csid1/tfe1): demux, black level 64, white-balance gains, demosaic, GTM, a 64-entry gamma,
RGB→YCbCr BT.601 full range with the colour matrix folded in, a downscaler, NV12 write masters (FD_Y = WM7, FD_C = WM8); the raw
RDI path (WM 23..27) stays in the driver, unused. The TFE register layouts were taken from Qualcomm's CamX settings code (the
driver says so) and checked against test patterns. 3A is the daemon's: AE target mean luma 112 (lowered by highlights), every 3
frames, exposure lines first, then analogue gain (rear: the raw code, 32 = 1×), then ISP digital gain (≤ 8×); fast acquisition then
±6 % steps with a 0.25 filter; grey-world AWB on mid-tones. Photos: torch on, drain the queue, wait for AE, capture, torch off
(an explicit off: closing V4L2 does not turn the torch off).

## 4. Design

**4.1 Kernel: PianoCamera (raw first).** One kext, two sensors, one at a time:
1. *Power and clocks*: TITAN_TOP + one TFE GDSC, camcc and cambistmclkcc branches (CCI, CSIPHYn + timer, CSID, TFE-lite or TFE
   for the write master, camnoc, AXI), MCLK; the RPMh votes for L1I/L6B/L7B/L10B/L2G/L3G (PianoWifi's RPMh regulator path is the
   template) and the MMCX/MXC corners; the fixed 2.8 V rails by TLMM gpio175/176; the interconnect votes (open: the BCM path
   natively, shared with B8/B10).
2. *Control bus*: CCI0 master 1 (queue writes, a report-status completion; IRQ 426) for the rear; the front's GENI I2C (QUP2 SE1:
   a small GENI FIFO-mode I2C, or bit-banged as `BitI2c.h` at 400 kHz for bring-up); chip-ID read first (0x38e1 / 0x5632).
3. *Sensor*: power-on sequence (§2), the init and mode register lists, exposure/gain/VTS for AE, the 180° flip on the rear.
4. *Receive*: CSIPHY (2-phase D-PHY, 4 lanes, settle count from the link rate), CSID RX (lanes, data type RAW10 0x2b, VC 0), **RDI0**
   → a TFE-lite (or TFE) RDI write master in plain packing: raw Bayer into a ring of 4 buffers. The write master takes
   `address >> 8` (40-bit addresses), so XNU's RAM above 4 GiB is reachable, but every write goes through stream 0x1c00: it needs a
   context that maps the ring (identity, or B1b's window technique); the bank's state at XNU time is unknown. Frame done from CSID
   BUF_DONE (the TFE's own IRQ does nothing in Linux).
5. *User client*: `IOUserClient` (public IOKit): open/close a sensor, start/stop at a mode, the ring as shared memory
   (`clientMemoryForType`), a frame-ready notification (async port), AE controls (exposure, gain, frame length) and the torch
   (calls PianoTorch's `PianoTorchSet`; the extension is not an administrator, so not through PianoTorch's property).

**4.2 User space: a CoreMediaIO camera extension.** macOS 12.3+ has a public camera-extension API (`CoreMediaIO/CMIOExtension*.h`
in the SDK: `CMIOExtensionProvider` / `Device` / `Stream`, `startServiceWithProvider:`, `sendSampleBuffer:...`); **DAL plug-ins
(`CMIOHardwarePlugIn.h`) are marked deprecated since 12.3** ("Use CMIOExtension instead") and, since macOS 14, apps no longer load
them (*inferred* from Apple's deprecation and third-party virtual cameras moving to extensions), so the extension is the path. It is a system extension inside an app (Info.plist `CMIOExtension` → `CMIOExtensionMachServiceName`), activated through
`OSSystemExtensionRequest` (SystemExtensions.framework). The extension opens PianoCamera's user client and turns each raw frame into
a `CVPixelBuffer` (`420f` NV12 full range, or BGRA) with a **software ISP**: half-resolution quad demosaic (each RGGB quad → one
pixel: 2040×1536 from the rear's 4080×3072, 1632×1224 from the front), black level, AWB gains, a 3×3 colour matrix, gamma, then
1920×1440 / 1280×960 out; on the A830 through Metal compute when the GPU path is up, NEON otherwise (~94 M quads/s at 30 fps: NEON
is marginal, Metal comfortable). AE/AWB as Ubuntu's daemon (§3) in the extension, driving the sensor through the user client.

**4.3 Why raw + software ISP first.** The TFE pixel path is cheaper in power but its register programming exists only in GPL code
derived from vendor settings (§11); the RDI path needs a handful of documented registers. The hardware TFE is a later power
optimisation (an owner decision on the source).

**4.4 Install on our stack.** System extensions need a signed app, a restricted entitlement and user approval. Our images already
stage ad-hoc signed code into the Data seed with its CDHashes in the static trust cache (`stage-soft-metal.py`, used for the Metal
plugin); the same staging for the host app + extension may let sysextd and AMFI accept them; otherwise `systemextensionsctl
developer on` (needs SIP off) is the development route. **Needs a test** (C5 below) before any camera code: a dummy extension that
streams a test pattern.

## 5. Dependencies

The SMMU window technique on another stream (from B1b), RPMh regulator and corner votes (PianoWifi's / PianoA830's helpers), an
interconnect vote path, CCI + GENI I2C, PianoTorch (B12, done: the photo flash as a torch), the A830 Metal path for a fast ISP
(optional).

## 6. Boot args (proposed)

`piano-camera=N` (0; 1 survey: GDSC/clock/rail/pin state, CCI and CAMSS versions, read only; 2 power + chip ID; 3 CSIPHY/CSID
receive with counters only; 4 frames to memory; 5 the user client), `piano-camera-sensor=rear|front`, `piano-camera-mode=N`.

## 7. Kernel collection

`package/PianoCamera` + `tooling/k27/build-native-piano-camera.sh`; host tests for the CCI queue encoder, the sensor power
sequences and register lists, CSIPHY settle arithmetic, the CSID/RDI configuration words, the ring. The extension:
`tooling/native/piano-camera/` (built on the Mac, staged into the seed).

## 8. Ladder and effort (RAM boots, attended)

| Step | What | Pass | Effort |
|---|---|---|---|
| C0 | offline: register maps, sequences, host tests | host tests | 1–2 weeks |
| C1 | survey (read only) | GDSC/clock/rail state, CCI HW_VERSION, CAMSS versions | 2 d + 1 session |
| C2 | rails, MCLK, reset; CCI chip-ID read (rear), GENI I2C (front) | 0x38e1 and 0x5632 | 1–1.5 weeks |
| C3 | sensor streaming into CSIPHY/CSID; CSID RX counters, then the sensor's test pattern | frames counted, no CRC/ECC errors | 1–2 weeks |
| C4 | RDI write master + SMMU stream 0x1c00: raw frames in RAM, dumped over USB | a recognisable raw test pattern, then a scene | 1–2 weeks |
| C5 | a dummy camera extension (test pattern, no kernel) installed on our stack | FaceTime/Photo Booth lists it | 1 week (can run first) |
| C6 | the user client + the extension's software ISP + AE/AWB | a usable rear and front image in Photo Booth | 2–3 weeks |
| C7 | photo flash via PianoTorch, both cameras switchable, power off when closed | no leak of rails/clocks after close | 1 week |
| C8 | (later) the TFE pixel path in hardware | the same image at lower power | 3–5 weeks, owner decision |

**Verdict:** feasible natively with public APIs only (CMIOExtension + IOKit user client); the hardware is fully described by Ubuntu
and needs no firmware. The blockers are (a) the sensor register lists (§11, owner decision), (b) installing a system extension on
our boot (C5). Effort ≈ **2.5–4 months** to two working cameras through a software ISP; the hardware ISP is extra.

## 9. Fallback

Without `piano-camera` nothing is powered. Any failure powers the sensor down in reverse order and releases rails and clocks.

## 10. Open questions

1. **Sensor register lists**: the S5KJN1 init/mode tables and the OV32D40 tables (hundreds of writes each) exist only in GPL
   drivers (and vendor blobs, which we do not read). Facts alone cannot rebuild them. Options for the owner: accept the lists as
   specification data (as B7's USB3 and B8's DP PHY tables); or record the I2C writes Ubuntu's driver makes on this tablet (an I2C
   trace: the sensor vendor's settings as observed); or keep the camera out of scope. → owner decision before C2.
2. CAMSS GDSC/clock state at XNU time, and whether stream 0x1c00 has a bank → C1.
3. Does our boot accept a staged system extension (trust cache) or only with SIP off? → C5.
4. RPMh LDO votes for camera rails natively, and the interconnect path → C2 (shared with B8, B10).
5. The front sensor's GENI I2C: shared with anything else on QUP2 SE1? (the DT shows only the sensor) → C1.

## 11. Licence note

The camss, sensor, CCI, clock drivers and the piano DT (GPL-2.0) are the specification only (addresses, fields, sequences, timings).
The TFE pixel-path programming in Ubuntu's driver is itself derived from Qualcomm CamX settings code; it is not used. The camera
extension uses only public SDK headers (CoreMediaIO, CoreVideo, IOKit, SystemExtensions); no Apple binary is read, no vendor blob.
