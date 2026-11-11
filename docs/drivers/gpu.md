# GPU: Adreno 830 (PianoA830 = Adreno830 + piano's board glue; Metal side = AdrenoMetal plugin + Turnip on ABI v2)

Status 2026-11-11: `package/PianoA830` is in the native kext list (`configs/k27-native-kexts.txt`), inert unless `piano-gpu=N`.
On the tablet: the read-only survey (piano-gpu=1, 07:12), S1..S10 all ok on the first full run (`GPU RUN 160M`, 44-53 C), and with
the NativeAdrenoMetal plugin (Turnip on ABI v2) in the seed **the macOS login screen composited by WindowServer on the A830**
(camera, about 3.5 min into the boot). Plan: the local plan `gpu-native-plan.md` (not published).

## 1. Purpose and gate

GPU-composited macOS desktop (WindowServer/Core Animation on Metal) on the real A830 instead of the VM's virtio tunnel. Gates
**M5** (plan §5). Until then WindowServer composites in software into the GOP framebuffer (display.md, soft-metal.md).

## 2. Hardware facts

| Item | Value | Source | State |
|---|---|---|---|
| GPU | A830, Gen8, chip id `0x44050001` | boot DT `compatible = "qcom,adreno-44050001"`; survey | verified |
| SoC | SMEM socinfo **639**, feature code 2 (13T: 618) | survey S0; boot DT `qcom,msm-id 0x100027f`; stock DT lists 618/639 | verified |
| Speed bin | fuse `0x221c8138` bits 8:0 = **0xe8** = KGSL bin 2 (14 levels to 1100 MHz) | survey S0 | verified |
| Windows | GPU `0x3d00000+0x40000` (boot DT: +0x6d000), RSCC `0x3d50000`, CX_DBGC `0x3d61000`, CX_MISC `0x3d9e000`, GMU `0x3d37000+0x68000` (KGSL base; boot DT `gmu@3d6c000+0x32000` inside), GMU AO `0x3d40000`, GPU_CC `0x3d90000+0x9800`, GCC `0x100000`, SMMU `0x3da0000+0x40000` | `package/PianoA830/board/PianoGpuInputs.h` | verified (DTs) |
| Interrupts (INTID) | GPU 332, cx_host 112, GMU hfi 336 / gmu 337, SMMU global 706 + CB 710..713 (25 banks), SCM wait queue 994 | same | verified (DTs); unused: polled |
| Streams | GPU SID 0, LPAC SID 1, GMU SID 5 | boot DT iommus | verified |
| Reserved | cmd-db `0x81c60000+0x20000`, SMEM `0x81d00000+0x200000`, ZAP `0x9b09a000+0x2000` (all no-map) | boot DT | verified; ZAP readable before PAS (survey) |
| RPMh | apps RSC DRV-2 `0x16520000`, v3, NTCS 8, NCMD 16, TCS 0..2 idle at hand-over | survey | verified |
| Power state at hand-over | GCC GPU_CFG_AHB on (0x88000003), GPLL0 locked, GPU votes clear; GPU_CC: CX GDSC collapsed (CX_GDSCR 0x0222f801), PLL0 off (L 0x440000), GMU/HUB RCGs off, AHB/CX_GMU/SMMU-vote branches off | survey | verified: GPU fully off, as the 13T's cold state |
| Firmware | `gen80000_gmu.bin` (= 13T), `_sqe.fw`, `_aqe.fw` (piano builds), piano-signed `gen80000_zap.mbn`; pins in `board/piano-gpu-firmware.json` | stock vendor_a/odm_a copies | verified (S0 "FW ok") |

## 3. Ubuntu as the specification

Linux `drm/msm` (a8xx), KGSL and `clk/qcom` (GPL-2.0) are read as specification only; nothing is copied. Adreno830's `A830Regs.h` and
the generated `A830HwInit.h` are register facts as upstream ships them. piano's two device trees are the board input (§7).

## 4. UniXNU template to build on

**Adreno830** 1a13210 (MIT), the 13T's whole A830 kernel driver (S0-S10 and a GPU desktop under iPadOS on the same SoC and Gunyah),
with **ArmSmmu4K** 3ba9245 (the revision it was tested with) and **AdrenoPM4**; board glue derived from **UniXNU-Pagani_SM8750**
25b1109 (`PaganiGpuBoard.cpp`, the inputs generator, the firmware embed); the macOS accelerator from the VM-era GPU
kext. Provenance, hashes and every changed file: `package/PianoA830/UPSTREAM`.

macOS-27 deltas (compiled only with `-DADRENO830_MACOS`; host tests build both ways):
- S9 without the iPadOS-only staged `IOAcceleratorES`: `native_adreno_publish` registers `NativeAdrenoGpuPublished`, the kext's
  `AGXAccelerator : IOAccelerator` personality matches it (MetalPluginName `NativeAdrenoMetal`, MetalPluginClassName
  `NativeAdrenoMetalDevice`, the VM's contract).
- WindowServer is the render server (the six "backboardd" sites, as Adreno740 7487f72); macOS client lists (`NativeAdrenoPublish.h`).
- The 128 KiB cmd-db snapshot and the 128 KiB BO-pool extent table on the heap (the KC's largest native `__DATA` gap is ~166 KiB).
- k27 KPIs: none missing for this code (`check-kext-imports.py`: 425 kernel imports, 0 unresolved); `armcpu_spi_*` stay the driver's
  weak refusing defaults (k27 has no `armcpu_spi_enable/disable`): every interrupt attach is refused and recorded, every stage polls.
- Weak definitions: the KC's native linker has no weak coalescing (each object would keep its own `adreno830_profile_value`, the
  publish shell its no-op `adreno830_dcvs_submit`); `tooling/k27/coalesce-weak.py` gives the objects ld's semantics (17 symbols).
- The BootProgress hooks are the kext's: the status rows, stage results and the register trace go through a line ring that a
  logger thread prints with IOLog (the driver calls them with interrupts off; printing there hung the first S1 run).

## 5. IOKit family / ABI

`PianoA830 : IOService` on `IOPlatformExpertDevice` (probe declines without `piano-gpu`); `NativeAdrenoGpuPublished` (S9);
`AGXAccelerator : IOAccelerator` (IOGraphicsFamily, vtable check 272 slots: 0 problems); user client `AGXDeviceUserClient`, open type
`0x4147` only, direct selectors 1-16 or the NTNL tunnel, **ABI v2** (`package/PianoA830/adreno830/NativeAdrenoABI.h`, unchanged).
Turnip builds against `package/PianoA830/abi` (v2 + the one v1 name the darwin KMD uses). A Turnip built against the VM's v1 ABI
cannot talk to it (version 1 vs 2, Submit 416 vs 3104 bytes). Offline handshake: `tooling/k27/check-a830-abi-host.py` (PASS).

The userspace half is built: the VM's `NativeAdrenoMetal.bundle` with Turnip on ABI v2, checked offline against Adreno830's host
kernel stack and staged by `build-native-image.sh` when PianoA830 is linked. [gpu-userspace.md](gpu-userspace.md) §4 lists what the
kernel side must provide (2026-11-11).

## 6. Boot args

| Argument | Default | Effect |
|---|---|---|
| `piano-gpu=N` | 0 | 0 off (nothing mapped or published); 1 read-only survey around the driver's S0; 2 staged bring-up S1..S10, polled |
| `piano-gpu-survey=<bits>` | 0x7f at N=1, 0 at N=2 | 1 inputs/firmware, 2 GCC, 4 GPU_CC, 8 SMMU, 0x10 GMU/RSCC/CX_MISC, 0x20 DRV-2, 0x40 ZAP region, 0x80 GX GDSC (opt-in) |
| `piano-gpu-delay=<s>` | 0 | the survey/bring-up thread starts that late (ladder images: 20, after the root) |
| `piano-gpu-stop=N` | none | stop after S_N (N=1 forces 0) |
| `piano-gpu-trace=0/1/2` | 1 at N=2 | the register trace: every access and SMC before it is made, counted / verbatim / off |
| `piano-gpu-status=0..3` | 2 at N=2 | status rows (2: the diagnostic page) |
| other `piano-gpu-*`, `piano-screenoff`, `piano-diag` | driver's | Adreno830's `pagani-*` knobs renamed (README of `package/PianoA830/adreno830`), e.g. `piano-gpu-profile`, `piano-gpu-dcvs-max` |
| `piano-gpu-pool=<MiB>` | an eighth of the RAM, 512..1536 (1536 on piano) | the write-combined BO pool S3 reserves (halved until it succeeds, 64..2048); §8, "The BO pool" |
| `piano-gpu-metal-opens=<n>` | 0 (no limit) | connections ever granted to the plugin |
| `piano-gpu-hang-ms=<ms>` | 0 (2000) | the time without progress (fence, ring read pointer, BR's IB1) the fence and recovery watchdogs call a hang, 1000..60000; 10000 for the first desktop sessions ([gpu-userspace.md](gpu-userspace.md), desktop section) |
| `piano-gpu-idle=0/1/2` | 1 | the GPU's idle power (§8, "GPU idle power"): 0 as before; 1 the DCVS floor (no wake boost, a 250 ms window while idle at 160 MHz, no touch pre-wake); 2 also the boot DDR vote dropped to the display's floor at S10, the host CX/MX votes lowered after `piano-gpu-idle-ms` quiet, and P1's full slumber |
| `piano-gpu-idle-ms=<ms>` | 1000 | quiet (nothing submitted, in flight or held) this long: the idle votes and the slumber of level 2; 100..60000 |
| `piano-gpu-idle-slumber=0/1` | 1 | with level 2, P1's full slumber too (0: the votes alone); `piano-gpu-slumber` given wins |
| `piano-gpu-idle-ddr=<level>` | the scanout's (5 on piano) | the host DDR bus level of the display's floor: the lowest level holding twice the boot framebuffer's 120 Hz read rate (3.28 GB/s at 3200x2136); 5 = 6.67 GB/s |
| `piano-gpu-idle-rails=<vlvl>` | 0x140 | the host CX/MX level while idle (RPMh vlvl; 0x140 NOM_L1), never above what is held; 0 keeps the rails |
| `piano-gpu-ddr-floor=<level>` | the boot level (11) | the driver's S5 floor; given, level 2's S10 drop is skipped and the idle votes lower the DDR only while idle |
| `piano-metal-report=0/1/2` | 1 | the plugin's abort reporter: off / the desktop's system processes / every process that loads the plugin (userspace, read through kern.bootargs) |
| `native-gop-display-gpu-fence=0/1` | 1 | NativeGopDisplay waits for the GPU's fence before a swap's copy (§8) |

## 7. Kernel archive / component

Native kext `com.unixnu.driver.PianoA830` (`tooling/k27/kext/PianoA830.json`, objects `@OUT@/native-kexts/PianoA830/objects.json`).
Build: `tooling/k27/build-native-piano-a830.sh`: (1) `gen-piano-gpu-inputs.py --check` (board inputs from `out/inputs/piano-boot.dtb`,
dtc'd to `out/native-kexts/PianoA830/inputs/piano-boot.dts`, plus the stock DT's recorded KGSL policy, cross-checked; `--compare` with
the 13T's header: only digests, SoC 639 and bin 1's length differ); (2) `stage-piano-gpu-firmware.py` (pins checked, copies in the
ignored `out/native-kexts/PianoA830/firmware`, `.incbin` from there; firmware is never committed); (3) `run-a830-host-tests.py`
(44 upstream and 43 macOS pass with `test_a830idle`; known: `test_sha256` SQE/AQE/ZAP against the 13T's built-in pin table, `test_a830regs` needs upstream's unpublished
`docs/inputs`, `test_userclient` asserts iPadOS names in the macOS pass, which has its own list check); (4) objects, coalescing,
import check. Image: `PIANO_EXTRA_KEXTS=$(tooling/k27/build-native-piano-a830.sh | tail -1) PIANO_EXTRA_ARGS="piano-gpu=1" tooling/k27/build-native-image.sh`;
ladder: `tooling/k27/build-gpu-ladder.sh` (out/ram-gpu/s1..s9, full).

## 8. Hardware test plan and results

1. **piano-gpu=1** (survey, read only; first tablet step). Look for, in order: `inputs:`, four `firmware ... = pin`, `board: SMEM socinfo
   id`, `adreno830_start: 0; stage 0 ... state 4` (stopped after S0), `S0 facts`, `GCC:`, `GPU_CC:` summary, SMMU/GMU lines or their
   `skipped` lines, `DRV-2`, the ZAP line, `survey done`. **Result 2026-11-11 07:12: as §2; no hang; S0 ok in 7 ms (row0 `GPU S0 INPUTS
   STOP ok 7ms p1`).**
2. **piano-gpu=2 ladder** (`piano-gpu-stop=1` → 9, then none), one RAM boot each, console live or from the pstore mailbox. Each stage
   prints `t g<S> <NAME> begin` and `end ok|stop|fail <ior> s<step> r<reason> <reg>=<value>`; the last `t` line before a hang names the
   register or SMC. S1 = DRV-2 MX/CX votes, GCC/GPU_CC power-up, CX_AHB timeouts; S2 SCM GPU_INIT_REGS + CN0/DDR votes; S3 SMMU CB0/CB2 +
   512 MiB BO pool; S4 GMU memory; S5 GMU cold boot + HFI; S6 CP; S7 ZAP (PAS 13); S8 fence self-test; S9 publish; S10 DCVS (`GPU RUN`).
   First run 07:20: hung after `t g start` — the trace hook printed with interrupts off; fixed (line ring + logger thread).
3. Then a Metal probe (plan step 5) with WindowServer kept on software, then WindowServer on Metal.
Fails safe: RAM boot, `piano-reset-after`, no partition is written; without `piano-gpu` nothing happens.

**The console mailbox after the login run (07:50, "no XNU log (signature 0x10054044)") was not overwritten.** XNU's DRAM is only
0x8c0000000-0xc00000000 (dts/piano.dts), so no kernel or GPU allocation can have a physical address in cvp@9cb00000, and PianoA830
maps nothing near it (MMIO, cmd-db, SMEM, ZAP at 0x9b09a000). The signature 0x10054044 is "DBGR" 0x52474244 with five 1-bits cleared
and no 0-bit set: DRAM retention decay across the reset (heavier after a hot run: the GPU at 56-57 C before the reset), which the
single-copy signature word and the reader's thresholds did not survive; the ring itself is printable log text in all 11 copies, no
foreign block. `tooling/native/xnu-log.py` now accepts a signature that only lost bits, and takes the vote threshold from the
1-population's own peak for the ring and the 11 header copies: the dump decodes (5653 lines, to "piano-reset-after: 300 s reached").
Guards in PianoA830 all the same: the logger watches the mailbox's signature word once a second ("CONSOLE MAILBOX OVERWRITTEN ..."
if it changes while XNU runs, when nothing decays), and after the bring-up and 60 s later `piano_a830_smmu_routes` logs where every
GPU-side stream ID 0..15 goes, flagging any that reaches memory untranslated (an S2CR bypass, a bank with SCTLR.M = 0, or no entry
with sCR0.USFCFG = 0): the only way the GPU could write memory XNU does not own, since every client window lies below 0x8c0000000.

**The display's fence rule.** WindowServer swaps a frame when its command buffer is scheduled, not when the A830 finished it, and
IOMFB's IOSurface fences do not see the A830's work. `native_adreno_fences(submitted, retired)` (NativeAdrenoPublish.cpp, lock-free)
gives the newest fence handed out and the newest the CP wrote. NativeGopDisplay finds PianoA830's registered service by class and asks
it with `callPlatformFunction("native-adreno-fences")` (no link dependency either way: the KC resolves a native kext's imports only
from natives linked before it): a swap records the newest fence handed out when it is programmed, and its copy waits until the GPU
retired it, at most 50 ms (then it copies anyway); `native-gop-display-gpu-fence=0` turns it off. Its 10 s line reports the waits.

**The session's GPU events (2026-11-11).** Besides the status rows (since then at most every 10 s once the GPU runs, at once when row
0's state, fault or hang count changes) and the minute's meter line, the publish shell prints `PianoA830: gpu ...` lines whatever
`piano-gpu-trace` says, the first time and then every 10th, 100th, ...: `call failed: <proc>[pid] slot N <ABI method> -> 0x...`, `open
refused: ... <why>`, `recovery: culprit ...`, `hang snapshot 1/3..3/3`, `recovery done`, `DEVICE LOST`, `page fault at 0x...: <proc>'s
window`, and `clients N (peak P, ...)` with every live connection's slot, process and MiB when they change (at most every 10 s). What
each means, and what the first desktop session must show: [gpu-userspace.md](gpu-userspace.md), "Desktop readiness, 2026-11-11".

**Default-on.** PianoA830 is listed (inert). `PIANO_GPU=2 tooling/k27/build-native-image.sh` adds `piano-gpu=2
piano-gpu-delay=${PIANO_GPU_DELAY:-20}`; make that the default once the plugin is staged in the default seed (and NativeSoftGpu stays
out): an accelerator without its MetalPluginName bundle would put WindowServer back into the crash loop of the VM-era stub. The
bring-up took about 1.1 s with the 512 MiB pool (S3's reservation 1.0 s of it; the 1536 MiB default, three times the pages, is to be
measured), so `piano-gpu-delay=0` would publish the accelerator before
WindowServer starts; try it once before making it the default. DCVS: keep the annibale default (at most 342 MHz, 160 MHz held from
70 C until below 60 C): the login run sat at 342 MHz and 56-57 C with no cooling and no charging control (PianoPower is read-only so
far), and until `piano-gpu-idle=2` has passed its A/B (below) the GPU never slumbers (P0) while host CX/MX stay at their top
votes and DDR at level 11. Raise it (`piano-gpu-dcvs-max=607 piano-gpu-maxlevel=7`) only after a 10-minute desktop soak with the
temperatures logged; slumber (`piano-gpu-slumber=2`, or level 2's full slumber) is untested on piano.

**GPU idle power (2026-11-11).** What the driver held all day before: P0 (no slumber); on every frame after 64 ms of quiet the
DCVS's wake boost to the ceiling for 50 ms (the login screen, ~10 frames a second of 2.5 ms of work, spent 47 % of its time at
342 MHz); the DCVS thread awake every 16 ms whether anything ran or not; and host votes on apps DRV-2 that nothing lowers: CX and
MX at the top of their cmd-db tables, DDR at bus level 11 (18.6 GB/s, the table's top) with ACV. They are the HLOS's only votes for
those resources (UFS, the display and the CPU have none of their own here; PianoAdsp votes lcx/lmx, PianoWifi and PianoKeyboard
regulators), so the DDR ran at its top frequency and CX/MX at their top voltage all day, in the installed profile too.
`piano-gpu-idle` (`adreno830/A830Idle.h`, the board's `adreno830_board_idle`):

- **1 (default): the DCVS floor.** No wake boost: an isolated frame runs at the floor (2.5 ms of 342 MHz work is about 5 ms at
  160 MHz, inside a 120 Hz frame); a sustained load still climbs through the governor's up rules (≥ 90 % busy: two levels; demand
  above the level: demand x 1.1) within a window or two. While the governor idles at the floor its window is 250 ms, not 16 ms; the
  first submission wakes the thread and the windows are 16 ms again. No touch pre-wake (nothing on piano counts touches for it).
  The down rule (demand under 85 % of the next level down for 4 windows, 6 after a step up) was checked against a mostly idle desktop: host-tested, the login
  screen's frames (a third of a 16 ms window at 160 MHz), an isolated frame and a light 120 Hz desktop (64 % busy at 160 MHz,
  demand 137 MHz) all stay at 160 MHz; with the design's boost every frame of the login screen went above it.
- **2: also the votes and the slumber.** (a) At S10 the boot DDR vote falls to the display's floor for the rest of the boot
  (`gpu idle: the boot DDR vote went to the display's floor, level 5 (6671 MB/s); the GMU votes the GPU's own`): the GMU votes
  each GPU level's bandwidth itself (its table from the DT's opp-peak-kBps), so the host vote only has to carry the scanout. (b)
  After `piano-gpu-idle-ms` with nothing submitted, in flight or held, the host CX/MX votes go to `piano-gpu-idle-rails` (NOM_L1)
  and, if a chosen `piano-gpu-ddr-floor` is above the display's, the DDR to it; back at the next submission, rails first, on the DCVS
  thread (the GPU's work does not wait: the GMU votes its own GX, CX and DDR through its RSC, and its CX levels are LOW_SVS/SVS).
  Every vote goes through `Sm8750RpmhWriteActive`, which takes the DRV-2 owner word (`sm8750_rpmh_drv2_owner`) as for every vote of
  this driver; cx is lowered first, raised last (`Sm8750RpmhHostRailsIdle`). (c) With `piano-gpu-idle-slumber=1` A830PowerState's
  P1 full slumber after the same quiet time: PREPARE_SLUMBER, RSCC sleep (the GMU's votes drop through its RSC's sleep set), GBIF
  halt, CX power-off; a submission wakes it (acquire, a few ms).
  Fail-safe: a failed vote puts back what went down and ends the idle votes for the boot; a failed slumber entry or wake goes
  through the recovery as before and ends P1 for the boot (`A830PowerStats::slumberBlocked`), so a slumber that does not work
  costs one recovery, not three.

The floors, from the Ubuntu port (facts only: [ubuntu-history/kernel.md](../ubuntu-history/kernel.md) §2.7, §3.4, and its
`sm8750.dtsi`/piano DT): the boot framebuffer's display path votes 3.28 GB/s average and 4.26 GB/s peak (3200x2136 at 120 Hz with the
2310-line vertical total) and the DPU at least 0.8 GB/s: level 5 (6.67 GB/s) is above all three. The same DT holds MMCX at NOM_L1
for the boot framebuffer (the MDP at 575 MHz), and on SM8750 MMCX has CX as its parent domain, so Linux keeps CX at NOM_L1 while the
panel is lit; UFS at HS-G5 (403 MHz), USB and PCIe Gen3 need NOM; this driver never votes MMCX. The other facts of that history,
checked against this driver: the boot bandwidth vote must go once the GMU votes bandwidth or DDR stays at top speed (K:97567648b85d:
level 2's S10 drop); the MX dependency index of each GX level comes from the GPU's own OPP levels (K:cbb3f4900bb4: A830Votes takes
the first mx level at or above each level's vlvl from the boot DT's table); GMXC is never at retention while GX is on
(K:925bbbba6eca: the GX vote's secondary is the first gmxc level at or above the GFX corner, 50 at 160 MHz; level 0 "off" only with
GX off); the HFI interrupt clears only its fault bits (K:891bc63f703a: 336's top half clears bits 1, 2, 15 and 23, never MSGQ or
the OOB acks) and the GMU's interrupts are masked before its boot (K:ce9c0ed98aab: the annibale profile, piano's, masks both and
polls); the GMU firmware's init wait is 100 ms (K:d86e3f7b2204: `A830_T_FW_INIT_US`). Linux slumbers this GPU 66 ms after its last
work and runs IFPC (hysteresis 0x1680); level 2 starts at 1000 ms and without IFPC.

What the lines say (all `PianoA830: gpu ...` through the line ring, the first 8 of each kind and then every 10th, 100th, ...):
`idle: piano-gpu-idle=2 (slumber 1, after 1000 ms); idle DDR level 5 (6671 MB/s; 3200x2136 scanout 3280 MB/s at 120 Hz), rails vlvl
0x140` at S8 (the power state's binding); `idle: votes down after 1000 ms quiet (#n): DDR level L (M MB/s), CX 0x140 MX 0x140` and `idle: votes up after N ms
down (#n): ...`; `idle -> slumber after 1000 ms quiet (#n, entry U us); votes CX x MX y DDR level z (M MB/s)`; `wake after N ms asleep
(#n): the wake took X.XXX ms (what the first submission waited)`: the first-frame latency after idle; once a minute with anything in
it `idle last 60 s: votes down D, up U, down P % of the time; slumbers S, wakes W, longest wake X.XXX ms`. Trouble: `idle: a host
vote failed (rpmh R, #n): no idle votes for this boot`, `idle: a slumber or wake failed (step S, E, F failures): no slumber for this
boot` (with the `gpu recovery` lines before it), `the boot DDR vote could not drop`. What level 2 could break, and its lever: a
display that flickers or tears at DDR level 5 (no underrun counter is wired on piano: watch the panel and the thumbnails;
`piano-gpu-idle-ddr=6` or `piano-gpu-ddr-floor=11`); UFS errors (`NativeUfs` lines) with the rails at NOM_L1
(`piano-gpu-idle-rails=0`); CPU-heavy work under a still screen with the DDR at 6.67 GB/s (no CPU DDR governor here;
`piano-gpu-ddr-floor=11` keeps the boot vote and lowers it only while the GPU idles); a first frame after a slumber a few ms late
(the wake line).

**The A/B power test** (PianoPower's `[Ns] battery: V mV, I mA (discharging), ...` line every 30 s; the same brightness, a still
wallpaper, charger unplugged, nothing touched after the login):

```sh
for idle in 0 2; do
  PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=1200 \
  PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-power=1 exec-log=1 stderr-log=1 piano-gpu-hang-ms=10000 piano-gpu-idle=$idle" \
    tooling/k27/build-native-image.sh || break
  rm -rf out/ram-gpu-idle$idle && cp -R out/ram out/ram-gpu-idle$idle
done
tooling/native/native-loop.sh out/ram-gpu-idle0/boot-piano-macos-ram-A-bundled.img   # then out/ram-gpu-idle2/...
```

Both images carry the same seed and kernel collection; only the boot line differs. Per run: log in, leave the desktop alone, average
the `battery:` current from minute 3 to minute 10 after the login (14 lines), note `row0`'s temperature and, for level 2, the minute
summaries (an idle desktop should be down most of the time) and the longest wake. Then, one variable at a time: `piano-gpu-idle-ms=100`
(nearer Linux's 66 ms), `piano-gpu-ifpc=1` (P2, GX collapse between frames; Linux runs it on this GPU), `piano-gpu-idle-slumber=0`
(the votes without the slumber, if a wake line shows trouble).

Not done here: system suspend. The host votes are ACTIVE-only on DRV-2 and nothing programs its SLEEP/WAKE sets, so they survive
into sleep: a CX-off suspend needs the GPU in P1's full slumber (the GMU's votes dropped) and every host vote lowered to its sleep minimum
first, and put back on resume, through the same `Sm8750RpmhHost` calls; the entry point belongs to
the power contract when it lands (kernel.md §3.2). The thermal ceiling PianoThermal asks for at its passive trip
(`callPlatformFunction("piano-gpu-ceiling", kHz)`, [thermal.md](thermal.md) §10.5) is in: `adreno830_dcvs_set_ceiling`, a line per
change (`gpu ceiling N MHz (...)`, `gpu ceiling cleared`); macOS Low Power Mode will use the same call (BACKLOG B5).

**The BO pool (2026-11-11).** The write-combined pool S3 reserves while memory is unfragmented holds the BOs Turnip allocates
(`BoAlloc`; imported IOSurfaces are not in it): a pooled BO is one span of the GPU's table and needs no page attribute change. What
does not fit is, as before, a contiguous attempt and then scattered pages: 3 ms a BO and 90-130 ms a surface-sized one of attribute
changes, one span per 4 KiB page out of the VM's 262144 (int-137 ran out at 65536). The login run used 486 MiB of the 13T board's
512 MiB with 6 clients (WindowServer 446 MiB); the desktop adds 15-25 Metal clients (Dock, Finder, SystemUIServer, ControlCenter,
NotificationCenter, WindowManager, Spotlight, ...) of tens of MiB each and WindowServer's surfaces for them, so most of it would have
been scattered. The default is now an eighth of the RAM in 256 MiB steps, 512..1536 MiB (`piano_a830_pool_default`, compile-time
checked): 1536 MiB with piano's 13 GiB, so the desktop's BOs stay pooled and the rest of the RAM stays the system's
(`board: BO pool 1536 MiB (an eighth of the RAM, 512..1536; RAM N MiB)`; S3 halves it until a reservation succeeds, as before;
`piano-gpu-pool=<MiB>` wins, 64..2048). The `MEM` row shows pool and use; S3's time grows with the pool (1.0 s for 512 MiB).

## 9. Fallback

`piano-gpu` absent: software compositing (soft-metal.md). After S9 the accelerator exists; a failed stage publishes nothing.

## 10. Open questions

1. Does Gunyah on piano keep only ACTLR `0x303` (13T)? (S3 trace + survey after S1.)
2. Does the SCM path (GPU_INIT_REGS, PAS 13 with piano's ZAP) work from XNU at EL1 on piano? (S2, S7.) Every SCM request and the
   ZAP's whole PAS sequence hold the kernel-wide secure-call lock, whose word PianoA830 owns (`../scm-lock.md`, 2026-11-11).
3. DRV-2 sharing: PianoPower (PianoAdsp's lcx/lmx), PianoWifi and PianoKeyboard (regulators) take `sm8750_rpmh_drv2_owner` around
   every vote now, as PianoA830 does (2026-11-11 tree); none of them votes cx/mx or the DDR BCMs. `piano-gpu-idle=2` sends at run
   time (a vote pair per idle period), so the lock matters more than at the boot's few votes; on the tablet, the TCS lines of each
   driver with `piano-gpu-idle=2` and `piano-power>=2` together are still to be seen.
4. Real Metal client count on macOS (32 slots); the 3200x2136 hang seen under virgl.
5. Whether the 50 ms fence wait is ever reached (NativeGopDisplay's 10 s line), and the real frame time on the panel.

## 11. Licence note

Adreno830, ArmSmmu4K, AdrenoPM4: MIT (their LICENSE files in `package/PianoA830`). Board glue and tooling: BSD-2-Clause-Patent
(`package/PianoA830/LICENSE`). Linux/KGSL: specification only. Vendor firmware: never in this repository.
