# Using native macOS on piano (operator's guide)

How the installed system is used, updated and recovered day to day. The design and the install itself are in
`docs/persistent-boot.md`; driver details in `docs/drivers/`; the debug loop in `docs/bringup-debugging.md`. Commands run from this
repository's root on the Mac the tablet is cabled to (USB-C). `~/.config/piano/env` holds the local settings (serial, hosts, NAS,
`PIANO_K27_TREE`: the k27 tree the build reads, `docs/bringup-debugging.md` §4); nothing private is in this repository.

## 1. What runs where

| Piece | Where | Changed by |
|---|---|---|
| BootShim + FD (UEFI) | `boot_a` (signed v5, 13 GiB DarwinBoot window) | `maint-session.py boot-a` (rare) |
| UniXNU payload: kernel collection, ADT, trust cache, Data seed, boot arguments | ESP, LUN 0 partition 37 | `tooling/k273/build-k273-esp.py` + `maint-session.py esp-final` |
| macOS root (OS + SystemOS) | windows in `linux_rootfs` | the §13.8 maintenance session |
| Data (user changes) | the 200 GiB store over `linux_data` (LBA 71859642) over the ESP's seed | macOS itself |
| Our userland (agents, the USB root shell, tools, the pmset defaults) | Data: `/usr/local/{bin,libexec}`, `/Library/Launch{Daemons,Agents}` | `configs/userland.toml`: the seed's boot job or a live update (`docs/userland.md`) |

Power on: ABL → UEFI → UniXNU (menu timeout 0) → macOS, about 2.5 minutes to the login screen (`docs/boot-time.md`).
A `fastboot boot` RAM image always wins over the installed copy (`firmware/patches/unixnu/0001`), which is how every test runs.

## 2. The installed profile

The installed profile is the 27.3-only `macos-27.3` entry built by `tooling/k273/build-k273-esp.py`; the installed ESP is
`esp-k273-v4` (Wi-Fi on; written over v2 by `esp-delta`; v5 next: Wi-Fi back after sleep). The root is on `linux_rootfs` and the owner's 200 GiB Data store on `linux_data`. The exact
inputs, RAM test, install session and rollback are [persistent-boot.md](persistent-boot.md) §13 and §13.8 (the retired
`build-esp.py --profile daily` / `esp-daily<N>` line is not used). Opt-in drivers join the installed profile only after a RAM
test passed on the tablet.

## 3. From the Mac

| Task | Command |
|---|---|
| Live kernel console + monitor | `python3 tooling/native/usb-console.py` (Ctrl-] quits; `help` lists the monitor commands) |
| Record the console | `python3 tooling/native/usb-console.py --capture 600` (logs in `logs/usb-console/`, with Mac timestamps in `.times.txt`) |
| Type the login password | `python3 tooling/native/piano-input.py type --secret --enter` (read without echo; never logged, never in argv) |
| Keys, pointer | `piano-input.py key cmd+space`, `move 200 0`, `click`, `scroll -3` (`--help` for all) |
| To ABL fastboot (no buttons) | `python3 tooling/native/usb-console.py --send 'reset fastboot' --capture 5` |
| Restart into macOS | monitor command `reset` (in the first 180 s of a boot it lands in ABL fastboot: the boot guard's window, `docs/boot-guard.md`; `fastboot reboot` goes on) |
| What is attached | `python3 tooling/native/usb-console.py --list` |

Without the USB console (an older profile, or a hang): hold **power + volume-down for 15–20 s** until the fastboot screen. With the
boot guard (an ESP from a k27 0063 collection) a hang resets by itself: within the first 180 s into ABL fastboot (`fastboot reboot`
goes on to macOS), later into macOS.

## 4. Test a change (RAM boot, nothing written)

```sh
K273_KERNEL=<the prepared kernel-k273-...-native.macho> tooling/k273/build-k273-image.sh
# Current root/store layout: see the caveat below before selecting or booting an image.
tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img
```

Only 27.3 images are built. The script's `K273_ROOT=k273` default still names the superseded root on `linux_data`; the current
root is on `linux_rootfs` and Data on `linux_data`. Use the `--root-part linux_rootfs` ESP recipe and its RAM test in
`docs/persistent-boot.md` §13.8 for that layout, not the retired 27.0.1 builder or `K273_ROOT=k27`.

Data caution (`docs/data-store.md`): set `PIANO_INSTALLED_SEED` in `~/.config/piano/env` once (the installed 200 GiB store's
seed is `9bd6f902…` since the 2026-11-14 whole-disk layout; `8f333110…` and `aced5bf9…` belonged to retired stores). Then a RAM image whose Data seed is not the installed one
gets `data-persist=0` by default (Data volatile, the store untouched), and `build-ram-images.py` says so on stderr. A kernel
collection built from this tree also has the store guard: with `PIANO_DATA_PERSIST=2` and another seed it keeps the installed
store and runs Data volatile ("kept"). Images built before the guard still start another seed's store over, so never boot one with
`data-persist=2` and a new seed. After the store is flattened, every `data-persist=2` image resumes the owner's Data, whatever its seed.

## 5. Update the installed ESP

The Data store is a block overlay over one seed, its **base**. Every ESP that keeps the owner's Data carries that seed byte for byte.
The Metal plugin and our userland change; they are the **system layer**, installed as files on the running system
(`docs/data-store.md` §9). The current plan and state, with exact commands, are `docs/persistent-boot.md` §13 and §13.8
(the 27.3-only ESP and whole-disk layout); §11's ESP v3 was a retired 27.0.1 plan.

1. Build and test the change as a RAM image (§4).
2. Once: pin the installed base, `python3 tooling/persist/base-seed.py pin --esp <persist checkout>/out/persistent/<installed ESP>.img`.
   It checks the identity against `PIANO_INSTALLED_SEED`.
3. If the plugin or the userland changed: `python3 tooling/userland/stage.py --live out/layer --gpu-bundle out/turnip-a830/NativeAdrenoMetal.bundle
   --installed-trust "$(python3 tooling/persist/base-seed.py path trust)"`.
4. Build the 27.3-only ESP with `python3 tooling/k273/build-k273-esp.py --root-part linux_rootfs`, using the current 200 GiB
   base seed and the other inputs listed in `docs/persistent-boot.md` §13.8. Do not use the retired `build-esp.py --profile daily`
   or `esp-daily<N>-macos` recipe. Rebuild just before installing.
   - Another `--seed` does not carry the owner's Data over: build-esp.py prints a loud note, and the store guard keeps the store and
     runs Data volatile. `--replace-data` is the explicit "start Data over", the owner's decision.
   - A trust cache that lacks a CDHash of the base's is refused: the resumed Data still has the base's plugin.
5. Tablet to ABL fastboot (§3), then on the install state of the persist worktree (`PIANO_PERSIST_OUT=<persist worktree>/out/persistent`,
   with the image and its record copied there; this checkout's tooling; steps before options):
   `python3 tooling/persist/maint-session.py maint-boot map backup [store-backup] esp-final check maint-exit --esp <esp-k273-...>.img --esp-final <esp-k273-...>.img --new-backup`
   then `abl-final reboot`. The installed ESP is `esp-k273-v4` (v5 next); `esp-k273-v2` and `esp-k273-only` are rollback targets. Every
   write is preceded by a verified backup and followed by a SHA-256 read-back. `store-backup` adds a verified copy of the Data
   store itself (200 GiB logical, 52430401 blocks; `rollback store-backup` restores it).
6. With a layer: install it on the running system (`docs/userland.md` §7; the first time with the owner, `docs/persistent-boot.md` §11.6).

## 6. Recover

| Situation | Way out |
|---|---|
| The installed macOS misbehaves | RAM-boot a known-good image (§4); the installed copy is untouched |
| A bad ESP | ABL fastboot, then `maint-session.py ... esp-final` with the previous ESP image (the images stay in `out/persistent/`). Once a system layer is installed whose CDHashes the previous ESP lacks, take the layer out first (the empty layer, `configs/userland-none.toml`) or restore the store backup with the ESP (`docs/persistent-boot.md` §11.9) |
| Undo the install | `maint-session.py rollback boot-a`, `esp`, `store`, `gpt` (`docs/persistent-boot.md` §7) |
| Back to Android | `docs/persistent-boot.md` §7 (Ubuntu is retired; `linux_rootfs` now carries the 27.3 root) |

## 7. Not there yet

Charging control (level 2's policy works, with the stop/near-full cases still open), audio by default (levels 4-8 ran; the daily
level 9 not yet), Bluetooth, the macOS camera extension, sensors and the remaining power-policy checks. The installed boot publishes
the battery (`pmset` lists InternalBattery-0 and the desktop has the battery icon). **Wi-Fi works end to end**, including DHCP,
gateway/Internet ping, DNS and HTTPS (boot 10i, 2026-11-14; `docs/drivers/wifi.md`).
