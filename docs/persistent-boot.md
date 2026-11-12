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

## 11. ESP v3: tonight's fixes, the owner's Data kept (planned 2026-11-12; not installed)

Built offline in an agent worktree, **not installed and not run**. It replaces the installed `esp-daily2-macos` (`e0acb375…`). It
keeps the store through the store's base ([data-store.md](data-store.md) §9). Every step below follows §4-§5: verified NAS backup
first, SHA-256 read-back after each write, the RAM-boot rollback. It writes only partition 37.

### 11.1 Contents

| Payload file | Installed (`esp-daily2-macos`) | v3 (`esp-daily3-macos`) |
|---|---|---|
| `kernel.macho` (KC) | `3850545b…`: kernel without 0060 and 0063 (no RTC calendar, no boot guard) | `557420c4…`: k27 **0064** (0063's boot guard, 0060's RTC, 0061/0062/0064 opt-in), the kext list of this tree: PianoA830's BoMap fixes (524288 VM ranges, a pool of a quarter of RAM up to 2 GiB, refusal reasons), NativeGopDisplay's cursor layer and copy fence, PianoTouch's Y inversion and watchdog recovery (inert without `piano-touch`), NativeRamBlock's volatile pool, PianoDwc3's cold start (`piano-usb-cold=auto`), PianoPolicy (inert) |
| `kernel.macho.adt` | `139af652…`, vmm-present 0 | `1532b37e…`, vmm-present 0 (`PIANO_VMM_PRESENT=0`) |
| `scratch-data.img` (seed) | `23a5ec59…`, identity `8f333110…` | **the same file**: the pinned base |
| `kernel.macho.trustcache` | `cd4aa536…`, 5 modules | `607c60ef…`: the same 5 modules plus 1 first, the layer's new CDHashes (Turnip with 0006, `pianousb-shell`, `piano-rotated`, `piano-torch`) |
| `unixnu.toml` (`macos`) | the daily profile, `data-persist=2`, `data-store-replace=0xaced5bf9` (inert since that store is gone) | the daily profile (`piano-gpu=2 piano-gpu-delay=20 panic_on_crash_disabled=1 piano-power=1 piano-usb=2 piano-usb-hid=1`), **`piano-boot-guard=180`**, `data-persist=2`, no `data-store-replace` |

Opt-in drivers stay out until a RAM test passes on the tablet (`USING.md` §2): touch (`piano-touch`), the RTC calendar
(`--rtc-offset`), the USB network (`piano-usb-net=1`, not run on hardware yet), suspend (0061/0062/0064).

How the store is kept: the seed is the store's base byte for byte, so NativeRamBlock finds its own seed in the header and resumes
(state 2), with the old collection as with this one. The record says `data_store.same_seed: true`. The trust cache keeps every
CDHash of the installed one (`data_store.trust.missing_cdhashes: 0`; build-esp.py refuses otherwise), so the base's Turnip plugin on
Data loads as today. Until the layer is in (§11.6), Data keeps that plugin, without 0006.

### 11.2 Build (Mac, offline, no device)

```sh
K=$(sed -n 's/^PIANO_K27_TREE=//p' ~/.config/piano/env)
K27_KERNEL=$K/out/k27/kernel-k27-0064-native.macho PIANO_VMM_PRESENT=0 tooling/k27/build-native-image.sh   # KC, ADT, Turnip, out/ram
python3 tooling/persist/base-seed.py pin --esp <persist checkout>/out/persistent/esp-daily2-macos.img     # once; then: show
python3 tooling/userland/stage.py --live out/layer-v3 --gpu-bundle out/turnip-a830/NativeAdrenoMetal.bundle \
        --installed-trust "$(python3 tooling/persist/base-seed.py path trust)"
python3 tooling/persist/build-esp.py --profile daily --default macos --boot-guard 180 --layer out/layer-v3 \
        --charging 1 --name esp-daily3-macos --out out/persistent
```

`--charging 1` keeps v3 at PianoPower level 1, as planned; the daily profile's default became level 2 on 2026-11-12
(`docs/drivers/charging-policy.md` §16): drop the option once `ram-chg-ramp` has passed on the tablet.

`build-esp.py` takes the KC and ADT from this checkout's `out/` and the seed from the pinned base, and the trust cache from the
layer. The layer is `out/layer-v3/userland-update.tar` (`5f48c125…`, 12 files, 6 units with `gpu-nativeadrenometal`) with its
trust cache `607c60ef…` (the base's `cd4aa536…` plus 4 CDHashes). Built 2026-11-12 (`--utc 1794427214`): `esp-daily3-macos.img` sha256 **`f6f7bb53658b3fd621d6dc5563bae6b1da2884613eb1b99a5767cbb484105309`**, seed identity `8f3331104e03c970`. The clock
starts from `--utc` (no RTC in the profile): rebuild with the same command just before the install. Only `unixnu.toml` and the
image hash change.

### 11.3 Verify (offline)

1. The record `out/persistent/esp-daily3-macos.json` has:
   - `seed.identity_sha256` `8f3331104e03c970…` and `inputs.seed.sha256` `23a5ec59…`;
   - `data_store.same_seed` true, `data_store.base.identity_sha256` the same, `data_store.trust.missing_cdhashes` 0;
   - `boot_guard` 180 and `kc_has_boot_guard` true;
   - `kc_natives` with PianoA830, PianoPower and PianoDwc3, and no NativeSoftGpu.
2. `python3 tooling/persist/base-seed.py show`: the base's files are still the pinned ones.
3. `python3 tooling/k27/check-native-kexts.py` (the native kext rule; build-esp.py also refuses VM-era bundles).
4. `python3 tooling/persist/test-data-store.py`, `python3 tooling/userland/test-stage.py`, `python3 tooling/metal/check-bundle-paths.py --self-test`.
5. The session as a dry run: no command reaches the tablet, ABL or the NAS (§11.5, with `--dry-run` before the steps).

### 11.4 RAM test of the exact payload (nothing written)

The same KC, ADT, trust cache and seed as a RAM image, `data-persist=1` (read-only probe: the store is reported, not applied, and
Data is the base, volatile). It resets into ABL fastboot after 600 s:

```sh
PIANO_SEED=$(python3 tooling/persist/base-seed.py path) PIANO_TRUST=out/layer-v3/kernel.macho.trustcache PIANO_DATA_PERSIST=1 \
  PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=600 PIANO_A830_METAL=0 \
  PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-power=1 piano-usb=2 piano-usb-hid=1 piano-boot-guard=180" \
  K27_KERNEL=$K/out/k27/kernel-k27-0064-native.macho tooling/k27/build-native-image.sh && cp -R out/ram out/ram-v3-probe
tooling/native/native-loop.sh out/ram-v3-probe/boot-piano-macos-ram-A-bundled.img     # with tooling/native/usb-console.py --capture 600
```

(`PIANO_A830_METAL=0`: no restage; the image takes `PIANO_SEED`/`PIANO_TRUST`.) Expected:
- `NativeRamBlock: read-only probe: the store's P pages are not applied to a large disk`, then `Data store state 6 … seed
  8f333110, store seed 8f333110`;
- `piano-boot-guard: armed`, then `healthy`, and the reset deferred to `piano-reset-after`;
- the login window on the A830 with the base's plugin;
- no `BoMap … NoResources` while using the desktop.

### 11.5 Install (cable session)

From the persist worktree, the checkout whose `out/persistent/session/state.json` installed daily2 (`USING.md` §5; steps before
options):

```sh
cp -c <this checkout>/out/persistent/esp-daily3-macos.img <this checkout>/out/persistent/esp-daily3-macos.json out/persistent/
P=tooling/persist/maint-session.py
python3 $P --dry-run maint-boot map backup esp-final check maint-exit --esp esp-daily3-macos.img --esp-final esp-daily3-macos.img
# tablet in ABL fastboot: power + volume-down (daily2's USB console does not enumerate on the normal boot path)
python3 $P abl-check
python3 $P maint-boot map backup esp-final check maint-exit --esp esp-daily3-macos.img --esp-final esp-daily3-macos.img --new-backup
python3 $P abl-final reboot
```

- `--new-backup` starts a new NAS set: the ESP region as it is now (daily2) is exported and verified (sha256 against the
  tablet's, the NAS copy re-read without cache), with both GPT copies and the boot partitions. `rollback esp` then restores daily2
  byte for byte.
- `map` reads the store header at LBA 68119455 and says whether it is the one this ESP resumes: `ok    the store at LBA 68119455
  (seed 8f3331104e03c970, …) IS the one this ESP resumes`. `esp` and `esp-final` refuse an ESP whose seed is not that store's,
  unless its build record says `--replace-data`. Its boots would run the owner's Data volatile.
  - Its older line `Data store is NOT usable with the ESP's seed … --store auto will let it start fresh` is about the store's place of
    before the install (LBA 68381599). Only the `store` step acts on it, and this session does not run that step.
  - A dry run reads no header and prints only that older line.
- `esp-final` writes partition 37 and reads it back through the partition and through LUN 0 (= the record's sha256).
- `check`, then `maint-exit`. Not written: `boot_a`, the GPT, the store, every other partition.

First boot (`tooling/native/usb-console.py --capture 600`, if PianoDwc3's cold start enumerates; else the next RAM dump):
- `NativeRamBlock: Data store state 2 (…), header 0, first LBA 68119455, 3145728 pages, direct 1, seed 8f333110, store seed
  8f333110, flags 0, generation G, present P`;
- `piano-boot-guard: … healthy`.

**Stop** on `state 3`, `state 7`, `kept` or `started over`.

Roll back, least invasive first:
1. A boot that hangs: the boot guard lands in ABL fastboot within 180 s, or power + volume-down.
2. RAM images still win (`ram-precedence`).
3. `maint-boot map backup esp-final check maint-exit --esp esp-daily2-macos.img --esp-final esp-daily2-macos.img` (the old image,
   still in `out/persistent`),
   or `rollback esp` (the verified NAS copy of daily2).

Both collections resume the same store: the seed did not change.

### 11.6 Then: the system layer (owner present, once)

The plugin with 0006 and our userland go onto the owner's Data as files ([data-store.md](data-store.md) §9, `userland.md` §7):
`out/layer-v3/userland-update.tar` and `apply-update.sh` (sha256 in the script). v3 already trusts their CDHashes. The base's Data
has no root shell and no network job, so the first time needs the owner and a transport:
1. RAM-boot the v3 payload plus `piano-usb-net=1` with `PIANO_DATA_PERSIST=2`: §11.4's command with `PIANO_DATA_PERSIST=2`,
   `piano-usb-net=1` added and no `PIANO_RESET_AFTER`. It is the owner's own Data, resumed: the seed is the base.
2. The owner logs in. In Terminal: `curl` the two files from the Mac over the link (`userland.md` §7.3), then
   `sudo sh apply-update.sh` with the owner's password (`piano-input.py type --secret`).
3. Check `install.sh --check` and `/private/var/db/piano-userland/live.log`.
4. Reboot into the installed v3: WindowServer loads the 0006 plugin.

The installer keeps the base's three plugin files in `/private/var/db/piano-userland/prev/`. A rollback (`userland.md` §7.4 step 4)
puts them back.

### 11.7 Decisions for the owner

- **The layer's first installation** (§11.6) needs the owner at the keyboard once, and the USB network's first hardware run. Until
  then the 0006 fix is not active.
- **`piano-usb-shell=1` in a later ESP** gives later layer updates without the owner, but opens an unauthenticated root shell on
  the cable.
- **A NAS copy of the store region** (13.96 GB, about 16 minutes) before v3's first boot. v3 changes no store code on the direct
  path, but its NativeRamBlock is a newer build. `maint-session.py` has no standalone store backup step (data-store.md T4).
- **Opt-in drivers** (§11.1) join a later ESP after their RAM tests.

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
