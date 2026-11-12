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
   jobs loaded, `--live done: ok`; `--check` exits 0. `cat /private/var/log/piano-still-wallpaper.log`: `login picture written`
   or `kept` per account.
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
