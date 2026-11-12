# Space in the native kernel collection (2026-11-11)

The native kexts live in the holes Apple's kept kexts leave in the 26A434 kernel collection, and those holes are few and fixed:
the largest `__DATA` hole was 64 KiB (PianoDpu's 72 KiB `__DATA` was refused once and fixed by moving a table to the heap), and
with Apple's USB host stack kept (`PIANO_USBHOST_STACK=1`) PianoTouch's 485 KiB `__DATA_CONST` did not fit and had to be left out.
This page measures the space (§2-4), changes the KC builder's placement (§5), adds a run-time firmware loader so that firmware
images leave the collection (§6), and says what each driver must do (§7). Totals: §8. Risks: §9.

## 1. How the builder places a native kext

The k27 tree's KC builder (`k27/kc-build27-piano.py` over `scripts/kc-build.py` and `kc_native.py`, read by
`tooling/kc/build-kc-k27.sh`, never copied here) keeps every kept Apple kext at its 26A434 address and the kernel at
`k27/segaddrs.sh`'s, then gives each native kext's four segments a place in one region per kind (offsets from the collection base):

| Kind | Region | From | To |
| --- | --- | --- | --- |
| `__TEXT` | `0x4000`-`0x810000` (8240 KiB) | after the collection header | the end of the collection's `__PRELINK_TEXT` |
| `__TEXT_EXEC` | `0xf88000`-`0x2d6c000` (30608 KiB) | the collection's `__TEXT_EXEC` | the kernel's `__TEXT_EXEC` |
| `__DATA_CONST` | `0x9f4000`-`0xf88000` (5712 KiB) | the end of the kernel's `__LASTDATA_CONST` | the end of the collection's `__DATA_CONST` |
| `__DATA` | `0x3ca4000`-`0x3e00000` (1392 KiB) | the kernel's `__PRELINK_DATA` | the end of the collection's `__DATA` (then `__LINKEDIT`) |

Unpatched, a segment goes to the first hole above the region's start that holds it (16-byte aligned), kext by kext in link order
(`configs/k27-native-kexts.txt`). A zerofill section (`__bss`, `__common`) takes collection space like any other: the collection is
one flat image.

## 2. The measuring tool: `tooling/k27/kc-space.py`

```sh
python3 tooling/k27/kc-space.py out/k27/kc-k27-native.bin              # per kext, per region, the gaps, the largest symbols
python3 tooling/k27/kc-space.py out/k27/kc-k27-native.bin --simulate   # + the placement replayed, first fit and largest first
python3 tooling/k27/kc-space.py KC --simulate --drop PianoTouch --add PianoKeyboard:DATA_CONST=87K,TEXT=14K,TEXT_EXEC=44K,DATA=1K
python3 tooling/k27/kc-space.py KC --fit DATA=72K                     # would one more 72 KiB __DATA fit now
python3 tooling/k27/kc-space.py KC --brief                            # one line; tooling/kc/build-kc-k27.sh prints it after each link
```

Everything is read back from the built collection (its fileset entries' load commands) and `KC.natives.json` (the natives'
external symbols): no k27 tree is needed. `--simulate` first replays the builder's first fit from the fixed segments (kernel +
kept kexts) and the natives' sizes and checks that it gives the build's own addresses (it does, for every collection measured
here), so its largest-first numbers are what the builder would do. `--json OUT` writes it all.

## 3. Main's collection today (471197c, k27 0059 kernel, 21 native kexts)

`out/k27/kc-k27-native.bin` SHA-256 `6ac75781...8ac6` (rebuilt in a worktree from main: the same bytes).

| Native kext | `__TEXT` | `__TEXT_EXEC` | `__DATA_CONST` | `__DATA` | KiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| ARMPlatformStandIn | 4.1 | 1.6 | 5.0 | 0.3 | 11.0 |
| NativeUfs | 12.9 | 52.7 | 5.6 | **33.3** | 104.6 |
| NativeRamBlock | 35.3 | 22.2 | 3.0 | 0.5 | 61.0 |
| NativeGopDisplay | 11.1 | 27.0 | 6.8 | 0.5 | 45.5 |
| PianoA830 | 26.2 | 456.9 | **256.7** | **102.7** | 842.4 |
| PianoBacklight | 9.7 | 22.0 | 2.7 | 0.8 | 35.1 |
| PianoDpu | 27.8 | 85.6 | 3.3 | 10.1 | 126.8 |
| PianoDwc3 | 17.0 | 64.1 | 9.7 | 2.5 | 93.2 |
| PianoInput | 6.3 | 9.9 | 5.6 | 0.4 | 22.2 |
| PianoNullEthernet | 4.3 | 2.0 | 3.5 | 0.2 | 10.1 |
| PianoPower | 26.1 | 126.5 | 6.8 | **16.0** | 175.5 |
| PianoTorch | 8.7 | 18.0 | 2.9 | 1.9 | 31.5 |
| PianoThermal | 13.3 | 62.0 | 4.2 | 2.7 | 82.1 |
| PianoTouch | 13.9 | 59.1 | **485.7** | 0.8 | 559.4 |
| PianoWifi | 24.0 | 80.9 | 4.9 | 8.8 | 118.5 |
| PianoAudio | 11.1 | 71.9 | 9.7 | 9.3 | 102.0 |
| PianoUsbHost | 18.8 | 51.8 | 5.3 | 1.6 | 77.5 |
| PianoVideo | 9.4 | 10.0 | 2.6 | 1.0 | 23.0 |
| PianoSensors | 11.0 | 69.8 | 4.5 | 1.2 | 86.6 |
| PianoBluetooth | 8.6 | 11.0 | 2.7 | 2.2 | 24.5 |
| PianoPolicy | 8.4 | 13.2 | 2.6 | 1.1 | 25.3 |
| **all natives** | 308.1 | 1318.2 | 833.7 | 197.8 | 2657.8 |
| kept Apple kexts (48) | 2696.1 | 8586.1 | 1687.7 | 857.0 | |

PianoKeyboard (not in the list then; linked with `PIANO_EXTRA_KEXTS`; in the list since 2026-11-12, after PianoInput):
13.4 / 44.0 / **87.3** / 1.0 KiB. A later main build with it (22
kexts) left `__DATA_CONST` 3102.9 KiB free (largest 565.7) and `__DATA` 333.3 KiB (largest 63.9).

| Region | Free | Gaps | Largest gap | Largest native segment |
| --- | ---: | ---: | ---: | --- |
| `__TEXT` | 4723.5 KiB | 30 | 954.0 KiB | 35.3 KiB NativeRamBlock |
| `__TEXT_EXEC` | 20703.5 KiB | 31 | 2931.8 KiB | 456.9 KiB PianoA830 |
| `__DATA_CONST` | 3190.5 KiB | 29 | 565.7 KiB | 485.7 KiB PianoTouch |
| `__DATA` | **336.9 KiB** | 29 | **63.9 KiB** | 102.7 KiB PianoA830 |

What fills them:

- `__DATA_CONST`: 720 of the natives' 834 KiB are firmware images embedded with `.incbin`: PianoTouch's two NT36532 panel images
  (`_piano_touch_firmware_boe`, `_csot`, 240.0 KiB each: 480.1 KiB with the size words) and PianoA830's four gen80000 images
  (`_native_adreno_sqe_firmware_image` 108.9, `_firmware_image` (GMU) 86.8, `_aqe_` 24.5, `_zap_` 11.8 KiB: 242.1 KiB with their
  page padding); PianoKeyboard adds its 81.5 KiB MCU image when linked. PianoTouch's calibration is under 1 KiB of constants.
  PianoA830's `.p2align 12` buys nothing: a native segment is only 16-byte aligned, so its images sit at `...800` addresses; the
  padding (about 10 KiB) is waste.
- `__DATA`: zerofill buffers: PianoA830 `__bss` 85.9 + `__common` 15.7 KiB, NativeUfs `__bss` 32.2, PianoPower `__bss` 15.2,
  PianoAudio `__bss` 8.9, PianoWifi `__bss` 6.5, PianoDpu `__data` 6.3 KiB. No firmware: the loader of §6 does not help `__DATA`.
- `__TEXT` and `__TEXT_EXEC` have megabytes left.

## 4. With Apple's USB host stack (`PIANO_USBHOST_STACK=1`)

Measured on scratch builds (the same objects; ARMPlatformStandIn with its cable keys, PianoUsbHostStandIn, the codeless
PianoUsbHostStack; 17 more Apple kexts kept at their addresses):

| Build | `__DATA_CONST` free / largest | `__DATA` free / largest | Result |
| --- | ---: | ---: | --- |
| first fit, PianoTouch left out (today's default) | 3087.0 / 470.2 KiB | 317.8 / 63.9 KiB | links |
| first fit, with PianoTouch | | | `PianoTouch: no room for its 0x796a0-byte native __DATA_CONST (largest gap 0x758a0)` |
| best fit (0001), with PianoTouch | 2601.5 / 470.2 KiB | 317.4 / 74.0 KiB | links (all 23; `check-usbhost-stack.py` and the native kext rule pass) |
| best fit + const to `__TEXT` (0001 + 0002), with PianoTouch | 3323.6 / 545.4 KiB | 317.4 / 74.0 KiB | links |

`kc-space.py --simulate --add PianoTouch:...` on the first build predicts both outcomes (first fit refuses at 470.2 KiB, best fit
places it) exactly as the builds then did.

## 5. The builder: `patches/k27-kc/`

Two patches to the k27 tree's builder, applied by hand (`patches/k27-kc/README.md`: `git -C "$PIANO_K27_TREE" apply ...`), each
inert unless its switch is set (with neither, the patched builder writes main's collection byte for byte: `6ac75781...`).

- **0001** (`scripts/kc-build.py`), `KC_NATIVE_PLACEMENT=best-fit`: a dry link (`kc_native.link` with an allocator that only
  records the four sizes and stops) sizes every native kext first; then, per kind, the segments are placed largest first, each into
  the smallest hole that holds it. The large segments get the large holes before small ones split them. The log ends the native
  link with one `native placement (...)` line per kind (largest gap, free, gaps), and a refusal now names the kext.
  `tooling/kc/build-kc-k27.sh` sets `KC_NATIVE_PLACEMENT=best-fit` (and notes when the tree lacks the patch), and
  `PIANO_USBHOST_STACK=1 tooling/k27/build-native-image.sh` no longer leaves PianoTouch out when the patch is there.
- **0002** (`scripts/kc_native.py`), `KC_NATIVE_CONST_TO_TEXT=1` (off unless set): an object's `__DATA,__const` /
  `__DATA_CONST,__const` section without relocations (an `.incbin` image, a table of numbers) goes to the kext's `__TEXT,__const`.
  It needs no chained fixup and stays read only; only the blobs move (main's natives' `__DATA_CONST` 833.7 -> 111.6 KiB).

Gains on main's collection (the same 21 kexts):

| | `__TEXT` free / largest | `__TEXT_EXEC` largest | `__DATA_CONST` free / largest | `__DATA` largest |
| --- | ---: | ---: | ---: | ---: |
| first fit (today) | 4723.5 / 954.0 | 2931.8 | 3190.5 / 565.7 | 63.9 |
| 0001 best fit | 4723.5 / 954.0 | **3791.1** | 3190.5 / 565.7 | **81.9** (+18.0 KiB, +28 %) |
| 0001 + 0002 | 4002.8 / 954.0 | 3791.1 | **3912.6** / 565.7 | 81.9 |

Again on main at fc86525 (its newer PianoA830 and PianoAudio): first fit `__DATA` 326.6 KiB free, largest 63.3 KiB; best fit largest
81.9 KiB; 0002 `__DATA_CONST` free 3187.9 -> 3910.0 KiB. At 586e2f6 (22 kexts: PianoDp added, PianoBluetooth's levels 2-4 with a
19.3 KiB `__bss`): `__DATA` 302.2 KiB free, largest 63.3 KiB first fit, 81.9 KiB best fit; `__DATA_CONST` 3182.8 KiB free.

Placement cannot change how much is free, only how it is cut: the gain is the largest hole, which decides whether the next big
segment fits (PianoDpu's 72 KiB `__DATA` now would; PianoTouch with the USB stack does).

## 6. The run-time firmware loader

`package/include/PianoFirmware.h` (the API and the pin), `package/common/PianoFirmwareCore.{h,c}` (portable: pins, the check, a
file from a FAT volume, an addon entry), `package/common/PianoFirmwareZlib.c` (the addon's zlib streams with the loader's own
allocator), `package/common/PianoFirmware.cpp` (the kernel side), with `package/PianoPower/FatVolume.c` (the reader PianoPower,
PianoWifi and PianoSensors already use). Each kext that uses it compiles these files in (no kext depends on another for it).

**The pin.** A kext carries, instead of an image, a `PianoFirmwarePin` (136 bytes, no pointers, so in its `__TEXT`): magic
`PianoFw1`, the path below `EFI/UniXNU/firmware/` (`<Kext>/<file>`), the size and the SHA-256. `tooling/persist/firmware.py stage`
writes the pins (`PIANO_FW_PIN(...)` lines) from the build script. A file pinned in committed code (`FILE=BYTES:SHA256`: the vendor
images, whose pins are committed today) keeps its pin whether or not the build host has the file, so the collection no longer
depends on which firmware the host holds; a file pinned as it is (this unit's own data) is private: no tool prints or records its
hash. The pins stay in the collection (`used`), so the collection itself says which files its kexts read: the ESP and RAM image
builders find them by their magic (`firmware.py pins KC`) and place exactly those files, checked, and nothing else.

**At run time** `piano_fw_load(pin, embedded, embedded_size, wait_s, &fw)` returns `pin->size` bytes whose size and SHA-256 match
the pin, from the first source that has them (each failure is noted in `fw.trail` for the kext's log, and the loader logs one line):

1. **built in**: the embedded bytes, when the kext was built with them (`stage --embed`; size 0 otherwise). Checked like the others,
   not copied. This keeps the embedded path working for a kext that wants it.
2. **the boot's addon**: `/chosen/memory-map GpuAddon` (`{physical, bytes}`), DarwinBoot's addon slot: the pinned DarwinBoot
   (8544957) loads `Payloads/kernel.macho.gpuaddon` from the payload into the boot's static memory when the ADT has that slot, and
   refuses to boot when only one of the two is there. The container is UniXNU's `NGPUADD2` (`scripts/make-gpu-addon.py`'s layout:
   path, mode, size, zlib stream per entry), with entry paths `EFI/UniXNU/firmware/<path>`; k27 itself reads nothing there (the
   slot's name is from its first, VM-era use). Inflated into `IOMallocData` memory with libkern's zlib (`inflateInit`/`inflate`/
   `inflateEnd` with the loader's allocator: the kernel's zlib has no default allocator, so its `uncompress()` would call a null
   function).
3. **the ESP**: GPT partition `esp` through NativeUfs (`native_ufs_part_find` / `native_ufs_part_read`, read only; waits up to
   `wait_s` s for NativeUfs to publish it) and FatVolume: `EFI/UniXNU/firmware/<path>` read whole into `IOMallocData` memory.

`piano_fw_release(&fw)` frees it (nothing for built-in bytes). The memory is virtually contiguous only: hardware that reads the
image (DMA) gets it copied into its own buffer, as PianoTouch's SPI DMA buffer and the GPU's memory already are.
`piano-fw-sources=MASK` (default 7: 1 built in, 2 addon, 4 ESP) leaves sources out for a test, e.g. `piano-fw-sources=4` makes a
RAM image read the tablet's ESP.

**RAM boots** (decided): a RAM image carries the files as the addon, inside its payload FAT (`Payloads/kernel.macho.gpuaddon`).
`tooling/bootimg/build-ram-images.py` finds its collection's pins, takes the files from `PIANO_FW_DIR` (default `out/firmware`),
checks them and writes them as entries of the image's one addon. PianoSensors uses the same slot for this unit's sensor pack
(`PIANO_SENSOR_PACK`, [sensors.md](drivers/sensors.md) §5b, entry `sensors/piano-sensors.pack`): DarwinBoot carries one addon, so
with both the container holds both kinds of entries, and each reader takes only its own (checked with both readers). The payload's
ADT has the `GpuAddon` slot exactly when the payload has the addon: the input ADT's (made with `make-piano-adt.py --addon` for the
sensor pack), or added to the payload's copy when only firmware is carried. A collection without pins adds nothing: the images are
byte for byte what main's builder makes, with and without the sensor pack (checked).
`PIANO_FW_ADDON=0` builds without the addon (the kexts then read the tablet's ESP); a pin whose file is not staged is left out with
a note. `tooling/bootimg/verify-ram-images.py` checks that the addon and the slot come together, and every entry against its pin in
the payload's own collection (private ones without printing a hash). Why not plain files in the payload FAT: that FAT is the boot
image's ramdisk, which the firmware reads and the kernel is never told about; DarwinBoot's memory-map slots are the only way the
pinned loader hands a file to the kernel (the Data seed comes the same way, as `RAMDisk`, and NativeRamBlock maps it as the loader
maps the addon).

**The installed system**: `tooling/persist/build-esp.py` places every pinned file (also those of `--prev-kc`) at
`EFI/UniXNU/firmware/<path>` from `--firmware-dir` (default `<--from>/out/firmware`), checked against the pins; a missing or
different file stops the build unless `--allow-missing-firmware`. It records no hash of a private file, and refuses an ADT with the
`GpuAddon` slot (an ESP never carries an addon). Its independent FAT reader (`tooling/persist/common.py fat32_read`) now reads a
short name's case flags as EDK2 FatPkg does: mtools stores `firmware` (and `previous`) as `FIRMWARE` with the lower-case flag.

**Files staged, never committed**: `out/firmware/<Kext>/<file>`, copied there by `firmware.py stage` from the build script's sources.

**Cost to a kext** (the scratch probe kext linked into a KC with these files): about 9 KiB `__TEXT_EXEC`, 0.4 KiB `__DATA_CONST`
(`IOMallocType` views, a jump table), 136 bytes of `__TEXT` per pin, no `__DATA`.

**Checks**: `python3 package/common/tests/run-firmware-tests.py` (synthetic files only): `firmware.py stage` (pins, private files,
a missing pinned file, a wrong one refused); `firmware_host` under ASan/UBSan: pins and malformed ones, an ESP-like FAT32 image made
by mtools as `build-esp.py` formats the ESP (4096-byte sectors), the embedded check, the addon `firmware.py addon` makes from the pins
it finds in the test binary itself, thirteen damaged containers, corrupt zlib streams, no leak (zlib's blocks counted); pin scanning
(noise, duplicates, conflicts); the ADT slot added to a synthetic ADT and to `out/adt/piano-k27.adt` (nothing else changes; walked as
DarwinBoot walks it); the pins in C++20; the portable files freestanding with general registers only. Also done once by hand: the
loader compiled with the kernel's recipe (`check-kext-imports.py`: 27 kernel imports, the two NativeUfs ones, none unresolved), a
scratch kext using it linked into a KC (pins found there by `firmware.py pins`), a RAM image built from that KC and verified, an ESP
built with pinned test files and read back by the independent reader.

## 7. What each driver must do

The kexts that carry images (PianoTouch, PianoA830, PianoKeyboard) and PianoAudio are being edited elsewhere, so none of them is
converted here (no other native kext carries an image); their embedded path keeps working untouched. The steps, for whoever owns
each:

### 7.1 The conversion, in general

1. **Build script**: instead of generating the `.incbin` source, call
   `python3 "$D/tooling/persist/firmware.py" stage --kext <Kext> --header "$O/gen/<Kext>FirmwarePins.h" --source "$O/gen/<Kext>Firmware.c" ${PIANO_<KEXT>_EMBED:+--embed} "$SRC=<bytes>:<sha256>" ...`
   (the same pinned files and hashes the script checks today). It checks each file, copies it to `out/firmware/<Kext>/`, writes the
   pins and, per file `<f>`, the symbols `piano_fw_<kext>_<f>` / `piano_fw_<kext>_<f>_size` (size 0 unless `--embed`; with `--embed`
   the image sits in the kext's `__TEXT`, not `__DATA_CONST`) and `piano_fw_<kext>_<f>_pin` (names lower case, non-alphanumerics
   `_`).
2. **Sources**: add to the `build-native-kext.py` call `"$D/package/common/PianoFirmware.cpp" "$D/package/common/PianoFirmwareCore.c"
   "$D/package/common/PianoFirmwareZlib.c" "$D/package/PianoPower/FatVolume.c"` (not the last one if the kext already compiles a
   FatVolume.c copy), `"$O/gen/<Kext>Firmware.c"`, and `-I "$D/package/common" -I "$D/package/PianoPower" -I "$O/gen"`. In
   `check-kext-imports.py` add `--library-prefix _native_ufs_part_`.
3. **Descriptor** (`tooling/k27/kext/<Kext>.json`): `"com.unixnu.driver.NativeUfs": "1.0.0"` in `libraries` (NativeUfs links second,
   before every other kext, so the import resolves).
4. **Driver**: `#include "<Kext>FirmwarePins.h"`; where the image is first needed, in thread context (never in `start()` itself, nor
   with a lock held: the ESP read may wait for NativeUfs):
   ```cpp
   PianoFirmware fw;   // a member: keep it while the image may be needed again (re-download, recovery)
   const int rc = piano_fw_load(&piano_fw_<kext>_<f>_pin, piano_fw_<kext>_<f>, piano_fw_<kext>_<f>_size, 120, &fw);
   note("firmware: %s from %s (%s)", piano_fw_error(rc), piano_fw_source(fw.source), fw.trail);
   if (rc) { /* the level that needs it stops, as it does today without the image */ }
   // use fw.data / fw.size where the embedded symbol and its size were used; piano_fw_release(&fw) in stop/free
   ```
5. **Tests**: run `python3 "$D/package/common/tests/run-firmware-tests.py"` from the build script; host tests that took the images from
   `out/native-kexts/<Kext>/firmware/` take them from `out/firmware/<Kext>/`.

Then the ESP (`build-esp.py`) and the RAM images (`build-native-image.sh` -> `build-ram-images.py`) carry the files by themselves.

**Interim, one line, if a kext keeps its own `.incbin` generator for now**: emit the image into `.section __TEXT,__const` instead of
`.section __DATA,__const` (the image and its size word have no relocations). It then takes the collection's roomy `__TEXT` instead of
`__DATA_CONST`, the same effect as patch 0002 for that kext, with no other change.

### 7.2 PianoTouch (`tooling/k27/build-native-piano-touch.sh`, `package/PianoTouch/PianoTouch.cpp`): -480 KiB `__DATA_CONST`

- Build script: replace the `for PANEL in boe:... csot:...` loop that writes `$GEN` with one `firmware.py stage --kext PianoTouch
  --header "$O/gen/PianoTouchFirmwarePins.h" --source "$GEN" ${PIANO_TOUCH_EMBED:+--embed}
  "$FW_DIR/novatek_nt36532_piano_fw_boe.bin=245760:b7999389d49decb14019e24ce68c6fbb0a388e26391b9b0ab913e285c5e44c6b"
  "$FW_DIR/novatek_nt36532_piano_fw_csot.bin=245760:14b30bb7799d273f6c5d967376694c2cbd3056f3939135ceda70ebec64abfa7a"`
  (a host without the images still builds the same kext: the pins stay, and `firmware.py` warns; `PIANO_TOUCH_NO_FIRMWARE` is no
  longer needed). `STAGED` (the host test's images) = the files present under `$D/out/firmware/PianoTouch/`. Steps 2, 3 and 5 of
  §7.1 as written.
- `PianoTouch.cpp`: drop the `extern ... piano_touch_firmware_{boe,csot}{,_size,_sha}` declarations (line 88-90) for the header. At
  the first download (line 868-869, after `lcd-id` gives the panel) load only that panel's image:
  `piano_fw_load(boe ? &piano_fw_pianotouch_novatek_nt36532_piano_fw_boe_pin : &..._csot_pin, boe ? piano_fw_pianotouch_novatek_nt36532_piano_fw_boe : ..., boe ? ..._boe_size : ..._csot_size, 120, &fFirmware)`,
  then `fFw = fFirmware.data; fFwBytes = fFirmware.size;` (a `PianoFirmware fFirmware` member; release it in `free()`). The survey's
  `kImages` (line 522) reports each pin's path and size, and parses an image only when it is built in.
- The calibration (`PianoTouchCalibration.h`, constants under 1 KiB) stays compiled in: moving it would need a parser for under
  1 KiB. If it should leave the collection later (one collection for every unit), stage its JSON as a bare (private) file.

### 7.3 PianoA830 (`tooling/k27/stage-piano-gpu-firmware.py`, `package/PianoA830/board/`): -242 KiB `__DATA_CONST`

- `stage-piano-gpu-firmware.py`: keep its pin checks against `board/piano-gpu-firmware.json` and `PianoGpuFirmwarePins.h`, and
  replace the `PianoA830Firmware.c` writer by a `firmware.py stage --kext PianoA830 --header "$O/gen/PianoA830FirmwarePins.h"
  --source "$O/gen/PianoA830Firmware.c" [--embed] <source>/qcom/gen80000_gmu.bin=88896:1ad8175e... <...>/gen80000_sqe.fw=111476:30ee3301...
  <...>/gen80000_aqe.fw=25044:67562ad5... <...>/sm8750/xiaomi/piano/gen80000_zap.mbn=12088:bb942413...` call (the json's bytes and
  sha256). Drop the `.p2align 12` (it never page-aligned anything, §3).
- `PianoA830Board.cpp` `fillBoard()` (line 130-133): `image()` gets each image from a `PianoFirmware` loaded with its pin
  (`piano_fw_pianoa830_gen80000_gmu_pin` ... `_zap_pin`) and the embedded symbol as the built-in source; load them where S0 runs (the
  start thread, after `piano-gpu-delay`), not in `start()`; keep them for the kext's life (the GMU and ZAP images are needed again
  on recovery and power collapse). `PianoA830Survey.cpp` (line 25-28, 108-111) reports the pins. The weak zero-length defaults
  (`NativeAdrenoFirmwareDefaults.cpp`) can stay for the upstream sources.
- Descriptor: PianoA830 gets `com.unixnu.driver.NativeUfs` as a library (it links fifth, after NativeUfs). The daily ESP then has to
  carry the four files: `build-esp.py` places them from `out/firmware/PianoA830/`, and stops when they are not staged.

### 7.4 PianoKeyboard (`tooling/k27/build-native-piano-keyboard.sh`, `package/PianoKeyboard/PianoKeyboard.cpp`): -81.5 KiB

- Build script: the `GEN` writer becomes `firmware.py stage --kext PianoKeyboard --header "$O/gen/PianoKeyboardFirmwarePins.h"
  --source "$GEN" ${PIANO_KEYBOARD_EMBED:+--embed} "$FW=83436:c03d6b650cb85a983359c6686d831875df10ee1206fd31afc1403baefeb7e407"`.
  The staged copy is `out/firmware/PianoKeyboard/MCU_Upgrade.bin` (the host test's argument).
- `PianoKeyboard.cpp`: `piano_keyboard_firmware` / `_size` (line 105-106, used at 554, 662-663, 843-860) become `fFirmware.data` /
  `fFirmware.size` of a `PianoFirmware` loaded with `piano_fw_pianokeyboard_mcu_upgrade_pin` before the image check of step 5;
  release it when the update is done (it is only needed for an MCU update).

### 7.5 PianoAudio: the preset on the loader (done 2026-11-12)

Nothing is embedded in it (9.7 KiB `__DATA_CONST` at 471197c: vtables and tables with pointers). Its amplifier preset is now a pin of
this loader: `tooling/k27/build-native-piano-audio.sh` runs `firmware.py stage --kext PianoAudio ... fs19xx.fsm=4348:ab060b84...`
(the published size and SHA-256, the facts `AudioSha256.c` already holds, so the pin is committed and not private), compiles the
loader's three files in, and stages the owner's copy (`PIANO_AUDIO_PRESET`, `private/audio/fs19xx.fsm`, or the Ubuntu project's
checkout next to this repository) in `out/firmware/PianoAudio/`. `AudioBoard.cpp` loads it with `piano_fw_load` (addon, then the
ESP's `EFI/UniXNU/firmware/PianoAudio/fs19xx.fsm`) and falls back to its own reader of `build-esp.py --audio-preset`'s
`EFI/UniXNU/piano-fs19xx.fsm` (the only path for a non-stock preset); the container and safety checks stay the driver's. So a RAM
image built with `PIANO_FW_ADDON=1` carries it ([audio.md](drivers/audio.md) §10.3), and `build-esp.py` places it from the pin (an
ESP build on a host without the copy then stops unless `--allow-missing-firmware`). Cost: the loader's code (about 9 KiB
`__TEXT_EXEC`) and 136 bytes of `__TEXT` for the pin. Its `__DATA` holds an 8.9 KiB `__bss`: see §7.6 (levels 8-10's rings and
state are allocated at run time, not static).

### 7.6 `__DATA`, the tight kind: the static buffers

`__DATA` has 336.9 KiB free in 29 holes and no blobs; it fills with zerofill buffers. Each buffer that only needs to exist while the
kext runs can move to `IOMallocData`/`IOMallocType` at start, as PianoDpu's table did: PianoA830 `__bss` 85.9 + `__common` 15.7 KiB
(`native_adreno_metal_rows` 4.5, `_client_log` 3.0, `_client_text` 2.6 KiB among them), NativeUfs `__bss` 32.2 KiB, PianoBluetooth
19.3 (since 586e2f6), PianoPower 15.2, PianoAudio 8.9, PianoWifi 6.5, PianoDpu `__data` 6.3 KiB; `__DATA` lost 34.7 KiB of free space
between 471197c and 586e2f6. Not changed here: a buffer the hardware reads by physical address relies on
the collection's physically contiguous memory (heap memory is not), and NativeUfs carries the root; each owner has to check that
first.

## 8. Space recovered

| Change | `__DATA_CONST` | `__TEXT` | `__DATA` largest gap | State |
| --- | --- | --- | --- | --- |
| 0001 best fit | holes: PianoTouch fits with the USB stack | | 63.9 -> 81.9 KiB (74.0 with the USB stack) | patch, measured |
| 0002 const to `__TEXT` | +722 KiB free | -721 KiB free (of 4724) | | patch, measured |
| PianoTouch on the loader | +480 KiB | (+480 KiB with 0002) | | steps §7.2 |
| PianoA830 on the loader | +242 KiB | (+242 KiB with 0002) | | steps §7.3 |
| PianoKeyboard on the loader | +81.5 KiB when linked | | | steps §7.4 |

With the three conversions the natives' `__DATA_CONST` drops from 834 (+87 with PianoKeyboard) to about 120 KiB, and the images leave
the collection altogether: it no longer depends on which firmware the build host has (only PianoTouch's calibration, under 1 KiB,
still makes it a unit's own).

## 9. Risks (nothing here has run on the tablet)

- The addon path is new on piano: DarwinBoot's code (pinned 8544957) loads and places it as read here, but no RAM image had booted
  with an addon when this was written (PianoSensors' pack test images are the first to carry one). Firmware entries only appear once
  a kext carries pins; the first such RAM test tests them too. If DarwinBoot refuses an addon, it stops with `EFI_COMPROMISED_DATA`
  before XNU; `PIANO_FW_ADDON=0` builds the image without the firmware entries (the kexts then read the ESP).
- One addon for two users: a third user has to add entries to the same container (`firmware.py container`), never write its own
  `kernel.macho.gpuaddon`; both readers ignore paths that are not theirs.
- The ADT gets one property in `/chosen/memory-map`; `build-ram-images.py` adds it only with the addon and `build-esp.py` refuses an
  ADT that has it, because DarwinBoot refuses a slot without its file.
- `piano_fw_load` from the ESP waits for NativeUfs (up to `wait_s`) and reads UFS: never from `start()` or with locks held. A driver
  converted before its files are staged builds, but its level that needs the image then reports `not found`; `build-esp.py`
  stops on a missing pinned file unless told otherwise.
- 0001 moves native segments to other addresses (as every kext change does); 0002 moves relocation-free constant data into `__TEXT`
  (read only in both places). Both are off without their switch, and `KC_NATIVE_PLACEMENT=first-fit` returns to the old placement.
- Private pins (unit data) put that unit's file on its ESP and into its RAM images (as the sensor pack does): keep those images local.
