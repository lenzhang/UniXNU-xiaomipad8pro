# PianoDpu

piano's display processor for native macOS 27: the SM8750 MDSS / DPU 12.0 that scans the boot framebuffer out to the NT36532 panel
through two bonded DSI 6G controllers. **Level 1**: a read-only survey of what the boot stages left driving the panel, so that the
zero-copy flips (B1b), refresh switching (B1c) and Night Light (B1d) start from measured facts. **Level 2 (B1b)**: the zero-copy
flips, a ladder of four steps (`piano-dpu-flip=1..4`), the last of which serves NativeGopDisplay (`native-gop-display-flip=1`).

- `piano-dpu=1`: the display clocks first (nothing of MDSS is loaded unless GCC's display AHB, the MDSS GDSC, its AHB and the MDP
  clock are on), then every pipe, CTL, layer mixer, DSPP (PCC/GC), MERGE_3D, DSC encoder, INTF and both DSI controllers; the panel ID
  pin; the apps SMMU's entries and bank for the DPU's streams 0x800-0x803; the refresh measured from INTF1/INTF2's frame counters
  read twice; the path and what a zero-copy flip would write. Every MMIO access is a load; no store at all. NativeGopDisplay is not
  touched. `piano-dpu-ahb-vote=1|2` (`DpuAhbVote.h`) adds the one register level 1 may write, GCC_DISP_AHB_CBCR, before the survey,
  under PianoA830's GCC vote lock: 1 its enable if clear and the gate's GCC check by Linux's rule (enabled and hardware-gated counts
  as on, as the tablet reads it), 2 also hardware gating off (forced on, CLK_OFF must clear); undone after the survey unless a later
  stage reads MDSS.
- `piano-dpu=2`: level 1, then B1b on the same thread: the pipes the survey found on the boot framebuffer re-pointed by their
  source address and stride with one CTL flush, latched at the next frame (the vsync interrupt, SPI 83, enabled only around a flush);
  step 1 re-flushes their own addresses, step 2 shifts the boot framebuffer by 64 rows for 2 s, step 3 reprograms the DPU stream's own
  SMMU bank in place (identity blocks for 2-4 GiB plus four 32 MiB windows below 2 GiB, never moving the stream) and shows a test
  pattern through a window, step 4 maps each IOSurface NativeGopDisplay hands over into a window and flips to it. Any missed latch,
  SMMU fault or underrun puts the boot framebuffer back, the bank as found, and turns flips off for the boot (NativeGopDisplay copies).
- Writing slices, each inert without its own argument (and all on top of `piano-dpu` >= 1): `piano-dpu-refresh=plan|60|120|auto`
  (B1c: 120 <-> 60 Hz by the vertical front porch, verified, reverted on an anomaly, an idle policy), `piano-dpu-pcc=1|2` (B1d: Night
  Light in the DSPPs' PCC from macOS's colour matrices), `piano-dpu-panel=1` (B1e: the panel's light sleep with display sleep over the
  DSI command FIFO, the timing engines stopped slave first). One thread, its own page maps and a store guard that cannot reach TLMM.

Design, hardware facts, the staged plan, the boot arguments and the test plan with the expected lines: `docs/drivers/dpu.md`.

| File | What |
|---|---|
| `PianoDpu.cpp` | the IOService on IOPlatformExpertDevice; the survey thread, its log lines and the `piano-dpu-survey` property; level 2 hands over to `DpuFlip.cpp` and forwards `callPlatformFunction` |
| `DpuSurvey.h` | portable: what level 1 reads (no store in the file), the clock gate, the decoders, the path and route verdicts, the walk |
| `DpuAhbVote.h` | portable: `piano-dpu-ahb-vote`, the GCC display AHB branch's enable / hardware gating under the GCC vote lock, and its undo |
| `DpuHw.h` | the facts, each with its source, including the blocks level 1 does not read |
| `DpuSmmu.h` | portable (B1b): the DPU's SMMU bank in place: geometry and plan (stage 1 or 2 from the bank), the table and windows, the enable and its exact undo, the TLB sync, the window slots |
| `DpuFlip.h` | portable (B1b): the flip path from the survey, program and latch, the ladder steps, the `PianoDpuFlip` service, the reverts, the rate-limited log |
| `DpuFlip.cpp`, `DpuFlipKernel.h` | B1b's kernel side: MMIO and SMMU pages, SPI 83, table and surface memory, cache cleans, the test pattern, the watch, the boot arguments |
| `PianoDpuFlipClient.h` | the `PianoDpuFlip` request NativeGopDisplay sends (header only, no link dependency) |
| `tests/dpu_host.cpp` | host tests of level 1 against register models (the gate, what is and is never loaded, decoders, verdicts, SMMU, walk) |
| `tests/dpu_flip_host.cpp` | host tests of B1b against DPU/MDSS/SMMU/RAM models (`dpu_flip_host N` prints step N's lines) |
| `DpuSlices.h` | portable: what B1c/B1d/B1e share: the Io, the store guard, the live path, frame edges, line windows, latches, the rate |
| `DpuRefresh.h`, `DpuPcc.h`, `DpuPanel.h` | portable: B1c's switch and policy, B1d's conversion and stores, B1e's DCS FIFO packets and sleep/wake |
| `DpuSlices.cpp` | the slices in the kernel: arguments, the worker thread, page maps, the platform functions NativeGopDisplay calls |
| `tests/dpu_model.h`, `tests/dpu_{refresh,pcc,panel}_host.cpp` | a timed register model and the slices' host tests |

Build: `tooling/k27/build-native-piano-dpu.sh` (all host tests, the objects, the import check). PianoDpu is in
`configs/k27-native-kexts.txt` and inert without `piano-dpu`. Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
