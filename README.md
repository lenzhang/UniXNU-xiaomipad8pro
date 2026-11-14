# UniXNU-piano

Native macOS 27 on the Xiaomi Pad 8 Pro (`piano`, Snapdragon 8 Elite SM8750), as a [UniXNU](https://github.com/UniXNU/UniXNU)
device repository: https://github.com/lenzhang/UniXNU-xiaomipad8pro (formerly lenzhang/UniXNU-piano; the local
checkout keeps the name UniXNU-piano). The 27.3 line's k273 XNU build (the k27 source tree with `patches/k27` 0053-0066 and
`patches/k273`) runs as the HLOS at EL1, with
Gunyah staying at EL2. Boot chain: ABL → BootShim → Mu-Silicium UEFI (the FD, piano-uefi's `pianoPkg` with this repository's
deltas) → the UniXNU payload and menu → DarwinBoot → XNU. UniXNU core, Mu-Silicium, piano-uefi, DarwinBoot and the SoC module
are pinned dependencies (`unixnu.lock`, `feeds.toml`), not forks.

Related public repositories: [UniXNU-sm8750-common](https://github.com/lenzhang/UniXNU-sm8750-common) is a placeholder for the
SM8750 pieces to be shared later (the code still lives here, and the build pins another SM8750 module); [veil](https://github.com/lenzhang/veil)
is the frozen archive of the earlier Linux + Gunyah guest path. The map of repositories, local trees and rules:
[docs/PROJECT-MAP.md](docs/PROJECT-MAP.md).

## State (2026-11-14)

- macOS boots natively on the tablet, also by itself from power-on (an installed copy, see below); root and Data are on UFS
  (`package/NativeUfs`, `package/NativeRamBlock`).
- The Adreno 830 GPU (`package/PianoA830` with Turnip) composites the login screen and, since 2026-11-11, the logged-in desktop
  (Dock, menu bar, Finder, Control Center as GPU clients); the first sessions' resets and GPU map refusals are fixed, and
  the display now draws the pointer layer and copies no frame before its GPU fence (built and host-tested, to be run:
  `docs/drivers/native-gop-display.md` §5a).
- Input: the Xiaomi keyboard cover (keys, touchpad) and the touch screen (taps, drags, two-finger scroll) reach macOS; the
  touch screen's orientation on this unit and its controller watchdog recovery are fixed (built, to be run).
- A USB-C debug console with a small kernel monitor (`package/PianoDwc3`, `tooling/native/usb-console.py`) and the buttons run;
  the monitor's `reset fastboot` returns the tablet to ABL fastboot without buttons.
- The installed system (2026-11-14): **macOS 27.3 (26C5035r) only**, Mac17,11 shown as "Xiaomi Pad 8 Pro" with a
  "Snapdragon 8 Elite" chip, 13 GiB of RAM. Its root is on `linux_rootfs`; the owner's 200 GiB Data store spans `linux_data`
  (LBA 71859642, seed `9bd6f902`). The retired 27.0.1 volume/store was cleared and has no menu entry
  ([docs/persistent-boot.md](docs/persistent-boot.md) §13.8, [docs/results-2026-11-14.md](docs/results-2026-11-14.md)).
  The USB root shell and ssh over the cable; charging that keeps going (level 2, the DSP started at once). The current ESP is
  **v5** (installed 2026-11-14): it carries Wi-Fi's sleep and crash recovery. The desktop and the lock screen show Apple's stock
  still picture and render right (run h1 on v5, photos).
- Cameras: frames from both sensors and the first pictures through a user client (`piano-camera-grab`); a macOS camera
  extension is next ([docs/drivers/camera.md](docs/drivers/camera.md)).
- Wi-Fi: the installed system joins by itself at boot (WPA2, DHCP, DNS, HTTPS), rejoins after a drop, and came back by itself
  after a system sleep and after a firmware crash (boot 10m; ESP v5 brings both to the installed system — its first boot joined
  on its own). The macOS Wi-Fi menu is in progress ([docs/drivers/wifi-menu.md](docs/drivers/wifi-menu.md)): airportd starts by
  itself and runs scans through our driver (17 networks served, boot wm1), but its corewifi side has not adopted the interface
  yet (wm2), so there is no menu network list yet; joining stays a profile file and our supplicant until then
  ([docs/drivers/wifi.md](docs/drivers/wifi.md)).
- Boot time is being worked on: most of launchd's 40 s is our 4 KiB code-signature pre-hash of the compressed dyld shared
  cache (about 37 s on one CPU, hash-bound not disk-bound, probe ufs3). The owner decided 2026-11-14 to skip the eager
  pre-hash on this development tablet behind a boot-arg switch (`cs-native4k-skip=1`, verification can be turned back on;
  a parallel version stays the fallback); probes built, then into the installed ESP ([docs/boot-time.md](docs/boot-time.md) §9).
- Not working yet: the Wi-Fi menu's network list and menu-driven joining, Bluetooth, the sensors, hardware video decode, and
  a clean restart — every reboot of the 27.3 line ends in IOPlatformExpert's panic 30 s after the halt begins
  (`Halt/Restart Timed Out`, console captures since 2026-11-13).
- Latest results: [docs/results-2026-11-14.md](docs/results-2026-11-14.md) (Wi-Fi on the installed system with sleep and crash
  recovery, the built-in `en0` handed to Wi-Fi, the boot-time probes); the day before: [docs/results-2026-11-13.md](docs/results-2026-11-13.md).
- Most other drivers (power and charging, touch, keyboard cover, backlight, audio, sensors, thermal, USB host, video,
  Bluetooth, display processor, ...) are built and linked but inert behind their boot arguments, waiting for their tests on the
  tablet. Per-driver state: [docs/PROJECT-MAP.md](docs/PROJECT-MAP.md) §7 and [docs/drivers/README.md](docs/drivers/README.md).

## Layout

| Path | What |
| --- | --- |
| `patches/k27/` | the k27 kernel patches 0053-0066 on top of the k27 source tree (xnu-12377 + the baseline series 0001-0052) |
| `patches/k273/` | the 27.3 line's kernel series on top of `patches/k27` |
| `patches/k27-kc/` | the KC builder's native placement (best fit, relocation-free constants in `__TEXT`), applied by hand to the k27 tree ([docs/kc-space.md](docs/kc-space.md)) |
| `patches/xnu/` | the device series of the UniXNU macOS 27.3 line (empty: none yet) |
| `package/` | the native kexts; each is inert without its boot argument |
| `configs/` | the native kext list (`k27-native-kexts.txt`), the ADT identity and display node (`k27-adt.toml`, `k27-gop-display.toml`), the 27.3 line's kernel config |
| `dts/piano.dts` | piano's hardware for the ADT |
| `tooling/` | `k273/build-k273-image.sh` (the 27.3 kexts → kernel collection → ADT → RAM images, `K273_KERNEL`), `k273/build-k273-esp.py` (the installed 27.3 ESP), `kc/` (the KC build), `adt/`, `bootimg/` (RAM images and their verification), `native/` (the test loop, the USB console, input), `persist/` (the installed copy), `firmware/` (the FD) |
| `firmware/` | deltas to piano-uefi's `pianoPkg`, Mu-Silicium and UniXNU |
| `docs/` | bring-up notes, driver pages (`drivers/`), plans and evidence |
| `logs/`, `artifacts/ARTIFACTS.md` | committed verification logs; SHA-256 of built artifacts and inputs |
| `private/`, `out/` | ignored: keys, per-unit calibration; build outputs |

## Build

Inputs: the k27 tree, a local kernel tree the build only reads (its place is `PIANO_K27_TREE`, in the environment or in
`~/.config/piano/env`), the pinned UEFI checkouts of `unixnu.lock`, and the OS image inputs of `out/inputs/` (recorded in
`artifacts/ARTIFACTS.md`, never committed).

```sh
tooling/k273/prepare-k27-tree.sh                             # the 27.3 tree and its relinked kernel; prints K273_KERNEL's path
tooling/unixnu.sh build firmware --skip-preflight           # the FD
K273_KERNEL=<path printed by prepare-k27-tree.sh> tooling/k273/build-k273-image.sh
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
```

Only the 27.3 line is built. The retired `tooling/k27/build-native-image.sh` 27.0.1 path is not used. **Layout note
(2026-11-14):** the root is now on `linux_rootfs` and Data on `linux_data`; `build-k273-image.sh`'s `K273_ROOT=k273` default
still emits the superseded `native-ufs-root-part=linux_data`, so do not use that default for the current layout. The installed
ESP recipe uses `build-k273-esp.py --root-part linux_rootfs` ([docs/persistent-boot.md](docs/persistent-boot.md) §13.8).

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
