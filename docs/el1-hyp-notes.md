# piano at EL1 under Gunyah: hypervisor and exception-level notes (workstream a)

Written 2026-11-10 for slice 1 (plan `el1-native-plan.md` §4(a)). Every item is marked **verified** (read from source,
the running system or a backup, evidence named), **inferred** (follows from verified facts or another SM8750 port, not
observed for XNU on piano) or **needs-boot** (only a native RAM boot can settle it, M1/M2). Tablet access for this page was
read-only (`cat`, `dmesg`, cpufreq sysfs); evidence files are in `hyp-evidence/`.

## 0. Baseline

| Fact | State | Evidence |
|---|---|---|
| Linux runs at EL1 ("All CPU(s) started at EL1"), Gunyah 51/v1 at EL2, `/dev/gunyah` present, no `/dev/kvm` | verified | plan §2 (tablet dmesg) |
| arm64e k27 (xnu-12377 + 0001-0054) already runs the macOS 27.0.1 userland at EL1 under this Gunyah, as a VM guest | verified | the earlier VM-based port's accept path |
| UniXNU ports on SM8750 (OnePlus 13T, Ace 6, Odin 3) run XNU at EL1 as the HLOS under the same hypervisor | verified (their records) | `UniXNU/devices.toml`, `UniXNU-Pagani_SM8750` |

## 1. Watchdogs

| Item | State | Evidence |
|---|---|---|
| The hypervisor watchdog is a Gunyah SMCCC vendor-hyp service (fast SMC32, owner VENDOR_HYP): CONTROL 0x86000005, STATUS 0x86000006, PING 0x86000007, SET_TIME 0x86000008 (bark/bite ms, ≤ 0x8000), CONTROL value 3 = enable, 2 = disable | verified | `drivers/watchdog/gunyah_wdt.c` (linux-sm8750-piano v1.1.0, the private build host `~/kfirst/linux`) |
| Linux registers it from `qcom_scm` only when the hypervisor UUID is Gunyah's and no `qcom,kpss-wdt`/`arm,sbsa-gwdt` node is available; its probe reads STATUS and then **stops** the watchdog; systemd arms it later with 30 s (`RuntimeWatchdogSec`) | verified | `qcom_scm.c:qcom_scm_gunyah_wdt_init`; `gunyah_wdt_probe`; `hyp-evidence/cpus-psci-watchdog-running.txt` (systemd lines) |
| The live DT has no APSS (`qcom,kpss-wdt`, 0x17c10000) or SBSA watchdog node; `CONFIG_QCOM_WDT=m` is unused | verified | live DT; `hyp-evidence/config-grep.txt` |
| The watchdog is **not armed** when the HLOS starts, so XNU needs no petting path for the first boots | inferred | the 13T's XNU runs for minutes without any pet and uses the Gunyah watchdog only deliberately as a fallback reset (`UniXNU-Pagani_SM8750/patches/xnu/phone-bootstrap-tables.patch`: SET_TIME 2000/4000, CONTROL 3); Linux's stop-at-probe is a defensive reset, not evidence of arming |
| Confirmation | needs-boot | M2: a native boot that stays up > 60 s at the panel log without any pet; if it resets at ~30 s, add a PING from a timer (the same SMC) |
| UEFI's own EFI watchdog (WatchdogTimerDxe, 5 min boot-manager timer) ends at ExitBootServices | verified (EDK II behaviour) | Mu-Silicium `WatchdogTimerDxe` in the FV |

## 2. PAC / arm64e at EL1

| Item | State | Evidence |
|---|---|---|
| `CONFIG_ARM64_PTR_AUTH=y`, `CONFIG_ARM64_PTR_AUTH_KERNEL=y` in the running kernel | verified | `hyp-evidence/config-grep.txt` |
| `paca pacg` in `/proc/cpuinfo`; "Address authentication (architected QARMA5 algorithm)" at boot | verified | `hyp-evidence/cpus-psci-watchdog-running.txt` |
| So Gunyah leaves PAC enabled for the HLOS (HCR_EL2.API/APK) as it does for its VMs (the arm64e k27 guest) | verified (Linux uses kernel PAC at EL1 on this system) | the two rows above |
| k27's PAC use (kernel JOP keys, user A keys) needs nothing else from EL2 | inferred | plan §4(c): no GXF/SPRR/APRR, non-SPTM pmap |

## 3. Memory map

Full table: `hyp-evidence/memory-map-running-dt.txt` (16 GiB unit; ABL-fixed `/memory` + `/reserved-memory`).

| Item | State | Evidence |
|---|---|---|
| DDR regions 0x80000000+2 GiB, 0x880000000+6 GiB, 0xa00000000+8 GiB; `/memory` 15.19 GiB | verified | memory-map file |
| Largest window without holes or reserved nodes: **0x8c0000000..0xc00000000 (13 GiB)**; the 0x880000000 region has firmware holes at 0x8af6fd000 (9 MiB), 0x8b5500000 (2 MiB), 0x8b7500000 (139 MiB) | verified | memory-map file |
| Chosen as the ADT DRAM range (`dts/piano.dts` `memory@8c0000000`, `/chosen dram-base 0x8c0000000 dram-size 0x340000000`) | decision | `logs/adt/piano-k27-adt.txt` |
| What XNU gets: DarwinBoot hands it exactly its allocated window (boot_args PhysicalBase/MemorySize = the window), sized by `/chosen/darwinboot-memory-size` = `configs/k27-adt.toml` `owned_memory_size`. Until 2026-11-12 capped at 8 GiB (`WindowSize.h`): **8 GiB** (hw.memsize 8589934592) in every native boot before. Since 2026-11-12 (`firmware/patches/darwinboot/0001`, bound 13 GiB): **0x33fe00000 = 13 GiB less 2 MiB** at 0x8c0004000..0xbffe04000: the kernel collection sits 0x4000 into its 2 MiB block (lowest vmaddr 0xfffffff007004000) and DarwinBoot starts the window at that offset in the run, so the 13 GiB run holds no larger 2 MiB multiple. A request no run holds whole (UEFI pages at the top of DRAM) is shrunk to the largest run, never below 8 GiB (`DarwinWindowFit`; the screen prints `window ... requested, ... taken (largest run)`). XNU side as in the VM at 13 GiB: k27 0051 (BOOTSTRAP_TABLE_SIZE 48 pages: 2 + 2 x 13 GiB-regions = 28 used) and 0052 (ZONE_MAP_VA_SIZE 12 GiB); `pmap_first_pnum` from `/chosen dram-base` 0x8c0000000, the window inside it. Check on the console: `NativeRamBlock: RAM window 0x8c0004000..0xbffe04000 (13310 MiB) in DRAM 0x8c0000000..0xc00000000` | verified (source, offline ADT) / needs-boot |
| DarwinBoot takes the first conventional-memory run that fits; on failure it draws `NO WINDOW ... FREE ...` on the panel | verified (source) | `DarwinBoot.c` AllocateWindow |
| UEFI memory map: pianoPkg's static `MemoryMapLib` (from Project SunUEFI, generated from piano's `xbl_config`) covers RAM below 4 GiB only (FD at 0xA7100000/3 MiB, "Kernel" 256 MiB at 0xA8000000, DXE heap ~421 MiB at 0xBD930000, Display_Reserved = splash 0xFC800000); `RamManagerDxe` (in the FV) maps every RAM partition of the Qualcomm RAM-partition protocol as conventional memory, which is how the high DDR appears | verified (source) / inferred (runtime) | `piano-uefi .../MemoryMapLib.c`; `SiliciumPkg/Drivers/RamManagerDxe`; FV listing in `logs/fd/` |
| The high window is free conventional memory in UEFI when DarwinBoot runs | inferred for 8 GiB from 0x8c0004000 (every native boot found its window, and no other run holds 8 GiB: it starts in the 0x8c0000000 run and crosses 0xa00000000); the whole 13 GiB run: needs-boot (the shrink rule covers UEFI pages at its top) | M2; 2026-11-12 window change |
| Gunyah-owned and VM regions (gunyah-hyp 0x80000000 18 MiB, trust-ui-vm 0xf3800000, oem-vm 0xf8800000, vm-shared, pvmfw) are all below 4 GiB, outside the XNU window | verified | memory-map file |
| Splash framebuffer `splash_region@fc800000` 43 MiB, 3200x2136x4 linear, live (simpledrm scans it out on Ubuntu) | verified | live DT; plan §4(b).1 |

## 4. CPU topology

| Item | State | Evidence |
|---|---|---|
| MPIDRs: 0x0, 0x100, 0x200, 0x300, 0x400, 0x500 (cluster 0, core in Aff1) and 0x10000, 0x10100 (cluster 1 in Aff2) | verified | live DT `cpu@*`; `UniXNU-sm8750-common/dts/sm8750.dtsi` |
| Capacities 1792 (cores 0-5, 3.53 GHz) and 1894 (cores 6-7, 4.32 GHz) | verified | `hyp-evidence/cpus-psci-watchdog-running.txt` |
| ADT cluster types: cores 0-5 **E**, cores 6-7 **P** (k27 is AMP and needs an E cluster for background QoS; the six slower cores are it) | decision | `dts/piano.dts`; `logs/adt/piano-k27-adt.txt` |
| k27 masked MPIDR Aff1/Aff0 only in the reset handler (`and x0, x15, #0xFFFF`) and `ml_get_cpu_number`, so cpu6/cpu7 would have taken cpu0's/cpu1's cpu_data: **fixed** in k27 patch 0054 (Aff2|Aff1|Aff0; neutral in the VM where Aff2 = 0), as UniXNU-sm8750-common's `sm8750-smp.patch` does for UniXNU kernels | verified (source, disassembly) | `patches/k27/0054-k27-SM8750-MPIDRs-Affinity2-in-the-reset-handler-s-and-ml_ge.patch`; `llvm-objdump` shows `and x0, x15, #0xffffff` and `and w8, w8, #0xffffff` |
| ArmCpu (CPU_ON over the ADT /psci method, SGIs with Aff2 encoded in ICC_SGI1R) handles these MPIDRs | inferred (source) | `ArmCpu/ArmCpu.cpp` (sgi1r encoding, start check on the low 16 bits) |
| Bring-up order: 1 CPU first (`cpus=1` entry), 8 CPUs as the second menu entry | decision | `out/ram` macOS image menu |
| 8 CPUs with two cluster types under k27 | VM test (item 4) / needs-boot | `el1-slice1-report.md` item 4 |

## 5. Conduits and traps

| Item | State | Evidence |
|---|---|---|
| PSCI natively over **SMC** (`/psci method "smc"`, PSCI 1.0 compatible) | verified | live DT |
| k27 0053: the kernel's own PSCI calls (halt_all_cpus SYSTEM_OFF/RESET) use the ADT /psci method; the build flag `XNU_VM_GUEST` only sets the fallback (1: HVC, 0: SMC). Native KC built with 0 (`ml_psci_conduit` initial value 2 = SMC; VM kernel 1 = HVC) | verified (source, binary) | `patches/xnu-12377/0053-*`; Mach-O `__DATA_CONST` read of `_ml_psci_conduit` |
| Gunyah handles PSCI SYSTEM_RESET from the HLOS (SMC #0, 0x84000009); SYSTEM_RESET2 is advertised but returns -1 on the 13T | inferred | 13T `phone-bootstrap-tables.patch` comment |
| SCM calls (PAS for ADSP/ZAP, `set_remote_state`) are SMC from the HLOS and work from Linux | verified (Linux) / inferred (XNU) | Ubuntu runs the GPU and ADSP; Adreno830 S7 "PAS 13" on the 13T |
| IMPDEF system registers: k27 runs under Gunyah as a VM without undefined-instruction traps, and the HLOS is trapped less than a VM; the 13T kernel (same xnu base, BCM2837 config) runs natively | inferred | VM accept path; 13T |
| Architected timer 19.2 MHz (virt); k27 0042 scales it to the 24 MHz timebase userspace expects (`droidvm-timebase-hz=24000000`, sheng's `timebase-hz`); ADT `timebase-frequency` 0x124f800 | verified | dmesg; ADT |

## 6. SMMU ownership

| Item | State | Evidence |
|---|---|---|
| The HLOS programs stage 1 of the apps SMMU `arm-smmu-500 @0x15000000` and the adreno SMMU `@0x3da0000`; Gunyah owns stage 2 | verified (Linux does it on this system) | `CONFIG_ARM_SMMU_QCOM=y`; running DT |
| SIDs: UFS 0x60, USB 0x40, MDSS 0x800 mask 2 (+ stock overlay 0x801 secure CB), GENI QUP 0x423/0xa3 and GPI DMA 0x436/0xb6, GPU 0 and 1 (adreno SMMU), GMU 5, video 0x1940/0x1947, ISP 0x1c00 | verified | `hyp-evidence/iommus-running-dt.txt` |
| UniXNU's `ArmSmmu4K` / `Sm8750Platform` "SMMU takeover" is the model for native drivers | inferred | UniXNU kits |
| Whether UEFI leaves streams in bypass or with its own context banks (affects the first DMA of NativeUfs) | needs-boot | ufs.md |

## 7. Logging without a UART, and what stays unknown

| Item | State | Evidence / plan |
|---|---|---|
| Bring-up console: XNU verbose text on the GOP framebuffer handed over by DarwinBoot (`-v`, `panic-spin=1`); DarwinBoot and UniXNU draw their own diagnostics | verified (source) | DarwinBoot DiagDraw; UniXNU menu |
| k27 `XNU_DT_PANIC_REGION`: uses ADT `/pram` + `/chosen/embedded-panic-log-size`; without them no panic region (safe). The piano ADT has none yet: Linux's `ramoops@a3500000` (4 MiB) overlaps pianoPkg's `Display_Demura` UEFI region (0xA3500000, 44.5 MiB), which the display DXE may read. A free, UEFI-unused region that Ubuntu can read back is still to be chosen and tested with a marker | verified (source) / open | `pexpert/arm/pe_init.c`; MemoryMapLib.c |
| Watchdog not armed; high DDR visible to DarwinBoot; IMPDEF registers untrapped for the HLOS; SCM PAS from XNU; UFS stream state left by UEFI; GOP mode and stride as UEFI sets it | needs-boot | M1/M2 |
