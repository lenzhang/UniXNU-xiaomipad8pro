# Display: GOP adoption first, then DPU address-only flips

Status: design, 2026-11-10. Nothing built or run. Template: `README.md`. The GOP step (1. below) is built for the k27
collection as `NativeGopDisplay`: [native-gop-display.md](native-gop-display.md) (2026-11-11, not run). The DPU half (the `dpu=2`
readout, direct flips, the SMMU window, refresh, colour, panel sleep) is now [dpu.md](dpu.md) (2026-11-11), with SM8750's v12
register layout, piano's facts and `PianoDpu`; where this page and dpu.md differ, dpu.md is current.

## 1. Purpose and gate

Give WindowServer an IOMobileFramebuffer on the real panel. It gates **M2**: the kernel log, then the login window, on the
panel. Two bottom halves under one IOMFB top half:

1. **GOP/splash adoption (M2, first light).** WindowServer's frame is copied by the CPU (or GPU copy, `fb-gpu=1`) into
   the framebuffer ABL/UEFI left scanning out. No DPU register is written. No vblank (timer).
2. **DPU address-only flips (after M2, `dpu-scanout=2`).** The pipeline ABL left running stays as it is; the driver only
   rewrites the SSPP source address/stride and flushes CTL, so the panel scans the IOSurface directly. Then DPMS and
   brightness. A cold modeset (DSI PHY PLL, DSC, panel init) is **not** in scope.

## 2. Hardware facts

| Item | Value | Source | State |
|---|---|---|---|
| Panel | `xiaomi,piano-nt36532`, 3200x2136, dual-DSI, DSC (slice 800x24), video mode; 239x163 mm; Linux modes 60/90/120/144 Hz | piano dts `panel@0` (~:1108); piano-uefi `Binaries/piano/RawFiles/Panel_P81_42_02_0a_dualdsi_dsc_vid.xml` | verified |
| Panel variants | id GPIO100 low = CSOT p81 42-02-0a, high = BOE p81 35-02-0b; reset GPIO98 active-low | `panel-novatek-nt36532.c:nt36532_probe` (:723-736) | verified |
| Boot (UEFI) refresh | 120 Hz (`DSIRefreshRate` 0x780000 in 16.16); Linux simple-framebuffer node also says 120 Hz, vtotal 2310 | panel XML; piano dts :44-100 | verified |
| Splash framebuffer | `splash_region@fc800000`, 0x2b00000 bytes (ABL needs this exact node name); GOP 3200x2136, stride 3200 px = 12800 B, BGRX (x8r8g8b8) | piano dts :297-299; piano-uefi `selector/boot.c:89-91`, `MemoryMapLib.c:33` ("Display_Reserved", write-through); Project-SunUEFI `docs/devel/boot/uefi-display.md:30` | verified |
| MDSS | `display-subsystem@ae00000` (0x1000), **SPI 83** level, MDSS is its own interrupt controller (DPU = hwirq 0; status at MDSS+0x10) | `sm8750.dtsi:3335`; `msm_mdss.c` chained handler (:82-101) | verified |
| DPU (MDP) | `ae01000`, len 0x93000; VBIF `aeb0000`; DPU core 12.0 | `sm8750.dtsi:3368`; `dpu_12_0_sm8750.h` | verified |
| SSPPs (from MDP base) | VIG0-3 0x4000/0x6000/0x8000/0xa000 (QSEED3 v3.4); DMA0-5 0x24000..0x2e000 step 0x2000 | `dpu_12_0_sm8750.h:70-134` | verified |
| SSPP registers | SRC_SIZE 0x00, OUT_SIZE 0x0c, **SRC0_ADDR 0x14**, **SRC_YSTRIDE0 0x24**, SRC_FORMAT 0x30, FETCH_CONFIG 0x48, MULTIRECT_OPMODE 0x170 | `dpu_hw_sspp.c:21-42` | verified |
| CTL | CTL0-5 at 0x15000 + 0x1000·n; **FLUSH +0x18**, START +0x1C, INTF_FLUSH +0x110 | `dpu_12_0_sm8750.h:42-64`; `dpu_hw_ctl.c:25-45` | verified |
| LM / PP / DSC | LM0-7 0x44000.. (pairs); PINGPONG0-7 0x69000..; DSC dce_0..3 0x80000.. | `dpu_12_0_sm8750.h` | verified |
| Vsync/TE | INTF1 0x35000 (vsync INTR bit 27, underrun 26), INTF2 0x36000 (vsync 29, underrun 28); per-INTF intr EN/STATUS/CLEAR at INTF+0x1c0/0x1c4/0x1c8; tear block at 0x34800+0x1000·n; top INTR_EN/STATUS/CLEAR 0x10/0x14/0x18 | `dpu_12_0_sm8750.h:411-427`; `dpu_hw_interrupts.c:34-41,138`; `dpu_hwio.h:14-20` | verified |
| SMMU | apps SMMU `0x15000000`, display stream **SID 0x800 mask 0x2** | `sm8750.dtsi` mdss `iommus` | verified |
| Power/clocks | MDSS_GDSC (dispcc); clocks MDSS_AHB, GCC_DISP_HF_AXI, MDSS_MDP; interconnects mdp0-mem, cpu-cfg; reset MDSS_CORE_BCR. All left on by ABL; not touched in this design | `sm8750.dtsi:3335` | verified (left on: needs-boot) |
| Coherency | MDSS is not `dma-coherent` | DT | verified |
| Backlight | 2x KTZ8866 at I2C 0x11: primary on `i2c_hub_3` (@98c000, enable TLMM 2), secondary on `i2c8` (@880000); 400 kHz; 11-bit level, max 2047 | piano dts :971-981, :1061-1073 | verified |
| Panel bias | VSP/VSN 5.8 V on TLMM 117/118; vddio L12B | piano dts :233-258 | verified |
| Measured (Linux KMS) | 144 Hz mode: one plane on `sspp_8`, multirect split 2x1600x2136, `XR24`; MDSS IRQ ~137/s with vblank on, ~8/s idle | the local bring-up notes (`thin-direct-measurements.md`, not published) | verified (Linux, not ABL) |

Which SSPP(s), CTL and LM pair ABL itself uses at 120 Hz is **needs-boot** (the `dpu=2` readout below). The sheng code
assumes VIG0+VIG1 and CTL0; on piano that is plausible (same DPU generation offsets) but unproven.

## 3. Ubuntu as the specification

Kernel `linux-sm8750-piano` v1.1.0 (`~/kfirst/linux`, commit d2710cd1). Read, do not copy.

- **Pipe programming:** `drivers/gpu/drm/msm/disp/dpu1/dpu_hw_sspp.c` (source address/stride/format setup per rect;
  multirect for the 2x1600 split), `dpu_hw_ctl.c` (flush mask: one bit per SSPP, written to CTL_FLUSH; in video mode
  no CTL_START is needed, the next frame latches it), `dpu_formats.c:190-200` (linear pitch >= width x 4, <= 0xffff).
- **Vsync:** `dpu_hw_interrupts.c` (7xxx interrupt set, per-INTF register triples) and `msm_mdss.c` (one SPI, status
  bit per sub-block). The INTF vsync bit for the master interface (INTF1, DSI0 is master) is the flip-done source.
- **Plane limits:** `dpu_plane.c:812` (upscale <= 20, downscale <= 4), `max_linewidth 8192`, `max_mixer_width 2560`
  (so a 3200-wide plane always spans two LMs).
- **DPMS:** `drivers/gpu/drm/panel/panel-novatek-nt36532.c`: `nt36532_off` sends DCS display-off, waits
  20 ms (piano `display_off_delay_ms`), DCS enter-sleep, waits 100 ms; `nt36532_unprepare` asserts reset and drops the
  supplies in reverse with 2/3/2 ms gaps; the on tail is exit-sleep, 120 ms, display-on. `piano_reset` is
  active-low with 10/3/3/15 ms steps. Note `packaging/release/changes/v0.3.1.md:35,41` in xiaomipad-8pro-mainline: the
  screen-off flash was fixed by **not resetting the shared display controller while the panel scans**; 42 DPMS cycles
  clean. Acceptance tools: `tools/piano-display-mode-check.py`, `tools/piano-suspend-check.py`.
- **Brightness:** `drivers/video/backlight/ktz8866.c:ktz8866_set_brightness` writes level bits [2:0] to 0x04 and
  level>>3 to 0x05, mirrored to the secondary (`kinetic,secondary`); BL_EN 0x08 (bit 6 enable, bits 5:0 sinks).
- **Lid/DPMS from the VM path:** `thin-direct-report.md` (SW_LID → DPMS off/on; EINVAL needed a full modeset).

## 4. UniXNU template to build on

| Piece | Base (pin) | Change for piano |
|---|---|---|
| IOMFB top half | `GopFramebuffer` d711e8c, class `NativeLegacyDisplay : IOMobileFramebufferLegacy` (`NativeLegacyDisplay.cpp:183`); macOS behaviour (console release via `PE_initialize_console(.., kPEDisableScreen)` on first swap :947-952, `publishDisplayAttributes` :1037-1082, `IOMFBUUID` :1155-1163) already follows the VM-era display kext | none for first light; take sheng's macOS 27.3 arg set. Cross-check slot/offset table against the generated `IOMobileFramebufferABI.h` (27.0.1, `out/k27/iomfb-abi/`, see `package/NativeGopDisplay/UPSTREAM`) vs UniXNU's `OriginalMobileFramebufferABI.h` for 27.3 |
| GOP copy | same, GOP via `PE_current_console`; vsync = `IOTimerEventSource` at `vsync` Hz (no hardware vblank) | `vsync=120` to match the 120 Hz UEFI mode |
| Pipe readout | `dpu=2` (docs/porting/display.md "Read the display pipes" :73-99): reads VIG0/1 source/size/format/fetch, CTL0, both INTF tear checks, display streams in the apps SMMU | run first on piano to learn ABL's pipe set |
| Direct flip | `NativeLegacyDisplay::startDirect`/`flip` (:296-333): writes SRC0_ADDR/YSTRIDE0 on VIG0+VIG1, `dsb`, CTL0 FLUSH=0x3, no START; 4 `DirectSlot`s; fence rule (wait until Adreno's completed fence passes the swap's submitted fence; without it the panel flickers) | **hardcoded sheng constants**: `kBootIova=0xb8000000`, `kBootStride=0x2fa0` → piano `0xFC800000`, `0x3200` (12800); flush mask from the readout; make them board properties, not constants |
| Display SMMU window | lives in ShengUfs (`NativeUfs.cpp` ~:1288-1330 `dpuEnable`, `native_ufs_dpu_map`), mode >= 1: stage-1 table keeping 0x80000000-0xffffffff identity so the boot scanout survives, plus surface windows; TCR2 SEP=7 | streams **0x800/mask 2** instead of sheng's 0x1c00/0x1c02; the splash at 0xFC800000 lies inside the identity range |
| Backlight | UniXNU-sheng `package/AppleARMBacklight.cpp` (`ShengBacklight`, answers the AppleARMBacklight metaclass), `package/GeniI2c.{h,c}`; doc `docs/drivers/sheng-backlight.md` (also 2x KTZ8866 @0x11) | piano buses: `i2c_hub_3` and `i2c8` (two GENI SEs, not one); enable GPIO2 |
| Timing probe | UniXNU-sheng `DisplayTimingProbe.cpp` (`dpu=1`) | read 120 Hz from INTF timing |

Piano UniXNU attempts so far (`piano-unixnu/board.toml:18,36,63`) use the Pagani `NativeMobileDisplay` (GOP only, "vblank
not implemented") with `offline-legacy-gop=1`; GopFramebuffer is the better base because it has the direct path.

## 5. IOKit family / ABI

`IOMobileFramebufferLegacy` subclass in IOMobileFramebuffer.kext of the macOS 27.3 KC (Mac17,11 profile). The family
runs the swap queue and user client; the driver supplies `swap_begin`/`program_hardware`, timing and colour elements
(8-bit SDR, sRGB), `notify_swap_done`. Backlight publishes an AppleARMBacklight-compatible service.

## 6. Boot args (27.3 names)

`offline-legacy-register=1 offline-legacy-gop=1 offline-legacy-unit-scale=2 offline-legacy-phase=4` (+ sheng's
`offline-mobile-graphics-*`, `offline-legacy-pm/validate/transfer/cpu-present=1`), `vsync=120 vsync-fixed=1`,
`vc-text=1` (boot log), `fb-bgra=1` (WindowServer renders 8-bit BGRA; required for direct scanout, see
`native-display-8bit-scanout-design.md` §1), `fb-gpu=1` (GPU copy fallback), `dpu=2` (readout only), `dpu-scanout=2`
(direct, off by default), `backlight=1`. Gamut sRGB (P3 gives the unscannable `'b3a8'`).

## 7. Kernel archive / component

`components`: `GopFramebuffer` (pinned), board glue `build-gop-legacy-display-27.py` as in UniXNU-sheng
`board.toml:20`; backlight + GENI I2C + timing probe from the piano layer (`drivers/PianoBacklight`, sheng-derived).
The DPU SMMU window comes with the storage component (`ShengUfs`/`NativeUfs`), so `dpu-scanout>=1` needs it linked.

## 8. Hardware test plan

- **M1:** UniXNU menu on the GOP proves the splash fb is live (already implied by simpledrm on Linux).
- **M2a (GOP only):** `vc-text=1`, no `dpu*` args. Pass: boot log, then the login window on the panel at 3200x2136
  (scale 2), colours right. Fail-safe: a reboot returns to Ubuntu; nothing written.
- **M2b (`dpu=2`, read-only):** record SSPP set, formats, SRC addr (= 0xFC800000?), stride, CTL in use, flush bits,
  INTF1/2 tear/vsync state, the display SMMU stream's context. No writes. Compare with the Linux KMS state.
- **M2c (`dpu=3`):** rewrite the address of the readout pipes to the same splash (no-op), then to a second buffer;
  time the latch (< one 8.3 ms frame at 120 Hz). Then wire INTF vsync (SPI 83, master INTF) into
  `notify_swap_done` instead of the timer; MDSS IRQ rate should be ~120/s.
- **M2d (`dpu-scanout=2`):** IOSurfaces mapped through the display SMMU window, with the fence rule. Pass: no flicker,
  no underrun bits, no SMMU fault, a moving bar shows no tearing. Any refusal (0xdead000n codes) falls back to copy.
- **Later:** DPMS (display-off/sleep-in on idle; on with the documented delays; never reset MDSS while scanning) and
  brightness 0..2047 on both KTZ8866.

## 9. Fallback

`dpu-scanout=0` → GPU copy (`fb-gpu=1`) → CPU copy into the GOP. Without IOMFB at all, `vc-text=1` still shows the
kernel log. The Gunyah VM path (`gunyah-guest-SWITCHBACK.md`) is untouched.

## 10. Open questions

1. Which SSPPs/CTL/LMs and multirect mode does ABL leave at 120 Hz (Linux uses `sspp_8`, a DMA pipe, at 144 Hz)? If
   ABL uses a DMA pipe with multirect, the flip must write both rects' addresses. → M2b readout.
2. Is the master INTF vsync interrupt usable without Linux's interrupt setup (enable bits left by ABL)? → M2c.
3. Does the display stream (0x800) already have a context bank from ABL/Gunyah, and can the HLOS reprogram it under
   Gunyah? → `dpu=2` SMMU readout; plan §4(a).6.
4. Native 3200x2136 1:1 vs 1600x1068 scaled by a VIG/QSEED: the scaled path needs the scaler programmed, which is
   more than an address flip. Start with 1:1 (or CPU/GPU-copy at scale 2).
5. 120 Hz only, or a later switch to 144 Hz? Mode switching is a modeset; out of scope.
6. IOMFB 27.3 ABI: confirm GopFramebuffer's 27.3 slot table on the Mac17,11 KC; our 27.0.1 table is a cross-check.

## 11. Licence note

The msm DPU, panel and KTZ8866 drivers are GPL-2.0 and are read as the specification only (registers, sequences,
delays, cited above). GopFramebuffer and the sheng packages are reused under their own licences (check `LICENSE` in
each repo). No Linux code is copied.
