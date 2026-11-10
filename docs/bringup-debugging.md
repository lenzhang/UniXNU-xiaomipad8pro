# Native bring-up on piano: how to boot, debug and iterate without watching the screen

Learned during the first native RAM boots, 2026-11-10 (`droidvm-build/piano-patches/el1-home-ram-boot-result.md`, session log
`logs/home-ram-boot-session-20261110.md`). Rules that never change:
- RAM boots only (`fastboot boot`). Never `flash`, `erase`, `set_active`, `flashing` or a partition write.
- `oem` goes only to our own in-RAM UEFI (`-s piano-uefi`), never to ABL.
- Never `systemctl isolate` on the tablet.

The one approved exception to the first rule is the persistent install (`docs/persistent-boot.md`). Once its ESP exists on UFS,
every RAM image, the loop's dump image above all, must be built with FD A carrying `firmware/patches/unixnu/0001` (the boot
image's own RAM payload wins); an older image boots the ESP's configuration instead.

## 1. The boot chain, and what each stage needs

```
ABL (fastboot boot) ─► BootShim (kernel field) ─► Mu-Silicium UEFI (FD at 0xA7100000) ─► UniXNU.efi menu
   ─► DarwinBoot.efi ─► k27 XNU at EL1 (Gunyah at EL2)
```

| Stage | Requirement | Symptom when it is missing |
|---|---|---|
| ABL → kernel | The kernel field's ARM64 header must look like a Linux Image: `text_offset 0`, `flags 0xa`, `image_size` covering shim + FD (`build-ram-images.py` rewrites Mu's BootShim header, which carries the FD base in `text_offset`) | `fastboot boot` answers OKAY, the panel goes black, the tablet is back in ABL fastboot about 80 s later, and every further `boot` in that ABL session fails with `Load Error` (reset ABL with `fastboot reboot bootloader`) |
| UEFI drivers | FD **A** (piano-uefi's bundled DXEs). FD B (this tablet's own OS3.0.303 DXEs) resets after 10–15 s | early reset back to Ubuntu ~26 s after `fastboot boot` |
| UEFI → UniXNU payload | SEC must keep x0 (the DT from ABL) in the entry-argument HOB: `firmware/patches/mu-silicium/0001` | Piano_Log: `UNIXNURAMDISK: no entry argument HOB`; the menu has only the built-in entries and falls through to the UEFI shell |
| DarwinBoot → XNU | A linear BGRA GOP: `firmware/patches/0002` (SimpleFbDxe instead of piano-uefi's Blt-only ScaledFbDxe) | console: `DARWINBOOT: boot refused: Unsupported; firmware retained`; the menu redraws (looks like a flash) |
| XNU CPUs | Boot with all 8 ADT CPUs (`sheng-smp=1`), **not** `cpus=1` | `cpus=1`: `Kernel data abort` in `recount_switch_thread+0x84` (far 0xfffffff36ee62004, esr 0x96000007) |
| XNU root | NativeUfs (`package/NativeUfs`, `native-ufs-root=1`): the SHNGROOT windows OS + SystemOS in linux_rootfs (`tooling/native/make-root-manifest.py`); Data = the RAM seed with its changed pages in NativeUfs's store window at the end of linux_rootfs (`data-persist=2`, `tooling/k27/build-native-ram-block.sh`) | `Waiting on <dict> IOMedia … UUID C73696D4…` |
| UFS: SMMU | piano's firmware leaves SID 0x60 on context bank 1 (PKX110: bank 0); the entry is redirected to the bypass bank | `SMMU plan for stream 0x60: conflicting UFS stream mapping (slot 1 smr 80000060 s2cr 1)` |
| UFS: link | the link is in Hibern8 when XNU starts: UniXNU-Odin3's SM8750 link handling (AHIT off, Hibern8 exit, IACR off) | `FAILED at NOP OUT` (LINK tx=1) |
| UFS: writes | the development build writes the windows in place (`NATIVE_UFS_UNVERIFIED_ROOT`); without it APFS's writes fail | `disk5s1: device is not ready.` by the hundred |

Menu input: volume keys move, the power key is Enter. Images boot their default at once (`PIANO_MENU_TIMEOUT`, default 0; -1 waits for a key;
`PIANO_MENU_DEFAULT`, default `macos-8cpu`).

## 2. Seeing what happened without a camera

**In UEFI** (before DarwinBoot exits boot services), our FD exposes piano-uefi's debug fastboot as a second USB device,
`piano-uefi`:

```sh
fastboot -s piano-uefi oem log        && fastboot -s piano-uefi get_staged console.txt   # what the console printed
fastboot -s piano-uefi oem screenshot && fastboot -s piano-uefi get_staged shot.bmp      # the panel
fastboot -s piano-uefi oem dump D7F00000 100000 && fastboot -s piano-uefi get_staged pianolog.bin  # DEBUG log (Piano_Log)
```

The DEBUG log has every driver load, `UNIXNU:` and `UNIXNURAMDISK:` line. `reboot-bootloader` is not supported there;
`fastboot -s piano-uefi reboot` goes back to Ubuntu. The USB timeline alone tells the stage: `<serial> fastboot` (ABL) →
nothing → `piano-uefi fastboot` (our UEFI) → nothing (DarwinBoot handed over to XNU).

**In XNU**, k27 patch 0055 (`pstore-console=0x9cb00000 pstore-console-size=0x700000`) mirrors every console byte into
a RAM mailbox, `cvp@9cb00000` (7 MiB, no-map, used by neither Ubuntu nor the firmware), which Ubuntu reads back through
`/dev/mem` (`tooling/native/xnu-log.py`, aligned 8-byte accesses only: unaligned ones raise SIGBUS on Device memory).
Measured 2026-11-10:
- Ubuntu's own `ramoops@a3500000` does not survive even an Ubuntu reboot: the region is `Display_Demura` in the firmware's
  memory map.
- DRAM loses about 4 % of the bits of the mailbox across any reset (a plain Ubuntu reboot as much as XNU's PSCI reset). So
  the ring is stored 11 times, and the reader takes the bitwise majority (residual ~2e-6 per bit).
- A hard power-off (power + volume-down) loses everything.

**Live, over the USB-C cable** (not yet run on the tablet): with `piano-usb=2` in the image's arguments, `package/PianoDwc3`
takes the USB controller over in device mode once XNU runs; the USB timeline then continues with `1209:0001 piano macOS debug`
(two CDC-ACM ports). `tooling/native/usb-console.py` on the Mac shows the console live (the retained log first) and talks to
a small kernel monitor. Design, levels and test steps: `docs/drivers/usb-device-debug.md`.

**Getting back on its own:** k27 patch 0056 (`piano-reset-after=<s>`) resets through PSCI after N seconds and pets the
Gunyah watchdog until then (bite 32 s after the last pet: the backstop for hangs and panics). The panic path restarts by
itself when `panic-spin=1` is absent.

**Decoding a panic photo** (fallback): the verbose console draws every glyph twice ("ppaanniicc"); keep every second
character. DarwinBoot loads the kernel at its link address (no slide), so pc/lr look up directly in
`out/k27/kc-k27-native.nm` (+ the KC's `natives.json`).

## 3. The unattended loop (no Ubuntu, 2026-11-11)

```sh
tooling/k27/build-native-image.sh                                    # kexts -> KC -> out/ram (test) + out/ram-dump (reset after 1 s)
tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img
```

The tablet waits in ABL fastboot. One test: `fastboot boot` the image → XNU runs until `piano-reset-after`
(`PIANO_RESET_AFTER`, default 45) resets it; k27 0057 set the PMIC restart reason, so ABL stays in fastboot → `fastboot boot` the
dump image → while its UEFI runs, `fastboot -s piano-uefi oem dump 9cb00000 700000` + `get_staged` pull the test's log → the
dump image's XNU resets at once → parked in ABL fastboot again. About reset time + 70 s per test; results in
`logs/native-runs/<ts>-loop/{loop.log,xnu-console.txt}`. The UniXNU menu boots its default at once (`PIANO_MENU_TIMEOUT=0`).

Ubuntu's root is gone (linux_rootfs holds the macOS windows), so the old Ubuntu-based `native-run.sh` was removed. The
maintenance RAM image of the Ubuntu release (USB NCM, telnet root shell: `tooling/native/maint-shell.py`) is the way to write
partitions from the tablet itself.

A person is needed only when the tablet is parked (no reset, no fastboot): power + volume-down for 15–20 s.

## 4. Rebuild after a change

```sh
K27_VARIANT=native ../droidvm-build/macOS-DroidVM/build-k27-piano.sh   # kernel (k27 tree: patches 0001-0052 + patches/k27 0053-0058)
tooling/unixnu.sh build firmware --skip-preflight                          # FD (A: Binaries/piano -> the bundled set)
tooling/k27/build-native-image.sh                                          # kexts (package/) -> KC -> ADT -> out/ram + out/ram-dump
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
```

`tooling/build-slice1.sh` runs all of it. The native kext set is `configs/k27-native-kexts.txt`; what is left of the VM era and why:
`docs/native-droidvm-inventory.md`.

## 5. No DroidVM kexts natively (rule, 2026-11-11)

The native boot loads no DroidVM kext: the native kernel collection links only this repository's kexts
(`configs/k27-native-kexts.txt`; descriptors in `tooling/k27/kext/`, sources in `package/`, objects in `out/native-kexts/`).
`tooling/kc/build-kc-k27.sh` refuses any other descriptor and checks the linked collection again
(`tooling/k27/check-native-kexts.py`), so `PIANO_EXTRA_KEXTS` / `PIANO_KEXT_LIST` cannot bring an archive kext back either.
VM-era code native piano needs is taken over as a package of this repository with an `UPSTREAM` file (NativeGopDisplay,
PianoNullEthernet, ARMPlatformStandIn, NativeRamBlock, PianoInput's HID layer).

What the DroidVM kexts did in native boots (runs in `logs/native-runs/`, 2026-11-11):

| Kext(s) natively | Effect |
|---|---|
| DroidVMDisplay (as the display, or as NativeGopDisplay's nub), with the VM's virtio kexts | IOBSD matching did not finish in 30 s (`!BSD`), both UFS windows stayed busy, **the root never mounted**. The mechanism (k27 0059's busy dump): a window published before IOBSD had matched let AppleAPFSContainer::start take APFS's module lock while com_apple_filesystems_apfs::start waited for it. NativeUfs now publishes only after IOBSD has matched, but the DroidVM kexts' start timing is what triggered it |
| DroidVMGpu | a stub accelerator: WindowServer read its MetalPluginName, its type-0x4147 opens failed (`kIOReturnBusy`, slots reserved for iOS's backboardd) and WindowServer crash-looped every 30–40 s |
| DroidVMEthernet, DroidVMAudio, DroidVMVideo, DroidVMInput, NativeVirtioBlock | start with no virtio device behind them; without them SystemOS matches in 0.5 s instead of 4 s |
| DroidVMEthernet | its no-device `en0` turned out to be needed (TimeSync takes its clock ID from a built-in interface; without one WindowServer and the login session never start): replaced by `package/PianoNullEthernet`, not by linking the DroidVM kext |

The inventory of every VM-era piece and its fate is `docs/native-droidvm-inventory.md`.
