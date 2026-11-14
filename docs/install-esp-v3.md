# Installing ESP v3: the owner's checklist (retired 27.0.1 plan)

**Retired 2026-11-14; do not run this checklist.** ESP v3 (`esp-daily3-macos.img`, sha256
`fa5c0485a1d29634284ef77cdbd3bd4e608a56caa9a77cde8ebdf55e2aa29ca5`) was a proposed 27.0.1 daily system, never installed.
The tablet now runs macOS 27.3 only from `esp-k273-v2` with the v3 delta; its root is on `linux_rootfs`, and the owner's 200 GiB
Data store is on `linux_data` (LBA 71859642, seed `9bd6f902`). The old `8f3331104e03c970` store was cleared. Use
[persistent-boot.md](persistent-boot.md) §13 and §13.8 for the current installation. Everything below is the historical v3
checklist, including commands and expected output that are **not valid for the current tablet**.

## 1. Your decisions (before the session)

| Decision | Default in v3 | Choose |
|---|---|---|
| **Charging level** | `--charging 1`: the PMIC charges on its own, read only | keep 1 until the 0 mA question of level 2 is answered; level 2 means a rebuild with `--charging 2` (one command: persistent-boot.md §11.4) |
| **Audio on by default** | off | off for now: it needs charging level 2 (the DSP), and level 9 (the daily volume) has not run yet; its first run is with you listening (`drivers/audio.md` §8.8 run 7) |
| **`piano-usb-shell=1`** (a root shell on the cable) | no | yes makes later layer updates possible without you at the keyboard, but anyone with the cable gets root |
| **Backup of the Data store** (then 12.9 GB; the current store is 200 GiB) | not taken | historical v3 option; current backup and rollback: persistent-boot.md §13.8 |
| **The system layer** (GPU plugin with today's fixes, the still login wallpaper, our tools) | not installed | once, after v3, with you typing your password (§4) |
| **The quieting** (`launchctl disable` of four looping system jobs: PerfPowerServices alone takes 1.7 cores at the idle login window) | not done | optional, in the same Terminal session as the layer |
| **Verbose boot text** (`-v`) | off (`--fast-boot 1`) | on only with a rebuild (`--fast-boot 0`) |

## 2. Before the session (historical Mac checklist; the 27.0.1 image must not be installed)

1. The NAS is mounted (`PIANO_BACKUP_DIR` in `~/.config/piano/env`); the tablet is charged (`fastboot getvar battery-voltage`).
2. The unattended RAM test passed: `v3-probe` (persistent-boot.md §11.6: the guard healthy at 180 s, Data store state 6, the login
   window, back in ABL fastboot at about 750 s). Before the layer also `v3-net` (the USB network's first run).
3. The image and its record next to the install state, and a dry run (sends nothing):
   ```sh
   PERSIST=<the persist checkout>/out/persistent
   cp -c out/persistent/esp-daily3-macos.img out/persistent/esp-daily3-macos.json $PERSIST/
   export PIANO_PERSIST_OUT=$PERSIST
   python3 tooling/persist/maint-session.py --dry-run maint-boot map backup store-backup esp-final check maint-exit \
       --esp esp-daily3-macos.img --esp-final esp-daily3-macos.img --new-backup
   ```
   Without the store backup, leave `store-backup` out here and below.

## 3. The session (retired; do not run these commands)

From this checkout, in a shell with `PIANO_PERSIST_OUT` exported as in §2 (every `maint-session.py` command below needs it: the
install state is the persist checkout's):

```sh
# tablet: hold power + volume-down until the fastboot screen (15-20 s)
python3 tooling/persist/maint-session.py abl-check
python3 tooling/persist/maint-session.py maint-boot map backup store-backup esp-final check maint-exit \
    --esp esp-daily3-macos.img --esp-final esp-daily3-macos.img --new-backup
python3 tooling/persist/maint-session.py abl-final reboot
```

Every write is preceded by a verified NAS backup (the tablet's sha256, the received bytes' sha256, the NAS copy read again without
the cache) and followed by a read-back of the partition and of LUN 0 (= `fa5c0485…`); any mismatch stops the session.
The historical v3 gate was `ok    the store at LBA 68119455 (seed 8f3331104e03c970, ...) IS the one this ESP resumes`. That
store no longer exists; this gate cannot pass and is not today's check. The current gate is the 200 GiB store at LBA 71859642 on
`linux_data`, seed `9bd6f902` (persistent-boot.md §13.8).

The historical first-boot expectation was `Data store state 2 ... seed 8f333110, store seed 8f333110`; today's installed store
reports seed/store seed `9bd6f902`. The boot-guard expectation remains `piano-boot-guard: healthy after 180 s, restart reason normal`. **Stop and roll back** on `state 3`, `state 7`,
`kept` or `started over`. The login window still shows the old plugin's boxes behind the clock until the layer is in.

## 4. The system layer (once, you at the keyboard)

persistent-boot.md §11.8: boot `out/ram-v3-layer` from ABL fastboot (your own Data, with the USB network), log in, then in Terminal
fetch `userland-update.tar` and `apply-update.sh` from the Mac, check the archive's sha256 (`69010daffa42b036…`), and run
`sudo sh apply-update.sh`. **You type your password yourself** (on the cover, or `python3 tooling/native/piano-input.py type
--secret` on the Mac, which reads it without echo and never logs it); nothing types it for you. Then
`sudo sh /private/var/piano/userland/install.sh --check` and Apple menu › Restart: v3 boots with the new plugin.

## 5. If something goes wrong (least invasive first)

1. **A hang or panic in the boot**: the boot guard lands in ABL fastboot within 180 s; `fastboot reboot` tries again.
   **Always**: power + volume-down for 15-20 s gives ABL fastboot; from there every RAM image boots as before.
2. **v3 misbehaves, layer not yet installed**: write daily2 back (in ABL fastboot, then):
   ```sh
   python3 tooling/persist/maint-session.py maint-boot map backup esp-final check maint-exit \
       --esp esp-daily2-macos.img --esp-final esp-daily2-macos.img
   python3 tooling/persist/maint-session.py abl-final reboot
   ```
   or daily2 from the verified NAS copy: `maint-session.py maint-boot`, then `maint-session.py rollback esp-final`, then
   `maint-session.py maint-exit` (a rollback runs on its own command line, in the maintenance image). Your Data is kept: both use
   the same seed.
3. **After the layer**: daily2 does not trust the new plugin. First the empty layer (`out/layer-none`, installed like §4), then
   step 2. Or, with the store backup, everything back to before v3 at once: `maint-session.py maint-boot`, then
   `maint-session.py rollback esp-final store-backup`, then `maint-session.py maint-exit` (your Data as it was at the backup;
   changes since are lost).
4. **The layer alone**: the empty layer puts the base's plugin files back byte for byte; Restart.
