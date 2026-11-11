# NativeGopDisplay: macOS 27's display on the GOP framebuffer (k27 native kext)

> **Status 2026-11-11:** the native display is NativeGopDisplay alone. It carries its own `AppleParavirtGPU` nub, the default
> ADT has `/arm-io/gop-display` (`make-piano-adt.py --no-gop-display` leaves it out), and it is in the native kext list
> (`configs/k27-native-kexts.txt`). Build and run: `tooling/k27/build-native-image.sh`, then
> `tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img`. Measured: nub, display started on the boot
> framebuffer, root and Data unchanged.

First step of [display.md](display.md) (M2a, GOP adoption); no DPU register is written.

## 1. Approach and why

The panel already scans a linear BGRA buffer: ABL's splash region (`0xfc800000`, 3200x2136, stride 12800, `dts/piano.dts`),
which SimpleFbDxe exposes as the GOP and DarwinBoot hands to XNU as the boot console (`PE_state.video`). XNU's console
draws there today. The display driver only has to give WindowServer an IOMobileFramebuffer and copy each swap's surface
into that buffer with the CPU.

The base is the VM-era display kext (provenance: `package/NativeGopDisplay/UPSTREAM`). macOS 27's WindowServer already drove
it on this same 27.0.1 collection, and its stage 1 is exactly this CPU copy into the boot framebuffer. GopFramebuffer (UniXNU,
d711e8c) was read but not used: its macOS display attributes follow the same kext, and it binds IOMFB through UniXNU's
XnuPrelink fixtures for the 27.3 collection, not ours. `UPSTREAM` lists what was taken and what changed. In short, the
virtio-gpu scanout and the GPU-work waits are gone, piano defaults are added (60 Hz, 239x163 mm, from the ADT or
`native-gop-display-*`), every init step logs, and the `AppleParavirtGPU` nub is this package's own. It publishes **one**
display, registry class `NativeGopDisplay`, and is the base for the later DPU flips. Its probe refuses in three cases: with
boot-arg `native-gop-display=0`, on a node without the `native-gop-display` marker, or with a `/arm-io/virtio-gpu` node
present (a VM's ADT); the nub then has no display.

A native run of 2026-11-11 with the VM-era GPU kext linked showed why a stub accelerator is worse than none: WindowServer
read its MetalPluginName and loaded the plugin, the plugin could not get a device (§6.1), and WindowServer exited; it was
relaunched 6 times in 240 s. Without it and without a display, WindowServer launched once and stayed up.

## 2. How WindowServer finds it

1. `tooling/adt/make-piano-adt.py` adds `/arm-io/gop-display` from `configs/k27-gop-display.toml` (unless `--no-gop-display`).
   Its `compatible` is exactly `paravirtualizedgraphics,gpu`, the string the framework looks for. It also carries the marker
   `native-gop-display`, `panel-width-mm`/`panel-height-mm`, `refresh-hz` and the `framebuffer-*` description. It has no
   `reg`, so the arm-io window does not change. Everything else is byte for byte the default ADT (checked with `adt-dump.py`).
2. ArmDTPlatform (in the kernel) creates an IOPlatformDevice nub for every arm-io child. NativeGopDisplay's own
   `AppleParavirtGPU` personality matches the nub by `IONameMatch`, sets `DisplayPortCount = 1` and registers.
3. IOKit matches the display on that `AppleParavirtGPU`: NativeGopDisplay (score 5000), unless its probe refuses (§1).
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
| `native-gop-display=0` / `=1` | the ADT marker (1 in `configs/k27-gop-display.toml`) | 0: NativeGopDisplay leaves the nub (no display); 1: it takes it even on a node without the marker |
| `native-gop-display-hz=N` | ADT `refresh-hz`, else 60 | rate told to WindowServer and of the VBL timer (30..240). The panel stays at ABL's 120 Hz |
| `native-gop-display-mm=WxH` | ADT, else 239x163 | physical size, so the density and macOS's HiDPI ("looks like 1600x1068") modes |
| `native-gop-display-bars=1` | off | 8 colour bars instead of the solid colour until the first frame (left to right: white, yellow, cyan, green, magenta, red, blue, black) to check channel order |
| `native-gop-display-refresh=0` | 1 | stop re-copying the shown surface on ticks 1, 2, 4, 8, 16 and 30 after a swap (only useful for a GPU renderer) |
| `native-gop-display-thumb=N` | 30 | a thumbnail of the frame on the panel (see §5) on the first frame and then every N s; 0: none |
| `native-gop-display-copy=BITS` | 0 | the copy's cost (§6 risk 3): bit 0 skips the refresh copies of a swap whose copy waited for the GPU's fence and saw it retired (the last one, tick 30, still runs); bit 1 maps surfaces cacheable and cleans/invalidates the rows by VA before each read. Not run on the tablet yet; the periodic `copy mode ...` line says what was saved |
| `piano-backlight=2` | off | (PianoBacklight's argument) each display power change is also handed to PianoBacklight ([backlight.md](backlight.md)) |
| `piano-dpu-pcc=1\|2` | off | (PianoDpu's, [dpu.md](dpu.md) §4.4) `swap_set_color_matrix` and `set_gamma_table` log each new matrix / table (raw words: B1d-0) and hand it to PianoDpu's PCC; without it both stay stubs |
| `piano-dpu-panel=1` | off | (PianoDpu's, §4.5) each display power change also goes to PianoDpu (`PianoDpuPanel`: the panel's light sleep), after the backlight's request |
| `piano-dpu-refresh=auto` | off | (PianoDpu's, §4.3) each frame shown is reported to PianoDpu (`PianoDpuSwap`: its 60/120 Hz policy) |

**The display power for other drivers** (2026-11-11, `docs/policy.md` §5.6; built and linked, not run): every power change call
publishes `piano-display-power` (boolean: on while both the system and WindowServer want the display on), whatever the boot
arguments, and a change of it sends `kIOMessageServicePropertyChange` (argument: the property's name) to the service's interested
clients (PianoPolicy, PianoKeyboard's `piano-keyboard-blank=1`). Before WindowServer's first call the property is absent, as before.
Nothing else changed: the backlight callout still needs `piano-backlight=2`, the DPU's `piano-dpu-panel=1`. The clients' handlers
run inside the framebuffer's gated power change, so they only record it (none may call back into this service there).

No boot arg is needed for the display. Everything else is the native set as it is (`K27_ARGS` in
`tooling/bootimg/build-ram-images.py`). For a log of the WindowServer stage, give the run more than the default 45 s
(`PIANO_RESET_AFTER=150` or more). The extra boot args above go in `PIANO_EXTRA_ARGS`.

## 4. Build

NativeGopDisplay is in the native kext list, so `tooling/k27/build-native-image.sh` builds and links it (and the default ADT
has its node). The objects alone: `tooling/k27/build-native-gop-display.sh` (-> `out/native-kexts/NativeGopDisplay`; it
generates `IOMobileFramebufferABI.h` from the KC base `out/kc/kc27.bin` of the k27 tree, `PIANO_K27_TREE`, cached in
`out/k27/iomfb-abi/`). The command line is under DarwinBoot's 1023 bytes (`build-ram-images.py` checks it).

First built 2026-11-11 (offline) on kernel `kernel-k27-0058-native.macho` (sha256 a5204d95…) in a display-test KC (sha256
ce373b20…, vtable check 0 problem slots) with its ADT (c2a0ee63…); since then it is part of every native KC.

## 5. Progress in the pstore console mailbox

The lines below are in order (`tooling/native/xnu-log.py`, or `xnu-console.txt` of a run). `NativeGopDisplay: AppleParavirtGPU nub on gop-display, 1 display port`
comes first, from the nub. Then:

```
NativeGopDisplay: probe on AppleParavirtGPU (ADT gop-display): taking it         | "...: off (boot-arg native-gop-display=0)..." -> no display
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
NativeGopDisplay: N swaps submitted, N shown, ..., last frame P% non-black mean #RRGGBB centre #RRGGBB; N copies mean N us (... convert N write N) ...
                                                                                  (every ~10 s while swaps arrive)
NativeGopDisplay: thumb frame N: 3200x2136 'BGRA', P% non-black, mean #RRGGBB, centre #RRGGBB, 80x27 cells of 40x79 px, ramp " .:-=+*#%@"
NativeGopDisplay: thumb 00 |....80 characters, one per cell: its mean luma, dark to bright, as " .:-=+*#%@"....|   (27 rows)
                                                                                  (on the first frame, then every native-gop-display-thumb s)
```

The "last frame" figures and the thumbnail come from every 4th pixel of every 4th row as written to the panel, after conversion, in
any source format. "Non-black" means a channel above 8 of 255. Before 2026-11-11 the line said `last frame N lit centre X`, and that
was counted only for converted formats ('RGhA', 'b3a8'): on a 'BGRA' display it always read 0, meaning "not measured", not black.
The thumbnail shows only the main layer. A cursor layer (`swap layers 0x3: ... 1: 128x128`) is not copied to the panel, so it is not in
the thumbnail either.

Reading a run: up to `started, display ...` is the kernel side alone. `first swap submitted` means WindowServer composited
a frame. A WindowServer that exits about 30 s after `started` with no swap is the Metal wait (§6.1).

Failure lines: `IOMobileFramebufferLegacy::start failed`, `createDisplayAttributes failed (...)`, `swap_wait(...) = 0x...`
(the first 20), `surface format 0x... is not copied`. The probe's refusal texts still end in "...'s display takes the nub" (the
VM-era display kext's name, kept in the binary); natively no other display is linked, so the nub then has none.

## 6. Risks

1. **WindowServer needs a Metal device. Natively there is none, and this blocks the desktop, not the display.** In the
   VM, a display without Metal meant WindowServer aborting every ~30 s ("No suitable Metal devices present after 30
   seconds", the VM-era GPU notes). Natively, with the VM-era GPU kext and no display, the discovery works but ends the same
   way: WindowServer exits after its opens and is relaunched, 6 times in 240 s. Without it WindowServer stays up, but there
   was no display in that run either. That kext's `AGXAccelerator` is found and the plugin opens it, but the opens cannot
   become a device:
   - `0xe00002d5` is `kIOReturnBusy` from `native_adreno_new_user_client`, at `if(!connection->mount(kernel))` in its
     NativeAdreno 805c7e4 sources (`NativeAdrenoPublish.cpp:703`). `NativeAdrenoMaxClients` is 3 (:112), and slot 0 is kept
     for the process named `backboardd`, the iOS render server (:231). So WindowServer can only get slots 1 and 2. Its first
     two type-0x4147 connections hold them, and the third open finds no slot. The per-name cap (8 opens) was not reached.
   - A third slot would not help. The type-0x4147 core sits on that kext's never-attached NativeAdrenoVm, Ring and Timer,
     so every map or submit answers NotReady or Unsupported. Its `gDeviceInfo` is all zeros.
   - Type 0x4149 (virtgpu) opens with 0, but every 0x72xx call then returns `kIOReturnNotReady`: its virtio-gpu transport
     only starts from `/arm-io/virtio-gpu`.
   - What WindowServer needs is an MTLDevice from the plugin that can allocate and render. There are three options:
     (a) the real A830 driver ([gpu.md](gpu.md), M5);
     (b) a software Vulkan device under metal2vulkan in place of Turnip, for example Mesa's lavapipe. This is user-space
     work: an arm64e build, LLVM, and JIT in WindowServer under AMFI;
     (c) a plugin device without an accelerator port, per the plugin's own note (`overlay/metal/NativeAdrenoMetalPlugin.mm`):
     SkyLight then composites in software (CompositorSW, `WSGetDefaultMetalAccelerator`). This is the path being taken:
     `package/NativeSoftGpu` + `package/NativeSoftMetal`, **[soft-metal.md](soft-metal.md)**, which has the evidence, the boot
     arguments and the test recipe. Measured 2026-11-11: WindowServer opened this display (`display on`) and took the device,
     then aborted with no swap submitted; the plugin now answers every device selector and reports the abort.
   No kernel change in this package addresses this.
2. **'RGhA' frames.** The native ADT keeps `vmm-present = 1`, so Core Animation keeps a half-float framebuffer: 54.7 MB a
   frame, converted per pixel. The periodic line's `convert` time shows the cost. `vmm-present = 0` gives 'BGRA' (as in
   the VM) but changes MobileGestalt's IsVirtualDevice.
3. **CPU cost and tearing.** A 3200x2136 BGRA copy is 27 MB. Nothing syncs to the panel's scan (no vblank, 120 Hz
   panel vs a 60 Hz timer), so motion can tear. Measured on the tablet (2026-11-11 runs, A830 path, BGRA): a copy takes
   ~39 ms, of which ~34 ms read the surface (~0.8 GB/s: single uncached loads), ~1.4 ms write the framebuffer (write-combined)
   and ~2.5 ms prepare/map. Each swap is followed by up to six refresh copies (ticks 1, 2, 4, 8, 16, 30), so even a blinking
   text cursor (about two swaps a second) can cost 12-14 copies a second, roughly half a CPU core. Evaluation (2026-11-11), for battery life:
   - *Damage regions:* the family's swap gives the driver whole layer surfaces; nothing in the IOMFB ABI this kext uses carries a
     dirty region. Comparing against the previous frame per tile still needs the whole source read, which is the cost (34 of
     39 ms); it would only save the 1.4 ms write. Not done.
   - *Zero-copy scanout* (the DPU's source address at the swap's surface, `display.md` §4 "Direct flip"): needs the DPU pipe
     readout (M2b, not run), the display SMMU stream's context under Gunyah (unknown) and page tables for surfaces that are not
     physically contiguous. The facts are not solid: not done. Design and the read-only survey that settles the facts (B1a,
     `piano-dpu=1`): [dpu.md](dpu.md) §4.2.
   - *What is done, opt-in (`native-gop-display-copy`):* bit 0 drops the refresh copies a GPU fence already covers (the fence
     wait guarantees the GPU finished the frame before the copy read it; the last refresh, tick 30, stays as a safety net):
     up to five of the seven copies per swap. Bit 1 reads through a cacheable mapping after a by-VA clean+invalidate (DC CIVAC)
     of exactly the rows read: expected several times faster reads, at the cost of the invalidation pass, measured by the
     `copy mode ...` line (`cache invalidations mean N us`) against the read time of the periodic line. Test both on the tablet
     before any default changes (`PIANO_EXTRA_ARGS="... native-gop-display-copy=1"`, then `=3`; compare `copies mean ... read
     ...` and the `copy mode` line with a run without it, and look for stale tiles on the panel with `=3`).
   - Left: the 60 Hz VBL timer runs while nothing is shown (60 wakeups a second at idle).
4. **The display is not `AppleParavirtDisplay`.** WindowServer's paravirt handling (`SLCADisplay::read_pv_preferences`,
   naming the display by `IOMFBUUID`) does not apply to it. The display UUID is then WindowServer's own, kept in the Data
   volume, and a touch driver cannot name the display by `IOMFBUUID` the way the VM-era HID kext did.
5. **The nub** (resolved 2026-11-11): NativeGopDisplay carries its own `AppleParavirtGPU` class and personality on the ADT
   node, so the native KC needs no other display kext.
6. **Framebuffer mapping.** The splash region lies outside XNU's DRAM window (`memory@8c0000000`). It is mapped as I/O,
   write-combined, as the console maps it. The base comes from `PE_state.video`. The ADT's `framebuffer-*` values only
   cross-check it, or stand in when there is no console info.
7. **No power control.** "display off" stops nothing on the panel. There is no DPMS (display.md, later); with `piano-backlight=2` the
   display power goes to PianoBacklight, which turns the backlight off and on (backlight.md); with `piano-dpu-panel=1` also to
   PianoDpu, which puts the panel to sleep and stops the DPU's timing engines (dpu.md §4.5, not run yet).
8. The boot console keeps drawing over the solid colour until WindowServer's first frame
   (`PE_initialize_console(kPEDisableScreen)`).

## 7. Files

- `package/NativeGopDisplay/{NativeGopDisplay.cpp,UPSTREAM,LICENSE}` (BSD-2-Clause-Patent; provenance in `UPSTREAM`)
- `tooling/k27/kext/NativeGopDisplay.json`: personality `IOClass NativeGopDisplay`, `IOProviderClass AppleParavirtGPU`,
  `IOProbeScore 5000`; vtable check of all 483 IOMobileFramebufferLegacy slots
- `tooling/k27/build-native-gop-display.sh`
- `tooling/adt/make-piano-adt.py` (`--no-gop-display` leaves the node out), `configs/k27-gop-display.toml`
- `tooling/bootimg/build-ram-images.py`: `PIANO_EXTRA_ARGS`

## 8. Licence note

The VM-era display kext (`UPSTREAM`) and GopFramebuffer are BSD-2-Clause-Patent. Code was taken from the former only. No GPL
code, and no Linux code, was read into this package. The IOMFB declarations are generated from the collection's symbols
at build time, not copied.
