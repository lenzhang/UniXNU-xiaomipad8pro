# Using native macOS on piano (operator's guide)

How the installed system is used, updated and recovered day to day. The design and the install itself are in
`docs/persistent-boot.md`; driver details in `docs/drivers/`; the debug loop in `docs/bringup-debugging.md`. Commands run from this
repository's root on the Mac the tablet is cabled to (USB-C). `~/.config/piano/env` holds the local settings (serial, hosts, NAS);
nothing private is in this repository.

## 1. What runs where

| Piece | Where | Changed by |
|---|---|---|
| BootShim + FD (UEFI) | `boot_a` (signed v4) | `maint-session.py boot-a` (rare) |
| UniXNU payload: kernel collection, ADT, trust cache, Data seed, boot arguments | ESP, LUN 0 partition 37 | `build-esp.py` + `maint-session.py esp-final` |
| macOS root (OS + SystemOS) | windows in `linux_rootfs` | not by the daily tools |
| Data (user changes) | the store window at the end of `linux_rootfs` over the ESP's seed | macOS itself |

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
| Restart into macOS | monitor command `reset` |
| What is attached | `python3 tooling/native/usb-console.py --list` |

Without the USB console (an older profile, or a hang): hold **power + volume-down for 15–20 s** until the fastboot screen.

## 4. Test a change (RAM boot, nothing written)

```sh
tooling/k27/build-native-image.sh            # with PIANO_* variables, see docs/bringup-debugging.md §4
tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img
```

Data caution (`docs/data-store.md`): set `PIANO_INSTALLED_SEED` in `~/.config/piano/env` once (the installed seed is
`aced5bf9794b42f3c8ffeeef3078a408ff506f5b0b374e02eed6f0ab56e52616`). Then a RAM image whose Data seed is not the installed one
gets `data-persist=0` by default (Data volatile, the store untouched), and `build-ram-images.py` says so on stderr. A kernel
collection built from this tree also has the store guard: with `PIANO_DATA_PERSIST=2` and another seed it keeps the installed
store and runs Data volatile ("kept"). Images built before the guard still start another seed's store over, so never boot one with
`data-persist=2` and a new seed. After the store is flattened, every `data-persist=2` image resumes the owner's Data, whatever its seed.

## 5. Update the installed ESP

1. Build and test the change as a RAM image (§4).
2. Build the ESP from the same inputs: `python3 tooling/persist/build-esp.py --profile daily --kc ... --adt ... --trust ... --seed ...
   --default macos --name esp-daily-macos --out out/persistent` (rebuild just before installing: the clock starts from the build time
   until the RTC is in the profile, `docs/drivers/rtc.md`). A seed other than the installed one does not carry the owner's Data
   over: build-esp.py prints a loud note, refuses it with a collection that has no store guard, and `--replace-data` is the
   explicit "start Data over". The way to change the seed without losing Data is a flatten first (`docs/data-store.md` §4, T4);
   build-esp.py also warns when the new trust cache lacks CDHashes of the installed one.
3. Tablet to ABL fastboot (§3), then from the persist worktree (steps before options):
   `python3 tooling/persist/maint-session.py maint-boot map backup esp-final check maint-exit --esp esp-daily-macos.img --esp-final esp-daily-macos.img`
   then `abl-final reboot`. Every write is preceded by a verified NAS backup and followed by a SHA-256 read-back.

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
