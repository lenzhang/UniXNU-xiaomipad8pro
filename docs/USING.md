# Using native macOS on piano (operator's guide)

How the installed system is used, updated and recovered day to day. The design and the install itself are in
`docs/persistent-boot.md`; driver details in `docs/drivers/`; the debug loop in `docs/bringup-debugging.md`. Commands run from this
repository's root on the Mac the tablet is cabled to (USB-C). `~/.config/piano/env` holds the local settings (serial, hosts, NAS,
`PIANO_K27_TREE`: the k27 tree the build reads, `docs/bringup-debugging.md` §4); nothing private is in this repository.

## 1. What runs where

| Piece | Where | Changed by |
|---|---|---|
| BootShim + FD (UEFI) | `boot_a` (signed v4) | `maint-session.py boot-a` (rare) |
| UniXNU payload: kernel collection, ADT, trust cache, Data seed, boot arguments | ESP, LUN 0 partition 37 | `build-esp.py` + `maint-session.py esp-final` |
| macOS root (OS + SystemOS) | windows in `linux_rootfs` | not by the daily tools |
| Data (user changes) | the store window at the end of `linux_rootfs` over the ESP's seed | macOS itself |
| Our userland (agents, the USB root shell, tools, the pmset defaults) | Data: `/usr/local/{bin,libexec}`, `/Library/Launch{Daemons,Agents}` | `configs/userland.toml`: the seed's boot job or a live update (`docs/userland.md`) |

Power on: ABL → UEFI → UniXNU (menu timeout 0) → macOS, about 2.5 minutes to the login screen (`docs/boot-time.md`).
A `fastboot boot` RAM image always wins over the installed copy (`firmware/patches/unixnu/0001`), which is how every test runs.

## 2. The installed profile

`tooling/persist/build-esp.py --profile daily`: the A830 GPU, PianoPower level 1 (battery and Type-C readout, read only), the USB
debug console (PianoDwc3) with the Mac's keyboard and pointer. Opt-in drivers join the profile only after a RAM test passed on the
tablet (`docs/PROJECT-MAP.md` §7 lists their state).

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
tooling/k27/build-native-image.sh            # with PIANO_* variables, see docs/bringup-debugging.md §4
tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img
```

Data caution (`docs/data-store.md`): set `PIANO_INSTALLED_SEED` in `~/.config/piano/env` once (the installed seed is
`8f3331104e03c970…` since the daily2 ESP; `aced5bf9…` before). Then a RAM image whose Data seed is not the installed one
gets `data-persist=0` by default (Data volatile, the store untouched), and `build-ram-images.py` says so on stderr. A kernel
collection built from this tree also has the store guard: with `PIANO_DATA_PERSIST=2` and another seed it keeps the installed
store and runs Data volatile ("kept"). Images built before the guard still start another seed's store over, so never boot one with
`data-persist=2` and a new seed. After the store is flattened, every `data-persist=2` image resumes the owner's Data, whatever its seed.

## 5. Update the installed ESP

The Data store is a block overlay over one seed, its **base**. Every ESP that keeps the owner's Data carries that seed byte for byte.
The Metal plugin and our userland change; they are the **system layer**, installed as files on the running system
(`docs/data-store.md` §9). The current plan, with its exact commands, is `docs/persistent-boot.md` §11 (ESP v3).

1. Build and test the change as a RAM image (§4).
2. Once: pin the installed base, `python3 tooling/persist/base-seed.py pin --esp <persist checkout>/out/persistent/<installed ESP>.img`.
   It checks the identity against `PIANO_INSTALLED_SEED`.
3. If the plugin or the userland changed: `python3 tooling/userland/stage.py --live out/layer --gpu-bundle out/turnip-a830/NativeAdrenoMetal.bundle
   --installed-trust "$(python3 tooling/persist/base-seed.py path trust)"`.
4. Build the ESP: `python3 tooling/persist/build-esp.py --profile daily --default macos [--layer out/layer] --name esp-daily<N>-macos
   --out out/persistent`. The daily profile takes the base's seed and trust cache. `--layer` takes the layer's trust cache, the base's
   plus the new CDHashes. Rebuild just before installing: the clock starts from the build time until the RTC is in the profile,
   `docs/drivers/rtc.md`.
   - Another `--seed` does not carry the owner's Data over: build-esp.py prints a loud note, and the store guard keeps the store and
     runs Data volatile. `--replace-data` is the explicit "start Data over", the owner's decision.
   - A trust cache that lacks a CDHash of the base's is refused: the resumed Data still has the base's plugin.
5. Tablet to ABL fastboot (§3), then from the persist worktree (steps before options):
   `python3 tooling/persist/maint-session.py maint-boot map backup esp-final check maint-exit --esp esp-daily<N>-macos.img --esp-final esp-daily<N>-macos.img --new-backup`
   then `abl-final reboot`. Every write is preceded by a verified NAS backup and followed by a SHA-256 read-back.
6. With a layer: install it on the running system (`docs/userland.md` §7; the first time with the owner, `docs/persistent-boot.md` §11.6).

## 6. Recover

| Situation | Way out |
|---|---|
| The installed macOS misbehaves | RAM-boot a known-good image (§4); the installed copy is untouched |
| A bad ESP | ABL fastboot, then `maint-session.py ... esp-final` with the previous ESP image (the images stay in `out/persistent/`) |
| Undo the install | `maint-session.py rollback boot-a`, `esp`, `store`, `gpt` (`docs/persistent-boot.md` §7) |
| Back to Android or Ubuntu | `docs/persistent-boot.md` §7 |

## 7. Not there yet

Charging control (level 1 only reads; whether a part-empty battery charges without the DSP is being measured), the keyboard cover
and touch screen (drivers built, first tests pending), Wi-Fi (months: `docs/drivers/wifi.md`; a USB network to the Mac is built),
audio, Bluetooth, cameras, sensors, sleep (CPU idle states and suspend-to-idle are built as k27 0061/0062, opt-in and not run yet:
`docs/power.md`).
