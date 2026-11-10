# Native macOS at EL1 on piano (Xiaomi Pad 8 Pro, SM8750): implementation plan

Written 2026-11-10 (evening SGT). Planning only: nothing was built, flashed, started or stopped. The Gunyah guest/VM
path is frozen as of this date and documented for a later return in `gunyah-guest-SWITCHBACK.md`.

Companion documents in this directory: `native-display-gpu-research.md` (topology dossier, driver ratings),
`native-display-8bit-scanout-design.md` (8-bit scanout and DPU plane rules), `thin-direct-report.md` and
`thin-direct-measurements.md` (panel and input measurements), `streamB-input-report.md`, `streamC-charging-report.md`,
`streamC2-auth-report.md`, `kernel-first-vmm-plan.md` (Gunyah ioctl/RM knowledge), `crosvm-source-review.md`. Earlier
feasibility work: `../../unixnu-research/PIANO-XNU-PLAN.zh-CN.md` (2026-11-08, UniXNU bare-metal survey for piano).

---

## 1. Goal and non-goals

**Goal.** Boot macOS 27 natively on the tablet in the UniXNU style: an Android `boot.img` whose kernel field is
BootShim + a Project Mu UEFI firmware volume, UEFI runs `UniXNU.efi` and `DarwinBoot.efi`, DarwinBoot starts a
source-built, patched XNU (our k27: xnu-12377 + 52 patches, 4 KiB pages, arm64e) **at EL1 as the HLOS** with Gunyah left
in place at EL2. Apple's kexts come from the 27.0.1 kernel collection as today; the hardware is driven by Darwin drivers
written against IOKit/IOMFB/IOGPU/IOHID, with the Ubuntu drivers used as the behavioural specification.

End state for the first release: power on → UniXNU menu → macOS login window on the panel at the panel's native size,
touch and keyboard cover working, storage on UFS partitions of our own, Wi-Fi, charging, GPU-composited desktop.
Ubuntu stays installed and bootable from the same tablet.

**Non-goals (hard).**
- No EL2 takeover. `hyp_a`/`hyp_b`, `xbl`, `xbl_config`, `tz`, `abl`, `vbmeta`, `dtbo`, `persist` and every `_b`
  partition are never written. Snapintosh-style EL2 and KVM are out of scope for this plan.
- No boot-chain write (no `boot_a`/`init_boot_a`/`vendor_boot_a` flash, no GPT change) until the USB-C cable to the Mac
  is connected and the rollback image set is verified on the host. `fastboot boot` of a RAM image also needs the cable.
- No unattended experiments on the tablet, and never while the macOS VM is running unless the owner has agreed to stop it.
- No copying of Linux (GPL) driver code into Darwin drivers. Linux is read as the specification, Darwin code is new.
- Do not break today's fallback: Ubuntu dual boot and the 1600x1068 direct-KMS VM path (`gunyah-guest-SWITCHBACK.md`).

---

## 2. Why EL1 is viable here (verified facts, 2026-11-10 ~18:05 SGT, read-only)

| Fact | Source |
|---|---|
| Kernel log: `CPU: All CPU(s) started at EL1` | tablet dmesg (read-only) |
| Kernel log: `gunyah: Running under Gunyah hypervisor 51/v1` | tablet dmesg |
| `/dev/gunyah` exists, `/dev/kvm` does not | tablet |
| Partitions `hyp_a`, `hyp_b`, `xbl`, `xbl_config`, `tz`, `uefi`, `abl`, `vbmeta` present | tablet GPT; full images in the 2026-11-10 backup |
| A custom mainline Ubuntu kernel boots at EL1 from `PARTLABEL=linux_rootfs` (7.2.15-piano58usb1) | the running system |
| Reserved regions `gunyah-hyp@80000000` (18 MiB, no-map), `oem-vm`, `trust-ui-vm`, `pvmfw`, `vm-shared` | live DT (`PIANO-XNU-PLAN.zh-CN.md` §2) |
| PSCI 1.1, conduit SMC, OSI mode; 8× `qcom,oryon`; GICv3 dist `0x16000000` / redist `0x16080000` | live DT |
| UniXNU ports on this SoC (OnePlus 13T, Ace 6, Odin 3) run XNU at EL1 under the same Gunyah; the 13T has a Metal desktop on the A830 | `unixnu-research/UniXNU/devices.toml`, `UniXNU-Pagani_SM8750/README.md` |
| A UniXNU device record for piano exists: "first XNU boot from RAM (OnePlus 13T base)", iPadOS 17 profile, not a running port | `UniXNU/devices.toml` (~line 2117) |
| Our k27 kernel already runs the macOS 27.0.1 userland (WindowServer, Dock, Finder, Metal via the bridge) on 4 KiB pages, at EL1, under Gunyah, as a VM guest | `PROGRESS-thin-macos.md`, `README-macos.md` |

The last row is the difference from every published attempt: UniXNU has **no macOS profile** and `macOS-ForSheng`
stopped at the Recovery language picker because WindowServer exited. Here the macOS userland problem is already solved
in the VM. What is new is running the same kernel as the HLOS with real devices.

---

## 3. Repo split

- **Device repo: `lenzhang/xiaomipad-8pro-macos`.** Everything specific to piano: `board.toml`, `porting.toml`,
  `feeds.toml`, `dts/piano.dts` (+ the ADT generation inputs), the UEFI platform package `pianoPkg` (or a pointer to the
  Mu-Silicium tree that holds it), the Darwin drivers for piano hardware (panel/DPU bottom half, NT36532E touch, keyboard
  cover, PMIC/charging, backlight), board glue for Adreno830/Sm8750Platform, boot-image build and signing scripts (key
  stays private), the install/partition procedure, and this plan. Private material (`boot-signing.pem`,
  `thp-calibration.json`, MAC addresses, any password) is never committed; keep an `.gitignore` for `private/`.
- **Shared tooling repo (name not final; "Veil" was a candidate).** Board-independent pieces that came out of the VM
  work and are not UniXNU upstream: the k27 XNU patch set and build scripts (`build-k27-piano.sh` generalised), the
  kernel-collection tooling (`k27/kc-build27*.py`, `kc27-layout.py`, `segaddrs`), the macOS DarwinBoot variant and
  payload tooling (`scripts/build-darwinboot.py`, `piano-macos-boot.py` make_adt/kernel_args), the IOMFB "top half"
  display driver (DroidVMDisplay minus virtio), the Metal plugin/bridge/Turnip build for macOS 27, the owner-root and Data
  seed staging (`stage-piano.sh`, `stage-gpu-plugin.py`, systemos/os image recipes). The name does not block anything:
  start it as a directory in the device repo (`tooling/`) and split when a second board needs it.
- **Upstream dependencies, not forks:** `UniXNU/UniXNU` (core, kits, `XnuPrelink`), `UniXNU-sm8750-common`,
  `Adreno830`, `AdrenoMetal`, `ShengUfs`, `QcomDwc3`, `ShengNovatekTouch`, `Ath12kWifi`, `QcaBluetooth`, Mu-Silicium
  (`hackios-mu-silicium` `annibalePkg` as the SM8750 Xiaomi template). Pin commits in `unixnu.lock`.

Git for this session: none. The follow-up implementation session creates the repos. If it wants to snapshot the frozen
guest path first, a local archive branch in `droidvm-build/` is fine as long as it touches no remote and no boot chain.

---

## 4. Workstreams that run in parallel now (no flashing, no tablet writes)

Each workstream has an owner-independent input set and a verifiable output. None needs the cable.

### (a) Read-only hypervisor / exception-level notes

Mostly done (section 2). Produce `el1-hyp-notes.md` in the device repo with the verified table above plus answers to
these, from sources that need no tablet access (the piano kernel source `~/kfirst/linux` v1.1.0 on the private build host, the
tablet's `/proc/config.gz` copy, the live DT dump already used for `PIANO-XNU-PLAN.zh-CN.md`, the backups):

1. **Watchdogs.** Does the HLOS have to pet anything to stay alive? Check `GUNYAH_WATCHDOG=y` in the piano config and
   `drivers/virt/gunyah/*watchdog*` (or the qcom `gh_watchdog`): is the hypervisor watchdog armed by firmware or by
   the Linux driver? Same for the APSS `qcom,apss-wdt` at `0x17c10000`. If either is armed before HLOS runs, XNU needs a
   petting path from day one, or the first boot reboots after ~30 s. The 13T port is evidence that it is solvable.
2. **PAC/arm64e at EL1.** Confirm `CONFIG_ARM64_PTR_AUTH*` in the HLOS config and `paca`/`pacg` in a saved
   `/proc/cpuinfo`. Our arm64e k27 already runs under this Gunyah as a guest, so the hypervisor does set
   `HCR_EL2.API/APK` for VMs; the HLOS is expected to have it too. Record the evidence.
3. **Memory map.** Extract every `memory` range and `reserved-memory` node (no-map or not) from
   `sm8750-xiaomi-piano.dts` and the live DT. XNU takes one contiguous DRAM range from `boot_args`; decide which window
   is the primary (the high `0x880000000..` segment looked largest in the 2026-11-08 survey) and what the UEFI memory
   map must reserve. k27 patches 0051/0052 already cover bootstrap tables and zone VA to ~17 GiB.
4. **CPU topology.** 8 Oryon cores, two clusters. k27 is an AMP kernel (`sched_amp`) and needs at least one cluster
   typed "E" or background work never runs (the VM's EEPPPP finding, `piano-macos-gpu-host` notes). Decide the ADT
   `cluster-type` assignment (prime pair vs the six performance cores) and the MPIDR list for ArmCpu's PSCI `CPU_ON`.
5. **Conduits and traps.** PSCI goes over SMC natively (the VM used HVC: k27 patch 0051). SCM calls (zap loader PAS,
   `set_remote_state`) are SMC from the HLOS and are known to work from Linux. List the IMPDEF system registers XNU's
   BCM2837/ArmCpu configuration touches and check none is trapped by Gunyah (the 13T port implies none).
6. **SMMU ownership.** Under Gunyah the HLOS programs stage 1 of `arm-smmu-500` (`0x15000000`, apps) and the adreno SMMU
   (`0x3da0000`). UniXNU's `ArmSmmu4K`/`Sm8750Platform` "SMMU takeover" is the model; note the SIDs from
   `native-display-gpu-research.md` §1a (GPU 0/1, GMU 5, MDSS `0x800` mask 2, UFS/USB on the apps SMMU).
7. **Still unknown after this:** nothing in the above can be settled 100 % without a RAM boot. Mark each item as
   verified-from-source / inferred / needs-boot.

Output: the notes file. No tablet writes. Reading the tablet is allowed only with explicit owner consent and only
read-only (`cat`, `zcat`, `dmesg`); the owner has already captured the dmesg lines in section 2.

### (b) Boot-chain scaffolding: UEFI, BootShim, DarwinBoot, boot image (build and verify artifacts only)

Inputs: `hackios-mu-silicium` (`Platforms/Xiaomi/annibalePkg`, Redmi K90, Xiaomi SM8750), `uefi-sheng` (UniXNU
integration), the stock `uefi`/`xbl_config`/`imagefv` partition images from the 2026-11-10 backup (the private build host
`~/piano-backup-20261110/`, Mac subset `droidvm-build/piano-backup-20261110/`), the merged piano DTB (Ubuntu set's
`vendor_boot` base DTB + `dtbo`, or the mainline `sm8750-xiaomi-piano.dtb`), `boot-profile.json` (header fields of the
stock v2 container) and `tools/aosp-avb.json`, `tools/build-boot.py` from `xiaomipad-8pro-mainline` (how the Ubuntu set
is packaged and signed with `boot-signing.pem`).

Steps:
1. `pianoPkg` from `annibalePkg`: `Resources/Configs/piano.toml` (FD base/size, check `0xA7100000`/3 MiB against piano's
   `uefiplat.cfg` in `xbl_config`), memory map from the DT reserved regions, DXE binaries extracted from piano's own
   `uefi.elf`/`imagefv.elf`, the known Xiaomi SM8750 DXE patches (ButtonsDxe, PmicDxe/SPMIDxe, QcomChargerDxe, TzDxe,
   UFSDxe, UsbPwrCtrlDxe) applied as in annibale, `SimpleFbDxe` on the ABL splash framebuffer
   (`splash_region@fc800000`, 3200x2136x32 linear; Linux `simpledrm` scans it out at boot today, so it is live),
   `UniXNURamDiskDxe`, DiskIo, Fat, `UniXNU.inf`, `DarwinBoot.inf`, a `PianoBootMemoryDxe`.
2. BootShim built with `-DFD_BASE -DFD_SIZE -DREQUIRES_KERNEL_HEADER=1`; kernel field = gzip(BootShim ‖ FD), DTB
   appended; ramdisk = the FAT payload (`UniXNU.efi` config, `DarwinBoot.efi`, k27 kernel collection, ADT, trust caches,
   Data seed). Reuse `macOS-DroidVM/scripts/build-darwinboot.py` and the `darwinboot-reclaim-memory-and-512mb-kernel`
   patch; the macOS DarwinBoot variant already loads our KC.
3. Package two images with the stock v2 container exactly as the Ubuntu set is built: `boot-piano-uefi-ram.img`
   (for `fastboot boot`, UniXNU menu only) and `boot-piano-macos-ram.img` (menu + DarwinBoot + KC). Sign with the same
   key and tooling as the Ubuntu set (the key never leaves the tablet/the private build host `private-install-work/`).
4. Verify offline: header fields equal `boot-profile.json` except sizes; `avbtool verify_image` with the public key;
   the gzip stream unpacks to BootShim+FD with the expected FD base; the appended DTB is the piano DTB; the FAT payload
   mounts and lists the files; SHA-256 of every artifact recorded in `ARTIFACTS.md`. For UEFI itself there is no
   emulator; the first real check is M1 below.

Definition of done: both images built reproducibly by one script from pinned inputs, verification log committed,
nothing flashed. The images wait for the cable.

### (c) XNU: 4 KiB pages, EL1 without Apple's EL2/SPTM contract, device tree / ADT

Inputs: `macOS-DroidVM/patches/xnu-12377/0001..0052`, `build-k27-piano.sh`, `k27/`, the KDK recipe (memory
`piano-macos-k27-build`), `scripts/piano-macos-boot.py` (make_adt from an FDT), UniXNU `docs/xnu/four-k-page-tables.md`
and `docs/porting/arm64e.md`, `UniXNU-sm8750-common/patches/xnu/sm8750-smp.patch`, `drivers/ArmDTPlatform`, `ArmCpu`.

What already exists and carries over unchanged: the BCM2837 4 KiB machine config; the macOS-target fixes (0003, 0007);
the 4 KiB user ABI pieces (0010 user VA width, 0017 4 KiB code signing, 0037 AMFI page shift, 0047 16 KiB mmap
alignment, 0045 A-keys for arm64 processes); owner root and Data volume mounts (0008, 0023/0024, 0038/0039, 0044); AMP
(0021); k27 KPIs (0025–0033); the non-SPTM pmap (no SPTM/TXM anywhere). The arm64e PAC runs as JOP/user keys in the
kernel; nothing depends on an Apple EL2 contract (GXF, SPRR, APRR are not used in this configuration).

Work:
1. **Native build variant.** `XNU_VM_GUEST=0` for the HLOS build: PSCI conduit SMC (0051 keeps HVC for the VM), keep
   0050's 16-bit ASIDs (harmless), keep 0051 bootstrap-table sizing and 0052 zone VA. Add a build flag for
   `XNU_NATIVE_ARCH_TIMER` already in `start.S` and check the 24 MHz timebase patch (0042) against the real
   `cntfrq` (Linux reports it; usually 19.2 MHz on this SoC: the VM's 24 MHz came from crosvm).
2. **ADT from the real DTB.** Extend `piano-macos-boot.py`'s make_adt (or UniXNU's `make-droidvm-adt.py`) to take the
   piano FDT: `/cpus` with 8 entries and `cluster-type`, `/arm-io` children for GIC, timer, the SPMI/PMIC, GENI SE for
   touch (`spi@a88000`) and keyboard (`i2c@a98000`), UFS `0x1d84000`, DWC3 `0xa600000`, MDSS `0xae00000`, GPU
   `0x3d00000` + GMU `0x3d6c000`, `/defaults`, `/options`, `/chosen` with the NVRAM proxy, `dram-base`/`dram-size` from
   the chosen window, the board identity (keep `VirtualMac2,1`? decide: the vma2 identity made apsd work; a Mac
   identity with real hardware may be better for WindowServer. Default: keep vma2 until something needs otherwise).
3. **Test rig without flashing: the existing crosvm VM.** Boot the natively configured KC (SMC conduit replaced by a
   build switch, 8-vCPU ADT with two cluster types, the new ADT generator) inside the Gunyah VM in a session where the
   owner allows stopping the VM. Everything except real devices is covered this way. The VM has 6 vCPUs today; use
   `CPUS=8` boot disks for the topology test.
4. **Drivers in the kernel, UniXNU style.** Natively, drivers are static archives linked into the kernel
   (`[kernel] archives`), gated by boot args and started by `Stage7Probe`. Plan the archive list for piano:
   ArmDTPlatform, ArmCpu, NativeAES, NativeRamBlock, NativeUfs, the IOMFB display top half, GopFramebuffer, QcomGeniSpi,
   touch, keyboard, PMIC/charging, Adreno830 + Sm8750Platform + the macOS AdrenoMetal glue (`DroidVMGpuKext.cpp`
   becomes the AGXAccelerator publisher for the real KMD).
5. **Logging without a UART.** Decide the bring-up console: UniXNU's on-panel log window (`sheng-log`) plus a panic
   region (`XNU_DT_PANIC_REGION=1` already in the build) that Ubuntu can read back after a reboot if the region survives
   (test with a marker). The DockChannel serial dext stays for a later USB gadget console.

Definition of done: the native KC builds from the same tree with one flag; the ADT generator produces a piano ADT from
the mainline DTB with a diff against the VM ADT reviewed; the native KC boots to the login window **in the VM** with the
8-CPU/two-cluster ADT and the SMC-vs-HVC switch exercised (one VM session, owner present).

### (d) Native Darwin drivers (design + offline code; hardware tests follow M2/M3)

Order by what gates a login: display first frame → storage → input → charging → GPU → Wi-Fi → audio.

1. **Display.** Keep DroidVMDisplay's IOMFB top half (swap queue, timing/colour elements, `IOMobileFramebufferABI.h`
   for 27.0.1) and give it two bottom halves:
   - **GOP/splash adoption** (first light): write the composited frame into the ABL splash framebuffer (CPU copy, as
     the VM's simplefb stage did). UniXNU `GopFramebuffer` / `NativeMobileDisplay` with `sheng-dpu-scanout=2` style
     pipe readout is the template (`docs/porting/display.md`).
   - **DPU direct scanout**: WindowServer renders 8-bit `'BGRA'` (the `droidvm-fb-bgra=1` WindowServer-only branch
     patch in `native-display-8bit-scanout-design.md` §1a; `MACOS_GAMUT=sRGB`), and the kext flips SSPP addresses on
     the pipeline ABL left running: VIG SSPP with QSEED for 1600x1068→3200x2136 or a 1:1 3200x2136 plane, dual-LM
     split, CTL flush, INTF/TE vsync IRQ inside the MDSS interrupt controller (SPI 83) → `notify_swap_done`, GPU fence
     wait before flip, IOSurface pages mapped in the apps SMMU (SID `0x800`). Formats, scaler and pitch limits are in
     that design's §2d. Then DPMS (panel sleep DCS sequence of `panel-novatek-nt36532.c` as the spec), brightness
     through 2× KTZ8866 over GENI I2C (UniXNU-sheng `AppleARMBacklight.cpp`, `GeniI2c.h` as templates), and only
     much later a cold modeset (dual-DSI DSC video mode, 3 nm PHY PLL) if ever needed.
   - Panel facts to bake in: 3200x2136 @ 144 Hz (also 60/90/120), physical 239x163 mm, DSI connector on `card1`,
     `thin-direct-measurements.md` for DPU IRQ rates and the `XR24` scanout the DPU accepts.
2. **Storage.** `NativeUfs` (ShengUfs lineage, "host-verified" on the 13T) for `qcom,sm8750-ufshc` at `0x1d84000`
   (MCQ at `0x1da0000`, 4 KiB logical blocks, 6 LUNs). Target layout: APFS OS, SystemOS and Data partitions carved from
   the tail of `linux_data` on LUN0 (section 6). Until then the first boots use `NativeRamBlock` + a Data seed and
   read-only UFS probes. Reuse the owner-root policies and image recipes from the VM (`make-macos-systemos.sh` at
   16384 MiB, `os.raw` from the IPSW, `stage-piano.sh` seed) by writing the same containers to real partitions.
3. **Input.**
   - Touch: NT36532E on GENI SPI (`spi@a88000`, QUP1 SE2). The Linux side is host-processed (THP:
     `piano-thp`/`piano-thp-live`, firmware per panel vendor, BOE on this unit, `thp-calibration.json`). First
     check whether the controller firmware can report coordinates itself (ShengNovatekTouch path, same NT36532
     family). If not, write a Darwin THP userspace daemon feeding an IOHID digitizer, using the Linux service as the
     specification (electrode table 40x60, calibration file, Y inversion, 12 slots, ranges X 0..31999, Y 0..21359 from
     `streamB-input-report.md`). This is the largest single uncertainty of the whole plan.
   - Keyboard cover: "Xiaomi Keyboard" over GENI I2C (`i2c@a98000`, nanosic HID bridge, MCU firmware
     `nanosic/piano/MCU_Upgrade.bin` identity only) plus the cover touchpad (clickpad 3200x2136) and the tablet-mode
     switch: a Darwin I2C-HID transport kext for IOHIDFamily, with Ubuntu's `hid-nanosic` behaviour as spec.
   - Keys: volume on TLMM gpio-keys, power on PM8550 PON over SPMI, lid/cover switch (SW_LID today).
4. **Charging and power.** Required early: when `piano-charging.service` stopped, the cells drained at about
   -1.8 A/cell even on the cable (`PROGRESS-thin-macos.md` 11:11 incident), so a tablet running native XNU without a
   charging driver is battery-only. Spec from Linux: `xiaomi_mca` (ADSP-mediated charger state over pmic_glink/MCA relay),
   PD contract then **data-role swap to DFP before reading SVID 0x2717** (`streamC2-auth-report.md`,
   `xiaomi-mipps-auth-dfp-before-svid.diff`), MiPPS authentication, PPS APDO 10 V/6.1 A, `sc8541` charge pump over I2C,
   `bq27z561` gauges, thermal staging at 42.5–44 °C. Decide early whether the ADSP (remoteproc PAS load, glink) is a
   prerequisite for charging or only for the fast path; read `device/power` and `device/platform` in
   `xiaomipad-8pro-mainline` for the stock sequence. Minimum viable: keep the input path on at the stock 9 V/1.6 A so
   sessions do not drain the battery; MiPPS fast charge later.
5. **GPU.** Port UniXNU `Adreno830` (A830, chip id `0x44050001`, same as the 13T) + `Sm8750Platform` kit to piano:
   firmware `gen80000_{sqe,gmu,aqe}` and the device-signed `gen80000_zap.mbn` from `/lib/firmware/qcom/sm8750/xiaomi/piano/`,
   cmd-db votes, GMU gen8 HFI, adreno-SMMU takeover. The userspace stays what the VM uses today (NativeAdrenoMetalPlugin +
   NativeBridgeMetalFrontend + metal2vulkan + Turnip with `freedreno-kmds=darwin`); Turnip's darwin KMD talks to the
   NativeAdreno user client, which natively fronts the real hardware instead of virtio. Bring over the render-barrier and
   ring-priority fixes as knowledge, not as crosvm patches. Expect the 3200x2136 hang investigation to restart on
   different ground (no virgl, no host preemption).
6. **Wi-Fi, Bluetooth, audio, sensors** (later): `Ath12kWifi` with WCN7860 PCIe (`pcie@1c00000`) firmware
   `ath12k/WCN7860/hw2.0`, `QcaBluetooth` with `gngbtfw20.mbn` on `serial@898000`, LPASS + SoundWire + 4× FS19xx
   (new), ADSP FastRPC sensors.

Each driver gets a one-page design in the device repo before code: register map source, Ubuntu behaviour used as spec,
IOKit family it binds to, boot arg, test on hardware, fallback.

---

## 5. Later, serial, gated on USB-C + rollback

Prerequisites for every step here: the cable is connected and `fastboot devices` lists the tablet; the Ubuntu and Android
image sets are on the host and verified (`SHA256SUMS`); the 2026-11-10 full backup `verify.sh` passes; battery ≥ 40 %;
owner present. Rules from `xiaomipad-8pro-mainline/docs/FLASHING.en.md`: never write `vbmeta`, `dtbo`, `persist`, any
`_b`, never `--set-active=b`, never `flash_all_lock`.

- **M1: UEFI RAM boot.** `fastboot boot boot-piano-uefi-ram.img` → UniXNU menu on the panel (splash GOP), UEFI shell
  sees the UFS LUNs and the GPT. Nothing written. Power-off or `fastboot reboot` returns to Ubuntu.
- **M2: XNU RAM boot.** `fastboot boot boot-piano-macos-ram.img` → kernel log on the panel, PSCI brings up 8 cores,
  the RAM Data seed mounts, the login window draws through the GOP bottom half (CPU). Watchdog, memory map, PAC and
  topology questions from (a) get their final answers here. Still nothing written. Each run needs the cable.
- **M3: UFS and partitions.** Read-only NativeUfs tests on existing partitions first (hash a known block range
  against Linux). Then the partition change: boot the release's RAM maintenance image (`fastboot boot
  boot-ram-maintenance.img`), shrink `linux_data` (ext4 `resize2fs` then `sgdisk`, read back and compare with the saved
  GPT), create `macos_os`, `macos_systemos`, `macos_data` on LUN0, write the APFS containers built by the VM recipes,
  verify SHA-256. `RESTORE-20261110.md` scenario C is the rollback (GPT backup + partition images).
- **M4: Persistent boot.** Add a third image set "macos" to the switching tooling (`piano-boot-switch` store, host
  `piano-switch.py`), with slot-success handling: ABL decrements slot A's retry count until the booted OS marks success
  (`piano-slot-success` on Ubuntu), so native XNU must either mark success itself (GPT attribute write via NativeUfs) or
  the set is flashed with the counter reset and a bounded number of unattended boots. Until then, every XNU boot is a
  `fastboot boot` with the cable.
- **M5: Install and polish.** Data volume on UFS, Wi-Fi, charging, GPU, brightness, sleep, dual-boot UX.

---

## 6. Reuse from the guest path vs leave frozen

| Reuse now | Where it goes |
|---|---|
| k27 kernel + patches 0001–0052, build recipe, KDK handling, KC tooling | workstream (c), shared tooling |
| macOS DarwinBoot variant, payload/FAT tooling, ADT generator (`piano-macos-boot.py` make_adt) | (b), (c) |
| DroidVMDisplay IOMFB top half + `IOMobileFramebufferABI.h`; the 8-bit scanout design (format selection patch, DPU format/scaler/pitch table, fence and lifetime rules, R1–R10 risks) | (d.1) |
| Metal plugin, bridge, metal2vulkan, macOS Turnip build, render-barrier fix, `TU_DEBUG=sysmem` knowledge | (d.5) userspace unchanged |
| Owner root policies, systemos/os image recipes, Data seed staging (`stage-piano.sh`, disabled daemons list: fskitd, AppSSODaemon, cryptexd), VirtualMac2,1 identity, E-core AMP finding, en0 seeding idea | (d.2), (c) |
| KMS/evdev measurements: panel modes/IRQs/power (`thin-direct-measurements.md`), touch ranges/calibration/letterbox math in `crosvm-kms/display_kms.c`, keyboard/touchpad/lid device facts (`streamB-input-report.md`) | (d.1), (d.3) as specification |
| Charging: `streamC*-report.md`, the DFP-before-SVID fix, PD/PPS/sc8541/bq27z561 facts, `charging-sample.sh` sampler | (d.4) |
| Topology dossier: DT nodes, IRQs, SMMU SIDs, firmware names, Gunyah lend/IRQ findings (`native-display-gpu-research.md` §1) | (a), (d) |
| Disk layout ideas: OS/SystemOS/Data split, `data-store` persistence model, 16 GiB systemos size | (d.2), M3 |
| ghk stage 0–4 notes and `kernel-first-vmm-plan.md`: Gunyah RM/ioctl/vCPU behaviour, watchdog observations | (a) background; also the knowledge base if macOS ever hosts VMs |
| Backup + restore procedure, flashing rules, image-set tooling | section 5 |

| Leave frozen (do not port, do not delete) | Note |
|---|---|
| crosvm-mmio tree, `crosvm-kms/`, every crosvm/virgl/virtio-media patch and `.py` patcher | VM only |
| ghk.ko, crosvm-ghk anchor, tiny guest, `kernel-first/` | VM only; stage 4 passed, vhost-net+ghk not rerun |
| Thin roots (`/home/thin/thin-{2..5}.sqfs`), `99thin` dracut picker, `thin/`, `thin-direct/` scripts | VM only; still need the cable to bake into a boot image |
| DroidVM virtio kexts: Ethernet, Audio, Video (VT paravirt), VirtioBlock, Input, Gpu tunnel | replaced natively by real drivers |
| The 3200x2136 GPU-hang investigation under virgl | restart natively if it reappears |
| `macos-direct.sh`, `direct.sh`, `direct-ended.sh`, `macos-vm-*` units | documented in `gunyah-guest-SWITCHBACK.md` |

---

## 7. Risks and rollback

| # | Risk | Mitigation |
|---|---|---|
| R1 | A hypervisor or APSS watchdog reboots the tablet ~30 s into the first native boot | (a.1) answers it from source before M2; add a petting path or disable in UEFI; a RAM boot that reboots is harmless (Ubuntu returns) |
| R2 | Boot-chain write leaves slot A unbootable and ABL falls to the dead stock slot B | Nothing is flashed until M4; `fastboot` + the Ubuntu/Android sets restore slot A (FLASHING scenario); full backup verified |
| R3 | GPT/partition mistake on LUN0 | Only from the RAM maintenance image, with `sgdisk` backup/compare, scenario C rollback; `/home` tarball exists |
| R4 | Battery-only native sessions (no charging driver) | Short sessions, charge in Ubuntu between runs, (d.4) early; never leave a native boot unattended |
| R5 | Thermal: no DVFS/thermal control early | Keep early sessions short, idle CPU in WFI (patch 0020 style), GPU off until (d.5) |
| R6 | Touch needs THP in Darwin (new daemon + calibration) | Check the firmware's coordinate mode first; keyboard cover gives a login path meanwhile |
| R7 | Memory map: multi-segment DRAM vs XNU's single range | Pick the largest window in (a.3); accept less than 16 GiB at first |
| R8 | WindowServer behaves differently with a non-virtual board identity | Keep `vmm-present`/vma2 identity in the first native boots; switch deliberately with the BGRA branch patch |
| R9 | Video-mode dual-DSI DSC panel: no DPMS/brightness driver | Accept fixed brightness and no blanking until (d.1) DPMS; the GOP adoption needs no modeset |
| R10 | License contamination (Linux GPL into Darwin) | Spec-only reading; Darwin code written fresh; note sources in each driver's design page |
| R11 | Secrets leaking into repos (`boot-signing.pem`, calibration, PSKs, passwords) | `private/` ignored; existing tablet scripts that embed credentials stay on the tablet; scrub before any publish |
| R12 | Loss of the daily-use fallback | Ubuntu and the direct-KMS VM path are untouched; `gunyah-guest-SWITCHBACK.md` restores them |

Rollback summary: at any point before M4 nothing on the tablet has changed. After M3, `RESTORE-20261110.md` scenarios
A–C cover boot set, filesystems and GPT. The factory 303 backup remains the last resort.

---

## 8. Milestone order and the first implementation slice

Parallel from day one: (a) hyp notes, (b) boot-chain artifacts, (c) native XNU variant + ADT, (d) driver design pages
and offline code (display bottom halves, UFS, touch probe, charging, Adreno830 port scoping). Serial once the cable is
there: M1 → M2 → M3 → M4 → M5.

**First implementation slice (definition of done, no flashing):**
1. Device repo skeleton with `board.toml`, `feeds.toml`, `dts/piano.dts`, `docs/el1-hyp-notes.md` (workstream a, every
   item marked verified/inferred/needs-boot), `private/` ignored.
2. `pianoPkg` builds an FD; BootShim + FD + DTB + FAT payload packaged into the two RAM-boot images, signed like the
   Ubuntu set; the offline verification log (header, AVB, gzip layout, payload listing, SHA-256s) committed.
3. k27 native variant builds behind one flag; the ADT generator emits a piano ADT from the mainline DTB; a reviewed diff
   against the VM ADT.
4. The native-variant KC boots to the login window in the existing Gunyah VM with an 8-CPU two-cluster ADT (one owner
   approved VM session; the VM is restored to direct mode afterwards).
5. Design pages for display (GOP + DPU flips), UFS, touch, charging, GPU, each with its Ubuntu-as-spec references.

When all five are done, the only thing between the repo and M1 is the USB-C cable.
