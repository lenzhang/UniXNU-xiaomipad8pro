# DroidVM leftovers in the native line (inventory, 2026-11-11)

The native piano build started from the frozen macOS-DroidVM (Gunyah VM) path. This is every VM-era piece that was in the native RAM
image, kernel collection, ADT or build scripts, what happened to it, and why. The archive itself (`droidvm-build/`, `veil`) is not
changed. Every removal was checked with a native run on the tablet (`tooling/native/native-loop.sh`). Each run had to match the
baseline on five points: root mounted from UFS, Data resumed from its store, the same launchd boot tasks, no panic, and no I/O
errors.

## Kernel collection (`configs/k27-native-kexts.txt`)

| Kext | Decision | Reason / evidence |
|---|---|---|
| NativeVirtioBlock | removed | virtio root disk; natively the root comes from NativeUfs |
| DroidVMEthernet, DroidVMAudio, DroidVMVideo, DroidVMInput | removed | virtio devices, absent natively. With a display but no DroidVMGpu, one of them held IOBSD matching for 30 s (`!BSD`, root never mounted); without them the root mounts and SystemOS matches in 0.5–3.9 s instead of 4 s |
| DroidVMGpu | removed | a stub accelerator natively: WindowServer read its MetalPluginName, its type-0x4147 opens failed with `kIOReturnBusy` (3 slots, one reserved for iOS's backboardd) and it crash-looped. The future GPU driver keeps its WindowServer-facing contract on Adreno830 (`droidvm-build/piano-patches/gpu-native-plan.md`) |
| DroidVMDisplay | removed, replaced | its scanout is virtio; NativeGopDisplay (`package/NativeGopDisplay`) copies to the GOP framebuffer and now carries the `AppleParavirtGPU` nub itself |
| NativeAES | removed | nothing reads it natively (no AppleKeyStore, Data unencrypted); measured the same boot without it |
| ARMPlatformStandIn | kept, vendored (`package/ARMPlatformStandIn`) | the vma2 collection's IOMobileGraphicsFamily (the display's family) and its kexts link against AppleARMPlatform's classes |
| NativeRamBlock | kept, vendored (`package/NativeRamBlock`, Droid-VM/UniXNU 2e61915) | the Data disk; its VM data store (`DroidVMDataStore.c`, virtio) is gone, NativeUfs's store window serves it (`data-persist=2`) |
| NativeUfs, NativeGopDisplay | piano | — |

## Boot arguments (`tooling/bootimg/build-ram-images.py`)

Removed (nothing in the native collection reads them): `offline-hid-input`, `offline-hid-home`, `offline-key-events`, `sheng-aks`,
`sheng-aks-sls` (only NativeKeyStoreEvents, not linked), `ml-timeout-lock-scale`, `ml-timeout-lock-panic-scale`, `ca-triple-buffer`,
`ca-free-wakeup` (the VM's Metal plugin). The VM's device arguments (`droidvm-display-mm`, `vgpu-3d`, `droidvm-vt-paravirt`,
`droidvm-monitor`, `droidvm-data-persist`) were never in the native image.

Kept, with their readers:

| Argument | Read by | Why natively |
|---|---|---|
| `droidvm-utc`, `droidvm-tz` | the OS image's `settime.sh` boot job | the only clock until there is an RTC driver (PMIC RTC); then remove |
| `droidvm-disable-jobs` | the OS image's userland | the jobs listed have no hardware here (watchdogd, CommCenter, locationd, bluetoothd, identityservicesd) |
| `droidvm-timebase-hz=24000000` | k27 0042 (pexpert) | userland expects a 24 MHz timebase over SM8750's 19.2 MHz counter |
| `offline-apfs-load`, `owner-staged-apps`, `union-hide-lower`, `malloc-ranges-off`, `mac-amfi-launch-permissive`, `rootauth=0`, `exec-env`, `kt_fixed_zones`, `tlto_us`, `-no_slto_panic`, `vm_compression_limit`, `arm64_maxoffset` | the k27 kernel (DroidVM series) | they make the same macOS 27.0.1 userland run on a non-Apple machine; not VM-specific |
| `-apfs_shared_datavolume` | Apple's APFS (not verified) | kept until shown unused |

The names stay `droidvm-*` because the kernel and the OS image know them by those names; renaming means changing both.

## ADT (`configs/k27-adt.toml`, `configs/k27-gop-display.toml`)

| Item | Decision | Reason |
|---|---|---|
| `vmm-present` | kept at 1 | tried 0: watchdogd then runs as on real hardware, exits twice (no watchdog device) and launchd panics about 150 s into the run (`watchdogd[297] exited`); with 1 a 240 s run has no panic. Revisit with a watchdog driver |
| `model` VirtualMac2,1, `target-type` VMA2MACOS, `product-name` "Apple Virtual Machine 1", `has-virtualization` | kept | the kernel collection is Apple's vma2 one and its kexts match on this identity (plan R8) |
| `serial-number` DVMPIANO001 | kept | made up, not a unit serial; the Data seed was made with it |
| `/arm-io/gop-display`, compatible `paravirtualizedgraphics,gpu` | kept (now in the default ADT) | IOMobileFramebuffer.framework only looks for displays on a node with that string |

## Build tooling

Removed:
- `PIANO_DROP_KEXTS` and `build-kc-k27.sh`'s default VM kext set. `NATIVE` is now required, and `build-native-image.sh` sets it.
- `tooling/k27/build-display-test-kc.sh` and `tooling/k27/vm-native-test.sh` (the VM harness).
- `tooling/native/native-run.sh` (the Ubuntu-based loop; Ubuntu's root is gone).
- the unused `out/inputs/vm-DarwinBoot.efi` (DarwinBoot is built into the FD).

Vendored: `tooling/k27/build-native-kext.py` and `NativeKextGlue.cpp`. Every kext's objects now go to this repository's
`out/native-kexts/`, so the native build writes nothing into the archive.

Still read from the k27 build tree (`droidvm-build/macOS-DroidVM`, branch `el1/native-variant`, pinned in `unixnu.lock`):
- the k27 kernel tree and its build script (`build-k27-piano.sh`). The tree is xnu-12377 + the DroidVM series 0001-0052 + our
  `patches/k27` 0053-0058. The native variant builds with `XNU_VM_GUEST=0`, so the VM-guest code is compiled out.
- the kernel objects' compile recipes and generated headers (`out/x12377`: storage ABI, xnu headers), which kexts compile against.
- the KC base, Apple's macOS 27.0.1 vma2 kernelcache (`out/kc/kc27.bin`, an Apple payload: never published).
- the KC builder (`k27/kc-build27-piano.py`, `scripts/kc-build.py`, `fileset_kc`, `ptrauth_disc`, `kc27-layout.py`, `segaddrs`).
- `scripts/gen-iomfb-abi.py`. Its output is cached in `out/k27/iomfb-abi/`.

These are the k27 toolchain rather than DroidVM devices. Moving them out (their own repository, or vendored here with the kernel
patch series) is a separate change.

## Inputs that are the VM's macOS install

These stay, because they are the macOS install the tablet now runs. Replacing them means reinstalling from Apple's 27.0.1 restore
image with our own customisation:
- `out/inputs/kernel.macho.trustcache`: the OS image's binaries.
- `out/inputs/scratch-data.img`: the Data seed.
- the OS and SystemOS windows on linux_rootfs (the VM's `os.img` and `systemos.img`). They include the VM-era userland changes,
  such as `settime.sh` and the staged Metal plugin.
