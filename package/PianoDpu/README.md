# PianoDpu

piano's display processor for native macOS 27: the SM8750 MDSS / DPU 12.0 that scans the boot framebuffer out to the NT36532 panel
through two bonded DSI 6G controllers. **Level 1 only**: a read-only survey of what the boot stages left driving the panel, so that
the zero-copy flips (B1b), refresh switching (B1c) and Night Light (B1d) start from measured facts.

- `piano-dpu=1`: the display clocks first (nothing of MDSS is loaded unless GCC's display AHB, the MDSS GDSC, its AHB and the MDP
  clock are on), then every pipe, CTL, layer mixer, DSPP (PCC/GC), MERGE_3D, DSC encoder, INTF and both DSI controllers; the panel ID
  pin; the apps SMMU's entries and bank for the DPU's streams 0x800-0x803; the refresh measured from INTF1/INTF2's frame counters
  read twice; the path and what a zero-copy flip would write. Every MMIO access is a load; no store at all. NativeGopDisplay is not
  touched.

Design, hardware facts, the staged plan and the test plan: `docs/drivers/dpu.md`.

| File | What |
|---|---|
| `PianoDpu.cpp` | the IOService on IOPlatformExpertDevice; the survey thread, its log lines and the `piano-dpu-survey` property |
| `DpuSurvey.h` | portable: what level 1 reads (no store in the file), the clock gate, the decoders, the path and route verdicts, the walk |
| `DpuHw.h` | the facts, each with its source, including the blocks level 1 does not read |
| `tests/dpu_host.cpp` | host tests against register models (the gate, what is and is never loaded, decoders, verdicts, SMMU, walk) |

Build: `tooling/k27/build-native-piano-dpu.sh` (host tests, the objects, the import check). PianoDpu is in
`configs/k27-native-kexts.txt` and inert without `piano-dpu`. Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
