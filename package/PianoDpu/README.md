# PianoDpu

piano's display processor for native macOS 27: the SM8750 MDSS / DPU 12.0 that scans the boot framebuffer out to the NT36532 panel
through two bonded DSI 6G controllers. **Level 1**: a read-only survey of what the boot stages left driving the panel, so that the
zero-copy flips (B1b), refresh switching (B1c) and Night Light (B1d) start from measured facts. **Level 2 (B1b)**: the zero-copy
flips, a ladder of four steps (`piano-dpu-flip=1..4`), the last of which serves NativeGopDisplay (`native-gop-display-flip=1`).

- `piano-dpu=1`: the display clocks first (nothing of MDSS is loaded unless GCC's display AHB, the MDSS GDSC, its AHB and the MDP
  clock are on), then every pipe, CTL, layer mixer, DSPP (PCC/GC), MERGE_3D, DSC encoder, INTF and both DSI controllers; the panel ID
  pin; the apps SMMU's entries and bank for the DPU's streams 0x800-0x803; the refresh measured from INTF1/INTF2's frame counters
  read twice; the path and what a zero-copy flip would write. Every MMIO access is a load; no store at all. NativeGopDisplay is not
  touched.
- `piano-dpu=2`: level 1, then B1b on the same thread: the pipes the survey found on the boot framebuffer re-pointed by their
  source address and stride with one CTL flush, latched at the next frame (the vsync interrupt, SPI 83, enabled only around a flush);
  step 1 re-flushes their own addresses, step 2 shifts the boot framebuffer by 64 rows for 2 s, step 3 reprograms the DPU stream's own
  SMMU bank in place (identity blocks for 2-4 GiB plus four 32 MiB windows below 2 GiB, never moving the stream) and shows a test
  pattern through a window, step 4 maps each IOSurface NativeGopDisplay hands over into a window and flips to it. Any missed latch,
  SMMU fault or underrun puts the boot framebuffer back, the bank as found, and turns flips off for the boot (NativeGopDisplay copies).

Design, hardware facts, the staged plan, the boot arguments and the test plan with the expected lines: `docs/drivers/dpu.md`.

| File | What |
|---|---|
| `PianoDpu.cpp` | the IOService on IOPlatformExpertDevice; the survey thread, its log lines and the `piano-dpu-survey` property; level 2 hands over to `DpuFlip.cpp` and forwards `callPlatformFunction` |
| `DpuSurvey.h` | portable: what level 1 reads (no store in the file), the clock gate, the decoders, the path and route verdicts, the walk |
| `DpuHw.h` | the facts, each with its source, including the blocks level 1 does not read |
| `DpuSmmu.h` | portable (B1b): the DPU's SMMU bank in place: geometry and plan (stage 1 or 2 from the bank), the table and windows, the enable and its exact undo, the TLB sync, the window slots |
| `DpuFlip.h` | portable (B1b): the flip path from the survey, program and latch, the ladder steps, the `PianoDpuFlip` service, the reverts, the rate-limited log |
| `DpuFlip.cpp`, `DpuFlipKernel.h` | B1b's kernel side: MMIO and SMMU pages, SPI 83, table and surface memory, cache cleans, the test pattern, the watch, the boot arguments |
| `PianoDpuFlipClient.h` | the `PianoDpuFlip` request NativeGopDisplay sends (header only, no link dependency) |
| `tests/dpu_host.cpp` | host tests of level 1 against register models (the gate, what is and is never loaded, decoders, verdicts, SMMU, walk) |
| `tests/dpu_flip_host.cpp` | host tests of B1b against DPU/MDSS/SMMU/RAM models (`dpu_flip_host N` prints step N's lines) |

Build: `tooling/k27/build-native-piano-dpu.sh` (both host tests, the objects, the import check). PianoDpu is in
`configs/k27-native-kexts.txt` and inert without `piano-dpu`. Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
