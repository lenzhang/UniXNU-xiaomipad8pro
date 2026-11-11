# UniXNU-piano

Native macOS 27 on the Xiaomi Pad 8 Pro (`piano`, Snapdragon 8 Elite SM8750), as a [UniXNU](https://github.com/UniXNU/UniXNU)
device repository: https://github.com/lenzhang/UniXNU-xiaomipad8pro (formerly lenzhang/UniXNU-piano; the local
checkout keeps the name UniXNU-piano). The k27 XNU kernel (XNU 27.0.1) runs as the HLOS at EL1, with
Gunyah staying at EL2. Boot chain: ABL → BootShim → Mu-Silicium UEFI (the FD, piano-uefi's `pianoPkg` with this repository's
deltas) → the UniXNU payload and menu → DarwinBoot → XNU. UniXNU core, Mu-Silicium, piano-uefi, DarwinBoot and the SoC module
are pinned dependencies (`unixnu.lock`, `feeds.toml`), not forks.

Related public repositories: [UniXNU-sm8750-common](https://github.com/lenzhang/UniXNU-sm8750-common) is a placeholder for the
SM8750 pieces to be shared later (the code still lives here, and the build pins another SM8750 module); [veil](https://github.com/lenzhang/veil)
is the frozen archive of the earlier Linux + Gunyah guest path. The map of repositories, local trees and rules:
[docs/PROJECT-MAP.md](docs/PROJECT-MAP.md).

## State (2026-11-11)

- macOS boots natively on the tablet, also by itself from power-on (an installed copy, see below); root and Data are on UFS
  (`package/NativeUfs`, `package/NativeRamBlock`).
- The Adreno 830 GPU (`package/PianoA830` with Turnip) composites the login screen and, since 2026-11-11, the logged-in desktop
  (Dock, menu bar, Finder, Control Center as GPU clients); the first sessions' resets and GPU map refusals are fixed, the
  pointer layer and a flicker are being fixed.
- Input: the Xiaomi keyboard cover (keys, touchpad) and the touch screen (taps, drags, two-finger scroll) reach macOS; the
  touch screen's orientation on this unit and a controller watchdog issue are being fixed.
- A USB-C debug console with a small kernel monitor (`package/PianoDwc3`, `tooling/native/usb-console.py`) and the buttons run;
  the monitor's `reset fastboot` returns the tablet to ABL fastboot without buttons.
- Charging: the battery charges from a USB-C port without the DSP; the DSP charger path starts from macOS and is being tuned.
- Latest results: [docs/results-2026-11-11.md](docs/results-2026-11-11.md).
- Most other drivers (power and charging, touch, keyboard cover, backlight, Wi-Fi, audio, sensors, thermal, USB host, video,
  Bluetooth, display processor, ...) are built and linked but inert behind their boot arguments, waiting for their tests on the
  tablet. Per-driver state: [docs/PROJECT-MAP.md](docs/PROJECT-MAP.md) §7 and [docs/drivers/README.md](docs/drivers/README.md).

## Layout

| Path | What |
| --- | --- |
| `patches/k27/` | the k27 kernel patches 0053-0063 on top of the k27 tree (xnu-12377 + the baseline series 0001-0052, XNU 27.0.1) |
| `patches/k27-kc/` | the KC builder's native placement (best fit, relocation-free constants in `__TEXT`), applied by hand to the k27 tree ([docs/kc-space.md](docs/kc-space.md)) |
| `patches/xnu/` | the device series of the UniXNU macOS 27.3 line (empty: none yet) |
| `package/` | the native kexts; each is inert without its boot argument |
| `configs/` | the native kext list (`k27-native-kexts.txt`), the ADT identity and display node (`k27-adt.toml`, `k27-gop-display.toml`), the 27.3 line's kernel config |
| `dts/piano.dts` | piano's hardware for the ADT |
| `tooling/` | `k27/build-native-image.sh` (kexts → kernel collection → ADT → RAM images), `kc/` (the KC build), `adt/`, `bootimg/` (RAM images and their verification), `native/` (the test loop, the USB console, input), `persist/` (the installed copy), `firmware/` (the FD) |
| `firmware/` | deltas to piano-uefi's `pianoPkg`, Mu-Silicium and UniXNU |
| `docs/` | bring-up notes, driver pages (`drivers/`), plans and evidence |
| `logs/`, `artifacts/ARTIFACTS.md` | committed verification logs; SHA-256 of built artifacts and inputs |
| `private/`, `out/` | ignored: keys, per-unit calibration; build outputs |

## Build

Inputs: the k27 tree, a local kernel tree the build only reads (its place is `PIANO_K27_TREE`, in the environment or in
`~/.config/piano/env`), the pinned UEFI checkouts of `unixnu.lock`, and the OS image inputs of `out/inputs/` (recorded in
`artifacts/ARTIFACTS.md`, never committed).

```sh
K27_VARIANT=native "$PIANO_K27_TREE/build-k27-piano.sh"    # the kernel, with patches/k27 applied in the k27 tree
tooling/unixnu.sh build firmware --skip-preflight           # the FD
tooling/k27/build-native-image.sh                           # kexts -> KC -> ADT -> out/ram (test) + out/ram-dump
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
```

Details, the boot arguments and what is read from the k27 tree: [docs/bringup-debugging.md](docs/bringup-debugging.md) §4.

## Boot and test

Everyday development uses RAM boots: `fastboot boot` of a RAM image, which takes priority over the installed copy, with an
unattended loop that boots a test, resets and reads the test's log back (`tooling/native/native-loop.sh`,
[docs/bringup-debugging.md](docs/bringup-debugging.md) §3). The installed copy ([docs/persistent-boot.md](docs/persistent-boot.md))
writes only the regions that document lists, after verified backups of each, with a read-back check after every write and a
rollback for each step; it never writes firmware partitions. Do not flash boot or firmware partitions by hand.

## The native kext rule

The native kernel collection links only this repository's kexts (`configs/k27-native-kexts.txt`). `tooling/k27/check-native-kexts.py`
enforces it on every build, before and after the link: no descriptor from elsewhere, none of the VM-era paravirtual kexts.

## Where to read next

- [docs/USING.md](docs/USING.md): using, updating and recovering the installed system.
- [docs/drivers/README.md](docs/drivers/README.md): every driver, its levels and its state.
- [docs/PROJECT-MAP.md](docs/PROJECT-MAP.md): repositories, local layout, hard rules.
- [docs/bringup-debugging.md](docs/bringup-debugging.md): the boot chain, debugging without a screen, the test loop, rebuilding.
- [docs/BACKLOG-hardware.md](docs/BACKLOG-hardware.md): the hardware still to bring up.
- [docs/kc-space.md](docs/kc-space.md): the kernel collection's room for native kexts, the run-time firmware loader.

## Licence and contents

Code here is BSD-2-Clause-Patent unless a package's `LICENSE`, a file's SPDX header or an `UPSTREAM` file says otherwise.
Vendor firmware, partition images and Apple payloads (kernel collections, trust caches, IPSW-derived files) are never in this
repository; neither are secrets, keys, per-unit calibration, local network or storage details, or machine names.
