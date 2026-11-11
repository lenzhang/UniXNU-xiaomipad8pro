# Native DPU display path: survey (B1a), zero-copy flips (B1b), refresh (B1c), Night Light (B1d), panel off/on (B1e)

Status, 2026-11-11: **B1a built and host-tested** (`package/PianoDpu`, `piano-dpu=1`, in the kext list, inert without the argument);
not run on the tablet. **B1b built and host-tested** (`piano-dpu=2` with the ladder `piano-dpu-flip=1..4`, NativeGopDisplay's opt-in
`native-gop-display-flip=1`; §4.2, §8); not run on the tablet. B1c..B1e are design. Template: `README.md`. Supersedes `display.md`
§2/§4/§8 for the DPU (that page's GOP half is `native-gop-display.md`). Backlog item B1 (`../BACKLOG-hardware.md`).

## 1. Purpose and gate

NativeGopDisplay copies every WindowServer frame into the boot framebuffer with the CPU: ~39 ms per copy (34 ms reading the surface
uncached), up to seven copies per swap (the swap and refresh ticks 1, 2, 4, 8, 16, 30), so a blinking cursor costs about half a CPU
core, and the panel runs at ABL's 120 Hz whether anything moves or not. The DPU can scan an IOSurface directly (a *flip*: one source
address per pipe, latched at the next frame), switch the panel to 60 Hz while idle, tint the picture (Night Light) in its colour
block for free, and stop scanning on display sleep. Gate: battery life (owner priority) and daily use; prerequisite of B8 (DP).

## 2. Hardware facts

Sources: the piano boot DT (`out/inputs/piano-boot.dtb`, "DT"); the Ubuntu piano kernel's msm DPU/DSI driver and SM8750 catalog
(`unixnu-research/ubuntu-ref/kernel`, piano/dev 7f8d6bed, GPL: facts only, "msm"); the piano panel driver ("panel"); the stock panel
XML in the UEFI trees ("XML"); Project-SunUEFI's piano notes ("SunUEFI"); other UniXNU ports ("sheng" = SM8550 under the same
hypervisor family, "K90" = an SM8750 phone). Every offset is in `package/PianoDpu/DpuHw.h`.

| Item | Value | Source | State |
|---|---|---|---|
| MDSS | `ae00000` (0x1000), SPI 83 level, its own interrupt controller: DPU hwirq 0, DSI0 4, DSI1 5, DP 12; HW_INTR_STATUS +0x10; GDSC + clocks MDSS_AHB/MDP/vsync (19.2 MHz) | DT | verified |
| DPU 12.0 | `ae01000` (0x93000); VBIF `aeb0000`, REGDMA (LUT DMA) `af80000`; MDP clock ≤ 575 MHz | DT, msm catalog | verified |
| Pipes | VIG0..3 `+0x4000/6000/8000/a000` (QSEED3, CSC), DMA0..5 `+0x24000..2e000` (DMA4/5 cursor); SRC_SIZE +0, SRC_XY +8, OUT_SIZE +0xc, **SRC0..3_ADDR +0x14..0x20 (32 bits)**, YSTRIDE0/1 +0x24/28, SRC_FORMAT +0x30, UNPACK +0x34, MULTIRECT +0x170 (rect 1 regs +0x160..17c) | msm | verified |
| CTL | CTL0..5 `+0x15000` step 0x1000; DPU 12 has no CTL_LAYER: PIPE_ACTIVE +0x12c / LAYER_ACTIVE +0x130 / INTF_ACTIVE +0xf4 / DSC_ACTIVE +0xe8; FLUSH +0x18 (VIG0 b0, VIG1 b1, VIG2 b2, VIG3 b18, DMA0..5 b11,12,24,25,13,14; DSPP b29, INTF b31); video mode needs no START | msm | verified |
| LM | LM0..7 `+0x44000` step 0x1000, pairs; 11 blend stages at +0x40+0x30·s (FG_SRC_SEL +4: type/rect/id per byte, two pipes per stage); max mixer width 2560, so 3200 needs two LMs | msm | verified |
| DSPP | DSPP0..3 `+0x54000` step 0x2000 (LM n → DSPP n); **PCC v6 +0x1700**: enable +0, 3×3 S3.15 (0x8000 = 1.0) at +0x10..0x30; GC v2 +0x17c0 (its LUT only through REGDMA's indexed port); flush CTL_DSPP_n_FLUSH +0x13c+4n bit 4 (PCC) / 5 (GC) + CTL_FLUSH b29 | msm | verified |
| DSC | cores `+0x80000..83000`, two encoders each; piano: 10 bpc → 8 bpp, slice 800×24, 2 slices/link, DSC 1.1, block prediction | msm, panel, XML | verified |
| INTF | INTF1 = DSI0 `+0x35000`, INTF2 = DSI1 `+0x36000` (INTF0/3 DP); timing in INTF clocks (HSYNC_CTL, VSYNC_PERIOD_F0 = vtotal·htotal); FRAME_COUNT +0xac, LINE_COUNT +0xb0 (enable +0xa8); vsync = TOP INTR bit 27 (INTF1) / 29 (INTF2) at `+0x10/0x14/0x18` | msm | verified |
| DSI | `ae94000` master + `ae96000`, bonded, sync, 4 lanes each, **video mode**, PHYs `ae95000/ae97000` (3 nm); timing shadow DB_MODE/TIMING_FLUSH near +0x1e4..0x1ec | DT, SunUEFI | verified; shadow offsets needs-boot |
| Panel | NT36532, 3200×2136, per link HFP 117 / HSYNC 16 / HBP 16, VFP 68 / VSYNC 2 / VBP 104 (120 Hz); link fixed 1197.5 Mbit/s/lane; ID gpio100 low = p81 42-02-0a, high = 35-02-0b | XML, panel | verified |
| Modes | refresh by **HFP**: 144 Hz 42, 120 Hz 234, 90 Hz 618 (full panel); **60 Hz = the 120 Hz timing with VFP + 2310 (vtotal 4620)**; panel rate group B2/B3 = 0x91/0x40 (120 and 60), 0x00/0x00 (144), 0x00/0x80 (90); 42-02-0a writes B3 then B2, 35-02-0b B2 then B3 | panel, XML, SunUEFI oem-refresh | verified |
| Stock switching | `dfps_immediate_porch_mode_vfp`: 120↔60 (and 50/48/30) by VFP only, seamless; SunUEFI's Linux port does the same (DSI timing double buffer + INTF_VSYNC_PERIOD_F0 in one frame-synced flush), touch firmware unaffected; 90/144 are full modesets | SunUEFI | verified (their device runs) |
| Splash | `fc800000` (0x2b00000, no-map), 3200×2136 x8r8g8b8, stride 12800; DDR vote avg 3 280 896 kB/s = 3200·2136·4·120 | DT | verified |
| XNU RAM | 0x8c0000000..0xc00000000 only: **every IOSurface is above 4 GiB**; the DPU's fetch addresses are 32-bit (msm limits its address space to 0x1000..4 GiB) | `dts/piano.dts`, msm_kms.c | verified |
| SMMU | DT `iommus = <apps_smmu 0x800 0x2>` (0x800, 0x802; 0x801 secure bank in the stock overlay); under UEFI one entry SMR 0x80020800 → **translate, bank 2** (stage 1, stage 2 bypass) | DT, SunUEFI smmu observer | verified (UEFI); at XNU: needs-boot |
| Boot route | Linux keeps 0x800 on its boot bank as an identity domain; **moving the live stream to the emulated bypass bank reset the SM8750** | msm arm-smmu-qcom.c | verified (Linux) |
| Bank 2 contents | translation on or off, TTBR0, what it maps | sheng: bank 3 SCTLR 0xe0 (off); SunUEFI fixture 0x1e5 (on?) | **needs-boot (B1a)** |
| Pipes ABL uses | which pipe(s), multirect or two pipes, alpha, CTL, LMs, DSPP in path | Linux at 144 Hz: one DMA pipe multirect 2×1600; K90: VIG0+VIG1 | **needs-boot (B1a)** |
| Clocks | MDSS AHB may be gated (an SM8750 phone read it off and hung on an ungated read) | Adreno830 docs, K90 | **needs-boot (B1a gate)** |

## 3. Ubuntu as the specification

- Pipe/CTL/LM/INTF programming: `dpu_hw_sspp.c` (address/stride/format per rect), `dpu_hw_ctl.c` (v12 active masks, flush),
  `dpu_hw_lm.c` (v12 source select), `dpu_hw_intf.c` (timing engine), `dsi_host.c:dsi_timing_setup` (bonded halves, DSC).
- Refresh: modes and rate registers in `panel-novatek-nt36532.c` (§2); Ubuntu switches by full modeset; SunUEFI's
  `0002-display.patch` (`msm_dsi_host_update_vfp`, `dpu_hw_intf_update_vtotal`) is the seamless VFP path to follow.
- Panel off (K:72cfce4c86e5, "use the P81 stock shutdown timing"): DCS display-off 0x28, 20 ms, sleep-in 0x10, 100 ms, reset
  asserted, supplies off in reverse with post-off waits avee 2 ms, avdd 3 ms, vddio 2 ms. On: supplies, reset pin (active low)
  physically 0 for 10 ms, 1 for 3, 0 for 3, 1 for 15, the init (vendor block, DSC PPS, B2/B3), sleep-out 0x11, 120 ms, display-on 0x29. Teardown (K:c55958fa09dd,
  K:b7a813e2245c, K:608ff1b238a0): stop the slave INTF timing engine, then the master; reset the shared CTL only after both stopped;
  DSI host 1 off before host 0, PHYs after; never reset MDSS while the panel scans (v0.3.1: 42 clean DPMS cycles).
- Night Light: GNOME's gamma LUT through GC v2 over REGDMA (`dpu_hw_gcv2.c`); `dpu_crtc.c` maps a CTM to the PCC (S3.15).
- Measured (Linux): MDSS IRQ ~137/s with vblank on, ~8/s idle; 144 Hz: one DMA plane, multirect 2×1600, XR24.

## 4. Design

**4.1 PianoDpu owns the DPU.** One driver writes DPU/DSI/SMMU-bank registers: PianoDpu (levels ≥ 2). NativeGopDisplay keeps the
IOMFB side and gains an opt-in `native-gop-display-flip=1` that hands each swap's main surface to PianoDpu through
`callPlatformFunction` (`PianoDpuFlip`, `package/PianoDpu/PianoDpuFlipClient.h`, no link dependency: PianoBacklight's pattern);
without the argument it stays byte-identical. As built: the copy thread makes the call after its GPU fence wait (the request carries
the IOSurface, its memory descriptor, offset, row bytes, size, format, planes, and NativeGopDisplay's mean copy time for PianoDpu's
log); the call returns once the hardware latched or refused. A refusal (not 'BGRA', not serving, a failure) → the copy thread copies
the frame into the boot framebuffer, which is off the panel while a surface is shown, and then asks for the boot framebuffer back
(`surface = nullptr`): the copy never shows half done. The swap completes on the work loop only after that (a provider-less
`IOInterruptEventSource` the copy thread fires), so the surface it replaces is released to WindowServer only once the new one latched;
a flipped swap needs no refresh copies, and NativeGopDisplay's VBL timer runs only while refresh copies or a stalled queue need it.

**4.2 Zero-copy scanout (B1b).** The verdict: **feasible, through the SMMU only.** XNU owns no memory below 4 GiB and the SSPP
address registers are 32-bit, so physically contiguous surfaces would not help: the DPU's stream needs a stage-1 translation that
maps IOVA windows below 2 GiB onto each surface's pages (surfaces need no contiguity). The route must stay on its boot bank (moving
the live stream reset the SoC on Linux); the bank itself is reprogrammed in place, as ShengUfs did on SM8550 (sheng i562–i564):
1. *Table* (`DpuSmmu.h`): 4 KiB granule, T0SZ 32 (32-bit IOVA, walk from level 1), L1[2], L1[3] = 1 GiB identity blocks for
   0x80000000–0xffffffff, read-write as the untranslated bank was (the splash at 0xfc800000 keeps scanning through the switch), L1[1]
   and everything below 0x10000000 invalid, L1[0] → L2 → L3 windows: 4 slots of 32 MiB at 0x10000000 + 32 MiB·n, read-only, each
   surface followed by a 4-page guard on a zeroed 16 KiB scratch page (a fetch burst past the last row reads zeros instead of
   faulting); one 3200×2136 surface is 6675 pages. 66 table pages, physically contiguous, written through the cacheable kernel mapping
   and cleaned by VA (DC CVAC) before every TLB sync (the walker is not coherent: TCR IRGN0/ORGN0 = 0, MAIR0 attr 0 = 0x44 normal
   non-cacheable; MAIR0's other attributes and MAIR1 are ours too but unused, MAIR1 is not written). TCR2 PASize = IDR2's output size
   (at least 36 bits: XNU's RAM ends at 0xc00000000; capped at 48) and SEP = 7 (sheng: SEP 0 faulted every IOVA with bit 31 set). The
   format is decided at run time from the survey: CBAR type 1 (stage 1, stage 2 bypass) → stage-1 descriptors and TCR/TCR2/MAIR0;
   type 0 (stage 2 only, translation off) → stage-2 descriptors (MemAttr normal NC, S2AP) and a VTCR-format TCR (SL0 level 1, PS);
   types 2/3, an AArch32 bank (CBA2R VA64 0), IDR0 without the stage, IDR2 without 4 KiB AArch64 tables: refused (copies).
2. *Bank off* (SCTLR.M = 0, sheng's case): TTBR0 (the found ASID kept), TCR2, MAIR0, TCR; TTBR0/TCR read back (else: "the bank
   ignores our writes", undone); FSR cleared (RESUME when stalled); TLBIALL + TLBSYNC + TLBSTATUS (bounded, never a zero store); then
   SCTLR = found − CFCFG + M + CFRE + CFIE (0xe0 → 0x61: a fault terminates, never stalls the display); M read back; FSR watched for
   50 ms of the display's own fetches; a fault (with FAR/FSYNR0 logged) undoes it all to the found values. *Bank on* (ABL's tables
   live): only with `piano-dpu-smmu=3`, after the walk of ABL's tables found the boot framebuffer mapped to itself every 2 MiB and at
   its last page; then the same sequence, through "translation off" first (identity either way, so the panel does not change) instead
   of a TTBR0 switch at vblank. The stream's SMR, S2CR, CBAR and CBA2R are never written; the kernel glue refuses any SMMU store
   outside the bank's page.
3. *Per swap* (`DpuFlip.h`, the "PianoDpuFlip" service, step 4): the request's rules: 'BGRA', 0 or 1 plane, at least 3200×2136,
   row bytes 12800..0xffff and a multiple of 64, offset a multiple of 64, rows fitting a 32 MiB window (a policy, not a measured DPU
   limit: the splash's 12800 and msm's 128-byte-aligned buffers meet it; anything else is copied). The window: found by (surface,
   memory) or the LRU slot that is neither shown nor pending (its old surface unmapped, TLB synced, then released); the memory
   `prepare()`d, its physical segments listed as 4 KiB pages (inside XNU's RAM, 4 KiB aligned), the PTEs written and cleaned, TLB
   synced, the first/last/guard page checked through our own tables; the surface and its memory retained while mapped. The clean
   (`piano-dpu-clean`, default 1): DC CVAC over the rows the DPU fetches through a read-only cacheable alias of the surface made once
   per window (when XNU refuses the alias, flipped without, counted; 2: refused instead) — the CPU may have drawn the frame
   (CompositorSW). The flip: for each pipe/rect of the survey's path, SRC = window + offset + (rect's splash offset / 12800) × row bytes
   + (offset % 12800) (VIG1 by SRC_XY or by its own address both work; multirect: SRC0 and SRC1, both YSTRIDE0 halves), YSTRIDE0 = the
   surface's row bytes, SRC_FORMAT with SRCC3_EN cleared when the path has per-pixel alpha (restored for the boot framebuffer); DSB,
   CTL_FLUSH = the pipes' bits (or `piano-dpu-flush`); FLUSH polled after each vsync interrupt (or every ~1 ms) until the pipes' bits
   read 0, at most `piano-dpu-latch` ms; then FSR and the underrun bits are checked. The previous window stays mapped (evictable) and
   NativeGopDisplay completes the swap only after this returns. The same surface again: cleaned, no flush.
4. *Vblank*: SPI 83 (INTID 115) through `armcpu_spi_attach` (`package/common/PianoIrq.h`'s SpiLine and storm guard), attached once
   setup has turned the DPU's INTR_EN off and found MDSS HW_INTR_STATUS 0 (else polled: "another block holds the line"); the handler:
   MDSS status bit 0 → DPU INTR_STATUS & INTR_EN → INTR_CLEAR, the waiter kicked (no bit: a stray for the guard). The INTF1 vsync bit
   (27) is enabled only around a flush, so an idle desktop takes **no** display wake-ups: no MDSS interrupt, and NativeGopDisplay's
   timer stops while nothing needs it. Three 10 ms waits in a row without an interrupt → polled for the boot. The underrun bits (26,
   28) are read raw (setup checks that the raw status latches with its enable off; if not, they are enabled so they latch, and the
   handler's cleared bits are kept for the check).
5. *Safety*: a missed latch, an SMMU fault, or an underrun (at a flip, at a step, or by the watch every `piano-dpu-watch` s while a
   surface is on the panel) → the pipes back to the boot framebuffer (as found, alpha bit included) and that latch awaited; only then
   the bank back as found and every window let go (with a pipe that might still fetch a window, the window and its memory stay:
   translation off would turn its IOVA into a physical address); flips off for the boot ("PianoDpuFlip" answers kPianoDpuFlipOff,
   NativeGopDisplay copies). A revert while idle leaves the last copied frame in the boot framebuffer on the panel until the next swap.
6. *Cursor*: WindowServer's cursor layer is not shown today (the copy takes the largest layer); a free DMA pipe staged above in
   LM0/LM1 (two rects across the split) is B1b's last step: not built.

**4.3 Refresh switching (B1c).** 120 ↔ 60 Hz is a VFP-only change: no DCS command (B2/B3 are the same), no clock change, no DSC or
touch impact (SunUEFI ran 30/48/50/60/120 seamlessly). At a frame boundary, on both links: DSI DB_MODE = 1, TOTAL.V_TOTAL = 4620 − 1
(2310 − 1 back), TIMING_FLUSH = 1; INTF1/INTF2 VSYNC_PERIOD_F0 = 4620·htotal; CTL_INTF_FLUSH (INTF1|INTF2) + CTL_FLUSH b31; DB_MODE =
0 after the latch. Programmable fetch stays off (the 120 Hz family has 106 lines of VSYNC+VBP, more than the 24-line worst case, so
none is needed; a moving prefetch offset would need its own update). Policy: 60 Hz after
~1 s without swaps, 120 Hz on the next swap burst; WindowServer keeps seeing one 120 Hz timing (frames simply take two periods
while idle) or, later, a 60/120 pair. 90/144 change HFP and the panel's rate group (B2/B3 over the DSI command path while video runs):
a full modeset in both Linux ports, and more power, so not planned. Risks: a missed DSI/INTF pairing shows a grey or torn screen
(Ubuntu's full-modeset 60 Hz was grey until the stock order was used); the TDDI touch follows display timing (watch touch at 60 Hz);
the DSI shadow offsets are confirmed by B1a's raw `timing words`.

**4.4 Night Light (B1d).** PCC in DSPP0 and DSPP1 (both halves, same matrix): output R from (R,G,B) at +0x10/+0x14/+0x18, G
+0x1c/+0x20/+0x24, B +0x28/+0x2c/+0x30 (msm's PCC_<input>_<output>_OFF names), S3.15; enable +0 bit 0; flush CTL_DSPP0/1_FLUSH bit 4
+ CTL_FLUSH b29, latched with the frame. A warm white point is a diagonal (e.g. 3400 K ≈ 1.0, 0.73, 0.45). macOS reaches the display through IOMFB's `swap_set_color_matrix` (slot
343) and `set_gamma_table` (slot 307), both stubs in NativeGopDisplay today: B1d-0 logs which one Night Shift calls; a per-channel
linear gamma ramp maps exactly onto a PCC diagonal, a non-linear one needs GC v2 via REGDMA (later). Re-applied at display-on; no
CPU cost per frame. Needs the DSPP in ABL's path (B1a reads the PCC/GC enables; LM n → DSPP n is the catalog's wiring).

**4.5 Panel off/on (B1e).** Display sleep today only turns the backlight off: the DPU still fetches 3.28 GB/s and the panel scans.
Off: backlight off (PianoBacklight) → DCS 0x28, 20 ms, 0x10, 100 ms (K:72cfce4c86e5) through the DSI command FIFO (no DMA buffer:
`UniXNU-sm8750-common` PeripheralIO's DSI0 FIFO helper is the BSD template) → stop INTF2's timing engine, then INTF1's (slave
first). Supplies, reset and MDSS stay on (no GDSC collapse, no CTL reset while anything scans). On: INTF1+INTF2 engines on, 0x11,
120 ms, 0x29, backlight on. A full power-off (reset, supplies) needs the whole init and PPS: later, if the light sleep is not enough.

**4.6 External DisplayPort (B8) needs:** the DP controller `af54000` (INTF0, INTF3 for MST) and the USB/DP combo PHY (lanes 4,
1.62–8.1 Gbit/s), AUX + link training (DPCD), HPD and orientation from the DSP's altmode service and the Type-C mux (B7, PianoPower's
DSP), DISPCC's DPTX link/pixel clocks from the PHY's PLL, a second CTL with its own LM pair (+ MERGE_3D for widths above 2560, i.e.
4K) and pipes, a from-scratch INTF0 timing (nothing is left by ABL), a second IOMFB display, the same SMMU windows (stream 0x800),
and DDR/MDP votes for 4K60 (+2 GB/s). B1b's flip machinery and SMMU windows are its base. The B8 design: `displayport.md`.

**4.7 Power estimate** (*inferred*: no measurement on piano yet; core and DDR energy are generic SM8750-class figures).

| Change | Saved | Order of power |
|---|---|---|
| Zero-copy (B1b), cursor blink, ~2 swaps/s | 12–14 copies/s × 39 ms ≈ 0.5 core-s/s, and ~0.7 GB/s of copy traffic (27 MB read + 27 MB write per copy) | ~0.3–0.7 W CPU + ~0.05 W DDR |
| Zero-copy, animation | the copy tops out near 25 frames/s (39 ms each); a flip costs a few register writes (plus ~50 KB of PTEs once per surface) | 60 fps possible at a few mW |
| Vblank IRQ masked when idle | 60 timer wake-ups/s → 0 | deeper CPU idle |
| 120 → 60 Hz when idle (B1c) | DPU fetch 3.28 → 1.64 GB/s, a lower DDR/bus floor; panel drive halves; DSI link unchanged (fixed rate, clock lane in HS) | ~0.1–0.3 W |
| Panel sleep + engines off on display sleep (B1e) | the remaining 3.28 GB/s fetch and the panel's drive | ~0.3–0.5 W over backlight-off alone |

## 5. IOKit family / ABI

Level 1: `PianoDpu : IOService` on IOPlatformExpertDevice, registers nothing; properties `piano-dpu`, `piano-dpu-survey` (one line),
`piano-dpu-refresh-mhz`. Level 2 (B1b) registers the service and publishes `piano-dpu-b1b` ("step N passed", "step 4 serving", "off
for the boot: why"). Built: `callPlatformFunction("PianoDpuFlip", request)` (`PianoDpuFlipClient.h`: a versioned request, synchronous
until the latch or the refusal, the result one of shown / off / format / planes / size / stride / align / memory / slots / clean /
latch / fault / underrun). Later: `PianoDpuRefresh` (60/120), `PianoDpuColorMatrix` (3×3), `PianoDpuPanel` (off/on), called from
NativeGopDisplay's `swap_*`, `do_power_state_change_gated`, `swap_set_color_matrix`/`set_gamma_table` (IOMobileFramebufferLegacy,
the 27.0.1 ABI generated at build time).

## 6. Boot args

| Argument | Default | Meaning |
|---|---|---|
| `piano-dpu` | 0 | 1: the read-only survey (loads only); 2: the survey, then B1b's ladder (`piano-dpu-flip`) |
| `piano-dpu-flip=N` | 1 | B1b's steps (§8): 1 the boot framebuffer's addresses flushed again ×3; 2 + shifted 64 rows for 2 s and back; 3 + the DPU's SMMU bank in place, a test pattern through window 0 for 2 s, the fault status after a minute, the bank back as found; 4 the bank in place and flips served to NativeGopDisplay |
| `piano-dpu-irq=0` | 1 | CTL_FLUSH polled every ~1 ms instead of the vsync interrupt (SPI 83) |
| `piano-dpu-clean=N` | 1 | a surface's fetched rows cleaned from the CPU caches before each flip: 0 never, 1 through a cacheable alias when XNU makes one, 2 refuse a surface without one |
| `piano-dpu-flush=MASK` | 0 | the CTL_FLUSH bits of a flip instead of the pipes' own (0x3 for VIG0+VIG1); e.g. 0x20003 adds the CTL's bit 17 if the pipes' bits alone do not latch |
| `piano-dpu-underrun=0` | 1 | an underrun neither stops the ladder nor reverts |
| `piano-dpu-latch=MS` | 50 | the latch timeout (10..500): past it, a revert and flips off for the boot |
| `piano-dpu-watch=S` | 10 | the fault/underrun check while a surface is on the panel (0 off; nothing wakes while the boot framebuffer is) |
| `native-gop-display-flip=1` | 0 | (NativeGopDisplay's) each swap's surface to PianoDpu's flip, the copy when refused; needs `piano-dpu=2 piano-dpu-flip=4` |
| `piano-dpu-smmu=N` | 2 | 0 skip the SMMU; 1 global pages; 2 also the bank's registers (B1b steps 3-4 need them); 3 also walk the bank's translation (loads from the boot loader's table memory: opt-in; with it B1b may take over a bank the boot loader translates) |
| `piano-dpu-measure=MS` | 1000 | the frame-counter interval (20..10000) |
| `piano-dpu-delay=S`, `piano-dpu-repeat=S`, `piano-dpu-samples=N` | 0, 0, 3 | wait first; re-measure every S s, N times (after WindowServer starts, across display sleep) |

## 7. Kernel collection

`tooling/k27/build-native-piano-dpu.sh` (host tests `tests/dpu_host.cpp`: the gate and what is never loaded (VBIF, REGDMA, DSI
PHYs, the DP controller and INTF0/3, a DSI controller whose byte or pixel clock is off), the decoders, a modelled ABL path, the SMMU scan, the walk; then the objects and the import check). PianoDpu is in
`configs/k27-native-kexts.txt`, so every `build-native-image.sh` links it. B1b: `DpuSmmu.h` (the bank, the table, the windows),
`DpuFlip.h` (the engine), `DpuFlip.cpp` (the kernel side), `PianoDpuFlipClient.h` (NativeGopDisplay's interface); host tests
`tests/dpu_flip_host.cpp` against models of the DPU (shadow and latched pipe registers, CTL_FLUSH pending to the next frame, TOP
INTR, the frame counter, every rect's first and last pixel fetched through the SMMU each frame), the SMMU (bank registers, a TLB only
TLBIALL empties, the walk of DRAM, SEP and level-0 faults, FSR/FAR) and RAM (only cleaned lines reach the walker): the table layout
and descriptors (both stages), the plan for every route and bank kind, the enable and its undo, the bank-on take-over, all four
steps, the service (windows, LRU, TLB invalidation on eviction, every refusal, the clean policy, the alpha bit, multirect, a VIG1 by
address), the reverts, the interrupt, the summary; mutations (no TLB sync on eviction, no clean of a table) fail them.

## 8. Hardware test plan (RAM boots; B1a unattended, B1b–B1e attended)

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-dpu=1" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-dpu1 && cp -R out/ram out/ram-dpu1
python3 tooling/native/usb-console.py --capture 1000 &   # then: tooling/native/native-loop.sh out/ram-dpu1/boot-piano-macos-ram-A-bundled.img
grep -a "PianoDpu" logs/usb-console/<newest>-port0.txt
```

**B1a expected** (seconds after the kext starts; nothing changes on the panel; `...` = values to record):
```
PianoDpu: start: piano-dpu=1 smmu=2 measure=1000 ms delay=0 repeat=0 samples=3 (MDSS 0xae00000, DPU 0xae01000, DSI 0xae94000/0xae96000)
PianoDpu: level 1: loads only, nothing written
PianoDpu: stage 1: clock gate: GCC display AHB, DISPCC MDSS GDSC / AHB / MDP (read only)
PianoDpu: clocks: gcc disp ahb ... hf-axi ...; dispcc mdss gdsc 8....... (on) ahb ... mdp ... (source N, divider ...)
PianoDpu: gate: MDSS powered, AHB and MDP clocks on
PianoDpu: stage 2: MDSS, DPU and DSI registers (read only)
PianoDpu: pipe VIG0 rect 0: 1600x2136 @0,0 -> 1600x2136 @0,0 addr fc800000 stride 12800 format ........ unpack 03020001 'BGRA' ... [splash at 0,0]
PianoDpu: pipe VIG1 rect 0: ... [splash at 1600,0]              (or one DMA pipe with rect 0 and rect 1: multirect)
PianoDpu: ctl0: video mode, group 15, intf INTF1+INTF2 (master 00000002), lm LM0+LM1, pipes ..., dsc 03, merge3d 00, ...
PianoDpu: lm0: out 1600x2136 ...; stage N: VIG0 rect 0 ...     lm1: ... (right of a split pair) ...
PianoDpu: dspp0: pcc off|on ... ; gc off|on ...                (dspp1 the same)
PianoDpu: dsc0 enc0: ... (video) conf ... (bpc 10, bpp 8.0, block-pred on, v1.1) picture 1600x2136 slice 800x24, input pingpong ...
PianoDpu: intf0 and intf3 (DP): not read (nothing drives DP)
PianoDpu: intf1 INTF1 (DSI0): engine on; htotal 576 (hsync 16 hbp 16 active 427 hfp 117) vtotal 2310 (vsync 2 vbp 104 active 2136 vfp 68); ...
PianoDpu: dsi0: ctrl ........ (on, video, 4 lanes, clock lane) ...: Ubuntu's 120 Hz mode (full-panel HFP 234, vtotal 2310); compression on ...
PianoDpu: panel id gpio100: cfg ... io ... -> p81 42-02-0a (rate-group registers B3 then B2)     (or 35-02-0b, B2 then B3)
PianoDpu: SMMU scr0 ... idr1 60000053: 127 stream-match groups, 83 banks, 128 pages
PianoDpu: SMMU group N smr 80020800 (id 0x800 mask 0x2) s2cr ........ (translate, bank 2) cbar ........ (stage 1, stage 2 bypass) ...
PianoDpu: SMMU bank 2: sctlr ........ (translation off|on, ...) tcr ... ttbr0 ... mair0 ... fsr 00000000
PianoDpu: refresh: intf1 frames A -> A+120 in 100xxxx us: 120.0xx Hz (INTF pixel clock ~159667 kHz); line ...
PianoDpu: path: VIG0+VIG1 (2 rects) from the splash, stride 12800 -> ctl0 (video, 1 CTLs on the DSI INTFs) -> LM0+LM1 (staged: yes) -> dsc 03 -> INTF1+INTF2 (dual DSI); 120.0xx Hz measured
PianoDpu: zero-copy: flip = the SRC address of VIG0+VIG1 (both rects when multirect) + ctl0 FLUSH 00000003 (no START in video mode); format takes a 'BGRA' IOSurface as is ...
PianoDpu: zero-copy route: 0x800 -> a stage-1 bank with translation off (IOVA = PA): ShengUfs's recipe applies ...   (or: translation on ...)
PianoDpu: summary: pipes ... stride 12800 ctl0 lm LM0+LM1 intf INTF1+INTF2 dsc 03 video 120.0xx Hz smmu bank-off bank 2 panel p81 ...
PianoDpu: stage 5: done
PianoDpu: done: N MMIO loads, 0 stores; P pages mapped, 0 map failures
```
The pipe names, alpha, timing numbers and route are what B1a is for: the INTF's compressed width (427) and wide-bus split are
inferred from Linux and may read differently. Failure lines: `gate: MDSS AHB clock off: ...` (nothing of MDSS read: the next image
adds a one-register AHB vote before the survey, attended), `dsi1: not read (its byte clock ... or pixel clock ... is off)` (the
slave link's clocks are not where Linux has them: record the CBCRs), `refresh: intf1 frame counter did not move ... disabled` (counters
off: the refresh then comes from B1b's vsync interrupt), a hang before `stage 2` lines (an ungated block: report the last line).
With a `bank-on` route, run once more with `piano-dpu-smmu=3`: `walk: iova 0xfc800000 -> pa 0xfc800000 ... (identity)`.

**B1b ladder** (built and host-tested 2026-11-11, not run; each step a separate RAM image, owner present, in order: a step runs
only after the previous one passed). Every step starts with B1a's lines (the survey is the path's source), then runs within a second
or two of PianoDpu's start, long before WindowServer; steps 1-3 change nothing after they end. The images:
```sh
# N = 1, 2, 3 (5 minutes is plenty)
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-dpu=2 piano-dpu-flip=N" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-dpu2-N && cp -R out/ram out/ram-dpu2-N
# step 4: the desktop through flips, 30 minutes ('BGRA' frames need vmm-present 0)
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=2100 \
  PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-dpu=2 piano-dpu-flip=4 native-gop-display-flip=1" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-dpu2-4 && cp -R out/ram out/ram-dpu2-4
grep -a "PianoDpu\|NativeGopDisplay: flip" logs/usb-console/<newest>-port0.txt
```
(`...` = values to record; the latch times are anywhere from a few µs to one frame, 8.3 ms at 120 Hz.)

*Step 1* (`piano-dpu-flip=1`): the boot framebuffer's own addresses written again and flushed, three times a second apart. Pass:
"latched in" below ~8400 us each time, the frame counter moving, no underrun, the panel unchanged.
```
PianoDpu: stage 6: B1b: zero-copy flips (docs/drivers/dpu.md section 8)
PianoDpu: B1b: piano-dpu-flip=1 irq=1 clean=1 flush=0 underrun=1 latch=50 ms watch=10 s smmu=2
PianoDpu: stage 7: B1b step 1: the boot framebuffer's addresses flushed again
PianoDpu: B1b: path VIG0+VIG1, ctl0 flush 00000003, alpha on|off...; dpu intr en was ........ (now 00000000), status in 20 ms 28......
          (vsync latches with its enable off, underrun clear), mdss status 00000000; intf1 frames A -> A+2
PianoDpu: vsync: SPI 83 (INTID 115, level) attached (0): readback 0xa003 (group 1 1, enabled 1, edge 0, priority 0xa0), router 0x...;
          INTF1's vsync wakes a latch wait, enabled only while a flush is pending
PianoDpu: flip 1: reflush 1/3: ctl0 flush 00000003 latched in .... us (1 vsync interrupts, 1 waits), frames B -> B+1, underrun none: pass
PianoDpu: flip 1: reflush 2/3: ...: pass
PianoDpu: flip 1: reflush 3/3: ...: pass
PianoDpu: B1b step 1 passed: 3 reflushes latched; the next image takes piano-dpu-flip=2
PianoDpu: B1b: N DPU stores, 0 SMMU stores (all to bank page 0), 0 refused stores
PianoDpu: stage 8: B1b: done
```
Failure lines: `reflush 1/3: ... did NOT latch ...: FAIL` (the pipes' bits alone do not latch on DPU 12: the next image adds the
CTL's own bit, `piano-dpu-flush=0x20003`; or the flush register reads back differently: record `flush ........`), `vsync does NOT
latch with its enable off` (the underruns then latch through their enables; nothing else changes), `mdss status ........ (another
block holds the line: no interrupt)` (polled: the step still runs), `vsync: ... NOT attached` (polled), `B1b: not flippable: ...`
(the survey's path is not what §2 expects: the B1a lines say why), `an underrun before any flip` (`piano-dpu-underrun=0` to go on).

*Step 2* (`piano-dpu-flip=2`): after one reflush, the boot framebuffer shown 64 rows lower for 2 s (the picture jumps up by 64 rows,
whatever follows the framebuffer in the splash region at the bottom), then back. No SMMU. Pass: both latched, no underrun, the panel
as before afterwards.
```
PianoDpu: flip 2: the boot framebuffer + 64 rows (address fc8c8000): latched in .... us; held 2000 ms
PianoDpu: flip 2: back to the boot framebuffer: latched in .... us; underrun none: pass
PianoDpu: B1b step 2 passed; the next image takes piano-dpu-flip=3
```

*Step 3* (`piano-dpu-flip=3`): after one reflush, the DPU's bank in place (translation on with the identity blocks, the display's
own fetches watched for 50 ms), a kernel test pattern (8 colour bars white..black as `native-gop-display-bars`, a 16-pixel white
border, a black-and-white checker at the top left) through window 0 for 2 s, back, the fault status after a minute, the bank back as
found. Pass: the pattern visible for 2 s, every `fsr 00000000`, no underrun, the panel unchanged otherwise.
```
PianoDpu: smmu: bank 2 (group N, stage 1, cbar 0001.... cba2r 00000001) as found: sctlr 000000e0 tcr ........ tcr2 ........ ttbr0 ...
          mair0 ........; idr0 ........ idr2 ........ (output NN bits); our table at 0x8........ (66 pages): identity
          0x80000000-0xffffffff, windows 0x10000000 + 4 x 32 MiB
PianoDpu: smmu: translation on in place: sctlr 00000061 tcr 00800020 tcr2 0003800N ttbr0 ........ mair0 00000044; fsr 0 for 50000 us of
          the display's own fetches
PianoDpu: flip 3: the test pattern (8 colour bars, a white border, a checker at the top left) at iova 0x10000000 (6676 pages): latched
          in .... us, fsr 00000000, underrun none; held 2000 ms
PianoDpu: flip 3: back to the boot framebuffer: latched in .... us; fsr 00000000, underrun none while shown
PianoDpu: flip 3: translation stays on with the identity blocks for 60 s, then the fault status again
PianoDpu: flip 3: after 60 s: fsr 00000000, underrun none: pass
PianoDpu: smmu: translation back as found (step 3 done): done (sctlr 000000e0 ttbr0 ... tcr ...)
PianoDpu: B1b step 3 passed; the next image takes piano-dpu-flip=4 with native-gop-display-flip=1
PianoDpu: B1b: N DPU stores, M SMMU stores (all to bank page 0x82000), 0 refused stores
```
(bank page 0x82000 = (128 pages + bank 2) × 4 KiB; another bank number moves it.) Failure lines: `smmu: translation NOT on: a fault
while the display fetched through the new table; fsr ... far ... fsynr0 ...` (FSYNR0's level field 0 means the walk started at
level 0 as on SM8550 before SEP 7: record it; a self-referencing L1 is the fallback, not built), `the bank ignores our writes (TTBR0/TCR
readback)` (the hypervisor owns the bank: B1b stops at the copy), `smmu: not taken: ...` (the route or the bank kind: the reason
says which; a bank with translation on needs `piano-dpu-smmu=3` and the walk's `map the boot framebuffer to itself` line), `revert:
...` lines (a fault or an underrun on the pattern: the boot framebuffer is back, the bank as found).

*Step 4* (`piano-dpu-flip=4 native-gop-display-flip=1`): the bank in place as in step 3, then the flips served; NativeGopDisplay
finds PianoDpu at its first swaps. Pass: the desktop shows (login, cursor), the cursor blinks, a dragged window does not tear,
NativeGopDisplay's periodic line shows `N swaps flipped ..., 0 copied` and its copy count near 0, CPU idle near 0 with a blinking
cursor, the summary's vsync interrupt count grows only while something moves, 30 minutes without a `revert:` line. Then once with
`PIANO_VMM_PRESENT=1`: every swap refused as `format` and copied, the desktop as before.
```
PianoDpu: B1b: serving flips (NativeGopDisplay with native-gop-display-flip=1): 4 windows, clean 1, latch timeout 50 ms, watch 10 s,
          vsync interrupt
NativeGopDisplay: flip mode (native-gop-display-flip): each swap's surface to PianoDpu's flip (piano-dpu=2 piano-dpu-flip=4), copied
          when refused; a swap completes once on the panel
NativeGopDisplay: flips through PianoDpu: a 'BGRA' swap is shown by the DPU itself, the others are copied
PianoDpu: flip 1: 3200x2136 row 12800 -> window 0 iova 0x10000000 (mapped now), clean .... us, ctl0 flush 00000003 latched in .... us
          (1 vsync interrupts)                                                     (the first 8 flips, then:)
PianoDpu: flips: N in the last 60 s (N in all: latch mean ... us max ... us; ... shown again; ... maps mean ... us; ... cleans mean ...
          us, 0 without; ... back to the boot framebuffer); refused: format 0 planes 0 size 0 stride 0 align 0 memory 0 slots 0 clean 0
          off 0; copies avoided N, CPU time saved ~... ms (copies at ... us mean minus ... ms of flip work); vsync interrupts N (live);
          state serving                                                            (at most once a minute, when anything happened)
NativeGopDisplay: flip mode: N swaps flipped (no copy, no refresh copies), M copied (PianoDpu refused them or is not serving)
```
Record: the latch mean/max, the clean mean (the CPU cost of a flip: a 27 MB clean by VA), `without` (a cacheable alias refused:
then the CPU's caches were not cleaned; a stale tile on the panel would say the frame needs it: `piano-dpu-clean=2`), the maps count
(WindowServer's swap chain length). Failure lines: `NativeGopDisplay: flip refused (...)` (the first 8, with the reason), `revert:
...` then `B1b off for the boot: ...` (the copy from then on), `flip: SMMU fault: fsr ... far ...` / `flip: underrun ...` / `watch:
...`, `vsync: 3 waits of 10 ms in a row with no interrupt ...: polled from now on`. A separate cursor layer (`swap layers 0x3`) is not shown in either mode (the
largest layer only, §4.2.6).

**B1c**: `piano-dpu-hz=60` then the idle policy; pass: B1a's counter reads
60.0/120.0 Hz, no grey screen or tearing, touch works at 60 Hz, 100 switches clean. **B1d**: identity PCC (no change), then blue
0.5 on both DSPPs (both halves equally warm), then Night Shift's schedule; survives display sleep/wake. **B1e**: `pmset
displaysleepnow` / wake 50 cycles; pass: no garbled frame, touch and backlight follow, MDSS IRQ 0 while asleep.

## 9. Fallback

Without `piano-dpu` nothing is mapped. Every later level falls back to NativeGopDisplay's copy per swap (refusal) or for the boot (a
fault or a latch timeout re-points the pipes at the splash, which the copy keeps current). A hang or reset returns to Ubuntu (RAM
boots, `piano-reset-after`). B1b as built: without `native-gop-display-flip` NativeGopDisplay is unchanged whatever PianoDpu does;
with it and PianoDpu absent or below step 4, every swap is refused (`off`) and copied, its completion only waiting for the copy. A
step that fails stops the ladder with the boot framebuffer on the panel (steps 1-3 leave nothing changed: their reverts and the
bank's undo return every register they wrote to its found value, except TOP INTR_EN, which stays off, the status bits they cleared,
and the SPI 83 handler, which stays attached: ArmCpu has no detach); step 4's failures turn flips off for the boot (§4.2.5).

## 10. Open questions

1. Is the MDSS AHB clock on at XNU time? → B1a gate line. If not, one vote (DISPCC 0x80b0 enable) precedes every later level.
2. Which pipes, rects, alpha, DSPP in path; INTF compressed width and wide bus? → B1a.
3. Bank 2: translation on or off, and is it the HLOS's to reprogram under Gunyah (sheng: yes on SM8550)? → B1a, then B1b(3).
4. Is TCR2 PASize 36 bits enough, and does the walker accept tables above 4 GiB? → B1b(3) (sheng used 48-bit, tables in RAM).
5. Exact DSI timing shadow offsets (0x1e4/0x1e8 relative to msm's shifted base) → B1a raw words, B1c.
6. Which IOMFB call carries Night Shift on this external-type display (`swap_set_color_matrix` or `set_gamma_table`)? → B1d-0 log.
7. Does WindowServer accept a refresh change under one advertised timing (frames at 60 while it paces at 120)? → B1c.
8. Panel sleep via the DSI command FIFO while video runs: does the NT36532 need LP or HS commands (XML: `dsi_hs_mode` for rate
   switches)? → B1e.
9. Does an SSPP-only CTL_FLUSH latch on DPU 12 (sheng's DPU 9.2 did), or does it need the CTL's own bit 17? → B1b step 1
   (`piano-dpu-flush=0x20003` is the second try).
10. Does TOP INTR_STATUS latch the vsync and underrun bits with their enables off (msm reads it that way)? → B1b setup line.
11. Does XNU make a read-only cacheable alias of WindowServer's surfaces (the clean), and what does a 27 MB clean by VA cost? → step 4
    summary (`cleans mean`, `without`).
12. Does WindowServer keep up when a swap completes only at the latch (a real display's pace) instead of at once, and does it never
    draw into the surface on the panel (the family's in-use count, not NativeGopDisplay's `swap_wait` answer, guards that)? → step 4:
    frames per second in NativeGopDisplay's line, no tearing.
13. Under Gunyah, are the DPU bank's TTBR0/TCR/SCTLR the HLOS's to write (sheng: yes), and is IDR2's output size real? → step 3.

## 11. Licence note

The msm DPU/DSI/SMMU and panel drivers, SunUEFI's Linux patches (GPL-2.0) and the stock panel XML are read as the specification
only (offsets, fields, sequences, delays); nothing is copied. PianoDpu is BSD-2-Clause-Patent; its shape comes from PianoAudio
(this repository). GopFramebuffer and ShengUfs (BSD-2-Clause-Patent) are the named donors of B1b: B1b re-expresses their address-only
flip, latch poll, window slots and in-place bank enable (with its SEP 7, terminate-on-fault and undo facts) in new code; the SMMU
registers and the descriptor format follow the ARM SMMU v2 architecture and VMSAv8-64 (ARM's public specifications). K90 (APSL 2.0)
was read for facts only. Provenance: `package/PianoDpu/UPSTREAM`.
