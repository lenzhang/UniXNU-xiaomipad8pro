# Native bring-up on piano: how to boot, debug and iterate without watching the screen

Learned during the first native RAM boots, 2026-11-10 (`droidvm-build/piano-patches/el1-home-ram-boot-result.md`, session log
`logs/home-ram-boot-session-20261110.md`). Rules that never change:
- RAM boots only (`fastboot boot`). Never `flash`, `erase`, `set_active`, `flashing` or a partition write.
- `oem` goes only to our own in-RAM UEFI (`-s piano-uefi`), never to ABL.
- Never `systemctl isolate` on the tablet.

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
| XNU root | An OS volume with UUID C73696D4-… (the VM's); none natively before UFS | `Waiting on <dict> IOMedia … UUID C73696D4…` (expected stop) |

Menu input: volume keys move, the power key is Enter. Images auto-boot their default after 8 s (`PIANO_MENU_TIMEOUT`,
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

**Getting back on its own:** k27 patch 0056 (`piano-reset-after=<s>`) resets through PSCI after N seconds and pets the
Gunyah watchdog until then (bite 32 s after the last pet: the backstop for hangs and panics). The panic path restarts by
itself when `panic-spin=1` is absent.

**Decoding a panic photo** (fallback): the verbose console draws every glyph twice ("ppaanniicc"); keep every second
character. DarwinBoot loads the kernel at its link address (no slide), so pc/lr look up directly in
`out/k27/kc-k27-native.nm` (+ the KC's `natives.json`).

## 3. The unattended loop

```sh
tooling/native/native-run.sh out/ram/boot-piano-macos-ram-A-bundled.img [WAIT_SECONDS]
```

It runs Ubuntu → `sudo piano-reboot-bootloader` → `fastboot boot` → XNU → reset → Ubuntu, then writes
`logs/native-runs/<ts>/{run.log,usb.log,xnu-console.txt}`. The tablet's sudo password comes from
`~/.config/piano/tablet-sudo` (mode 600, outside every repository) and goes to sudo on stdin only.

A person is needed only when the tablet is parked (no reset, no Ubuntu): power + volume-down for 15–20 s; a plain
10-second power press does nothing to a parked kernel.

## 4. Rebuild after a change

```sh
K27_VARIANT=native ../droidvm-build/macOS-DroidVM/build-k27-piano.sh            # kernel (patches 0001-0056)
tooling/kc/build-kc-k27.sh ../droidvm-build/macOS-DroidVM/out/x12377/build/xnu/k27-native-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped out/k27/kc-k27-native.bin
tooling/unixnu.sh build firmware --skip-preflight                                   # FD (A: Binaries/piano -> the bundled set)
python3 tooling/bootimg/build-ram-images.py --fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --out out/ram \
  --kc out/k27/kc-k27-native.bin --trust out/inputs/kernel.macho.trustcache --seed out/inputs/scratch-data.img --adt out/adt/piano-k27.adt
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
```
