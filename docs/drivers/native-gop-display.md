# NativeGopDisplay: macOS 27's display on the GOP framebuffer (k27 native kext)

> **Status 2026-11-11 (DroidVM cleanup):** the native path is NativeGopDisplay alone. It now carries its own `AppleParavirtGPU`
> nub, the default ADT has `/arm-io/gop-display` (`make-piano-adt.py --no-gop-display` leaves it out), and the native KC links
> neither DroidVMDisplay nor DroidVMGpu (`configs/k27-native-kexts.txt`). Build and run: `tooling/k27/build-native-image.sh`, then
> `tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img`. Measured: nub, display started on the boot
> framebuffer, root and Data unchanged. What follows is the original design note: Path A, `PIANO_DROP_KEXTS`,
> `build-display-test-kc.sh` and `native-run.sh` are gone.


Status: built and linked, 2026-11-11 (`out/k27/kc-k27-display-test.bin`, vtable check 0 problem slots). Not run on the
tablet. First step of [display.md](display.md) (M2a, GOP adoption); no DPU register is written.

## 1. Approach and why

The panel already scans a linear BGRA buffer: ABL's splash region (`0xfc800000`, 3200x2136, stride 12800, `dts/piano.dts`),
which SimpleFbDxe exposes as the GOP and DarwinBoot hands to XNU as the boot console (`PE_state.video`). XNU's console
draws there today. The display driver only has to give WindowServer an IOMobileFramebuffer and copy each swap's surface
into that buffer with the CPU.

The base is **macOS-DroidVM's DroidVMDisplay**. It is the display macOS 27 WindowServer already drove in the VM, on this
same 27.0.1 collection, and its stage 1 is exactly this CPU copy into the boot framebuffer. GopFramebuffer (UniXNU, d711e8c)
was read but not used. Its macOS display attributes come from DroidVMDisplay anyway, and it binds IOMFB through UniXNU's
XnuPrelink fixtures for the 27.3 collection, not ours. `package/NativeGopDisplay/UPSTREAM` lists what was taken and what
changed. In short, the virtio-gpu scanout and the GPU-work waits are gone, piano defaults are added, and every init step logs.

There are two ways to light the panel. Both need the same ADT node, and both publish **one** display:

| | Path A: DroidVMDisplay natively | Path B: NativeGopDisplay |
|---|---|---|
| What runs | DroidVMDisplay's own display (`AppleParavirtDisplay`). Natively its virtio transport finds no `/arm-io/virtio-gpu` and says so, then it copies with the CPU | `NativeGopDisplay` (this package), the same copy without the virtio code |
| Needs | the ADT node and a KC without NativeGopDisplay (any k27 KC has DroidVMDisplay) | the ADT node and a KC with NativeGopDisplay (and DroidVMDisplay, its nub) |
| Registry class | `AppleParavirtDisplay`, as in the VM (WindowServer's paravirt-display handling applies) | `NativeGopDisplay`. The name `AppleParavirtDisplay` stays DroidVMDisplay's, because its kext is loaded for the nub |
| Defaults | 120 Hz, 286x179 mm (`droidvm-display-hz`, `droidvm-display-mm`) | 60 Hz, 239x163 mm, from the ADT or `native-gop-display-*` |

Path A needs no new code: only the ADT changes. Path B is the driver to keep. It is piano-specific, carries no VM code,
logs every step, and is the base for the later DPU flips. NativeGopDisplay does not run beside DroidVMDisplay's display. Its
probe score (5000) makes IOKit try it first on DroidVMDisplay's nub. Its probe refuses in three cases: with boot-arg
`native-gop-display=0`, on a node without the `native-gop-display` marker, or with a virtio-gpu present (the VM). DroidVMDisplay's
display then takes the nub as before.

**Drop DroidVMGpu for the display tests** (`PIANO_DROP_KEXTS="DroidVMGpu"`, keep DroidVMDisplay as the nub). The native
runs of 2026-11-11 showed what DroidVMGpu does. With it, WindowServer reads MetalPluginName and loads the plugin. The plugin
cannot get a device (§6.1), and WindowServer exits; it was relaunched 6 times in 240 s. Without DroidVMGpu and without a display,
WindowServer launched once and stayed up.

## 2. How WindowServer finds it

1. `tooling/adt/make-piano-adt.py --gop-display` adds `/arm-io/gop-display` from `configs/k27-gop-display.toml`. Its
   `compatible` is exactly `paravirtualizedgraphics,gpu`, as the VM's `/arm-io/display`. It also carries the marker
   `native-gop-display`, `panel-width-mm`/`panel-height-mm`, `refresh-hz` and the `framebuffer-*` description. It has no
   `reg`, so the arm-io window does not change. Everything else is byte for byte the default ADT (checked with `adt-dump.py`).
2. ArmDTPlatform (in the kernel) creates an IOPlatformDevice nub for every arm-io child. DroidVMDisplay's
   `AppleParavirtGPU` personality matches the nub by `IONameMatch`, sets `DisplayPortCount = 1` and registers.
3. IOKit matches the display on that `AppleParavirtGPU`. NativeGopDisplay (score 5000) is probed first. If it refuses,
   DroidVMDisplay's display (score 0) is next. Both are in the default match category, so exactly one starts.
4. `IOMobileFramebufferLegacy::start` waits for IOSurfaceRoot, calls `start_hardware` (the display attributes, one
   3200x2136 timing, one 8-bit SDR colour element, power states, `hotPlug_notify(1)`), creates the default surface and registers.
5. WindowServer: IOMobileFramebuffer.framework (`iomfb_populate_all_display_infos`) walks `/arm-io`. For the child
   compatible with `paravirtualizedgraphics,gpu` it sums the `DisplayPortCount` of the `AppleParavirtGPU` services (1). It
   then waits for that many IOMobileFramebuffer services and opens ours. Swaps arrive through the family. On each VBL tick
   (an `IOTimerEventSource` at `refresh-hz`; there is no hardware vblank) the newest queued swap is programmed. Its largest
   surface goes to the copy thread, and the swap completes at once.

## 3. Boot args

| Arg | Default | Effect |
|---|---|---|
| `native-gop-display=0` / `=1` | the ADT marker (1 in `configs/k27-gop-display.toml`) | 0: NativeGopDisplay leaves the nub to DroidVMDisplay's display (Path A in the same KC); 1: it takes it. The opt-in is the ADT: the default ADT has no display node, so nothing changes without `--gop-display` |
| `native-gop-display-hz=N` | ADT `refresh-hz`, else 60 | rate told to WindowServer and of the VBL timer (30..240). The panel stays at ABL's 120 Hz |
| `native-gop-display-mm=WxH` | ADT, else 239x163 | physical size, so the density and macOS's HiDPI ("looks like 1600x1068") modes |
| `native-gop-display-bars=1` | off | 8 colour bars instead of the solid colour until the first frame (left to right: white, yellow, cyan, green, magenta, red, blue, black) to check channel order |
| `native-gop-display-refresh=0` | 1 | stop re-copying the shown surface on ticks 1, 2, 4, 8, 16 and 30 after a swap (only useful for a GPU renderer) |

No boot arg is needed for Path B. Everything else is the native set as it is (`K27_ARGS` in
`tooling/bootimg/build-ram-images.py`). For a log of the WindowServer stage, give the run more than the default 45 s
(`PIANO_RESET_AFTER=150` or more). The extra boot args above need `PIANO_EXTRA_ARGS`, which is new on this branch.

## 4. Build commands

Shortest, with the main worktree's own tooling (it honours `PIANO_DROP_KEXTS`, `PIANO_EXTRA_KEXTS` and `PIANO_ADT`). It
writes the default outputs `out/k27/kc-k27-native.bin` and `out/ram`, so only the session that owns them should run it.
`B` below is this branch's checkout:

```sh
B/tooling/k27/build-native-gop-display.sh                  # kext objects (already built 2026-11-11)
python3 B/tooling/adt/make-piano-adt.py out/adt/piano-k27-gop-display.adt --unique-chip-id 47554e5941480001 --data --trust --gop-display
PIANO_DROP_KEXTS="DroidVMGpu" PIANO_EXTRA_KEXTS="B/tooling/k27/kext/NativeGopDisplay.json" PIANO_ADT=out/adt/piano-k27-gop-display.adt \
  PIANO_RESET_AFTER=150 tooling/k27/build-native-image.sh   # Path B; leave out PIANO_EXTRA_KEXTS for Path A
```

The same build without touching the default outputs, from the UniXNU-piano checkout with this branch merged
(`K27_TREE` defaults to `droidvm-build/macOS-DroidVM`):

```sh
# kext objects only -> $K27_TREE/out/x12377/native-kexts/NativeGopDisplay (generates IOMobileFramebufferABI.h from out/kc/kc27.bin)
tooling/k27/build-native-gop-display.sh
# KC: build-native-image.sh's set (DroidVM defaults + NativeUfs [+ tooling/k27/kext/NativeRamBlock.json when present]) + NativeGopDisplay.
# Compiles NativeGopDisplay itself, takes every other kext as built, never writes kc-k27-native.bin.
PIANO_DROP_KEXTS="DroidVMGpu" tooling/k27/build-display-test-kc.sh   # -> out/k27/kc-k27-display-test.bin (+ .log .nm .sha256 .natives.json .kext.log)
# ADT with the display node (the default out/adt/piano-k27.adt is untouched)
python3 tooling/adt/make-piano-adt.py out/adt/piano-k27-gop-display.adt --unique-chip-id 47554e5941480001 --data --trust --gop-display
# RAM image in its own directory (Path B; PIANO_EXTRA_ARGS="native-gop-display=0" gives Path A from the same KC)
PIANO_RESET_AFTER=150 python3 tooling/bootimg/build-ram-images.py \
  --fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --out out/ram-display-test --kc out/k27/kc-k27-display-test.bin \
  --trust out/inputs/kernel.macho.trustcache --seed out/inputs/scratch-data.img --adt out/adt/piano-k27-gop-display.adt --utc "$(date +%s)"
python3 tooling/bootimg/verify-ram-images.py out/ram-display-test/build-A-bundled.json
# then, as for every native run (the tablet's owner session): tooling/native/native-run.sh out/ram-display-test/boot-piano-macos-ram-A-bundled.img
```

`PIANO_EXTRA_ARGS` is new on this branch (an additive line in `build-ram-images.py`). The command line is 878 bytes
today, and DarwinBoot takes at most 1023.

Built on 2026-11-11 (offline, not booted). Inputs: kernel `kernel-k27-0058-native.macho` (sha256 a5204d95…) and the main
worktree's then-current native set, minus DroidVMGpu, plus NativeGopDisplay. That set includes `tooling/k27/kext/NativeRamBlock.json`
and `data-persist=2`. Outputs:
- `out/k27/kc-k27-display-test.bin`: sha256 ce373b20…
- `out/adt/piano-k27-gop-display.adt`: sha256 c2a0ee63…
- `out/ram-display-test/boot-piano-macos-ram-A-bundled.img`: sha256 a78c3faa…, built with `piano-reset-after=150`;
  `verify-ram-images.py` PASS. Run it like any native image.

## 5. Progress in the pstore console mailbox

The lines below are in order (`tooling/native/xnu-log.py`, or `xnu-console.txt` of a run). `DroidVMDisplay: AppleParavirtGPU on gop-display, 1 display port` comes first, from
the nub. Then:

```
NativeGopDisplay: probe on AppleParavirtGPU (ADT gop-display): taking it         | "...: off (boot-arg native-gop-display=0)..." -> Path A lines
NativeGopDisplay: start on AppleParavirtGPU (ADT gop-display)
NativeGopDisplay: boot framebuffer 0xfc800000 3200x2136 row 12800 depth 32       | "no boot framebuffer" / "unusable framebuffer"
NativeGopDisplay: framebuffer mapped write-combined at 0x... (27340800 bytes)    -> the panel turns blue-grey (or shows the bars)
NativeGopDisplay: framebuffer cleared
NativeGopDisplay: copy thread started
NativeGopDisplay: display attributes published (one 3200x2136@60 timing, 8-bit SDR sRGB)
NativeGopDisplay: hardware started (VBL timer 60 Hz, power states registered, hot plug 1)
NativeGopDisplay: started, display 3200x2136 at 60 Hz, 239x163 mm, with refresh copies
NativeGopDisplay: display on                                                      <- system and API power states both on
NativeGopDisplay: first swap submitted
NativeGopDisplay: swap layers 0x1: 0: 3200x2136 0x52476841                        (format: 'BGRA', 'RGhA' or 'b3a8')
NativeGopDisplay: first frame (swap N); boot console released from the screen
NativeGopDisplay: surface 3200x2136 row ... offset ... format ... memory ...
NativeGopDisplay: first copy 3200x2136 format ... into the framebuffer in N us
NativeGopDisplay: N swaps submitted, N shown, ... copies mean N us (... convert N write N) ...   (every ~10 s while swaps arrive)
```

Reading a run: up to `started, display ...` is the kernel side alone. `first swap submitted` means WindowServer composited
a frame. A WindowServer that exits about 30 s after `started` with no swap is the Metal wait (§6.1).

Failure lines: `IOMobileFramebufferLegacy::start failed`, `createDisplayAttributes failed (...)`, `swap_wait(...) = 0x...`
(the first 20), `surface format 0x... is not copied`. Path A prints the matching `DroidVMDisplay: ...` lines (`scanout ...`,
`virtio-gpu scanout off: the transport did not start`, `hardware started`, `started, display ...`, `display on`, `first swap submitted`).

## 6. Risks

1. **WindowServer needs a Metal device. Natively there is none, and this blocks the desktop, not the display.** In the
   VM, a display without Metal meant WindowServer aborting every ~30 s ("No suitable Metal devices present after 30
   seconds": macOS-DroidVM `docs/TODO-gpu.md` run 18, `k27/TODO.md` try23/24). Natively, with DroidVMGpu and no
   display, the discovery works but ends the same way: WindowServer exits after its opens and is relaunched, 6 times in
   240 s. Without DroidVMGpu it stays up, but there was no display in that run either. Expect it to open the display now,
   then wait for Metal, and probably abort after 30 s as in the VM. The display test answers this. DroidVMGpu's
   `AGXAccelerator` is found and the plugin opens it, but the opens cannot become a device:
   - `0xe00002d5` is `kIOReturnBusy` from `native_adreno_new_user_client`. It is
     `refusal=kIOReturnBusy` at `if(!connection->mount(kernel))` (NativeAdreno 805c7e4 + `patches/droidvm-gpu`,
     exported to `macOS-DroidVM/out/x12377/src-gpu/drivers/NativeAdreno/NativeAdrenoPublish.cpp:703`).
     `NativeAdrenoMaxClients` is 3 (:112), and slot 0 is kept for the process named `backboardd`, the iOS render server
     (:231). So WindowServer can only get slots 1 and 2. Its first two type-0x4147 connections hold them, and the third
     open finds no slot. The per-name cap (8 opens) was not reached.
   - A third slot would not help. The type-0x4147 core sits on DroidVMGpu.cpp's never-attached NativeAdrenoVm, Ring and
     Timer, so every map or submit answers NotReady or Unsupported. Its `gDeviceInfo` is all zeros.
   - Type 0x4149 (virtgpu) opens with 0 because `droidvm_virtgpu_call` is linked from DroidVMDisplay's kext. Every
     0x72xx call then returns `kIOReturnNotReady` (`DroidVMVirtioGpu3D.cpp:832`), because the transport only starts from
     `/arm-io/virtio-gpu` (`DroidVMVirtioGpu.cpp:191`).
   - What WindowServer needs is an MTLDevice from the plugin that can allocate and render. There are three options:
     (a) the real A830 driver ([gpu.md](gpu.md), M5);
     (b) a software Vulkan device under metal2vulkan in place of Turnip, for example Mesa's lavapipe. This is user-space
     work: an arm64e build, LLVM, and JIT in WindowServer under AMFI;
     (c) a plugin device without an accelerator port. Per the plugin's own note (`overlay/metal/NativeAdrenoMetalPlugin.mm`),
     SkyLight then composites in software (CompositorSW, `WSGetDefaultMetalAccelerator`). Built 2026-11-11, not yet run:
     `package/NativeSoftGpu` (the accelerator, `PIANO_EXTRA_KEXTS`) and `package/NativeSoftMetal` (the plugin: accelerator port 0,
     no command queue, so Core Animation's display renders in software too), staged by `tooling/k27/stage-soft-metal.py`
     (`PIANO_SEED`, `PIANO_TRUST`). The macOS 26.6 SkyLight on the build Mac agrees with the note. With no device at all,
     WindowServer aborts after 30 s. With a device whose port is 0, the display's compositor is CompositorSW.
   No kernel change in this package addresses this.
2. **'RGhA' frames.** The native ADT keeps `vmm-present = 1`, so Core Animation keeps a half-float framebuffer: 54.7 MB a
   frame, converted per pixel. The periodic line's `convert` time shows the cost. `vmm-present = 0` gives 'BGRA' (as in
   the VM) but changes MobileGestalt's IsVirtualDevice.
3. **CPU cost and tearing.** A 3200x2136 BGRA copy is 27 MB. Nothing syncs to the panel's scan (no vblank, 120 Hz
   panel vs a 60 Hz timer), so motion can tear.
4. **Path B is not `AppleParavirtDisplay`.** WindowServer's paravirt handling (`SLCADisplay::read_pv_preferences`,
   naming the display by `IOMFBUUID`) does not apply to it. The display UUID is then WindowServer's own, kept in the Data
   volume, and a future touch driver cannot name the display by `IOMFBUUID` the way DroidVMInput does. If WindowServer
   treats it differently from the VM's display, Path A is the fallback with the proven class.
5. **The nub is DroidVMDisplay's.** A native KC without DroidVMDisplay has no `AppleParavirtGPU`. NativeGopDisplay would
   then need its own nub class of that name: about 10 lines, plus a personality on the ADT node.
6. **Framebuffer mapping.** The splash region lies outside XNU's DRAM window (`memory@8c0000000`). It is mapped as I/O,
   write-combined, as the console maps it. The base comes from `PE_state.video`. The ADT's `framebuffer-*` values only
   cross-check it, or stand in when there is no console info.
7. **No power control.** "display off" stops nothing on the panel. There is no DPMS or backlight (display.md, later).
8. The boot console keeps drawing over the solid colour until WindowServer's first frame
   (`PE_initialize_console(kPEDisableScreen)`).

## 7. Files

- `package/NativeGopDisplay/{NativeGopDisplay.cpp,UPSTREAM,LICENSE}` (BSD-2-Clause-Patent, derived from DroidVMDisplay)
- `tooling/k27/kext/NativeGopDisplay.json`: personality `IOClass NativeGopDisplay`, `IOProviderClass AppleParavirtGPU`,
  `IOProbeScore 5000`; vtable check of all 483 IOMobileFramebufferLegacy slots
- `tooling/k27/build-native-gop-display.sh`, `tooling/k27/build-display-test-kc.sh`
- `tooling/adt/make-piano-adt.py --gop-display`, `configs/k27-gop-display.toml`
- `tooling/bootimg/build-ram-images.py`: `PIANO_EXTRA_ARGS`

## 8. Licence note

DroidVMDisplay (macOS-DroidVM) and GopFramebuffer are BSD-2-Clause-Patent. Code was taken from DroidVMDisplay only. No GPL
code, and no Linux code, was read into this package. The IOMFB declarations are generated from the collection's symbols
at build time, not copied.
