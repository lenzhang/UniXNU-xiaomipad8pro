# Native bring-up on piano: how to boot, debug and iterate without watching the screen

Learned during the first native RAM boots, 2026-11-10 (the local bring-up notes `el1-home-ram-boot-result.md`, not published;
session log `logs/home-ram-boot-session-20261110.md`). Rules that never change:
- Routine tests are RAM boots (`fastboot boot`). Partition writes are allowed only through the owner-approved maintenance plan in
  `docs/persistent-boot.md` (ESP, `boot_a`, GPT and the Linux-era root/Data regions, with backups and read-back). Never write
  firmware/calibration partitions or any `_b` partition; never `flash_all_lock`.
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
| UEFI drivers | FD **A** (piano-uefi's bundled DXEs). FD B (this tablet's own OS3.0.303 DXEs) resets after 10–15 s | early reset about 26 s after `fastboot boot`; it returned to Ubuntu when Ubuntu was installed, while a current reboot reaches ABL/ESP (macOS 27.3) or Android |
| UEFI → UniXNU payload | SEC must keep x0 (the DT from ABL) in the entry-argument HOB: `firmware/patches/mu-silicium/0001` | Piano_Log: `UNIXNURAMDISK: no entry argument HOB`; the menu has only the built-in entries and falls through to the UEFI shell |
| DarwinBoot → XNU | A linear BGRA GOP: `firmware/patches/0002` (SimpleFbDxe instead of piano-uefi's Blt-only ScaledFbDxe) | console: `DARWINBOOT: boot refused: Unsupported; firmware retained`; the menu redraws (looks like a flash) |
| DarwinBoot → XNU: RAM | XNU's window (`configs/k27-adt.toml` `owned_memory_size`, 13 GiB less 2 MiB since 2026-11-12) needs FD A built with `firmware/patches/darwinboot/0001` (`tooling/firmware/build-fd.sh`: the `.ram-window` stamp `build-ram-images.py` checks); an older FD refuses an ADT asking for more than 8 GiB. Console check: `NativeRamBlock: RAM window 0x8c0004000..0xbffe04000 (13310 MiB) in DRAM 0x8c0000000..0xc00000000` (8192 MiB with an 8 GiB ADT) | older FD: the menu redraws after `DARWINBOOT: boot refused` (Compromised Data); no run for the window: `NO WINDOW <size> BIG RUN ...` and the free runs drawn on the panel; a shrunk window: `DARWINBOOT: window 0x33fe00000 requested, <size> taken (largest run)` and a smaller RAM window line |
| XNU CPUs | Boot with all 8 ADT CPUs (`sheng-smp=1`), **not** `cpus=1` | `cpus=1`: `Kernel data abort` in `recount_switch_thread+0x84` (far 0xfffffff36ee62004, esr 0x96000007) |
| XNU root | NativeUfs (`package/NativeUfs`, `native-ufs-root=1`, current `native-ufs-root-part=linux_rootfs`): the SHNGROOT windows OS + SystemOS; Data = the RAM seed with its changed pages in the 200 GiB NativeUfs store over `linux_data` (`data-store-part=linux_data`, LBA 71859642) | `Waiting on <dict> IOMedia … UUID C73696D4…` |
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
`fastboot -s piano-uefi reboot` returns through ABL to the installed ESP (macOS 27.3) or the selected Android path, not Ubuntu. The USB timeline alone tells the stage: `<serial> fastboot` (ABL) →
nothing → `piano-uefi fastboot` (our UEFI) → nothing (DarwinBoot handed over to XNU).

**In XNU**, k27 patch 0055 (`pstore-console=0x9cb00000 pstore-console-size=0x700000`) mirrors every console byte into
a RAM mailbox, `cvp@9cb00000` (7 MiB, no-map, used by neither the installed OS nor the firmware). The unattended loop's dump image
pulls it with `fastboot -s piano-uefi oem dump 9cb00000 700000` + `get_staged` (`tooling/native/xnu-log.py`, aligned 8-byte
accesses only). Ubuntu originally read it through `/dev/mem`; that installation no longer exists.
Measured 2026-11-10:
- Ubuntu's own `ramoops@a3500000` does not survive even an Ubuntu reboot: the region is `Display_Demura` in the firmware's
  memory map.
- DRAM loses about 4 % of the bits of the mailbox across any reset (a plain Ubuntu reboot as much as XNU's PSCI reset). So
  the ring is stored 11 times, and the reader takes the bitwise majority (residual ~2e-6 per bit).
- A hard power-off (power + volume-down) loses everything.

**Live, over the USB-C cable** (running on the tablet since 2026-11-12, [results-2026-11-12.md](results-2026-11-12.md)): with `piano-usb=2` in the image's arguments, `package/PianoDwc3`
takes the USB controller over in device mode once XNU runs; the USB timeline then continues with `1209:0001 piano macOS debug`
(two CDC-ACM ports). `tooling/native/usb-console.py` on the Mac shows the console live (the retained log first) and talks to
a small kernel monitor. Design, levels and test steps: `docs/drivers/usb-device-debug.md`.

**Getting back on its own:** k27 patch 0056 (`piano-reset-after=<s>`) resets through PSCI after N seconds and pets the
Gunyah watchdog until then (bite 32 s after the last pet: the backstop for hangs and panics). The panic path restarts by
itself when `panic-spin=1` is absent. The installed system, which has no `piano-reset-after`, gets the same backstop from k27
0063's boot guard (`piano-boot-guard=SECS`, [boot-guard.md](boot-guard.md)); a test run with `piano-reset-after` keeps 0056's.

**Decoding a panic photo** (fallback): the verbose console draws every glyph twice ("ppaanniicc"); keep every second
character. DarwinBoot loads the kernel at its link address (no slide), so pc/lr look up directly in
`out/k27/kc-k27-native.nm` (+ the KC's `natives.json`).

## 3. The unattended loop (no Ubuntu, 2026-11-11)

```sh
K273_KERNEL=<prepared kernel-k273-...-native.macho> tooling/k273/build-k273-image.sh  # 27.3 kexts -> KC -> out/ram + out/ram-dump; root-layout caveat in k273-line.md §4
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
tooling/k273/prepare-k27-tree.sh                             # current kernel: patches/k27 0053-0066 + patches/k273
tooling/unixnu.sh build firmware --skip-preflight          # FD (A: Binaries/piano -> the bundled set)
K273_KERNEL=<path printed by prepare-k27-tree.sh> tooling/k273/build-k273-image.sh
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
```

`tooling/build-slice1.sh` still calls the retired `tooling/k27/build-native-image.sh`; **do not use it for the current line**.
The native kext set is `configs/k27-native-kexts.txt` (§5). Only 27.3 images are built; see the `K273_ROOT` layout caveat in
[k273-line.md](k273-line.md) §4.

**The k27 tree** is the local XNU source tree: xnu-12377 + the baseline series 0001-0052 (pin `k27` of `unixnu.lock`),
with this repository's `patches/k27` (0053-0066) and, for the current 27.3 build, `patches/k273` applied by
`tooling/k273/prepare-k27-tree.sh`. The build only reads it, and no committed file names its place: set
`PIANO_K27_TREE` in the environment or as a line `PIANO_K27_TREE=/path` in `~/.config/piano/env`. Every script that needs it
goes through `tooling/k27/k27-tree.sh` (shell) or `tooling/k27/k27_tree.py` (Python) and stops with what to set when it is
missing. What the native build reads from it:
- the relinked 27.3 kernel (`out/k27/kernel-k273-...-native.macho`, selected by `K273_KERNEL`) and its objects' compile recipes
  (`out/x12377/build/xnu/k27-native-obj`), which every kext is compiled with (`tooling/k27/build-native-kext.py`);
- the generated headers under `out/x12377` (storage ABI, xnu's osfmk, IOKit hid/net/gfx) the kexts include;
- the current KC base, Apple's macOS 27.3 mac17j kernelcache (an Apple payload: never published); the 27.0.1 vma2 `out/kc/kc27.bin`
  base belongs to the retired line;
- the KC builder (`k27/kc-build27-piano.py`, `k27/kc27-layout.py`, `k27/segaddrs.sh`, `configs/kc-keep.txt`, `scripts/kc-build.py`,
  `kc_native.py`, `fileset_kc.py`, `ptrauth_disc.py`; its placement patches, applied there by hand: `patches/k27-kc`,
  [kc-space.md](kc-space.md)) and `scripts/gen-iomfb-abi.py` (its output is cached in `out/k27/iomfb-abi/`);
- `sources/UniXNU/drivers/DarwinBoot/Test/packed_ramdisk.py` (the Data seed's format, `tooling/k27/stage-soft-metal.py`);
- for the A830 GPU's userspace (`tooling/metal/build-turnip-a830.sh`): the Metal plugin's sources, its link stub and the prebuilt
  bridge (`overlay/metal`, `scripts/piano-frontend-patch.py`, `out/metal`), the virtgpu ABI header, and the UniXNU-turnip checkout
  next to the tree (`TURNIP_TREE`). Provenance: `tooling/metal/UPSTREAM`.

## 5. The native kext rule (2026-11-11)

A native boot loads only this repository's kexts: the native kernel collection links the set of `configs/k27-native-kexts.txt`
(descriptors in `tooling/k27/kext/`, sources in `package/`, objects in `out/native-kexts/`). `tooling/kc/build-kc-k27.sh`
refuses any other descriptor and checks the linked collection again (`tooling/k27/check-native-kexts.py`), so `PIANO_EXTRA_KEXTS`
/ `PIANO_KEXT_LIST` cannot bring a foreign kext in either; `tooling/persist/build-esp.py` refuses an ESP whose collection has
one. The check carries the VM-era paravirtual kexts as a denylist (their bundle identifiers, their legacy name prefix and the
virtio transport) and proves, before every collection, on synthetic descriptors that it still refuses them
(`check-native-kexts.py --self-test`; a build log says `native kext rule: N native kexts linked, none from outside this
repository`). Code native piano needs from the VM era is this repository's package with an `UPSTREAM` file, never the VM-era kext.

### 5.1 The VM-era kexts in native boots

The retired macOS 27.0.1 install was first brought up under the hypervisor, with paravirtual kexts for its virtual
devices. The tablet now runs macOS 27.3. What those kexts did in native boots (runs in `logs/native-runs/`, 2026-11-11), and what replaced them:

| Kext(s) natively | Effect | Natively now |
|---|---|---|
| the virtio display kext (as the display, or as NativeGopDisplay's nub), with the other virtio kexts | IOBSD matching did not finish in 30 s (`!BSD`), both UFS windows stayed busy, **the root never mounted**. The mechanism (k27 0059's busy dump): a window published before IOBSD had matched let AppleAPFSContainer::start take APFS's module lock while com_apple_filesystems_apfs::start waited for it; the virtio kexts' start timing triggered it | NativeUfs publishes only after IOBSD has matched; `package/NativeGopDisplay` copies into the GOP framebuffer and carries the `AppleParavirtGPU` nub itself |
| the stub GPU accelerator | WindowServer read its MetalPluginName, its type-0x4147 opens failed (`kIOReturnBusy`, slots reserved for iOS's backboardd) and WindowServer crash-looped every 30–40 s | `package/PianoA830` (the A830 GPU, its `AGXAccelerator`), or `package/NativeSoftGpu` for software Metal |
| the virtio Ethernet, audio, video, input and block kexts | start with no virtio device behind them; without them SystemOS matches in 0.5 s instead of 4 s | the root and Data from `package/NativeUfs` and `package/NativeRamBlock` (its virtio data store gone, NativeUfs's store window serves it, `data-persist=2`); the drivers of `docs/drivers/` |
| the virtio Ethernet kext's built-in `en0` without a device | turned out to be needed: TimeSync takes its clock ID from a built-in interface's MAC, and without one clocksyncd never answers, so WindowServer and the login session never start | `package/PianoNullEthernet`, that interface without virtio (measured: TimeSync's clock created, logind, loginwindow and WindowServer start, the display is turned on) |
| NativeAES | thought unused (no AppleKeyStore, Data unencrypted; the same boot without it), but AppleImage4's start waits for an IOAESAccelerator without a timeout, so without it the IOKit registry never goes quiet ([identity.md](identity.md) §8) | removed 2026-11-11; back 2026-11-13 as `package/SoftwareAES` (UniXNU's component, with the device key AppleImage4 entangles nonces with) |

Also this repository's packages: `ARMPlatformStandIn` (the vma2 collection's IOMobileGraphicsFamily and its kexts link against
AppleARMPlatform's classes) and PianoInput's HID class. Every removal was checked with a native run that matched the baseline on
five points: root mounted from UFS, Data resumed from its store, the same launchd boot tasks, no panic, no I/O errors.

### 5.2 The boot arguments

The arguments every image carries, who reads each, and the four legacy `droidvm-*` names the k27 kernel and the OS image still
read: [k27-userland-args.md](k27-userland-args.md).

**Beyond DarwinBoot's 1023 bytes (k27 0066, 2026-11-13).** DarwinBoot copies at most 1023 bytes of command line into
`boot_args.CommandLine` (the bootloader ABI, `BOOT_LINE_LENGTH` 1024), and the tools keep a line under 1000. With patch 0066 in the
kernel the rest lives in the ADT: the line's `piano-boot-args=NAME` makes the kernel append `/chosen/piano-boot-args-NAME` (a string)
to a copy of the line, right after the device tree is set up and before any tunable or kext reads an argument
(`piano-boot-args=0`: none; no selector: `/chosen/piano-boot-args` if the ADT has it). `PE_boot_args()` returns the copy (4096
bytes), so every `PE_parse_boot_argn()` / `bootArg()` sees both with no change in the kexts; without the property and the selector
nothing changes. One console line says what happened: `piano-boot-args: /chosen/piano-boot-args-ram (P bytes): T bytes appended,
boot-args M bytes (line L)` (or why nothing was appended: no such set, a byte outside printable ASCII, too long for 4095).
`kern.bootargs` keeps its 1024-byte answer, cut after the last whole argument; the USB console's `args` prints the whole line.
static-if keys (`validation_disables`, `serverperfmode`, `lcks`, `wdt`) must stay on the line (read before the device tree).

The tools split only a line that would reach the budget (`PIANO_BOOT_ARGS_SPLIT=1` forces it): every `piano-*` argument moves to the
ADT except `piano-reset-after`, `piano-reset-to` and `piano-boot-guard*` (the way back to fastboot even under a kernel without 0066);
the rest (k27 arguments, `native-ufs-*`, `data-*`, `pstore-*`, `droidvm-*`, `cpus`/`sheng-smp`) stays, order kept within each part, so
for one name the last value still wins. `tooling/adt/boot_args.py` (`--show ADT --line LINE` prints the sets and the boot-args the
kernel makes; `--self-test`), `build-ram-images.py` (set `ram` in the payload's ADT; refuses a collection whose kernel lacks 0066).
Host test: `tooling/k27/test-piano-bootargs.sh`. `build-esp.py` and `tooling/k273/build-dual-esp.py` do not split yet (the next ESP
is the 27.3 line's only; its entry would take its own set, `piano-boot-args=<entry id>`, in its kernel's ADT).

### 5.3 The ADT identity (`configs/k27-adt.toml`, `configs/k27-gop-display.toml`)

| Item | Value | Why |
|---|---|---|
| `model`, `target-type`, `product-name`, `has-virtualization` | VirtualMac2,1, VMA2MACOS, "Apple Virtual Machine 1", set | the VM era's identity, kept as the default. No kext of the native collection matches on it (the VM-era collection's platform kexts did); what reads it is userland, and another identity is a build switch for test images: [identity.md](identity.md) |
| `vmm-present` | 1 (`PIANO_VMM_PRESENT` overrides it per build) | with 0 watchdogd runs as on real hardware, exits twice (no watchdog device) and launchd panics about 150 s into the run (`watchdogd[297] exited`); with 1 a 240 s run has no panic. Revisit with a watchdog driver |
| `serial-number` | DVMPIANO001 | made up, not a unit serial; the Data seed was made with it |
| `/arm-io/gop-display` | compatible `paravirtualizedgraphics,gpu` | IOMobileFramebuffer.framework only looks for displays on a node with that string (`docs/drivers/native-gop-display.md`) |

### 5.4 The macOS install

The installed OS is macOS 27.3 only since 2026-11-14; the VM-era 27.0.1 volume was cleared. Replacing the root means installing
the 27.3 root onto `linux_rootfs` with the §13.8 maintenance recipe in [persistent-boot.md](persistent-boot.md), not restoring
27.0.1. The installed Data seed identity is `9bd6f902`; its 200 GiB store is on `linux_data`. The old 27.0.1 OS/SystemOS windows
and Data seed on `linux_rootfs` were cleared before the 27.3 root moved there ([persistent-boot.md](persistent-boot.md) §13).

## 6. A kernel panic in the mailbox log (2026-11-12: the 13 GiB boot's `vm_page_unwire`)

The native kernel runs unslid: `atos -o "$PIANO_K27_TREE/out/k27/kernel-k27-00NN-native.macho" -arch arm64e <pc> <lr>` names the
addresses of the panic line directly (the kexts' addresses: the KC's descriptor). An `Assertion failed` stops in a `<function>.cold.N`
stub (`brk #0xbffc`, esr `0xf200bffc`), so pc names the function, and lr is in the same function; the console prints no backtrace,
so the caller comes from the registers: x19-x28 are the caller's wherever the asserting function had not yet changed them (its
prologue and the path to the stub, `llvm-objdump -d`), and a constant the caller keeps in one of them (here `mov w26, #0x14` and
`adrp x28, vm_pageout_debug` in `vm_page_do_delayed_work`) gives it away. The mailbox copy may still have single bit errors after the
majority (here x24 = x0 with bit 45 cleared); the USB capture (`logs/usb-console/`) is exact up to where it stopped.

The 13 GiB boot of 19:17 (`logs/native-runs/20261112-191724-loop`): `vm_resident.c:6226 Assertion failed: !mem->vmp_gobbled`, pc
`vm_page_unwire.cold.3`, lr `vm_page_unwire+860`, the caller `vm_page_do_delayed_work`'s `DW_vm_page_unwire` (an IOPL's
`upl_commit_range`/`upl_abort_range` slow path), the page's object a zone `vm_object`. Evidence vs inference:

| Item | State | Evidence |
|---|---|---|
| In k27 only `vm_page_find_contiguous(wire=FALSE)` sets `vmp_gobbled` (`kmem_alloc_contig_guard()` → `cpm_allocate()`); `kernel_memory_populate_object_and_unlock()` and `vm_page_wire()` clear it; `vm_object_iopl_wire_empty()` wires a fresh page without either | verified (source) | `osfmk/vm/vm_resident.c`, `vm_kern.c`, `vm_pageout.c` |
| `kmem_alloc_guard_internal()` takes the contiguous pages before `kmem_find_space()`; on its failure `vm_page_free_list(..., FALSE)` puts them on the free list still gobbled and still in `vm_page_wire_count`/`vm_page_gobble_count`; `vm_page_grab()` hands them out as they are | verified (source) | `vm_kern.c` `out_error` |
| The kernel map refuses one mapping above `vm_map_kernel_max_simple_mappable_size()` (2 GiB less a page, mode REJECT) in `vm_map_locate_space_anywhere()`, after the pages were taken | verified (source) | `vm_map.c` |
| PianoA830's S3 asks for a 2048 MiB pool first on every boot since 2026-11-12 (a quarter of the RAM, at most 2048), never granted (`row2 MEM pool 1024`, 512 or 64) | verified (logs, source) | `logs/usb-console/*`; `NativeAdrenoMemory::reservePoolBestEffort` |
| When that search finds 2 GiB of contiguous pages, 2 GiB of gobbled pages leak: the 17:27 session (8 GiB) shows it as phantom wired pages, `vm_stat` free + active + inactive + speculative + wired = 2,517,918 pages for the 2,097,152 of the RAM (1.6 GiB counted twice) | verified (log arithmetic) / inferred (that the 2048 MiB search was what leaked, and that the phantom wired count is what made XNU refuse the GPU's BoAllocs of 17:40) | `logs/usb-console/20261112-172703-port0.txt` `DD7:` lines |
| The 13 GiB boot's panic is such a page, wired by an IOPL and unwired through `vm_page_unwire()` | inferred (no other source of a gobbled page in k27 short of memory corruption; if `vm_pages[0]` is the window's first page, the page is about 9.2 GiB into the window, in RAM only the 13 GiB window has) | the registers: x0 - x13 = 2365929 x 48 (x13 taken for the `vm_pages` base, x11 = 0x30 the 48-byte `struct vm_page`) |
| With the 13 GiB window the kmem ranges are 2611 MiB each instead of about 4.5 GiB (the physmap takes the rest), so a full range is the other way a contiguous allocation fails after its search | verified (console) / inferred (that it happens) | the boot's VM range lines |
| The A/B image (same KC, 8 GiB ADT, 19:40) reserved a 64 MiB pool: the contiguous searches failed (the boot reservation of the volatile Data disk spreads wired pages), so its 2048 MiB request most likely leaked nothing | verified (console) / inferred | `logs/usb-console/20261112-194044-port0.txt` |

The fix: PianoA830 never asks XNU for a contiguous block above 2 GiB less a page (`NativeAdrenoContiguousMaxBytes`: the pool from
2048 MiB starts at 1024, the size every boot got; a contiguous BO above it is refused unasked; host-tested in `test_memory`), and k27
patch 0065 (`patches/k27/0065-*`) gives a failed contiguous allocation's pages back ungobbled with both counts fixed and refuses a size
the map would refuse before the search. Until 0065 is in the kernel, a contiguous allocation that fails on a full kmem range (13 GiB:
the shared data range holds the GPU's buffers and the Data disk's fallback chunks) can still leak.

**A kernel stack overflow** (2026-11-13, c3rp: `logs/native-runs/20261113-000631-loop`): `Invalid kernel stack pointer (probable
overflow). at pc 0xfffffff009fda0f0, lr ...` and `MACH Reboot`, no other panic text. pc in `el1_sp0_synchronous_vector_long` (+0xf0:
the vector's `stp x0, x1, [sp, #0x8]` after `sub sp, sp, #0x350`) means the exception frame itself faulted: SP_EL0 at the first
exception was sp + 0x350, and far = sp + 8 lies in the unmapped page below the thread's stack. A kernel thread has 16 KiB
(`KERNEL_STACK_SIZE` on this DEVELOPMENT kernel; the vector's check is SP above kstackptr - 0x4000, a little below the real bottom,
so a store just past the bottom faults first and the frame's store is the one reported). lr and x19-x28 are the overflowing
thread's (here `vm_map_store_lookup_entry`, a 64 KiB range: PianoCamera mapping DRV-2). A kext's own frames from its thread's entry:
`tooling/k27/check-kext-stack.py OBJECT.o ENTRY --indirect NAMES` (a static bound; XNU's IOKit mapping path below needed about 3-4
KiB); at run time `ml_stack_remaining()` (PianoCamera logs its least at the end of a run).
