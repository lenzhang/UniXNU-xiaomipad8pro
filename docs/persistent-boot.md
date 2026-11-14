# Persistent native macOS boot on piano (no `fastboot boot`)

Status 2026-11-11: **prepared and tested offline only** (nothing has been written to the tablet). The owner approved making native
macOS boot by itself from power-on, replacing Ubuntu, with plan option A1 (local notes, `persistent-boot-plan.md`, not
published). On 06:42 the owner set the rules for the install session, which this procedure follows:

- no question before each write step (the script has `--dry-run` to see everything first);
- before any write, a **verified NAS backup** of every affected region;
- after every write, a read-back from the medium and a SHA-256 comparison; **any mismatch aborts**;
- a RAM-boot rollback that keeps working, and restores of `boot_a` and the GPT from the verified backups;
- never written: `xbl`, `abl`, `tz`, `hyp`, `vbmeta`, `dtbo`, `persist` or any `_b` partition (`boot_b` is backed up only).

This is the one exception to the RAM-boot-only rule of `docs/PROJECT-MAP.md` §6 and `docs/bringup-debugging.md`: it writes exactly
the regions in §1, by `dd` from the Ubuntu release's maintenance RAM image. It never uses `fastboot flash`, `erase`, `set_active`,
or any `oem` command to ABL.

## 1. What is written where

LUN 0 has 4096-byte LBAs (124552192 blocks). The partition numbers are the GPT's (1-based).

| What | Where | Size | Image | Before |
|---|---|---|---|---|
| LUN 0 primary GPT | LBA 0-5 | 24 KiB | `gpt-head.bin` | `CRITICAL-BACKUP/new-primary.bin` (`90a9d3bb…`) |
| LUN 0 backup GPT | LBA 124552187-124552191 | 20 KiB | `gpt-tail.bin` | `CRITICAL-BACKUP/new-backup.bin` (`21b04d67…`) |
| #35 `linux_rootfs` | 56060928-**71527423** (was …71789567) | 59 GiB | (GPT field only) | 60 GiB |
| #37 `esp` (new) | **71527424-71789567**, type `C12A7328-…` (EFI system), attributes 0 | 1 GiB | `esp-<profile>-diag.img`, later `esp-<profile>-macos.img` | NativeUfs's unused log reserve |
| Data store (only with `--store move`) | [68381599, +3145825) moved to **[68119455, +3145825)** | 12.9 GB | (copied in place) | — |
| `boot_a` (LUN 4) | whole partition | 96 MiB | `boot-piano-macos-persistent-v4-signed.img` | Ubuntu `389d4271…` (backup `13-boot_a`) |

Not written: `vendor_boot_a` (`50f70e36…`, Ubuntu's: its DTB is `out/inputs/piano-boot.dtb`) and `init_boot_a` (`bbebc3d1…`) stay;
ABL pairs them with the new `boot_a` exactly as it did with Ubuntu's kernel. `boot_b`, `userdata`, `linux_data`, `super`,
`metadata` and every firmware partition are untouched. Slot A's attribute bits are not touched (`dd`, not `fastboot flash`), unless
`slot-success` is needed (§5).

The new GPT differs from the current one in exactly 100 bytes (`out/persistent/gpt-diff.txt`): entry 35's EndingLBA, entry 37,
and the entries/header CRCs of both copies. `tooling/persist/build-gpt.py` refuses to write its output if any other byte differs.
The ESP's unique GUID is derived from the disk GUID (uuid5): `3F8B0988-D851-5539-AE47-6E53C621D288`.

Why the store moves: NativeUfs puts the store at `partition end + 1 - 1 GiB reserve - W` (`native_ufs_store_open`), so shrinking
`linux_rootfs` by 1 GiB moves its place by 262144 blocks. With `--store fresh` (or when the store on the tablet was made for
another seed, which `--store auto` detects from the store header's seed identity; a self-contained store, [data-store.md](data-store.md),
is moved whatever its seed) nothing is copied and the first boot starts a new store at the new place (`Data store state 3`). The plan's in-device scratch copy (its S2) is not used: the verified NAS backup of
the whole store region replaces it, and the move is one forward `dd` within `linux_rootfs` (the destination lies below the source,
so every block is read before it is overwritten).

## 2. Boot chain after the install

```
power-on -> XBL -> ABL (slot A, orange-state notice) -> boot_a: BootShim + FD (Mu-Silicium UEFI) -> UniXNU menu (3 s)
  -> config + payload from partition 37 (FAT32, 4096-byte sectors) -> DarwinBoot -> k27 XNU at EL1 -> root from linux_rootfs
```

- `boot_a` holds only the kernel field of the RAM images (BootShim with a Linux Image header + FD + the boot DTB,
  `build-ram-images.py kernel_field`, sha256 `a1df7f14…`) in an Android boot **v4** container (page 4096, os_version 15.0.0,
  patch level 2026-02, no ramdisk, empty command line: the installed Ubuntu `boot_a`'s header values), with an AVB hash footer made
  like Ubuntu's (`tooling/bootimg/sign-on-remote.sh`: partition `boot`, 100663296 bytes, SHA256_RSA4096, rollback index
  1769904000, the three `com.android.build.boot.*` properties, the key whose AVB sha1 is `f5151930…`, the one that signed the
  installed `boot_a`).
- The ESP holds build-ram-images.py's payload layout: `EFI/UniXNU/unixnu.toml`, `EFI/UniXNU/Payloads/{kernel.macho,
  kernel.macho.adt, kernel.macho.trustcache, scratch-data.img}`. UniXNULib finds `unixnu.toml` on any FAT volume
  (`Config.c`), FD A mounts FAT on UFS (FatPkg works through DiskIo, so 4096-byte sectors are fine; `logfs` is FS0 already).
- Menu entries (`tooling/persist/build-esp.py`): `macos` (the arguments), `macos-diag` (the same + `piano-reset-after=240
  piano-reset-to=bootloader`: a run of 240 s, then ABL fastboot), `fastboot` (reset into ABL fastboot after 1 s, no UFS root, no
  store, no console mirror, so the last run's log survives), `reset`, `shutdown`, `tools`. Test phase: default `macos-diag`;
  daily use: default `macos`. Menu keys may not move the selection yet (build-ram-images.py note of 2026-11-10 22:37), so only the
  default is reliably reachable.

### The firmware change (firmware/patches/unixnu/0001)

UniXNULib took the first FAT volume with `\EFI\UniXNU\unixnu.toml` in `LocateHandleBuffer` order. The UFS LUNs are published
(UFSDxe is APRIORI) before UniXNURamDiskDxe's RAM disk, so once partition 37 exists, every `fastboot boot` image would boot the
ESP's configuration instead of its own payload, and `native-loop.sh`'s dump image would stop working. The patch tries the
volumes whose device path starts with UniXNURamDiskDxe's vendor node (`VenHw(A9C6827C-1BD8-410C-885A-C46500F7D836)`) before the
others, and logs `UNIXNU: config: volume <device path>`. A flashed `boot_a` (v4) has no FAT ramdisk (ABL hands over Ubuntu's
vendor ramdisk, a compressed cpio), so it falls through to the ESP. `tooling/firmware/setup-mu.sh` applies the patch to the
UniXNU checkout (reverse-check idiom); `tooling/persist/build-all.sh` (FD=1) rebuilds FD A and refuses an FD without it
(`out/fd/piano-uefi-A-bundled.fd.ram-payload-first`).

**Consequence for the dev loop:** once the ESP exists, every RAM image (`out/ram`, and above all the loop's dump image
`out/ram-dump`) must be built with the patched FD. An older one boots the ESP's `macos-diag` instead of its own payload.

## 3. Build (Mac, offline)

```sh
FD=1 tooling/persist/build-all.sh          # FD A with the patch, boot_a candidates, ESP images (ESP_PROFILE), GPT, SHA256SUMS
RAM=1 tooling/persist/build-all.sh         # optional: also out/ram + out/ram-dump with that FD (or tooling/k27/build-native-image.sh)
tooling/persist/sign.sh                    # on the signing host: -signed images, offline AVB check, out/persistent/macos-set/
python3 tooling/persist/test-persist.py    # host-only tests: tablet.sh, and the whole session against a fake tablet
```

Outputs in `out/persistent/` (all gitignored; hashes in `artifacts/ARTIFACTS.md`):

| File | What |
|---|---|
| `boot-piano-macos-persistent-v4.img` / `-signed.img` | the `boot_a` image (v4) |
| `boot-piano-macos-persistent-vN.img` / `-signed.img`, `boot-vN.json`, `kernel-field-vN.bin` | a later `boot_a` series (`build-boot.py --series N`, §12): the same container with another FD; the record keeps the FD's DarwinBoot window bound |
| `boot-piano-esp-test-v2.img` / `-signed.img` | the same kernel field in the RAM v2 container with a 4096-byte zero ramdisk: the RAM test of the flashed path, and (signed) the `boot_a` fallback if ABL refuses v4 |
| `kernel-field.bin`, `boot.json` | the kernel field; the build and signing record |
| `esp-<profile>-diag.img`, `esp-<profile>-macos.img`, `.json` | the 1 GiB ESP (default `macos-diag` / `macos`); the record has every input's sha256, the command lines, the seed identity, the store places, the FAT geometry |
| `gpt-head.bin`, `gpt-tail.bin`, `gpt.json`, `gpt-diff.txt` | the new LUN 0 GPT copies and the byte-exact diff |
| `macos-set/` | `boot.img` (signed v4) + slot A's current `init_boot.img`/`vendor_boot.img` + SHA256SUMS, for `piano-switch.py --rollback` |
| `ref/` | slot A's current boot triplet, decompressed from the 2026-11-10 backup |

**Selectable ESP configuration** (`build-esp.py --profile`; build-all.sh `ESP_PROFILE`, `ESP_ARGS`):

| Profile | Inputs (from `--from` checkout's `out/`) | Arguments |
|---|---|---|
| `base` | `k27/kc-k27-native.bin`, `adt/piano-k27.adt` (vmm-present 1), `inputs/kernel.macho.trustcache`, `inputs/scratch-data.img` (`cb50ceac…`) | `K27_ARGS` without `piano-reset-after`/`-to` |
| `softgpu` | a KC with NativeSoftGpu (checked), `adt/piano-k27-vmm0.adt` (vmm-present 0, checked), `soft-metal/` seed and trust cache | + `panic_on_crash_disabled=1` (docs/drivers/soft-metal.md §10) |
| `gpu` | a KC with PianoA830 and without NativeSoftGpu (checked), `adt/piano-k27.adt` built with `PIANO_VMM_PRESENT=0` (checked), `turnip-a830/stage/` seed and trust cache (the Turnip plugin) | + `piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1` (the A830 run that showed the login screen, 2026-11-11 07:45, without its reset and log arguments) |
| `daily` | the pinned base's seed and trust cache (`--seed base --trust base`, or `--layer DIR`'s trust cache), a KC with PianoA830, PianoPower, PianoDwc3, PianoKeyboard, PianoTouch and PianoBacklight and their fixes, without NativeSoftGpu (checked), the vmm-present 0 ADT | gpu's + the USB console and the Mac's HID, `piano-gpu-hang-ms=10000`, the keyboard cover, the backlight, touch level 2, the charging level, `piano-boot-guard=180`, no `-v` (§11.2) |

`--charging 1|2` (a profile with a charging level: `daily`, default 2) sets PianoPower's level and what comes with it
(`docs/drivers/charging-policy.md` §16: 2 is level 2 with ChargePolicy, the stepped Type-C current and PianoThermal's ADC, refused
for a collection whose PianoPower lacks the steps; 1 is level 1, read only).
`--arg NAME=VALUE` adds or replaces one argument (for example `--arg piano-power=1`: battery gauges in the log, read only). Every
line is checked against DarwinBoot's 1000-byte limit. For `softgpu`, first build its inputs in the source checkout:
`PIANO_EXTRA_KEXTS=$PWD/out/k27/kext/NativeSoftGpu.json PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh` with
`PIANO_SOFT_METAL_LOG` unset (a staged log loop does not belong in a daily seed), then
`ESP_PROFILE=softgpu ESP_FROM=<that checkout> tooling/persist/build-all.sh`. For `gpu` the inputs come from
`PIANO_EXTRA_KEXTS=$PWD/out/k27/kext/PianoA830.json PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh` (it stages the
Turnip plugin itself); copy them aside first (`cp -c`) if that checkout keeps building, and pass the copy as `--from`.

The RAM tests of session 2 need an ESP whose default resets into ABL fastboot (`-diag`); the step refuses a `-macos` one, which would
never come back by itself. So with `gpu`: `esp --esp esp-gpu-diag.img` in session 1 and `esp-final --esp-final esp-gpu-macos.img`
in session 3, and the same `--esp` on every invocation (step `map` compares the store's seed with that ESP's).

The ESP image is reproducible (same inputs and `ESP_UTC` give the same bytes; checked twice). It is made by mtools (`mformat -F -S 5
-M 4096 -c 1 -H 71527424 -v PIANOESP -N 50494e4f`, file times 2010-01-01) and checked twice: by mtools (minfo, copy back) and by an
independent reader (`common.fat32_read`: EDK2 FatPkg's BPB rules, every file through its cluster chain, hashed against its input).
The FD build itself is not bit-reproducible (two clean builds differ), so the FD and everything containing it are identified by
their hashes.

## 4. Backups (before any write)

Step `backup` exports from the tablet, over one TCP connection per region, into `PIANO_BACKUP_DIR/persistent-boot-<time>/` on the
NAS (`PIANO_BACKUP_DIR` comes from `~/.config/piano/env` at run time; it is never printed or stored in a tracked file). Each copy:
received byte count and SHA-256 equal to the tablet's own SHA-256 of the region (page cache dropped), then the NAS file read back
without the page cache (`F_NOCACHE`) and hashed again. `SHA256SUMS` and `manifest.json` sit next to the files; regions up to
128 MiB are also kept in `out/persistent/session/`.

| Backup | Region | Size |
|---|---|---|
| `lun0-gpt-primary` | LUN 0 LBA 0-5 | 24 KiB (must equal `new-primary.bin`) |
| `lun0-gpt-backup` | LUN 0 LBA 124552187-124552191 | 20 KiB (must equal `new-backup.bin`) |
| `boot_a`, `boot_b`, `vendor_boot_a`, `init_boot_a` | whole partitions | 96 + 96 + 96 + 8 MiB |
| `linux_rootfs-tail-esp` | LUN 0 LBA 71527424-71789567 | 1 GiB |
| `linux_rootfs-store` (store moves) | LUN 0 LBA 68119455-71527423 (old and new place) | 13.96 GB |
| `linux_rootfs-store-header` (fresh store) | LUN 0 LBA 68381599 | 4 KiB |

Every write step checks that the verified backup of its region exists and equals the region's current SHA-256, else it stops. A
backup is never replaced: when this install has written a region since, the backup of before stays the rollback source; a region
that changed without this install writing it stops the session (`--new-backup` starts a new set). Expect about 30 MB/s to the
NAS: the store region takes about 8 minutes to export and as long to read back; `--store fresh` skips it.

## 5. The session (main session, with the tablet)

`tooling/persist/maint-session.py` runs the steps (`--list` describes each; `--dry-run` prints every command and sends nothing).
Commands reach the maintenance image's root shell through `tooling/native/maint-shell.py` (`PIANO_MAINT_HOST`); the tablet side is
`tooling/persist/tablet.sh`, which the host serves (HTTP on the USB-link address, a fixed list of files, sha256 checked) together
with the images; exports come back to a one-shot TCP receiver on the same address (ports 8000 and 9000; the Mac's application
firewall is off, else allow python3). Long jobs run detached on the tablet (`setsid`) and are polled. State:
`out/persistent/session/state.json`. Expect about 4 s per tablet command (maint-shell.py's login).

```sh
P=tooling/persist/maint-session.py
python3 $P --dry-run abl-check maint-boot map backup gpt store esp check maint-exit     # read it first
# ABL fastboot (power + volume-down), read only:
python3 $P abl-check            # product piano, unlocked yes, current-slot a, slot-unbootable:a no, battery-soc-ok yes,
                                # partition-size:boot_a 0x6000000; shows slot-successful:a, slot-retry-count:a, battery-voltage
# session 1, maintenance RAM image (boot_a untouched):
python3 $P maint-boot map backup gpt store esp check maint-exit
# session 2, RAM tests (nothing written; each takes ~240 s + the dump):
python3 $P ram-esp-test         # boot-piano-esp-test-v2.img: UEFI log "config: volume VenHw(860845C1-…)/HD(37,GPT,3F8B0988-…"
python3 $P ram-v4-signed        # the exact boot_a bytes; ABL takes vendor_boot_a/init_boot_a from the partitions
python3 $P ram-precedence       # out/ram-precedence (patched FD, data-persist=0): "config: volume VenHw(A9C6827C-…)", its own payload wins
# session 3, maintenance RAM image:
python3 $P maint-boot map backup boot-a check maint-exit     # + esp-final for daily use (default macos), + slot-success if needed
# ABL fastboot:
python3 $P abl-final reboot     # a, yes, no; then slot A boots macOS. Last test: power off, unplug, power on.
```

What the steps check, besides the backups and read-backs:

- `map`: LUN 0 is the disk on UFS target 0 with 4096-byte blocks and 510165778432 bytes; nothing from UFS is mounted;
  `linux_rootfs` and `boot_a` at their known places; both GPT copies are exactly the old tables or exactly the new ones;
  `vendor_boot_a`/`init_boot_a` are slot A's current images; block 0 of `linux_rootfs` is the SHNGROOT manifest and the root image
  plus 1024 blocks fits below the new store place; both store headers with their seed identity (SHNGSEED2 hash,
  `common.seed_info`), compared with the ESP's seed.
- `gpt`: backup copy first, then the primary, each read back; `blockdev --rereadpt`; `sgdisk -v` "No problems found";
  sysfs shows `linux_rootfs` 448487424+123731968 and partition 37 at 572219392+2097152 (512-byte units).
- `store`: the store window's SHA-256 before; forward `dd` inside `linux_rootfs`; the new place hashes the same; SHNGDATA.
- `esp`: the image fetched and checked, written through `/dev/<partition 37>`, read back through the partition and through LUN 0.
- `boot-a`: `vendor_boot_a`/`init_boot_a` unchanged; the signed image fetched and checked; written by `dd`; read back whole;
  `ANDROID!`.
- `check`: root manifest, `boot_b`, `vendor_boot_a`, `init_boot_a` unchanged; GPT = the new tables; `sgdisk -v` clean; ESP = the
  image written; the store header in place.
- `slot-success` (only if `abl-check` shows `slot-successful:a: no`): bit 54 of `boot_a`'s GPT entry, after backups of both GPT
  copies of its LUN; the read-back may differ from the backup only in that entry's attributes and the two CRCs. Never `set_active`.

If ABL refuses the v4 image for `fastboot boot` (`ram-v4-signed`: our UEFI never appears, or `Load Error`): `fastboot reboot
bootloader`, and either treat the v4 container as tested by its Ubuntu twin or write the signed v2 test image instead:
`python3 $P maint-boot map backup boot-a check maint-exit --boot-image boot-piano-esp-test-v2-signed.img`.

## 6. Verify

- UEFI DEBUG log (`ram-*` steps save it: `session/<image>-uefi.bin`): `UNIXNU: config: loaded \EFI\UniXNU\unixnu.toml`, then
  `UNIXNU: config: volume VenHw(860845C1-BE09-4355-8BC1-30D64FF8E63A)/HD(37,GPT,3F8B0988-D851-5539-AE47-6E53C621D288,…)`.
- XNU log (`session/<image>-xnu.txt`, read by the dump image): `NativeRamBlock: Data store state 2 … first LBA 68119455` (moved)
  or `state 3 … first LBA 68119455` (fresh), root from `linux_rootfs`, launchd running.
- After `reboot`: the orange-state notice, 3 s of the UniXNU menu, then macOS; with the `macos-diag` ESP it resets into ABL
  fastboot after 240 s, where `tooling/native/native-loop.sh`'s dump image (patched FD) reads its log.

## 7. Roll back (least invasive first)

**Always available:** power + volume-down (15-20 s from a hung or panicked kernel) reaches ABL fastboot; ABL is never written.
From there every `fastboot boot` RAM image works as before: with the patched FD its own payload wins over the ESP
(`ram-precedence` proves it), so the whole dev loop (`native-loop.sh`) and the maintenance image keep working with macOS
installed. The `fastboot` menu entry also resets into ABL fastboot.

| Goal | How |
|---|---|
| Undo one step | `python3 tooling/persist/maint-session.py rollback boot-a` / `esp` / `store` / `gpt` (in the maintenance image; each streams its verified backup back and reads it back). Order for a full undo: `boot-a`, `esp`, `store`, `gpt`. |
| Ubuntu | Ubuntu's root is gone, so: maintenance image, `rollback esp store gpt boot-a` (old tables, old `boot_a` = Ubuntu `389d4271…`); then restore `linux_rootfs` from the 2026-11-10 partition backup (the local restore notes of 2026-11-10, `RESTORE-20261110.md`, not published, scenario B: stream `35-linux_rootfs.img.zst` decompressed onto the partition, check its sha256). `vendor_boot_a`/`init_boot_a` are Ubuntu's already; `linux_data` (`/home`) is never touched. Keep the macOS root image (OS/SystemOS windows) for a later reinstall. |
| Android (charges) | ABL fastboot: `python3 $REPO/tools/piano-switch.py --serial $PIANO_SERIAL --images ~/Downloads/XiaomiPad8Pro-Ubuntu/CRITICAL-BACKUP/android-set --rollback out/persistent/macos-set`, then `fastboot -s $PIANO_SERIAL reboot` (`$REPO` = the Ubuntu release tree). `userdata`, `super` and `metadata` are untouched; Android marks the slot itself. Back to macOS: `piano-switch.py --serial $PIANO_SERIAL --images out/persistent/macos-set --rollback ~/Downloads/XiaomiPad8Pro-Ubuntu/CRITICAL-BACKUP/android-set` (all three, since `vendor_boot_a`/`init_boot_a` are Android's then), then `maint-session.py --new-backup abl-check maint-boot map backup slot-success check maint-exit` (a flash clears bit 54 and macOS does not set it; `abl-check` records the slot state `slot-success` acts on). |
| Factory | `CRITICAL-BACKUP/NAS-README.zh-CN.md` (original GPT `backup-meta/gpt/sda-*.bin`, the OS3.0.303 package; never `flash_all_lock`). |

## 8. Charging caveat

Native macOS charges only with **PianoPower level 2 and the DSP** (`docs/drivers/charging.md`): the charger PMIC's registers
belong to the DSP's execution environment, so charging control needs the DSP started from XNU, which is owner-attended work and
not part of this install (level 2 is not in any ESP profile). What was measured natively (level 1, read only, 240 s on the Mac's
USB-C port): at 99 % the battery current read 0 mA ("idle"): the system ran from the USB input without draining the pack. Whether a
partly discharged pack charges without the DSP is unmeasured. Note also:

- a powered-off tablet that is plugged in boots `boot_a`, i.e. macOS, not Android's off-mode charger;
- check the pack from ABL with `fastboot getvar battery-voltage`; Android (§7) remains the charging path until level 2 is done;
- `--arg piano-power=1` in an ESP build logs the gauges (voltage, %, current) without writing anything.

## 9. Open items and risks

- The v4 container with our kernel field has never booted (session 2, `ram-v4-signed`, tests it; fallback v2, §5).
- The 4096-byte-sector FAT32 ESP on UFS in EDK2: `logfs` is the evidence; `ram-esp-test` proves it before `boot_a` is written.
- The partition type is the EFI system GUID; the stock XBL/ABL ignore LUN 0's partitions beyond their own, and the ESP has no
  `\EFI\BOOT\BOOTAA64.EFI`. If a firmware stage ever reacts to it, `build-gpt.py` can be changed to Basic Data (FatPkg mounts by
  content, not by type).
- A dev RAM image with another Data seed: with the store guard ([data-store.md](data-store.md), 2026-11-11) its NativeRamBlock
  keeps the installed store untouched and runs Data volatile, and `build-ram-images.py` gives such an image `data-persist=0` by
  default anyway (`PIANO_INSTALLED_SEED`). Images built before the guard, and the installed ESP's own collection until it is
  rebuilt, still replace another seed's store. The loop's dump image is always built with `data-persist=0`.
- Payload updates still need the cable: maintenance image, `backup` (new set) and `esp` with a new image. NativeUfs could later
  publish partition 37 so macOS updates its own payload.
- The clock is the ESP build time (`droidvm-utc`) unless the ESP is built with `--rtc-offset` on a k27 0060 kernel: then the
  calendar starts from the PMIC RTC (`docs/drivers/rtc.md`; built, not yet run on the tablet).
- Watchdog: an ESP built from a k27 0063 collection with the daily profile has the boot guard (`piano-boot-guard=180`,
  [boot-guard.md](boot-guard.md)): a hang or panic in the first 180 s lands in ABL fastboot, a later one restarts macOS. Older ESPs
  arm no watchdog (panics restart into macOS, a hang stays); the escape is power + volume-down.

## 10. The Data store after the install

The store's format, the store guard (another seed's store is kept and Data runs volatile; `data-store-replace` starts it over), flatten
(`data-store-flatten=1`: a self-contained store that every later seed resumes), the plugin staging plan, the tooling
(`PIANO_INSTALLED_SEED`, `build-esp.py --replace-data/--flatten-data`, `store-test-images.py`) and the RAM test procedure that proves
the guard without risking the installed store: [data-store.md](data-store.md).

## 11. ESP v3: the daily system with the owner's Data (built 2026-11-12; not installed)

Built offline in an agent worktree on main `3c7ebec` plus the change that wrote this section, after the day's tablet results
([results-2026-11-12.md](results-2026-11-12.md)): the boot guard's G1-G4, PianoDwc3's cold start (C0-C5, C7 and the re-tests C6b and
C6c), touch's announced-frame rule, Turnip 0007/0008 with frontend 0003, audio levels 4-8, charging level 2's input steps. **Not
installed and not run.** It replaces the installed `esp-daily2-macos` (`e0acb375…`), keeps the store through the store's base
([data-store.md](data-store.md) §9), and writes only partition 37. The owner's checklist and decisions: [install-esp-v3.md](install-esp-v3.md).

### 11.1 Contents

| Payload file | Installed (`esp-daily2-macos`) | v3 (`esp-daily3-macos`) |
|---|---|---|
| `kernel.macho` (KC) | `3850545b…`: no 0060, no 0063 | `ed7430c4c6c2b7df…`: the k27 **0064** kernel `2fe50b346041ab9f…` (0060 RTC, 0061/0062/0064 opt-in, 0063 boot guard) and the 25 native kexts of `configs/k27-native-kexts.txt` (PianoKeyboard and PianoCamera now listed). `check-native-kexts.py`: 25 linked, none from outside this repository; every vtable check 0 problem slots |
| `kernel.macho.adt` | `139af652…`, vmm-present 0 | `acea0fecbba9536c…`, vmm-present 0 |
| `scratch-data.img` (seed) | `23a5ec59…`, identity `8f333110…` | **the same file**: the pinned base (pinned again in the worktree from the installed image: the same three files as the main checkout's `out/base/8f3331104e03c970`) |
| `kernel.macho.trustcache` | `cd4aa536…`, 5 modules | `a83bbee6d2b01eab…`: the same 5 modules plus 1 first, the layer's 5 new CDHashes (§11.3) |
| `firmware/` | none | `PianoAudio/fs19xx.fsm`, `PianoVideo/vpu35_4v.mbn`: the collection's pins (both drivers off in this profile) |
| `unixnu.toml` (`macos`) | GPU, PianoPower 1, USB console + HID, `data-store-replace=0xaced5bf9` | §11.2: the daily profile, `piano-boot-guard=180`, `data-persist=2`, no `data-store-replace`; menu timeout 3 s, default `macos` |

The kernel: the k27 tree's `out/k27/kernel-k27-0064-native.macho`, built at 06:39 from a scratch copy after 0061 and 0062 were amended
that morning (0062: `machdep.piano_sleep` `CTLFLAG_KERN`; 0061: PSCI's SMC32 results sign-extended). No build record names a hash,
and none can: the kernel's bytes depend on the tree's path (`__FILE__`). Checked instead:
- every file 0060-0064 touch in the tree's `xnu` has the blob id of its patch's last post-image in this repository (16 files, 0
  mismatches);
- the binary has `machdep.piano_sleep` at oid_kind `0x81e00003` (`CTLFLAG_KERN`), where 0063 and the earlier `0064pre`
  (`01d4ef0c…`) have `0x80e00003`;
- it has six `sxtw` in `piano_pm_probe`; `0064pre` has none.

### 11.2 The daily profile (`build-esp.py --profile daily`, the driver choices)

The profile now carries the desk images' input drivers (`build-desktop-images.sh`, the levels that ran on the tablet) and no `-v`.
It refuses a collection without the fixes those drivers need (`kc_markers`): touch's announced-frame rule, PianoDwc3's role rule,
PianoA830's map limits, NativeGopDisplay's cursor layer and PianoKeyboard's resting thumb.

| Driver, arguments | v3 | Why |
|---|---|---|
| GPU: `piano-gpu=2 piano-gpu-delay=20 piano-gpu-hang-ms=10000` | on | the login window and the desktop; a long full-panel pass at the login transition is no hang ([drivers/gpu-userspace.md](drivers/gpu-userspace.md)) |
| USB console and the Mac's keyboard/pointer: `piano-usb=2 piano-usb-hid=1`; cold start `piano-usb-cold=auto` (the default: no argument) | on | the way in from the Mac on the normal boot path: C0-C5, C7, C6b and C6c passed ([forensics-2026-11-11.md](forensics-2026-11-11.md) §7) |
| keyboard cover: `piano-keyboard=2 piano-keyboard-gestures=1` | on | keys and the touchpad delivered in attend1-4 (299 keys, 0 rollovers), the desk images |
| backlight: `piano-backlight=2 piano-backlight-keys=1` | on | off with display sleep and back with a key (attend2); the brightness keys not pressed yet |
| touch screen: `piano-touch=2` | on | the announced-frame rule: 25 minutes, 949 reads, one firmware download; this unit's Y from `PIANO_TOUCH_INVERT_Y` at build. `--arg piano-touch=0` is the fallback |
| charging: `--charging 1` (`piano-power=1`) | level 1 | level 2's input steps work, but the battery stays near 0 mA at 81-93 % while plugged in (being investigated); `--charging 2` once that is settled |
| RTC calendar: `--rtc-offset 1794397906` | on | the desk images' clock from the PMIC RTC; without it every boot starts at the ESP's build time |
| boot guard: `piano-boot-guard=180` | on | G1-G4 behaved as designed (healthy at 60 s, the watchdog fed 6 times, longest gap 10.0 s) |
| `--fast-boot 1` (no `-v`) | on | the desk and guard images ran so; 15-40 s less boot; the USB console and the mailbox keep every line. `--fast-boot 0` fits only with `--charging 1` (the 1000-byte line) |
| audio: `piano-audio` | **off** | (1) the DSP link needs `piano-power=2`, so at charging level 1 the Speaker and Mic would be silent devices; (2) level 9, the daily level (gain up to 192, full scale up to -1 dBFS, no speaker protection), has not run: levels 4-8 passed today, level 9's first run is attended (`drivers/audio.md` §8.8 run 7); (3) every audio run had PianoPower's writes off (`piano-power-write=0`): audio beside the charging policy's writes is untested. Once charging level 2 is decided and run 7 passed: `--charging 2 --arg piano-audio=9 --arg piano-audio-gain=176 --arg piano-audio-ceiling=6` |
| Wi-Fi, camera, Bluetooth, sensors, DisplayPort, video, USB host, the policy, the buttons (`piano-input`), CPU idle and sleep (0061/0062/0064) | off (inert in the KC) | not at a daily level, or not run with this profile (the buttons: the power key in macOS without `piano-sleep` is untried) |
| `piano-usb-net=1` | off | not run on hardware: `ram-v3-net` (§11.6) is its first run; the layer's session image has it (§11.8) |
| `piano-usb-shell=1` | off | an unauthenticated root shell on the cable: the owner's decision |

The `macos` line (798 bytes; `macos-diag` 846, `fastboot` 530; the limit is 1000):

```
debug=0x14e kt_fixed_zones=120 -no_slto_panic tlto_us=0 -apfs_shared_datavolume offline-apfs-load=1 owner-staged-apps=1
mac-amfi-launch-permissive=1 union-hide-lower=1 malloc-ranges-off=1 arm64_maxoffset=0x7FFE000000 droidvm-timebase-hz=24000000
exec-env=com.apple.WebKit.WebContent:GIGACAGE_ENABLED=no rootauth=0 vm_compression_limit=1572864 droidvm-tz=Asia/Singapore
pstore-console=0x9cb00000 pstore-console-size=0x700000 native-ufs-root=1 native-ufs-diagnostics=1 piano-gpu=2 piano-gpu-delay=20
panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-gpu-hang-ms=10000 piano-keyboard=2 piano-keyboard-gestures=1
piano-backlight=2 piano-backlight-keys=1 piano-touch=2 piano-power=1 piano-rtc=1 piano-rtc-offset=1794397906 piano-boot-guard=180
data-persist=2 droidvm-utc=<build time> sheng-smp=1
```

### 11.3 The system layer

`out/layer-v3` (`stage.py --live ... --gpu-bundle out/turnip-a830/NativeAdrenoMetal.bundle --installed-trust <the base's>`):
`userland-update.tar` `69010daffa42b036e943bda93eb8cca509e485cd04d48ff1da838dba29e0af9c` (17 files, 7 units), `apply-update.sh`
`f564e7edd4d73b34…` (the archive's sha256 inside), `kernel.macho.trustcache` `a83bbee6…`. The units:

| Unit | What | New CDHash |
|---|---|---|
| `gpu-nativeadrenometal` | `/Library/GPUBundles/NativeAdrenoMetal.bundle`: Turnip 0001-0008 and frontend 0001-0003, built from scratch in the worktree and byte for byte main's bundle (`libvulkan_freedreno.dylib` `c19b14a3…`, `NativeAdrenoMetal` `575393ed…`, `Info.plist` `9595a4d2…`) | `33f6503b…`, `3b48e955…` |
| `piano-still-wallpaper` (new) | the still login wallpaper, once a boot ([drivers/gpu-userspace.md](drivers/gpu-userspace.md)) | none |
| `pianousb-shell`, `piano-rotated`, `piano-torch` | inert without their arguments | `83876bc9…`, `c401d51b…`, `f386f217…` |
| `piano-policy-agent`, `piano-pmset-defaults` | scripts | none |

v3 trusts all five, so the layer can go in on v3's first boot. Until it does, Data keeps the base's plugin (Turnip without 0006-0008:
the boxes behind the login window's clock, no video wallpaper). The kernel side of the map fix is in v3 already. The undo is
`out/layer-none` (`configs/userland-none.toml`, archive `50737a17…`): every unit out, the base's plugin files back.

### 11.4 Build (Mac, offline, no device)

```sh
K=$(sed -n 's/^PIANO_K27_TREE=//p' ~/.config/piano/env)
K27_KERNEL=$K/out/k27/kernel-k27-0064-native.macho PIANO_GPU=2 PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh   # KC, ADT, Turnip
python3 tooling/persist/base-seed.py pin --esp <persist checkout>/out/persistent/esp-daily2-macos.img     # once; then: show
python3 tooling/userland/stage.py --live out/layer-v3 --gpu-bundle out/turnip-a830/NativeAdrenoMetal.bundle \
        --installed-trust "$(python3 tooling/persist/base-seed.py path trust)"
python3 tooling/userland/stage.py --manifest configs/userland-none.toml --live out/layer-none --installed-trust out/layer-v3/kernel.macho.trustcache
python3 tooling/persist/build-esp.py --profile daily --default macos --boot-guard 180 --layer out/layer-v3 --charging 1 \
        --rtc-offset 1794397906 --name esp-daily3-macos --out out/persistent
```

Built 2026-11-12 (`--utc 1794473979`): **`esp-daily3-macos.img` sha256 `fa5c0485a1d29634284ef77cdbd3bd4e608a56caa9a77cde8ebdf55e2aa29ca5`**,
seed identity `8f3331104e03c970`. With the RTC in the profile the build time is only the fallback clock. A rebuild just before
the install changes only `unixnu.toml` and the image hash.

### 11.5 Verify (offline; all done for the image above)

1. The record `esp-daily3-macos.json`: `seed.identity_sha256` `8f3331104e03c970…`, `inputs.seed.sha256` `23a5ec59…`;
   `data_store.same_seed` true, `kc_has_store_guard` true, `trust.missing_cdhashes` 0, `base.source.sha256` `e0acb375…`;
   `boot_guard` 180, `kc_has_boot_guard` true, `kc_has_rtc_calendar` true, `charging` 1, `fast_boot` 1; `kc_natives` 25, among
   them PianoA830, PianoPower, PianoDwc3, PianoKeyboard, PianoTouch and PianoBacklight, no NativeSoftGpu.
2. The image read back by the independent FAT32 reader: 7 files, each the record's sha256; the payload files are the build
   inputs; `unixnu.toml` parses (default `macos`).
3. `python3 tooling/persist/base-seed.py show`; `python3 tooling/k27/check-native-kexts.py --self-test` and
   `check-native-kexts.py <repo> --kc-log out/k27/kc-k27-native.log`; `python3 tooling/metal/check-bundle-paths.py --self-test`
   and on the bundle.
4. `python3 tooling/persist/test-data-store.py`, `python3 tooling/persist/test-persist.py` (the session against a fake tablet,
   `store-backup` and its rollback included), `python3 tooling/userland/test-stage.py` (the real layer and the empty one on a
   scratch root): all PASS.
5. The session as dry runs, on a copy of the installed session's state (nothing sent; the state file unchanged): `abl-check`;
   `maint-boot map backup store-backup esp-final check maint-exit --esp esp-daily3-macos.img --esp-final esp-daily3-macos.img
   --new-backup`; `abl-final reboot`; `rollback esp-final store-backup`; and the write-back of daily2.

### 11.6 RAM tests of the exact payload (nothing written; unattended)

Both images are the ESP's KC, ADT, trust cache and seed, built by `build-ram-images.py` with the `macos` line's arguments,
`PIANO_FAST_BOOT=1`, `PIANO_DATA_PERSIST=1` (the read-only probe: the store is reported, not applied, Data is the base and volatile)
and, instead of `piano-reset-after` (which would turn the guard off), the guard itself as the way back: `piano-reset-to=bootloader`
(the reason stays bootloader when healthy) and `piano-boot-guard-test=hang-at=720` (every CPU stops at 720 s; the watchdog bites into
ABL fastboot within 32 s). The dump image `out/ram-v3-dump` (`data-persist=0`, reset after 1 s) reads the mailbox afterwards.

```sh
P="piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-gpu-hang-ms=10000 piano-keyboard=2 \
piano-keyboard-gestures=1 piano-backlight=2 piano-backlight-keys=1 piano-touch=2 piano-power=1 piano-rtc=1 piano-rtc-offset=1794397906 \
piano-boot-guard=180"
I=(--fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --kc out/k27/kc-k27-native.bin --trust out/layer-v3/kernel.macho.trustcache
   --seed out/base/8f3331104e03c970/scratch-data.img --adt out/adt/piano-k27.adt --utc "$(date +%s)")
PIANO_FAST_BOOT=1 PIANO_RESET_AFTER=0 PIANO_DATA_PERSIST=1 PIANO_EXTRA_ARGS="$P piano-reset-to=bootloader piano-boot-guard-test=hang-at=720" \
  python3 tooling/bootimg/build-ram-images.py "${I[@]}" --out out/ram-v3-probe
PIANO_FAST_BOOT=1 PIANO_RESET_AFTER=0 PIANO_DATA_PERSIST=1 PIANO_EXTRA_ARGS="$P piano-usb-net=1 piano-reset-to=bootloader piano-boot-guard-test=hang-at=720" \
  python3 tooling/bootimg/build-ram-images.py "${I[@]}" --out out/ram-v3-net
PIANO_FAST_BOOT=1 PIANO_RESET_AFTER=1 PIANO_DATA_PERSIST=0 python3 tooling/bootimg/build-ram-images.py "${I[@]}" --out out/ram-v3-dump
```

Built (`verify-ram-images.py` PASS each): `ram-v3-probe` `098393ab8ea3ed65…` (858-byte line), `ram-v3-net` `4b747766a2a7de4e…`,
`ram-v3-dump` `99d3cdf8b882c776…`. Queue lines (`TAG IMG DUMP SECS`, the directories absolute):

```
v3-probe <this checkout>/out/ram-v3-probe <this checkout>/out/ram-v3-dump 800
v3-net <this checkout>/out/ram-v3-net <this checkout>/out/ram-v3-dump 800 need=v3-probe:piano-boot-guard:_healthy_after_180_s
```

Expected in `v3-probe` (USB console, then the mailbox):
- `PianoDwc3: cold: verdict (piano-usb-cold=auto): ...` (from ABL fastboot: a running device controller, as C1), enumeration;
- `NativeRamBlock: read-only probe: the store's P pages are not applied to a large disk; Data is the seed's this boot`, then
  `Data store state 6 ... seed 8f333110, store seed 8f333110`;
- `piano-boot-guard: armed: restart reason bootloader until healthy, ...`, `exec of launchd at N s`, `healthy after 180 s, restart
  reason stays bootloader (piano-reset-to)`, no `CPU N took no interrupt`;
- `piano-rtc: count ... the calendar starts there`; `PianoKeyboard: start: piano-keyboard=2`, `PianoTouch: start: piano-touch=2`
  and its `a frame read only when the controller announces it` rule, no endless `frames: watchdog` lines;
  `PianoBacklight: start: piano-backlight=2`;
- the login window on the A830 with the base's plugin (the boxes behind the clock are expected until the layer is in), no
  `BoMap ... NoResources`, no `exit: pid N WindowServer`;
- at 720 s `piano-boot-guard: test: every CPU stops now ...`, back in ABL fastboot within about 32 s.

`v3-net` adds §14.6 of [drivers/usb-device-debug.md](drivers/usb-device-debug.md): `net: CDC-NCM network function requested`, the
TX FIFO plan, `net: interface published`, `the Mac selected the NCM data interface`, `link up`, `macOS named the interface enN`, and
the console port working beside it. It is the transport's first hardware run, which the layer's session needs (§11.8).

Not for the queue: `out/ram-v3-layer` (`c3e77fb713d171c8…`) is the layer's session image: the same payload with `piano-usb-net=1`,
`PIANO_DATA_PERSIST=2` (the owner's own Data, resumed and written) and no reset at all. It is booted only with the owner present.

### 11.7 Install (cable session)

The install state is the persist worktree's (`out/persistent/session/state.json`, the checkout that installed daily2), whose tooling
is older than this tree's: it lacks the installed-store check and `store-backup`. So this checkout's `maint-session.py` runs on that
state with `PIANO_PERSIST_OUT` (the session directory follows it):

```sh
PERSIST=<persist checkout>/out/persistent
cp -c out/persistent/esp-daily3-macos.img out/persistent/esp-daily3-macos.json $PERSIST/
export PIANO_PERSIST_OUT=$PERSIST
P=tooling/persist/maint-session.py
python3 $P --dry-run maint-boot map backup store-backup esp-final check maint-exit --esp esp-daily3-macos.img --esp-final esp-daily3-macos.img --new-backup
# tablet in ABL fastboot: power + volume-down (daily2's USB console does not enumerate on the normal boot path)
python3 $P abl-check
python3 $P maint-boot map backup store-backup esp-final check maint-exit --esp esp-daily3-macos.img --esp-final esp-daily3-macos.img --new-backup
python3 $P abl-final reboot
```

- `--new-backup` starts a new NAS set: both GPT copies, the boot partitions and the ESP region as it is now (daily2), each exported,
  checked against the tablet's sha256 and read back from the NAS without the cache. `rollback esp-final` restores daily2 from it.
- `store-backup` (new, optional: the owner's decision) adds the installed store, 12.9 GB at LBA 68119455, verified the same way:
  about 7 minutes out and 7 back. `rollback store-backup` puts the owner's Data back to that moment. Without it, leave the step out.
- `map` must say `ok    the store at LBA 68119455 (seed 8f3331104e03c970, ...) IS the one this ESP resumes`. `esp-final` refuses
  an ESP whose seed is not that store's. The older line `Data store is NOT usable with the ESP's seed ... start fresh` is about the
  store's place of before the install (LBA 68381599): only the `store` step acts on it, and this session does not run it.
- `esp-final` writes partition 37 and reads it back through the partition and through LUN 0 (= `fa5c0485…`). Not written:
  `boot_a`, the GPT, the store, every other partition.

First boot (`python3 tooling/native/usb-console.py --capture 600`; the cold start should now enumerate on this path):
- `PianoDwc3: cold: verdict (piano-usb-cold=auto): the inherited state is NOT a running device controller (...)` and the cold
  start, or the link watch's cold start after 15 s, then enumeration; the `cold: inherited` lines record the normal path's state;
- `NativeRamBlock: Data store state 2 (...), header 0, first LBA 68119455, 3145728 pages, direct 1, seed 8f333110, store seed
  8f333110, flags 0, generation G, present P`;
- `piano-boot-guard: healthy after 180 s, restart reason normal`.

**Stop** on `state 3`, `state 7`, `kept` or `started over`.

### 11.8 The system layer onto the owner's Data (owner present, once)

After v3 is installed (its trust cache must trust the layer before the files exist: ESP first, files after). The base's Data has no
root shell and no network job, so the first time needs the owner at the keyboard and the USB network (after `v3-net` passed):

1. From ABL fastboot: `fastboot -s $PIANO_SERIAL boot out/ram-v3-layer/boot-piano-macos-ram-A-bundled.img`, with
   `python3 tooling/native/usb-console.py --capture 3600` running. It resumes the owner's Data (state 2) and writes into it.
2. The owner logs in on the cover, as every day.
3. The Mac: `python3 tooling/native/usb-console.py --list` names the network interface and its address (169.254.x.x when nothing
   assigns one, after up to a minute; if it has none, System Settings › Network lists the new interface, set to DHCP); then
   `cd out/layer-v3 && python3 -m http.server 8000 --bind <that address>`.
4. The tablet, in Terminal (the Mac may type these lines with `piano-input.py type`; **the password only the owner types**, on the
   cover or with `piano-input.py type --secret`, which reads it without echo and never logs it):
   ```sh
   for i in $(ifconfig -l); do ifconfig $i | grep -q 02:50:49:41:4e:02 && echo $i; done       # our interface, say en1
   sudo ipconfig set en1 DHCP; sleep 20; ipconfig getifaddr en1                                 # 169.254.x.x without a server
   cd /tmp && curl -O http://<the Mac's address>:8000/userland-update.tar -O http://<the Mac's address>:8000/apply-update.sh
   shasum -a 256 userland-update.tar                                                            # 69010daffa42b036...
   sudo sh apply-update.sh && sleep 15 && cat /private/var/db/piano-userland/live.log
   sudo sh /private/var/piano/userland/install.sh --check
   ```
   `live.log` lists each file `installed`, the base's three plugin files `kept as /private/var/db/piano-userland/prev/...`, the
   jobs loaded, `--live done: ok`; `--check` exits 0. `cat /private/var/log/piano-still-wallpaper.log`: `login picture written`,
   `replaced ours` or `kept the user's` per account (the stock default still picture; only our earlier choice is replaced).
5. Optional, the same Terminal (attended-desktop.md §3; each undone with `launchctl enable` and a restart):
   `sudo launchctl disable system/com.apple.PerfPowerServices` (1.7 cores at the idle login window), likewise
   `system/com.apple.watchdogd`, `system/com.apple.mobileactivationd`, `system/com.apple.security.xprotectd`.
6. Apple menu › Restart: past 180 s the guard has set the reason back to normal, so the installed v3 boots. WindowServer loads the
   new plugin: no boxes behind the clock, the still picture at the login window.

The layer's own rollback: `out/layer-none` the same way (its two files, `sudo sh apply-update.sh`), then a restart.

### 11.9 Roll back, least invasive first

1. A boot that hangs or panics: the boot guard lands in ABL fastboot within 180 s (later: macOS restarts); always: power +
   volume-down for 15-20 s. RAM images still win over the ESP (`ram-precedence`).
2. A bad v3, **before the layer**: `maint-boot map backup esp-final check maint-exit --esp esp-daily2-macos.img --esp-final
   esp-daily2-macos.img` (the old image, still in the persist checkout's `out/persistent`), or `maint-boot`, then `rollback
   esp-final` (the verified NAS copy), then `maint-exit`: a rollback is its own command line. Both collections resume the same
   store: the seed did not change.
3. **After the layer**: daily2's trust cache lacks the layer's CDHashes, so with the new plugin on Data its WindowServer would
   crash-loop. First the empty layer (§11.8, on v3 or `ram-v3-layer`), then step 2. Or, with the store backup: `maint-boot`,
   `rollback esp-final store-backup`, `maint-exit`, which puts the ESP and the owner's Data back to the moment of the backup
   together.

## 12. boot_a v5: the FD with DarwinBoot's 13 GiB window (built 2026-11-12; not signed, not installed)

The RAM images carry FD A built with `firmware/patches/darwinboot/0001` since 2026-11-12 18:40 (`tooling/firmware/build-fd.sh`), so
XNU gets 13310 MiB there; the installed `boot_a` (v4, `1b212281…`) carries the FD of 2026-11-11 (`6d15f0f8…`), whose DarwinBoot
refuses an ADT asking for more than 8192 MiB. The installed system gets the 13 GiB window with `boot_a` v5 **and** an ESP whose ADT
asks for it, in that order (§12.3). `boot_b` and every other partition stay untouched.

### 12.1 The image

```sh
python3 tooling/persist/build-boot.py --series 5 --fd out/fd/piano-uefi-A-bundled.fd \
        --same-kernel-as <checkout>/out/ram-desk-dev15/build-A-bundled.json --out out/persistent
tooling/persist/sign.sh out/persistent 5        # on the signing host (§12.2), then build-boot.py --verify --series 5
```

| | v4 (installed) | v5 |
|---|---|---|
| unsigned image | `605c3a60…`, 3301376 bytes | **`d1363bb16351281adb60e044944c344729ad7f53c667377725d7b4f38c135360`**, 3301376 bytes |
| boot v4 header (page 4096, os_version 15.0.0, patch level 2026-02, no ramdisk, empty command line) | | byte for byte v4's (first 4096 bytes equal) |
| BootShim (192 bytes, Linux Image header) | `7199075c…` | the same bytes |
| FD (3 MiB) | `6d15f0f8…`: unixnu/0001 | `ce719ffbe9998b0960e7f142a2bbd684046e24611f903e0cd5a11ba0f8d802af`: unixnu/0001 and darwinboot/0001 (both `build-fd.sh` stamps; bound `0x340000000`) |
| boot DTB | `50636eff…` | the same bytes |
| kernel field | `a1df7f14…` | `72bd1c28742090d64e0bcfc53cdc4878775de03ad7ee64c8b11b1eb689b30c59`: **the kernel field of the RAM images that ran on the tablet today** (desk-dev8n, -13, -14, -15: `--same-kernel-as` checks it) |
| signed | `1b212281…` (key sha1 `f5151930…`) | pending: 100663296 bytes, the same key |

The record `boot-v5.json` keeps the FD's bound (`fd.ram_window` = 13312 MiB); `maint-session.py` reads `boot.json` and every
`boot-v*.json` of its directory. `build-boot.py --verify` now records an image as signed only when every check passed (before, an
image that failed a check was still listed under `signed`, which step `boot-a` writes).

### 12.2 Signing: which key, where

- The AVB key is the RSA-4096 key that signed Ubuntu's `boot_a` (backup `13-boot_a`) and v4: public key sha1
  `f5151930dc69791637b2a48505cb175f5f78a522`. Its **private half exists only on the signing host** (named by `PIANO_SIGN_HOST` in
  `~/.config/piano/env`, never committed; the key's path there is `sign-on-remote.sh`'s default or `SIGN_KEY`). It is not on the
  Mac and not in `private/`, and it never leaves that host.
- `tooling/persist/sign.sh OUT 5` runs `tooling/bootimg/sign-on-remote.sh` (ssh/scp in BatchMode: key-based login, no password):
  it copies the pinned avbtool 1.3.0 and the unsigned image to a temporary directory there, extracts the public halves
  (`out/keys/piano-boot-signing.{avbpubkey,pub.pem}`, which come back), runs `add_hash_footer` (partition `boot`, 100663296 bytes,
  `SHA256_RSA4096`, rollback index 1769904000 and the three `com.android.build.boot.*` properties of the release's
  `boot-profile.json`) and `verify_image` there, brings the signed image back and removes its copies.
- Then, on the Mac, `build-boot.py --verify --series 5`: exactly 100663296 bytes, starts with the recorded unsigned image, the hash
  descriptor of partition `boot`, the algorithm, the rollback index, the properties, the public key sha1, and `avbtool
  verify_image` with the local public key. The same tool verifies the installed v4 (`1b212281…`) with the local public key: PASS.
- Rehearsed offline: the same `add_hash_footer` with a throwaway key on a copy of v5 gives the expected footer and passes every check
  but the two key checks, which fail; `boot-v5.json` then lists nothing as signed.

### 12.3 The pair rule (the order)

DarwinBoot is in `boot_a`'s FD; the RAM window it is asked for is in the ESP's ADT. Of the four pairs, one never boots:

| boot_a | ESP's ADT | Boots? | Evidence |
|---|---|---|---|
| v4 (8 GiB bound) | 8 GiB | yes | installed (2026-11-12 20:43: `RAM window 0x8c0004000..0xac0004000 (8192 MiB)`, store state 2, guard healthy) |
| v5 (13 GiB bound) | 8 GiB | yes | `ram-desk-dev8n` (new FD, 8 GiB ADT) ran 37 minutes on 2026-11-12 19:40 (`max_mem 8192 MiB`); `check-ram-window.py` |
| v5 | 13 GiB | yes | `ram-desk-dev14`/`-15` (new FD, 13 GiB ADT, KC with k27 0065): `RAM window 0x8c0004000..0xbffe04000 (13310 MiB)`; `check-ram-window.py` |
| v4 | 13 GiB | **no**: `DARWINBOOT: boot refused`, the menu redraws (the guard is not armed: XNU never starts) | `check-ram-window.py`: `PROBLEM: request 0x33fe00000 is past the FD's DarwinBoot bound 0x200000000` |

So: `boot-a` before `esp-final`; when undoing, `rollback esp-final` before `rollback boot-a`. Interrupted anywhere in between, the
tablet holds a bootable pair. `maint-session.py` enforces it against the tablet's bytes (not the state file): `esp-final` reads
`boot_a` and refuses an ESP its DarwinBoot does not take; `boot-a` and `rollback boot-a` read the ESP region and refuse an FD that
does not take its window; `rollback esp-final` checks the backed-up ESP against `boot_a`; `check` reports the pair. A dry run
counts a wrong order as a stop. The ESP's window comes from its record (`ram_window`, `build-esp.py` from now on), else its ADT
input, else the ADT read out of the image. `test-persist.py` runs both orders and both rollbacks on the fake tablet.

### 12.4 The ESP that goes with it

Any daily ESP built with the 13 GiB ADT (`out/adt/piano-k27.adt` of `PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh`,
`/chosen darwinboot-memory-size 0x33fe00000`) and `PIANO_BOOT_FD_RAM_WINDOW=1` (`build-esp.py`'s own gate: it states that `boot_a`
gets v5 first; the session checks it):

```sh
PIANO_BOOT_FD_RAM_WINDOW=1 python3 tooling/persist/build-esp.py --profile daily --default macos --boot-guard 180 \
        --layer <the layer-v3 directory> --charging 1 --rtc-offset 1794397906 --adt out/adt/piano-k27.adt [--kc KC] [--arg ...] \
        --name esp-<name>-macos --out out/persistent
python3 tooling/adt/check-ram-window.py --adt out/adt/piano-k27.adt --kc KC --fd out/fd/piano-uefi-A-bundled.fd   # 13310 MiB, exit 0
```

The KC must have the 13 GiB fixes: the k27 0065 kernel (`build-kc.log` names `kernel-k27-0065-native.macho`) and PianoA830's
contiguous limit (`bringup-debugging.md` §6). For the dry runs here: `esp-daily32-macos.img`
`a350e35944255d28c2e1d8ae9116aa265d8a555a66788db4ba705a2eb8e943ea`, the v3.1 ESP with only the ADT changed (its record differs from
v3.1's in the ADT's hash and paths alone: KC `ad1c5c45…` with k27 0065, trust `a83bbee6…`, seed `23a5ec59…`, identical `unixnu.toml`).

### 12.5 The session

Run this checkout's `maint-session.py` (the persist checkout's older one knows only `boot.json`) on the install state, as §11.7:

```sh
PERSIST=<persist checkout>/out/persistent; export PIANO_PERSIST_OUT=$PERSIST; P=tooling/persist/maint-session.py
B=boot-piano-macos-persistent-v5-signed.img; E=esp-<name>-macos.img
cp -c out/persistent/boot-piano-macos-persistent-v5.img out/persistent/$B out/persistent/boot-v5.json $PERSIST/
cp -c <where it was built>/out/persistent/${E%.img}.{img,json} $PERSIST/       # unless built there
python3 $P --dry-run maint-boot map backup store-backup boot-a esp-final check maint-exit --boot-image $B --esp $E --esp-final $E --new-backup
# ABL fastboot: from the running system python3 tooling/native/usb-console.py --send 'reset fastboot' --capture 5
python3 $P abl-check
python3 tooling/native/usb-console.py --capture 420 &      # the RAM test of v5's exact bytes (nothing written):
python3 $P ram-boot-a --boot-image $B                      # UEFI log "config: volume VenHw(860845C1-...)/HD(37,GPT,3F8B0988-..."
#   console: "RAM window ... (8192 MiB)" (the installed ESP's ADT with the new FD), "Data store state 2", guard healthy at 180 s
python3 tooling/native/usb-console.py --send 'reset fastboot' --capture 5
python3 $P maint-boot map backup store-backup boot-a esp-final check maint-exit --boot-image $B --esp $E --esp-final $E --new-backup
python3 $P abl-final reboot
```

- The dry run must end with `done (dry run: nothing was sent)` and no `would stop a real run` (before signing it names exactly one:
  the image is not signed yet).
- `--new-backup` starts a new NAS set: both GPT copies, `boot_a` (v4 `1b212281…`), `boot_b`, `vendor_boot_a`, `init_boot_a` (`boot_a`
  and the small ones also locally in `session/`), the ESP region as installed; each exported, checked against the tablet's sha256
  and read back from the NAS without the cache. `store-backup` (optional, 12.9 GB, about 7 + 7 minutes) is advised here: no boot has
  yet run the direct store (`data-persist=2`) with the 13 GiB window.
- `boot-a`: the pair check (v5 takes every window), `vendor_boot_a`/`init_boot_a` are slot A's, the image fetched by sha256, written
  by `dd`, read back whole (= the signed sha256), `ANDROID!`. `esp-final`: the pair check reads `boot_a` back from the tablet (= v5),
  then writes and reads back partition 37 through the partition and through LUN 0. `check`: everything else unchanged, the pair.
- `ram-boot-a` boots the installed system's normal path (its own Data) with the new FD; it is optional, but it is the one run of
  v5's ESP path (UniXNU finding partition 37) before `boot_a` holds it: every 13 GiB boot so far had its payload in RAM.

### 12.6 First boot

`python3 tooling/native/usb-console.py --capture 600` from `reboot`:
- `NativeRamBlock: RAM window 0x8c0004000..0xbffe04000 (13310 MiB) in DRAM 0x8c0000000..0xc00000000; max_mem 13310 MiB`
  (8192 MiB means the ESP's ADT is still the 8 GiB one);
- `NativeRamBlock: Data store state 2 (...), header 0, first LBA 68119455, 3145728 pages, direct 1, seed 8f333110, store seed
  8f333110, ...`;
- `piano-boot-guard: armed: ...`, then `piano-boot-guard: healthy after 180 s, restart reason normal`;
- `PianoDwc3: cold: verdict ...` and enumeration (the console itself); no `panic`, no `vm_page_unwire`.

**Stop** on `state 3`, `state 7`, `kept` or `started over` (§11.7). No console at all and the panel back at the UniXNU menu after a
`DARWINBOOT` line: the pair is wrong (cannot happen through the session; power + volume-down, then §12.7).

### 12.7 Roll back, least invasive first

1. A hang or panic in the first 180 s: the guard lands in ABL fastboot; always: power + volume-down 15-20 s. RAM images still win.
2. Both back, from ABL fastboot: `python3 $P maint-boot map`, then `python3 $P rollback esp-final boot-a` (a rollback is its own
   command line; this order: the ESP of before, then v4, each streamed from the new set and read back; `boot_a`'s copy is also
   local in `session/`), then `python3 $P map check maint-exit` (`map` again, so `check` compares with what is there now). The pair
   rule refuses `rollback boot-a` while the 13 GiB ESP is there.
3. Only the ESP back (v5 stays; it boots the 8 GiB ADT): `rollback esp-final`.
4. Copies kept for writing back by name: `$PERSIST/boot-piano-macos-persistent-v4-signed.img` (`1b212281…`), the earlier ESP images
   in `$PERSIST`, and the NAS sets of 2026-11-12 (each with `boot_a` = v4). Write by name with a new set: `maint-boot map backup
   esp-final boot-a check maint-exit --esp-final <8 GiB ESP> --boot-image boot-piano-macos-persistent-v4-signed.img --new-backup`
   (ESP first, as the rule demands).

### 12.8 Risks

- Signing needs the signing host (§12.2); until it signs, nothing can be installed (`boot-a` refuses an unsigned image).
- v5's ESP path has not run: the RAM boots that proved FD `ce719ffb…` all carried their payload in RAM (UniXNU's RAM disk first);
  the UniXNU in this FD is newer than v4's. `ram-boot-a` closes that gap without a write.
- The 13 GiB window with the direct store (`data-persist=2`) has not run (the 13 GiB boots were `data-persist=0`): `store-backup`,
  and a RAM test of the new ESP's payload with the read-only probe (`PIANO_DATA_PERSIST=1`, §11.6) before the install.
- If v5 never reaches XNU, the boot guard cannot act (it lives in XNU): power + volume-down needs the owner. `ram-boot-a` shows ABL
  takes v5 and the UEFI reaches the ESP.
- The FD build is not bit-reproducible: v5 is identified by hashes (`out/fd/piano-uefi-A-bundled.fd` = `ce719ffb…`; its `.prev` is
  v4's FD).

## 13. macOS 27.3 only: retiring 27.0.1 (installed 2026-11-14; the whole-disk layout of 13.8 installed the same day)

Owner decision 2026-11-14: (1) replace the installed dual ESP with a 27.3-only ESP; (2) only after that passes, back up and clear
`linux_rootfs`, which holds the 27.0.1 system volume and its Data store. `boot_a` (v5, DarwinBoot's 13 GiB window) stays as it is,
and nothing on `linux_data` is written: the 27.3 root from its block 0 and the 27.3 Data store at its end (LBA 117998394-124290042,
bound to that LBA). No GPT change in either step. The 27.3 line itself: [k273-line.md](k273-line.md) §7.

### 13.1 The image

```sh
python3 tooling/k273/build-k273-esp.py --base out/persistent/esp-dual33d-k273d.json --out out/persistent \
        --expect-seed 89860516 --expect-store-lba 117998394 --expect-store-part linux_data
```

`esp-k273-only.img`, 1073741824 bytes, sha256 **`9fe17e65015f8c16743bd1faf3c64e060003784a934c409261a021134a4dff3b`** (the same
bytes on every build from the same base), record `esp-k273-only.json` beside it:

| | |
|---|---|
| base | `esp-dual33d-k273d.img` (`93baea7a…`, installed since 2026-11-13 09:45), read by the independent FAT reader and by mtools, both checked against its record and against each other |
| payload, byte for byte | `Payloads/k273/kernel.macho` `3fd3ed71…`, `.adt` `7e1ce73d…` (13310 MiB window), `.trustcache` `72ebcaf8…`, `scratch-data.img` `848c8252…` (seed 89860516, 24 GiB) |
| firmware | what the 27.3 collection pins, the base's files: `PianoAudio/fs19xx.fsm` `ab060b84…`, `PianoVideo/vpu35_4v.mbn` `95c5882f…` |
| `macos-27.3` (default, timeout 3 s) | the base's entry, every key byte for byte; the line 989 bytes with `native-ufs-root-part=linux_data data-store-lba=117998394 data-persist=2` |
| `fastboot` | the 27.3 payload with the line minus the UFS root, the store, the console mirror and every `piano-*` driver, plus `data-persist=0 piano-reset-after=1 piano-reset-to=bootloader` (490 bytes) |
| `reset`, `shutdown`, `tools` | the standard actions |
| not there | the 27.0.1 payload (`Payloads/kernel.macho` and its three files), the `macos`, `macos-diag` entries; `macos-27.3-diag` (1037 bytes with the reset: over DarwinBoot's 1000) |

The builder takes the store facts from its inputs, nothing fixed: the partition and the LBA from the entry's line, the window's size
from the seed, and `common.check_store_size` checks that the LBA is NativeUfs's place for that seed; `--expect-*` only assert them.
It refuses a menu with any entry that uses linux_rootfs (`common.entry_uses_rootfs`: an entry that mounts a UFS root or opens a
store without `native-ufs-root-part` uses it as much as one that names it). After writing, the image is read back by both readers:
every 27.3 file and the line must be the base's. The record's `seed` and `store` are the 27.3 store's (`store.partition linux_data`,
`first_lba 117998394`, `blocks 6291649`, `bound`), which is what `maint-session.py` gates on (§13.3).

### 13.2 Its RAM test: `out/ram-k273-entry2` counts

No new RAM boot. `ram-k273-entry2` (`3fb5519b…`) carries exactly this entry: its collection, ADT, trust cache and seed hash to the
four payload files above, its 8-CPU line is the entry's line byte for byte (989 bytes), its firmware files are the same, and its FD
is v5's (`ce719ffb…`). And the installed dual ESP has booted this entry from these files as its default every day since 2026-11-13
09:45 (results-2026-11-13.md). What a RAM image cannot exercise is only what changes: the FAT layout and `unixnu.toml`, which UniXNU
reads from partition 37 alone. Those are covered by the host read-backs (two readers), the session's read-back of partition 37, and
the first boot, with `rollback esp-final` behind it. Running entry2 again would add nothing and would open the owner's 27.3 store
read-write (`data-persist=2`): leave it.

### 13.3 What maint-session.py checks for a 27.3-only record

A record whose `store` names its partition and first LBA (`esp_store`) is gated on that store, not on linux_rootfs's:
- `map` (with `--esp esp-k273-only.img`, else the ESP this install last wrote): linux_data's block 0 is the SHNGROOT manifest with
  the image + 1024 blocks below the window; the window lies inside linux_data as mapped and at NativeUfs's place for its size there;
  its header is valid, of the record's seed, bound to that LBA. The line a script gates on:
  `ok    the store at LBA 117998394 on linux_data (seed 89860516f973e537, generation G, bound to LBA 117998394) IS the one this ESP resumes (seed 89860516f973e537)`.
  linux_rootfs is only reported (`no entry of esp-k273-only.img uses it`; its block 0 as the 27.0.1 manifest or `zero (cleared)`).
- `esp-final` reads the same store again and refuses on any mismatch (bound elsewhere, unbound, another seed, blank, a window that
  is not NativeUfs's place); the pair rule as before (v5's DarwinBoot takes 13310 MiB).
- `check`: linux_data's manifest block as `map` read it, the store again.
- `rollback esp(-final)` restores the backed-up ESP and now writes `installed.json` for it (its record's seed); it refuses an ESP
  whose default entry boots from linux_rootfs while linux_rootfs holds no root image (after `rootfs-clear`: `rollback rootfs-clear`
  first).

### 13.4 Step 1: install the ESP (session A)

The install state is the main checkout's `out/persistent` (`installed.json`: `esp-dual33d-k273d.img`, `session/state.json`). Run
there, or from another checkout with `export PIANO_PERSIST_OUT=<main checkout>/out/persistent` and the image and record copied
into it (`cp -c out/persistent/esp-k273-only.{img,json} $PIANO_PERSIST_OUT/`). `PIANO_BACKUP_DIR` (the external SSD) mounted.

```sh
P=tooling/persist/maint-session.py; E=esp-k273-only.img
python3 $P --dry-run maint-boot map backup esp-final check maint-exit --esp $E --esp-final $E --new-backup
# ends "done (dry run: nothing was sent)", no "would stop a real run"
python3 tooling/native/usb-console.py --send 'reset fastboot' --capture 5        # the running 27.3 -> ABL fastboot
python3 $P abl-check
python3 $P maint-boot map backup esp-final check maint-exit --esp $E --esp-final $E --new-backup
python3 $P abl-final reboot
```

- `--new-backup`: a new set on the SSD with both GPT copies, the boot partitions and the ESP region as installed (the dual ESP,
  `93baea7a…`), each exported, its sha256 equal to the tablet's, read back without the cache. `rollback esp-final` restores the
  dual ESP from it.
- `map` must print the `ok ... IS the one this ESP resumes` line of §13.3. `esp-final` writes partition 37 and reads it back
  through `sda37` and through LUN 0 (= `9fe17e65…`); `check` then reads everything else as unchanged.
- Then set `PIANO_INSTALLED_SEED` in `~/.config/piano/env` to `89860516` (or `<checkout>/out/persistent/esp-k273-only.json`): it
  wins over `installed.json` and still names the 27.0.1 seed, which the image builders would otherwise compare against.

First boot (`python3 tooling/native/usb-console.py --capture 600`, started before `reboot`); the owner need not be there:
- `NativeUfs: ... root-part=linux_data`, the windows published, the root mounted;
- `NativeRamBlock: Data store state 2 (...), ... first LBA 117998394, 6291456 pages, ... seed 89860516, store seed 89860516, ...`;
- `piano-boot-guard: healthy after 180 s, restart reason normal`;
- the USB console and network up: `tooling/native/pianousb-shell` or `desk-ssh.sh` answers, `sw_vers` 27.3 26C5035r; the desktop on
  the webcam (`imagesnap`), login with `desk-login.sh`.

**Stop** on `state 3`, `state 7`, `kept` or `started over` (then `rollback esp-final`, §13.6).

Attended only, optional: picking `Reset into fastboot (ABL)` in the menu (3 s) lands in ABL fastboot within a few seconds.

### 13.5 Step 2: retire linux_rootfs (session B, after step 1 passed)

```sh
python3 $P --dry-run maint-boot map rootfs-backup rootfs-clear check maint-exit --esp $E
python3 tooling/native/usb-console.py --send 'reset fastboot' --capture 5
python3 $P maint-boot map rootfs-backup rootfs-clear check maint-exit --esp $E
python3 $P reboot
```

No `--new-backup` here: the rootfs backup joins session A's set, and that set stays the source of `rollback esp-final`.

`rootfs-backup` (read only on the tablet): the whole linux_rootfs as mapped (`sda35`, LBA 56060928-71527423, 63350767616 bytes) in
15 pieces of 4096 MiB into `$PIANO_BACKUP_DIR/<set>/linux_rootfs/piece-NNN.img.gz`. Compressed on the host; the checks stay exact
because all three are on the raw bytes: (1) the tablet's sha256 of the piece read from the medium (page cache dropped), (2) the
sha256 of the bytes as they arrive, taken before compression, (3) the stored file read back without the page cache and inflated,
its sha256 = (1). The sha256 of each stored file is recorded too (`SHA256SUMS`, `sha256sum -c` checks the set; `RAW-SHA256SUMS`
the inflated pieces and the whole partition), and after the last piece every piece is read back again for the whole partition's
sha256. For a slow USB link: each piece is a detached tablet job with keepalives and a time limit for at least 1 MiB/s, a stream
that stalls 300 s fails the piece, a piece is tried 3 times, and a stopped run resumes at the first piece not yet verified (run the
same command line again). The step checks the free space on the SSD first. At the 30 MB/s that `store-backup` measured (§11.7)
the transfer takes about 35 minutes, the hashes and the read-backs a few more; `rootfs-clear` hashes the 59 GiB on the tablet once
more (a few minutes). `--rootfs-piece-mib N` for smaller pieces, `--rootfs-compress none` to store them raw. It is read only, so it may also run in
session A (after `esp-final`) to save a maintenance boot: `rootfs-clear` hashes the partition again and refuses on any change.

`rootfs-clear` refuses unless:
1. the target is linux_rootfs as mapped: a partition of LUN 0, not the disk, not linux_data, not the ESP, overlapping neither;
2. the ESP on the tablet, read and hashed, is the image of a 27.3-only record in `out/persistent`, and its menu, read out of that
   image (which must hash to the record), has no entry that uses linux_rootfs;
3. a verified backup of exactly this partition (this session's set, or `--rootfs-backup-set NAME` for an earlier set) is on the SSD,
   every piece there with its recorded size and sha256 (read without the cache), and the partition read now hashes to the backup.

Then `blkdiscard` of `/dev/sda35` (BusyBox's applet; the maintenance image has no util-linux one), the node's PARTNAME, start, size
and mounts checked again by `tablet.sh` first. Zeros by `dd` instead when the disk takes no discards, or with `--rootfs-zero`.
Read back from the medium, these must be zero: the first and last 64 MiB and 64 MiB from each old store header place (LBA 68119455,
68381599); a device that keeps its data after a discard is zeroed and read again. The GPT copies, the ESP, linux_data's block 0
and the 27.3 store's header are hashed before and after: unchanged. Expected lines:
`ok    linux_rootfs read now = its backup (...)`, `WRITE discard /dev/sda35: all of linux_rootfs, LUN 0 LBA 56060928-71527423 (63350767616 bytes)`,
`ok    linux_rootfs reads zero from the medium: LBA 56060928+16384, LBA 68119455+16384, LBA 68381599+16384, LBA 71511040+16384`,
`ok    GPT head unchanged ...`; `check` then reads the cleared block 0 and the 27.3 store as expected. The next boot is the normal
27.3 boot of §13.4 (nothing it reads was touched).

### 13.6 Roll back, least invasive first

1. A hang or panic: the boot guard lands in ABL fastboot within 180 s; always: power + volume-down 15-20 s. RAM images still win.
2. Step 1, from ABL fastboot: `python3 $P maint-boot map`, then `python3 $P rollback esp-final` (the dual ESP from session A's set,
   read back; `installed.json` names it again), then `python3 $P map check maint-exit --esp esp-dual33d-k273d.img`. Its default is
   `macos-27.3` too, so the owner's 27.3 Data is resumed either way. Put `PIANO_INSTALLED_SEED` back if you changed it.
3. Step 2 (`rootfs-backup` writes nothing on the tablet: nothing to undo; the set stays on the SSD until the owner deletes it).
   After `rootfs-clear`: `python3 $P maint-boot map`, `python3 $P rollback rootfs-clear` (`--rootfs-backup-set NAME` for an earlier
   set): each piece served inflated, written by `dd`, read back = its sha256, a piece already right is skipped, so a stopped
   rollback resumes; then the whole partition = the backup's sha256), then `python3 $P map check maint-exit --esp $E`.
4. Both: `rollback rootfs-clear` first, then `rollback esp-final` (which refuses an ESP whose default boots from linux_rootfs while
   it is cleared). The dual ESP's 27.0.1 entries boot only once linux_rootfs is back.

### 13.7 Afterwards

linux_rootfs (LUN 0 LBA 56060928-71527423, 59 GiB) is free: discarded, its GPT entry unchanged, used by no entry. It can be taken for
27.3 (a larger root or Data, with linux_data's unused middle) by the repartition designed separately; nothing here fixes its role
or size: the steps take every partition from the map, the 27.3-only builder takes the store window from its base's line, and
`maint-session.py` checks the store where the record says it is.

Host tests (no tablet): `tooling/persist/test-k273-only.py` makes every fixture in a temporary directory (a dual ESP by mtools, a
GPT, the fake tablet of `test-persist.py`) and covers the builder, `tablet.sh`'s guard, the gate, both rootfs steps with their
refusals, a broken and a stopped transfer, a device that keeps data after a discard, and the rollbacks back to the first byte.

### 13.8 The whole-disk layout: the root on linux_rootfs, the owner's Data grown over linux_data (2026-11-14)

The ESP `esp-k273-v2.img` (`build-k273-esp.py --base esp-k273-only.json --kc/--adt out/k273-v2/kernel.macho* --trust <layer trust>
--seed seed-200.img --root-part linux_rootfs --arg piano-power-ps=1 --split`) mounts the 27.3 root from linux_rootfs and puts the
Data store on all of linux_data (`data-store-part=linux_data`, `data-store-lba=71859642`, 52430401 blocks). Its seed is the installed
24 GiB seed with zeros appended to 200 GiB (`compact-seed.py --grow-gib 200 --disk-only`: the same 276 extents), so the owner's store
is carried over instead of starting a fresh Data:

1. `k273-store-backup`: only what differs from the seed we flashed (owner, 2026-11-14): the installed store's header and presence
   bitmap (LBA 117998394, 193 blocks) with the three checks, then its present pages (about 534551, joined across gaps of up to 32
   pages; `tablet.sh send-list`) as one stream whose tablet-side SHA-256 = the received = the stored copy re-read without cache, and
   the runs list (in SHA256SUMS). A clear bit reads the seed's page, which the old ESP image holds. Rollback: `restore-list` (one
   HTTP range per run), read back, then the header and bitmap.
2. `linux-data-root --root-part linux_rootfs`: the root image onto the cleared linux_rootfs, read back (only after `rootfs-clear`).
3. `k273-store-grow`: refused unless the new seed is the old one grown (the extents compared in both ESP images), the windows do not
   meet, the old header is the installed seed's and bound to its LBA, and the old store's backup is verified. The old bitmap comes to
   the host; each run of present pages is copied on the tablet (`tablet.sh copy-list`, page i stays page i), the runs read back
   equal (`hash-list`), then the new bitmap (the old one, then zeros), then the header last (the new seed, page count and LBA, the
   old generation). Until the header lands, the new window's block 0 is no store, so a cut leaves the old layout whole.
4. `esp-final` writes v2 only when that store is valid and bound to LBA 71859642 (the same gate as 13.3).

Session (from ABL fastboot): `python3 $P maint-boot map backup k273-store-backup linux-data-root k273-store-grow esp-final check
maint-exit --esp esp-k273-v2.img --esp-final esp-k273-v2.img --root-part linux_rootfs --root-image <root.img> --root-sha256 <hex>
--new-backup`. Its RAM test is `out/ram-k273-v2ro`: the v2 line with `data-persist=1`, i.e. the grown store opened read-only
(state 6) and Data volatile over it. It must show the owner's Data, the root on linux_rootfs and the battery. On the running system:
`diskutil apfs resizeContainer <the Data container> 0` (after `diskutil repairDisk` if the GPT's backup header is not at the end).

Rollback: `rollback esp-final` (esp-k273-only), `rollback k273-store-grow --root-image <the old root> --root-sha256 <hex>` (the root
back onto linux_data; the old store window was only read), then `map check maint-exit --esp esp-k273-only.img`.

Found by the host test of this step: `build-k273-esp.py` wrote the base's payload over the given `--kc/--adt/--trust/--seed` and
over the ADT tail (the first esp-k273-v2, 573187af..., had the new line over the old payload). Fixed: the image is checked to
hold the given files and the tail. `test-k273-only.py` covers the whole session on the fake tablet (refusals, the copy, the
rollback).

## Done 2026-11-11 (sessions 1-3)

- Session 1 (07:5x): NAS backups with read-back (boot_a 389d4271, boot_b 17e89056, vendor_boot_a 50f70e36, init_boot_a bbebc3d1,
  both LUN 0 GPT copies, the 1 GiB linux_rootfs tail, the store header); new GPT written and read back (linux_rootfs ends at
  71527423, partition 37 esp at 71527424 + 262144, sgdisk clean); esp-base-diag written and read back.
- Session 2 (RAM only): ram-esp-test (UniXNU's config from HD(37) on LUN 0, XNU to its reset, Data store at LBA 68119455),
  ram-precedence (a dev RAM image still boots its own payload; judged by its 45 s reset), ram-v4-signed (ABL accepts the signed v4
  boot_a image, which loads the ESP).
- Signing host: the signing helper runs on the host named by PIANO_SIGN_HOST in ~/.config/piano/env (never committed); the key
  there is the one boot_a was signed with (public key sha1 f5151930...).
- Session 3 (09:0x): boot_a = boot-piano-macos-persistent-v4-signed (1b212281...) written by dd and read back; ESP = esp-gpu-macos
  (1120f7d8..., PianoA830 + Turnip, vmm-present 0) read back; vendor_boot_a, init_boot_a, boot_b, root manifest, GPT unchanged.
- `fastboot reboot` (09:05): the tablet boots by itself into native macOS: the login screen on the A830 at about 2.5 min.

The installed configuration has no piano-reset-after and no USB console: to get back to ABL fastboot for RAM boots, hold
power + volume-down. Rollback: `maint-session.py rollback boot-a|esp-final|gpt` from the verified backups, or RAM-boot any image.
