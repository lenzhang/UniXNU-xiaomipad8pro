# GPU: Adreno 830 (Adreno830 + Sm8750Platform + AdrenoMetal)

Status: design, 2026-11-10. Nothing built or run. Template: `README.md`.

## 1. Purpose and gate

GPU-composited macOS desktop (WindowServer/Core Animation on Metal) on the real A830 instead of the VM's virtio tunnel.
Gates **M5** (plan §5). Until then WindowServer composites in software into the GOP framebuffer (display.md).

## 2. Hardware facts

| Item | Value | Source | State |
|---|---|---|---|
| GPU | A830, Gen8, chip id `0x44050001` (same as the OnePlus 13T) | Adreno830 README | verified |
| Registers | `gpu@3d00000` (kgsl_3d0 0x40000), rscc 0x3d50000, cx_dbgc 0x3d61000, cx_misc 0x3d9e000; GMU `0x3d37000` (+ ao blk 0x3d40000); GPUCC `0x3d90000`; GX clkctl `0x3d68024` | `UniXNU-sm8750-common/dts/sm8750.dtsi` | verified |
| IRQs | GPU SPI 300, cx_host SPI 80; GMU hfi SPI 304, gmu SPI 305; adreno SMMU global SPI 674 + 25 context banks | sm8750.dtsi | verified |
| SMMU | adreno SMMU `0x3da0000` (`qcom,adreno-smmu`, dma-coherent): GPU SIDs 0 (user) and 1 (LPAC), GMU SID 5 | sm8750.dtsi; `native-display-gpu-research.md` §1a | verified |
| Speed bin | qfprom `0x221c8000` + 0x138, 9 bits | sm8750.dtsi | verified |
| Firmware | `gen80000_sqe.fw`, `gen80000_gmu.bin`, `gen80000_aqe.fw`, device-signed **`gen80000_zap.mbn`** in `/lib/firmware/qcom/sm8750/xiaomi/piano/` (piano's own ZAP, not the 13T's) | plan §4(d).5 | verified (names) |
| Votes | cmd-db (aop-cmd-db@81c60000) GPU/MX/CX rails; board vote tables | live DT; Adreno830 S0/S1 | verified (region); needs-boot (values) |
| Under Gunyah | the HLOS programs stage 1 of the adreno SMMU; ZAP is loaded through SCM PAS from the HLOS (Linux does it today) | plan §4(a).6, (a).5 | inferred (13T runs it under the same Gunyah) |

## 3. Ubuntu as the specification

Linux `drivers/gpu/drm/msm/adreno/a8xx_*` / `a6xx_gmu.c` (gen8 paths) and the msm SMMU code (GPL-2.0) for the power
sequence, GMU HFI and ZAP loading; Mesa Turnip/freedreno (MIT) for the userspace command streams. On Ubuntu the GPU
runs (`msm.separate_gpu_kms`), so the register/vote/firmware sequence is proven on this unit. The VM-era knowledge that
carries over as knowledge (not code): render-barrier and ring-priority fixes, `TU_DEBUG=sysmem`, the 3200x2136 hang
investigation (plan §6, "restart natively if it reappears").

## 4. UniXNU template to build on

- **Adreno830** 1a13210: the whole kernel driver at source level, stages S0–S10 (inputs, power, secure CX, SMMU, firmware
  memory, GMU/HFI, CP, ZAP via PAS 13, fences/self-test, Metal publication, DCVS), with a per-access trace into the
  ramoops log (`A830Trace.h`). First board: OnePlus 13T (`package/PaganiGpuBoard.cpp` fills `Adreno830Board`).
- **Sm8750Platform** (UniXNU-sm8750-common kit): GMU, command DB and GPU SMMU operations shared by SM8750 boards.
- **AdrenoMetal** c68f9a1: NativeAdrenoMetalPlugin + metal2vulkan + Turnip (darwin KMD), the userspace the VM already runs
  on macOS 27 (`freedreno-kmds=darwin`); natively Turnip's darwin KMD talks to Adreno830's NativeAdreno user client
  (`NativeAdrenoABI.h` v2) instead of the virtio tunnel.

Piano delta: a `PianoGpuBoard.cpp` (vote tables, speed-bin table, piano's ZAP image, reserved regions, cmd-db names),
the macOS 27 IOGPU/AGXAccelerator glue (k27's `DroidVMGpuKext.cpp` becomes the publisher for the real KMD; UniXNU-sheng's
macOS 27.3 `gpu-metal=1` path is the 27.3 reference).

## 5. IOKit family / ABI

IOGPUFamily / IOAccelerator (`AGXAccelerator` publication as Adreno830 S9 does), Metal plugin bundle in
`/Library/GPUBundles` (UniXNU 27.3's GPU addon), user client ABI `NativeAdrenoABI.h` v2.

## 6. Boot args

UniXNU 27.3 names (sheng): `gpu-power=9`, `gpu-metal=1`, `fb-gpu=1`, `fb-bgra=1`, `gpu-dcvs=1`, `gpu-addon=1`; Adreno830's
staged bring-up knobs (`pagani-gpu-stop=N`, `-trace=1`, `-status=2`) under a piano prefix. Default: GPU off.

## 7. Kernel archive / component

`component:Adreno830`, the Sm8750Platform kit, `fetch-adreno-inputs.py`-style board inputs (votes, ZAP) kept with the
board (UniXNU-sheng AGENTS.md: "Keep GPU votes and signed ZAP firmware inputs with the board"); ZAP and firmware come
from the tablet's own `/lib/firmware`, never committed.

## 8. Hardware test plan

After M3 (UFS) and the display GOP path, staged exactly like Adreno830's own bring-up: `stop=1` (power only) → `stop=3`
(SMMU) → `stop=5` (GMU/HFI) → `stop=7` (ZAP) → `stop=8` (self-test) → Metal. Every stage with the ramoops trace on, so a
hang is attributable after the manual reset. Pass per stage: the stage's status line; final: a known rendered image and
WindowServer on Metal.

## 9. Fallback

Software compositing into the GOP framebuffer (display.md); `gpu-metal=0`.

## 10. Open questions

1. ramoops/panic region for the trace natively: Linux's `ramoops@a3500000` overlaps pianoPkg's `Display_Demura` map entry;
   pick a region UEFI does not use and Ubuntu can read back (el1-hyp-notes §7).
2. Piano's ZAP: same PAS id (13) and the same SCM calls as the 13T? (source read of the Ubuntu dmesg + needs-boot)
3. Whether the 3200x2136 hang seen under virgl recurs on the native KMD.

## 11. Licence note

msm DRM (GPL-2.0): specification only. Adreno830/AdrenoMetal: their own licences (check `LICENSE`); Mesa (MIT).
