# Native DPU display path: survey (B1a), zero-copy flips (B1b), refresh (B1c), Night Light (B1d), panel off/on (B1e)

Status, 2026-11-12: **B1a passed on the tablet** with `piano-dpu-ahb-vote=1` (dpu-ahb1, `logs/usb-console/20261112-083454`): VIG0+VIG1
1600x2136 from the splash (stride 12800, 'BGRA' with SRCC3_EN, source split: VIG1's source and output at x 1600, both pipes at stage
5 of LM0 and LM1, blend op 0x400, constant alpha 0xffff0000), ctl0 video mode on INTF1+INTF2, DSC 03, 119.878 Hz, DMA0..5 and VIG2/3
idle, the SMMU route 0x800 -> bank 2 (stage 1, AArch64), translation off: B1b's bank-off route. B1b's ladder is next (§8: its images
need no other argument than `piano-dpu=2 piano-dpu-flip=N` since the vote rule is level 2's default). Status, 2026-11-11: **B1a built and host-tested** (`package/PianoDpu`, `piano-dpu=1`, in the kext list, inert without the argument);
not run on the tablet. **B1b built and host-tested** (`piano-dpu=2` with the ladder `piano-dpu-flip=1..4`, NativeGopDisplay's opt-in
`native-gop-display-flip=1`; §4.2, §8); not run on the tablet. **B1c, B1d, B1e built and host-tested**
(`piano-dpu-refresh`, `piano-dpu-pcc`, `piano-dpu-panel`, each opt-in on top of `piano-dpu` >= 1; `DpuSlices.cpp`, NativeGopDisplay's
callouts behind the same arguments; §4.3-4.5, §8.1-8.3); not run on the tablet. Template: `README.md`. Supersedes `display.md`
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
| Clocks | MDSS AHB may be gated (an SM8750 phone read it off and hung on an ungated read). On piano under XNU (2026-11-12, dpu1, dpu-c0, the panel lit): GCC_DISP_AHB_CBCR (GCC 0x27004) **0x88000003** = CLK_ENABLE (bit 0) and HW_CTL (bit 1, hardware clock gating) set, CLK_OFF (bit 31) set: enabled and hardware-gated, idle between accesses; hf-axi 0x08200001 running. Linux only sets bit 0 on it (gcc-sm8750's kept-on list) and its halt check skips CLK_OFF in hardware gating mode | Adreno830 docs, K90; tablet; msm gcc-sm8750.c, clk-branch.c | the reading: verified; that DISPCC reads behind it work: **needs-boot (B1a with `piano-dpu-ahb-vote`)** |
| Underrun, engine | TOP0 INTR_STATUS bits 26/28 latch INTF1/INTF2 underruns, enabled or not (INTR_CLEAR +0x18, write-one-to-clear); INTF_STATUS +0x26c bit 0 = engine running (EN = 0 stops it at its frame end); INTF_CONFIG bit 31 = programmable fetch; COUNT_EN 3 = both counters | msm dpu_hwio.h, dpu_hw_intf.c, catalog | verified (sources) |
| CTL flush | CTL_INTF_FLUSH +0x110 bit n = INTF n, CTL_DSPP_n_FLUSH bit 4 = PCC, CTL_FLUSH bit 31 (INTFs) / 29 (DSPPs); cleared by the hardware at the latch; video mode: the flush before a timing engine starts | msm dpu_hw_ctl.c, dpu_encoder_phys_vid.c | verified (sources); write-one-to-set: inferred |
| DSI command path | per controller (offsets from its base): CTRL bit 2 command mode (set beside video for a transfer); CMD_DMA_CTRL +0x3c (31 broadcast, 30 master, 28 from buffer/FIFO, 26 low power); DMA length +0x4c; TRIG_CTRL +0x84 (DMA trigger 2:0, 4 = software; bit 12 BLOCK_DMA_WITHIN_FRAME); TRIG_DMA +0x90; STATUS0 bit 1 DMA busy; INTR_CTRL +0x110 (bit 0 DMA done, write-one-to-clear; odd bits enables); TEST_PATTERN_GEN_CTRL +0x15c = 0x30006 makes the DMA read the controller's command FIFO +0x17c (no DMA buffer through the SMMU); +0x390 bit 1 cleared for the transfer; short write = header 0x80000000 \| 0x05 << 16 \| command, a pad word, length 4 | msm dsi.xml, dsi_host.c; UniXNU-sm8750-common PeripheralIO.h | verified (sources); +0x390's meaning, TRIG_CTRL as left: needs-boot (B1e logs them) |
| DCS mode | the panel's commands go in low power (MIPI_DSI_MODE_LPM), on both links (DSI0 then DSI1) | panel | verified (source) |
| Panel sleep | off: 0x28, 20 ms, 0x10, 100 ms; on: 0x11, 120 ms, 0x29, 10 ms; the stock term sequence is the same (05 28, FF 14, 05 10, FF 64) | panel, XML | verified (sources) |
| DSI timing shadow | SunUEFI's working patch: DB mode base + 0x1ec, timing flush + 0x1e8 (msm-relative 0x1e8/0x1e4); downstream's list as the UniXNU ports read it: + 0x1e8 / + 0x1e4, with + 0x1ec the TPG FIFO reset PeripheralIO pulses | SunUEFI, PeripheralIO | **conflicting: needs-boot** (`piano-dpu-refresh-db` selects; default SunUEFI's) |
| macOS's colour inputs | `swap_set_color_matrix`: IOMFBColorFixedMatrix taken as 9 × u64 S31.32, [output][input], 1.0 = 1 << 32 (Asahi's DCP driver sends that layout for the same-named type); `set_gamma_table`: IOMFBGammaTable taken as 3 × (1 + 256) u32, 0xc0c bytes (iOS night-light tools' layout) | Asahi, GammaThingy (facts) | **inferred: needs-boot (B1d-0 logs the raw bytes)** |

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
6. *Cursor*: since 2026-11-12 NativeGopDisplay composes WindowServer's cursor layer over the frame in the boot framebuffer
   (native-gop-display.md §5a); in flip mode a frame is offered to the DPU only while no cursor is on the panel, else composed and
   the boot framebuffer put back (flips resume once the cursor hides). Since the evening of 2026-11-12 the frames under a cursor are
   damage copies (native-gop-display.md §5c), so flip mode with a cursor costs one whole composition when the cursor shows up over a
   flipped frame and then only each frame's dirty region. *The cursor plane* (step 5, designed, not built: the frame stays flipped
   under the cursor), from the survey and msm's SM8750 catalog and v12 code read as the specification:
   - the pipe: DMA4 (msm marks DMA4/DMA5 cursor-capable), idle in the survey; one rect (no multirect); SRC_SIZE/SRC_XY the glyph
     rectangle of WindowServer's 128x128 cursor surface, OUT_SIZE/OUT_XY its place in panel coordinates (the survey: VIG1's output is
     at x 1600, so with source split a pipe's output is in the mixer pair's coordinates, and msm leaves a pipe narrower than
     max_linewidth (8192) whole across the LM0/LM1 boundary, staged in both LMs), clipped at the panel's edges; SRC0_ADDR the cursor
     surface through an SMMU window of its own (16 pages, a fifth small slot), YSTRIDE0 its row bytes; SRC_FORMAT 0x000237ff and
     UNPACK 0x03020001 as VIG0's, with SRCC3_EN kept (per-pixel alpha); SRC_OP_MODE 0x80000000 (PE_OVERRIDE, as msm and VIG0); the
     QoS, CDP and clock-control registers left as reset (a 128x128 pipe is ~8 MB/s at 120 Hz) unless the step's underrun check says
     otherwise;
   - the mixers: LM0 and LM1 stage 6 (0x40 + 0x30 * 6, above the splash pipes' stage 5): FG_SRC_SEL byte 0 = DMA type 0, rect 0, id 4
     (0x04), byte 1 none (0xc0); BLEND_OP 0x600 (FG constant alpha, BG alpha from the FG pixel, inverted: premultiplied over, msm's
     DRM_MODE_BLEND_PREMULTI with an opaque plane), CONST_ALPHA 0x03ff0000 (v12: 10-bit, fg << 16 | bg); hidden: FG_SRC_SEL 0xc0c0;
   - the CTL: PIPE_ACTIVE and FETCH_ACTIVE with DMA4's bit (4) added, CTL_FLUSH DMA4's bit (13) plus LM0/LM1's (6, 7) when the
     staging changes, a move or a new image only DMA4's; one flush with the frame's VIG0/VIG1 bits when both change;
   - NativeGopDisplay: with the plane serving, a frame flips whatever the cursor does and a cursor-only swap is a cursor request
     (position, image, hidden), not a copy; the plane's fault and underrun checks are the flips' (a revert hides it).
   Ladder: step 5 = step 4 plus a test cursor (a 128x128 checker with a transparent hole) shown at the panel's centre, across the
   LM split at x 1600 and off the right edge, 1 s each, over the boot framebuffer, then hidden; pass: visible at each place, the
   hole shows the panel through it, fsr 0, no underrun.

**4.3 Refresh switching (B1c).** 120 ↔ 60 Hz is a VFP-only change: no DCS command (B2/B3 are the same), no clock change, no DSC or
touch impact (SunUEFI ran 30/48/50/60/120 seamlessly). At a frame boundary, on both links: DSI DB_MODE = 1, TOTAL.V_TOTAL = 4620 − 1
(2310 − 1 back), TIMING_FLUSH = 1; INTF1/INTF2 VSYNC_PERIOD_F0 = 4620·htotal; CTL_INTF_FLUSH (INTF1|INTF2) + CTL_FLUSH b31; DB_MODE =
0 after the latch. Programmable fetch stays off (the 120 Hz family has 106 lines of VSYNC+VBP, more than the 24-line worst case, so
none is needed; a moving prefetch offset would need its own update). *Implemented* (`DpuRefresh.h`): the twelve stores are one list
(`plan()`, logged as is by `piano-dpu-refresh=plan`): DSI0 then DSI1 (the clock master first, SunUEFI's stock order; every store of
the frame is a shadow taken at the same frame start, so the order does not change the latch), then the INTFs and the CTL, all with
interrupts off inside INTF1's lines 200..2000 (a frame start at least 310 lines away); DB mode cleared after CTL_FLUSH bit 31 reads 0.
Verified: both periods read back, the refresh measured frame edge to frame edge over 250 ms (60.0 or 120.0 Hz within 2 %), INTF2
counting INTF1's frames (the halves in lockstep), no underrun (cleared first). Any anomaly switches back the same way and turns the
slice off for the boot. Refused before any store: not the 120 Hz family, a DSI TOTAL that disagrees with its INTF, programmable fetch
on, a gate closed. Policy (`auto`): 60 Hz once no *burst* of swaps (2 within 250 ms) came for 1 s, 120 Hz at the next burst, 200 ms
dwell: a blinking cursor (2 swaps/s) is no burst and stays at 60. NativeGopDisplay reports every frame it shows (`PianoDpuSwap`);
WindowServer keeps seeing one timing (frames simply take two periods while idle) or, later, a 60/120 pair. 90/144 change HFP and the panel's rate group (B2/B3 over the DSI command path while video runs):
a full modeset in both Linux ports, and more power, so not planned. Risks: a missed DSI/INTF pairing shows a grey or torn screen
(Ubuntu's full-modeset 60 Hz was grey until the stock order was used); the TDDI touch follows display timing (watch touch at 60 Hz);
the DSI shadow offsets are confirmed by B1a's raw `timing words`.

**4.4 Night Light (B1d).** PCC in DSPP0 and DSPP1 (both halves, same matrix): output R from (R,G,B) at +0x10/+0x14/+0x18, G
+0x1c/+0x20/+0x24, B +0x28/+0x2c/+0x30 (msm's PCC_<input>_<output>_OFF names), S3.15; enable +0 bit 0; flush CTL_DSPP0/1_FLUSH bit 4
+ CTL_FLUSH b29, latched with the frame. A warm white point is a diagonal (e.g. 3400 K ≈ 1.0, 0.73, 0.45). macOS reaches the display through IOMFB's `swap_set_color_matrix` (slot
343) and `set_gamma_table` (slot 307), both stubs in NativeGopDisplay unless `piano-dpu-pcc` is given: B1d-0 logs which one Night
Shift calls; a per-channel linear gamma ramp maps exactly onto a PCC diagonal, a non-linear one needs GC v2 via REGDMA (later).
Re-applied at display-on; no CPU cost per frame. Needs the DSPP in ABL's path (B1a reads the PCC/GC enables; LM n → DSPP n is the
catalog's wiring). *Implemented* (`DpuPcc.h`): the matrix decoded as S31.32 (each negative word read as two's complement or as
sign-magnitude, whichever is within ±4; else refused), one per IOMFBColorMatrixFunction, their product; a diagonal is moved from
linear light (where a display pipeline applies it) to the encoded values the PCC sees, d → d^(1/2.2) from a 257-entry table (no
floating point; `piano-dpu-pcc-encode=0` sends it as is), any other matrix is applied as sent and logged as an approximation;
negative coefficients clamped to 0 (msm drops the sign when it converts a CTM, so the PCC's negative encoding is unproven;
`piano-dpu-pcc-negative=1` sends two's complement). With `piano-dpu-pcc=2` a gamma table whose three ramps are scaled copies
(within 2 %) becomes the diagonal of their scales. The stores: every DSPP of the path the same, the nine words then the enable,
CTL_DSPP_n_FLUSH bit 4, CTL_FLUSH bit 29, the latch (60 ms) and a read-back. At the start the PCC as found (the identity, normally)
is written once: the write path checked with no visible change; a found non-identity matrix is kept as the base, macOS's applied
before it. A matrix that arrives while the panel sleeps is kept and written at the wake (the light sleep keeps the DSPP's registers
anyway).

**4.5 Panel off/on (B1e).** Display sleep today only turns the backlight off: the DPU still fetches 3.28 GB/s and the panel scans.
Off: backlight off (PianoBacklight) → DCS 0x28, 20 ms, 0x10, 100 ms (K:72cfce4c86e5) through the DSI command FIFO (no DMA buffer:
`UniXNU-sm8750-common` PeripheralIO's DSI0 FIFO helper is the BSD template) → stop INTF2's timing engine, then INTF1's (slave
first). Supplies, reset and MDSS stay on (no GDSC collapse, no CTL reset while anything scans). On: INTF1+INTF2 engines on, 0x11,
120 ms, 0x29, backlight on. A full power-off (reset, supplies) needs the whole init and PPS: later, if the light sleep is not enough.
*Implemented* (`DpuPanel.h`): per DCS command, on both links: refused unless the controller is enabled in video mode, its FIFO is
free and its DMA is software-triggered; STATUS0's DMA busy bit waited for (20 ms); INTR_CTRL's done bit cleared, CTRL's command mode
set beside video (msm's transfer), the FIFO selected and filled, CMD_DMA_CTRL low power (the panel's mode) without broadcast or
master, +0x390 bit 1 cleared, the length; then TRIG_DMA on DSI0 and DSI1 back to back. The hardware holds the DMA out of the active
frame (BLOCK_DMA_WITHIN_FRAME); where that bit is clear the trigger waits for INTF1's front porch. Done within 50 ms on both, every
word restored as found (PeripheralIO's restore, its +0x1ec pulse included); a link whose DMA did not finish is left to its engine and
the slice stops. Engines: EN = 0 on INTF2, INTF_STATUS bit 0 clear within 40 ms (Ubuntu's slave poll), then INTF1. Wake: the CTL's
INTF flush, then INTF1 and INTF2 enabled with interrupts off (msm: flush before the engine, master first), both counting in lockstep,
then 0x11, 120 ms, 0x29, 10 ms, the PCC re-applied. Failures undo what was done (engines on again, sleep-out, display-on); after a
failed stop of INTF1 the stopped INTF2 is started at INTF1's frame start so the halves stay in phase. The order against the backlight:
NativeGopDisplay records PianoBacklight's request first and PianoDpu's second; both run on their own threads, so at the wake the
backlight may light ~150 ms before display-on (a lit black panel, no garbage: a panel in sleep or display-off shows black).

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
latch / fault / underrun). With a writing slice on (at any level), the service is registered too and answers (each only records a
request and wakes the slices' thread):

| Function | Caller | Parameters |
|---|---|---|
| `PianoDpuSwap` | NativeGopDisplay's `program_hardware_gated`, each frame shown (`piano-dpu-refresh=auto`) | none |
| `PianoDpuColorMatrix` | NativeGopDisplay's `swap_set_color_matrix`, each new matrix of a function (`piano-dpu-pcc`) | p1 = the 72 bytes, p2 = the function, p3 = the third argument |
| `PianoDpuGammaTable` | NativeGopDisplay's `set_gamma_table`, each new table (`piano-dpu-pcc`) | p1 = the words, p2 = 771 with `piano-dpu-pcc=2`, else 0 (logged only) |
| `PianoDpuPanel` | NativeGopDisplay's `do_power_state_change_gated`, each change (`piano-dpu-panel=1`) | p1 = on |

Properties: `piano-dpu-refresh-hz` (60/120), `piano-dpu-panel` (`awake`/`asleep`), `piano-dpu-pcc` (the diagonal in thousandths).
All from the IOMobileFramebufferLegacy hooks (the 27.0.1 ABI generated at build time).

## 6. Boot args

| Argument | Default | Meaning |
|---|---|---|
| `piano-dpu` | 0 | 1: the read-only survey (loads only); 2: the survey, then B1b's ladder (`piano-dpu-flip`) |
| `piano-dpu-ahb-vote=0\|1\|2` | 0 at level 1, 1 at level 2 (2026-11-12) | §8 "B1a with the AHB vote": 1 the gate takes Linux's rule for GCC's hardware-gated display AHB branch (CLK_ENABLE set only if clear: nothing written on the tablet), 2 also forces it on. Level 2 needs the survey's path, which needs the gate open: dpu-ahb1 passed with 1 |
| `piano-dpu-flip=N` | 1 | B1b's steps (§8): 1 the boot framebuffer's addresses flushed again ×3; 2 + shifted 64 rows for 2 s and back; 3 + the DPU's SMMU bank in place, a test pattern through window 0 for 2 s, the fault status after a minute, the bank back as found; 4 the bank in place and flips served to NativeGopDisplay |
| `piano-dpu-irq=0` | 1 | CTL_FLUSH polled every ~1 ms instead of the vsync interrupt (SPI 83) |
| `piano-dpu-clean=N` | 1 | a surface's fetched rows cleaned from the CPU caches before each flip: 0 never, 1 through a cacheable alias when XNU makes one, 2 refuse a surface without one |
| `piano-dpu-flush=MASK` | 0 | the CTL_FLUSH bits of a flip instead of the pipes' own (0x3 for VIG0+VIG1); e.g. 0x20003 adds the CTL's bit 17 if the pipes' bits alone do not latch |
| `piano-dpu-underrun=0` | 1 | an underrun neither stops the ladder nor reverts |
| `piano-dpu-latch=MS` | 50 | the latch timeout (10..500): past it, a revert and flips off for the boot |
| `piano-dpu-watch=S` | 10 | the fault/underrun check while a surface is on the panel (0 off; nothing wakes while the boot framebuffer is) |
| `native-gop-display-flip=0\|1` | 1 with `piano-dpu=2 piano-dpu-flip=4`, else 0 (2026-11-12) | (NativeGopDisplay's) each swap's surface to PianoDpu's flip, the copy (damage copies, native-gop-display.md §5c) when refused or while a cursor is on the panel; 0 keeps the copy at step 4 |
| `piano-dpu-smmu=N` | 2 | 0 skip the SMMU; 1 global pages; 2 also the bank's registers (B1b steps 3-4 need them); 3 also walk the bank's translation (loads from the boot loader's table memory: opt-in; with it B1b may take over a bank the boot loader translates) |
| `piano-dpu-measure=MS` | 1000 | the frame-counter interval (20..10000) |
| `piano-dpu-delay=S`, `piano-dpu-repeat=S`, `piano-dpu-samples=N` | 0, 0, 3 | wait first; re-measure every S s, N times (after WindowServer starts, across display sleep) |
| `piano-dpu-refresh=plan\|60\|120\|auto` | off | B1c (needs `piano-dpu` >= 1): plan = the verdict and the twelve stores logged, nothing written; 60/120 = switch once at the start; auto = the idle policy (NativeGopDisplay reports swaps with the same argument) |
| `piano-dpu-refresh-db=0\|1\|2` | 1 | the DSI timing shadow: 1 SunUEFI's place (DB + 0x1ec, flush + 0x1e8), 2 downstream's (+ 0x1e8, + 0x1e4), 0 TOTAL written directly |
| `piano-dpu-refresh-test=N` | 0 | first N round trips 120 → 60 → 120, a second at each rate; stops at an anomaly |
| `piano-dpu-refresh-idle=MS`, `-burst=N`, `-window=MS`, `-dwell=MS` | 1000, 2, 250, 200 | the policy |
| `piano-dpu-pcc=1\|2` | 0 | B1d: 1 the PCC as found written once, then macOS's colour matrices (NativeGopDisplay logs and forwards `swap_set_color_matrix` and `set_gamma_table`); 2 also fits gamma tables (reads 0xc0c bytes) |
| `piano-dpu-pcc-encode=0`, `piano-dpu-pcc-negative=1` | 1, 0 | a diagonal as sent (default: linear → encoded); negative coefficients as two's complement (default: clamped) |
| `piano-dpu-pcc-test=R,G,B` | none | a test diagonal in thousandths written as the PCC's values after the start (0..4000 each) |
| `piano-dpu-panel=1` | 0 | B1e: the panel's light sleep with display sleep (NativeGopDisplay forwards its display power) |
| `piano-dpu-panel-engines=0`, `piano-dpu-panel-hs=1` | 1, 0 | DCS only (engines keep running); DCS in high speed |
| `piano-dpu-panel-test=N` | 0 | first N sleep/wake cycles, 5 s asleep and 5 s awake each; stops at a failure |

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
B1c-B1e: the writing slices' tests run on `tests/dpu_model.h`, a
timed register model (INTF frames and counters, CTL latches at the frame start, INTF/DSPP/DSI shadows, the DSI timing shadow at either
candidate place, the command DMA from the FIFO in the blanking, the panel halves following DCS, the store guard, fault injection):
`dpu_refresh_host.cpp` (the exact twelve stores, one frame and the line window, 100 round trips, both shadow layouts, the anomalies and
reverts, the policy and a 30 s session), `dpu_pcc_host.cpp` (S31.32 both sign conventions, the product, the gain table against
pow, S3.15 words, the stores read back by the survey's decoder, the gamma fit), `dpu_panel_host.cpp` (the packet, the stores and
their restore, blanking with and without the hardware's block, refusals, the sleep/wake order and timing, 50 cycles, failures).

## 8. Hardware test plan (RAM boots; B1a and B1c's plan unattended, the rest attended)

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
PianoDpu: clocks: gcc disp ahb ........ (enable 1, hw gating ., clk-off .: running|hardware-gated, idle) hf-axi ........ (clk-off 0)
PianoDpu: clocks: dispcc mdss gdsc 8....... (on) ahb ... non-gdsc-ahb ... mdp ... vsync ... pclk .../... byte .../... esc0 ...
PianoDpu: clocks: mdp rcg cmd ... cfg ... (source N, divider ...)
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
inferred from Linux and may read differently. Failure lines: `gate: MDSS AHB clock off: ...` or `gate: GCC display AHB clock off: ...` (nothing of MDSS read: B1a with
the AHB vote, below), `dsi1: not read (its byte clock ... or pixel clock ... is off)` (the
slave link's clocks are not where Linux has them: record the CBCRs), `refresh: intf1 frame counter did not move ... disabled` (counters
off: the refresh then comes from B1b's vsync interrupt), a hang before `stage 2` lines (an ungated block: report the last line).
With a `bank-on` route, run once more with `piano-dpu-smmu=3`: `walk: iova 0xfc800000 -> pa 0xfc800000 ... (identity)`.

**B1a with the AHB vote** (`piano-dpu-ahb-vote`, 2026-11-12, queue5 P13; `DpuAhbVote.h`). dpu1 and dpu-c0 stopped at
`gate: GCC display AHB clock off` with the CBCR at 0x88000003 (§2 Clocks): the software vote the plan was to write is already
there, the branch is hardware-gated. `piano-dpu-ahb-vote=1`: CLK_ENABLE set if clear (none on the tablet), under PianoA830's GCC
vote lock (`sm8750_gcc_vote_owner`, tag "DPU1"), then the gate's GCC check by Linux's rule (enabled + hardware-gated = on);
DISPCC's GDSC/AHB/MDP stay strict. `=2`: also HW_CTL cleared (the branch forced on), the strict rule then wants CLK_OFF clear
within 500 µs before any DISPCC load; the bits it changed are set back after the survey (kept for the boot with `piano-dpu=2` or
a writing slice). Without the argument level 1 stores nothing. Neither bit can stop the scan-out (inferred: the pixels come over
hf-axi and DISPCC's MDP clock; this branch is the register interface and the vote only makes it run more): unattended, the panel
stays lit; the failure mode is a stall on the first DISPCC load, which the Gunyah watchdog or `piano-reset-after` resets into ABL
fastboot (`piano-reset-to=bootloader`).
```sh
# unattended, 5 minutes; then the same with piano-dpu-ahb-vote=2 only if =1 stops after its "clocks: gcc" line
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=300 \
  PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-dpu=1 piano-dpu-ahb-vote=1" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-dpu-ahb1 && cp -R out/ram out/ram-dpu-ahb1
```
Expected, then B1a's lines from `stage 2` on:
```
PianoDpu: level 1: loads only, but piano-dpu-ahb-vote=1: GCC_DISP_AHB_CBCR (GCC 0x27004) enable set if clear, under the GCC vote lock, before the survey
PianoDpu: stage 1: clock gate: piano-dpu-ahb-vote first (GCC_DISP_AHB_CBCR, under the GCC vote lock), then GCC display AHB, DISPCC MDSS GDSC / AHB / MDP (read only)
PianoDpu: ahb vote 1: GCC_DISP_AHB_CBCR 88000003: already enabled (the boot's vote); nothing written; enabled and hardware-gated: on by Linux's rule
PianoDpu: clocks: gcc disp ahb 88000003 (enable 1, hw gating 1, clk-off 1: hardware-gated, idle) hf-axi 08200001 (clk-off 0)
PianoDpu: clocks: dispcc mdss gdsc 8....... (on) ahb ........ ...
PianoDpu: gate: MDSS powered, AHB and MDP clocks on
PianoDpu: stage 2: MDSS, DPU and DSI registers (read only)
...
PianoDpu: done: N MMIO loads, 0 stores; P pages mapped, 0 map failures
```
With `=2`: `ahb vote 2: GCC_DISP_AHB_CBCR 88000003 -> wrote 88000001 (hw gating off) under the GCC vote lock (1 tries); now
08000001 after N us: passes the gate's rule`, at the end `ahb vote 2: undone after the survey: GCC_DISP_AHB_CBCR 88000003 (found
88000003)` and `done: ... 2 stores (piano-dpu-ahb-vote's, GCC_DISP_AHB_CBCR only)`. Failure meanings: the console stops after
the `clocks: gcc` line (a DISPCC load stalled: the hardware gating does not wake on an access from this master; report it, try
`=2`); `gate: MDSS GDSC off` or `MDSS AHB clock off` (DISPCC read fine, its own branches are off under XNU: DISPCC 0x80b0's enable
is the next one-register vote, a later image); `does NOT pass the gate's rule` with `=2` (forcing does not start it: its parent is
off); `the GCC vote lock stayed taken` (another kext held it 50 ms: nothing written, run again).

**B1b ladder** (built and host-tested 2026-11-11, not run; each step a separate RAM image, owner present, in order: a step runs
only after the previous one passed). Every step starts with B1a's lines (the survey is the path's source), then runs within a second
or two of PianoDpu's start, long before WindowServer; steps 1-3 change nothing after they end. The images:
```sh
# N = 1, 2, 3 (5 minutes is plenty)
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-dpu=2 piano-dpu-flip=N" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-dpu2-N && cp -R out/ram out/ram-dpu2-N
# step 4: the desktop through flips, 30 minutes ('BGRA' frames need vmm-present 0); since 2026-11-12 NativeGopDisplay's flip mode is
# on by itself with piano-dpu=2 piano-dpu-flip=4 (native-gop-display-flip=0 keeps the copy)
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=2100 \
  PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-dpu=2 piano-dpu-flip=4" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-dpu2-4 && cp -R out/ram out/ram-dpu2-4
# or step 4 on the desk-dev desktop (its arguments are 959 bytes; these 29 more stay under DarwinBoot's limit)
DESK_RTC_OFFSET=... DESK_EXTRA="piano-dpu=2 piano-dpu-flip=4" tooling/k27/build-desktop-images.sh desk-dev
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
...`, `vsync: 3 waits of 10 ms in a row with no interrupt ...: polled from now on`. A cursor on the panel makes NativeGopDisplay
copy instead of flip (since 2026-11-12, §4.2.6: `flip mode: a cursor is on the panel: ...` once, and the periodic line's `or a cursor
was on the panel: N`); macOS hides the pointer while typing, which is when the flips show.

B1c, B1d, B1e: below (§8.1-8.3).

Every image of B1c-B1e is built like B1a's, with its arguments in `PIANO_EXTRA_ARGS` after `piano-dpu=1`, and copied away (each build
rewrites `out/ram`); `build-ram-images.py` refuses a command line over DarwinBoot's 1023 bytes, so keep each image to its own slice.
The survey's lines (B1a above) come first in every run; the slices start after its `stage 5: done`. `...` = values to record. They
are tested at `piano-dpu=1`, apart from B1b's ladder: with `piano-dpu=2` the slices' thread runs beside B1b's (each waits for its own
CTL_FLUSH bits; B1c clears the INTF underrun bits B1b watches), so the two are combined only after both passed alone.

```sh
img() { PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=${2:-300} PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-dpu=1 $3" \
          tooling/k27/build-native-image.sh && rm -rf "out/ram-dpu-$1" && cp -R out/ram "out/ram-dpu-$1"; }
img c0 300  "piano-dpu-refresh=plan"                          # B1c, unattended: nothing written
img c1 300  "piano-dpu-refresh=60"                            # B1c, attended: switch once, stay at 60
img c2 600  "piano-dpu-refresh=120 piano-dpu-refresh-test=50" # B1c, attended: 100 switches
img c3 900  "piano-dpu-refresh=auto piano-backlight=2"        # B1c, attended, desktop: the policy
img d0 900  "piano-dpu-pcc=1"                                 # B1d-0, attended, desktop: what Night Shift calls
img d1 300  "piano-dpu-pcc=1 piano-dpu-pcc-test=1000,1000,500" # B1d, attended: a visible tint
img e0 300  "piano-dpu-panel=1 piano-dpu-panel-test=3"        # B1e, attended: 3 sleep/wake cycles, backlight left on
img e1 1800 "piano-dpu-panel=1 piano-dpu-pcc=1 piano-dpu-pcc-test=1000,1000,500 piano-backlight=2 piano-usb-shell=1"   # B1e + B1d persistence
```
Run each as B1a (`usb-console.py --capture`, `native-loop.sh out/ram-dpu-<name>/boot-piano-macos-ram-A-bundled.img`), then
`grep -a "PianoDpu\|NativeGopDisplay: \(PianoDpu\|display\|swap_set\|set_gamma\)" logs/usb-console/<newest>-port0.txt`.

### 8.1 B1c refresh

**c0** (plan; nothing on the panel changes, no store):
```
PianoDpu: slices: piano-dpu-refresh=plan pcc=0 panel=0 (B1c/B1d/B1e: these write; the survey above stays read only)
PianoDpu: stage 5: done                                                        (the survey's last lines)
PianoDpu: slices: path: ok; ctl0 (top f0000000 intf 6 master 2) lms 03 dspps 3; intf1 en 1 status 1 frame ..., intf2 en 1 status 1 frame ...
PianoDpu: refresh: plan only (nothing written); now 2310 lines (120 Hz); DSI timing shadow: SunUEFI's (mode +0x1ec, flush +0x1e8); policy idle 1000 ms, burst 2 in 250 ms, dwell 200 ms; the path takes a VFP switch
PianoDpu: refresh: link 0: intf1 config ........ hsync 02400010 period 00144d80 (htotal 576 vtotal 2310) counters 3; dsi0 total 0905023f (vtotal 2310) shadow words ........ ........ ........
PianoDpu: refresh: link 1: intf2 ... (the same) ...; dsi1 total 0905023f (vtotal 2310) shadow words ...
PianoDpu: refresh: plan to 60 Hz, inside INTF1 lines 200..2000 of one frame: 0xae941ec=00000001 0xae9402c=120b023f 0xae941e8=00000001 0xae961ec=00000001 0xae9602c=120b023f 0xae961e8=00000001 0xae3600c=00289b00 0xae3700c=00289b00 0xae16110=00000006 0xae16018=80000000; after the latch 0xae941ec=00000000 0xae961ec=00000000
PianoDpu: slices: serving (refresh off, pcc off, panel off)
```
The INTF's 576-clock line and the DSI's 576-pixel link line are B1a's inference (compressed, wide bus); the plan uses whatever the
registers hold. Record the three `shadow words` per link: they say where the timing shadow is (a DB-mode word reads 0 or 1). A
refusal instead of `the path takes a VFP switch` (`programmable fetch is on`, `a DSI TOTAL does not match`, `INTF vtotal is neither
2310 nor 4620`) means no switch image is run until the survey explains it.

**c1** (60 Hz once): the same start, then (the panel must not flash, go grey or tear; touch keeps working):
```
PianoDpu: refresh: 60 Hz; now 2310 lines (120 Hz); DSI timing shadow: SunUEFI's (mode +0x1ec, flush +0x1e8); ...; the path takes a VFP switch
PianoDpu: refresh: 2310 -> 4620 lines (60 Hz, piano-dpu-refresh=60): ok in ... us: latched in ... us (< 8400), 12 stores, line window 1 of 3; measured 59.9.. Hz (frames 1./1. in 2..... us, lockstep); periods 00289b00/00289b00 dsi totals 120b023f/120b023f; switch 1
PianoDpu: slices: serving (refresh fixed, pcc off, panel off)
```
and the survey's repeated samples (add `piano-dpu-repeat=10`) read `60.0.. Hz`. Failure lines: `refresh: 2310 -> 4620 lines (60 Hz,
...): ANOMALY: <why>; switched back, verified` then `refresh: off for this boot` (the panel back at 120 Hz): `an INTF underran` or
`the measured refresh is not the expected one` with the SunUEFI place → the next image adds `piano-dpu-refresh-db=2`; `CTL_FLUSH bit
31 did not clear within 60 ms` → the CTL latch is not what msm says (report the line); `... SWITCHED BACK BUT THAT FAILED TOO` → a
grey or torn panel until `piano-reset-after`. The touch: with the panel at 60 Hz, `piano-touch=2` keeps reporting (watch it once).

**c2** (100 switches): 100 `refresh: ... ok` lines (the first 50 and the 100th are logged) and
`PianoDpu: refresh: test: 100 of 100 switches ok in ... s`; then `already 120 Hz`. Pass: no visible flicker beyond the rate change,
no anomaly.

**c3** (the policy on the desktop): `NativeGopDisplay: PianoDpu callouts: frames shown (piano-dpu-refresh=auto)`, `NativeGopDisplay:
PianoDpu service found: PianoDpu`, then about a second after the desktop settles `refresh: 2310 -> 4620 lines (60 Hz, idle): ok ...`,
on a scroll or a window drag `refresh: 4620 -> 2310 lines (120 Hz, swaps): ok ...`, and with only the cursor blinking it stays at 60.
`ioreg -l | grep piano-dpu-refresh-hz` follows. Every 10 minutes: `slices: alive: refresh 60 Hz (N switches, 0 failed, N swaps); ...`.

### 8.2 B1d Night Light

**d0** (B1d-0, the desktop; toggle Night Shift: System Settings → Displays → Night Shift → Turn On Until Tomorrow, slider warm):
```
NativeGopDisplay: PianoDpu callouts: colour matrices and gamma tables (piano-dpu-pcc)
PianoDpu: pcc: piano-dpu-pcc=1, dspp mask 3 (LMs 03), found off; encode diagonals to the encoded space, negative clamped to 0; macOS's matrices go in place of it
PianoDpu: pcc: base [1000 0 0 | 0 1000 0 | 0 0 1000] (thousandths, [output][input])
PianoDpu: pcc: the identity (no visible change) -> dspp mask 3: ok; latched in ... us, 23 stores (0 refused), 0 negative clamped, 0 saturated; words R 08000 00000 00000 G 00000 08000 00000 B 00000 00000 08000; apply 1
... then one of:
NativeGopDisplay: swap_set_color_matrix #1 (call N): function F arg 0x..: 0000000100000000 0000000000000000 ... | ... | ... 0000000100000000
PianoDpu: pcc: matrix call 1: function F arg 0x..: (the same words): taken
PianoDpu: pcc: macOS's matrices [1000 0 0 | 0 9.. 0 | 0 0 7..] (thousandths, [output][input])
PianoDpu: pcc: macOS's matrices -> dspp mask 3: ok; latched in ... us, 23 stores ...
... or:
NativeGopDisplay: set_gamma_table #1 (call N): ........ (16 words)
PianoDpu: pcc: gamma call 1: 0 words: piano-dpu-pcc=1: logged only (worst deviation from scaled ramps 0/1000)
```
The point is the raw bytes: the identity must read as `0000000100000000` on the diagonal (S31.32) for the matrix path; any other
shape (32-bit words, 16.16, a header) is reported and DpuPcc.h's decoder follows it. Night Shift's matrix (if it comes this way) should
be a diagonal with R ≈ 1000; the panel then warms on both halves equally. For the gamma path, the 16 words show the header and the
scale: the next image is `piano-dpu-pcc=2` (the fit: `pcc: gamma call N: 771 words: ok (worst deviation ... /1000)`). `matrix call
...: a word is outside +-4.0 read either way` means the layout is not S31.32.

**d1** (a visible tint without macOS): the identity line, then `pcc: test (piano-dpu-pcc-test) [1000 0 0 | 0 1000 0 | 0 0 500]` and
`pcc: the test diagonal -> dspp mask 3: ok; ... words R 08000 00000 00000 G 00000 08000 00000 B 00000 00000 04000; apply 2`. Pass: the
whole panel yellower, both halves the same. `CTL_FLUSH bit 29 did not clear within 60 ms` or `a PCC word did not read back`: the
DSPP is not in ABL's path (the survey's dspp lines say what was found); `pcc: off for this boot after 3 failed writes`.

### 8.3 B1e panel sleep

**e0** (no desktop needed; the backlight stays on, so the panel is lit black for 5 s three times):
```
PianoDpu: panel: dsi0 command path: ctrl ........ status ........ fifo ........ cmd-dma ........ dma1 ........ length ........ trig ........ (dma trigger 4 software, DMA blocked within the frame) intr ........ tpg 00000000 ack ........
PianoDpu: panel: dsi1 command path: ...
PianoDpu: panel: piano-dpu-panel=1: sleep with the display (low-power DCS, then the timing engines stopped); nothing of the panel's reset or supplies is touched
PianoDpu: panel: sleep (display off 0x28, 20 ms, sleep in 0x10, 100 ms, INTF2 then INTF1 stopped): ok (step 4); dcs 28 sent in ... us, 10 sent in ... us; engines intf1 ... us intf2 ... us status 0/0; at 0/2./12./13. ms
PianoDpu: panel: wake (INTF flush, INTF1 + INTF2 on, sleep out 0x11, 120 ms, display on 0x29): ok (step 3); dcs 11 sent in ... us, 29 sent in ... us; engines intf1 ... us intf2 ... us status 1/1, counting in lockstep; at 0/1../2../2.. ms
PianoDpu: panel: test cycle 1 of 3 done                                         (three times)
```
Pass: the picture goes black, comes back whole (no garbled frame, both halves aligned) each time; the survey's samples
(`piano-dpu-repeat=2`) show the INTF1 counter stopped while asleep (sample during the 5 s). The DSI command path line answers open
question 8 (`(dma trigger 4 software, ...)`; `no block: triggered in the front porch` works too). Failure lines: `sleep ...: DSI1's
command DMA did not finish within 50 ms (step 1)` (the FIFO route does not take the command: nothing else was done; report the
command-path lines), `INTF2's timing engine did not stop within 40 ms (step 3), the panel brought back`, `wake ...: a timing engine
did not report running within 50 ms (step 1)` followed by `panel: WAKE FAILED: the panel may stay dark until a reset
(piano-reset-after)`.

**e1** (with WindowServer, 50 cycles: `pmset displaysleepnow` in the root shell, wake with `piano-input.py key shift`):
`NativeGopDisplay: display off` → PianoBacklight's `backlight OFF` lines and `PianoDpu: panel: sleep (...): ok (step 4)`; at the wake
`NativeGopDisplay: display on` → `panel: wake (...): ok`, `pcc: re-applied at the wake [1000 0 0 | 0 1000 0 | 0 0 500]` and its
apply line, then the backlight. Pass: no garbled frame, the tint survives every wake, touch and keyboard work after the wake,
`slices: alive: ... panel awake (50 sleeps, ...)`. The panel may be lit black for ~150 ms before the picture (the backlight's thread is
quicker than the panel's wake): expected.

## 9. Fallback

Without `piano-dpu` nothing is mapped. Every later level falls back to NativeGopDisplay's copy per swap (refusal) or for the boot (a
fault or a latch timeout re-points the pipes at the splash, which the copy keeps current). A hang or reset returns to Ubuntu (RAM
boots, `piano-reset-after`). B1b as built: without `native-gop-display-flip` NativeGopDisplay is unchanged whatever PianoDpu does;
with it and PianoDpu absent or below step 4, every swap is refused (`off`) and copied, its completion only waiting for the copy. A
step that fails stops the ladder with the boot framebuffer on the panel (steps 1-3 leave nothing changed: their reverts and the
bank's undo return every register they wrote to its found value, except TOP INTR_EN, which stays off, the status bits they cleared,
and the SPI 83 handler, which stays attached: ArmCpu has no detach); step 4's failures turn flips off for the boot (§4.2.5).

## 10. Open questions

1. Is the MDSS AHB clock on at XNU time? → B1a gate line. 2026-11-12: GCC's display AHB branch reads enabled and hardware-gated
   (0x88000003), so the B1a gate closed on CLK_OFF; `piano-dpu-ahb-vote=1` reads behind it by Linux's rule (§8). DISPCC's own
   branches: that run's `clocks: dispcc` line; if its AHB (0x80b0) reads off, its enable is the next one-register vote.
2. Which pipes, rects, alpha, DSPP in path; INTF compressed width and wide bus? → B1a.
3. Bank 2: translation on or off, and is it the HLOS's to reprogram under Gunyah (sheng: yes on SM8550)? → B1a, then B1b(3).
4. Is TCR2 PASize 36 bits enough, and does the walker accept tables above 4 GiB? → B1b(3) (sheng used 48-bit, tables in RAM).
5. Exact DSI timing shadow offsets: SunUEFI's patch (base + 0x1ec DB, + 0x1e8 flush) and the downstream list (+ 0x1e8, + 0x1e4,
   + 0x1ec the TPG FIFO reset) disagree → c0's shadow words, then c1 (`piano-dpu-refresh-db=2` if the default underruns).
6. Which IOMFB call carries Night Shift on this external-type display (`swap_set_color_matrix` or `set_gamma_table`)? → B1d-0 log.
7. Does WindowServer accept a refresh change under one advertised timing (frames at 60 while it paces at 120)? → B1c.
8. Panel sleep via the DSI command FIFO while video runs: the panel driver sends in LP (MIPI_DSI_MODE_LPM), the default here;
   `piano-dpu-panel-hs=1` if LP fails. Is TRIG_CTRL's DMA trigger software and BLOCK_DMA_WITHIN_FRAME set as ABL left it? → e0.
9. Does an SSPP-only CTL_FLUSH latch on DPU 12 (sheng's DPU 9.2 did), or does it need the CTL's own bit 17? → B1b step 1
   (`piano-dpu-flush=0x20003` is the second try).
10. Does TOP INTR_STATUS latch the vsync and underrun bits with their enables off (msm reads it that way)? → B1b setup line.
11. Does XNU make a read-only cacheable alias of WindowServer's surfaces (the clean), and what does a 27 MB clean by VA cost? → step 4
    summary (`cleans mean`, `without`).
12. Does WindowServer keep up when a swap completes only at the latch (a real display's pace) instead of at once, and does it never
    draw into the surface on the panel (the family's in-use count, not NativeGopDisplay's `swap_wait` answer, guards that)? → step 4:
    frames per second in NativeGopDisplay's line, no tearing.
13. Under Gunyah, are the DPU bank's TTBR0/TCR/SCTLR the HLOS's to write (sheng: yes), and is IDR2's output size real? → step 3.
14. Does a DSI controller left in video mode with its INTF stopped (hours of display sleep) restart cleanly with the INTF? Linux has
   that state only between the encoder's and the host's disable. → e1's 50 cycles; the fallback is `piano-dpu-panel-engines=0`.
15. The TDDI touch while the panel sleeps (touch and display are one chip): PianoTouch may see errors until the wake → e1.
16. CTL_FLUSH taken as write-one-to-set (msm always writes its whole pending mask): B1b's flips (the pipes' bits) and B1c/B1d's
    flushes (INTF, DSPP) come from different threads, each waiting for its own bits only; a boot where one latch drops the other's
    bits would ask for one lock over every CTL_FLUSH writer. B1b's vsync handler clears underrun bits too, so with B1b running B1c's
    underrun check may see fewer (the rate and lockstep checks stay).

## 11. Licence note

The msm DPU/DSI/SMMU and panel drivers, SunUEFI's Linux patches (GPL-2.0) and the stock panel XML are read as the specification
only (offsets, fields, sequences, delays); nothing is copied. B1d's matrix layout is a fact from Asahi Linux's DCP driver (GPL-2.0
OR MIT, read only); the gamma table's layout is the public iOS night-light tools' (from memory, unverified: B1d-0); B1e's DSI command
FIFO route follows UniXNU-sm8750-common's PeripheralIO.h (BSD-2-Clause-Patent), re-written. No vendor binary was read. PianoDpu is BSD-2-Clause-Patent; its shape comes from PianoAudio
(this repository). GopFramebuffer and ShengUfs (BSD-2-Clause-Patent) are the named donors of B1b: B1b re-expresses their address-only
flip, latch poll, window slots and in-place bank enable (with its SEP 7, terminate-on-fault and undo facts) in new code; the SMMU
registers and the descriptor format follow the ARM SMMU v2 architecture and VMSAv8-64 (ARM's public specifications). K90 (APSL 2.0)
was read for facts only. Provenance: `package/PianoDpu/UPSTREAM`.
