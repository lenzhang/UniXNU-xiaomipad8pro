# NativeGopDisplay: macOS 27's display on the GOP framebuffer (k27 native kext)

> **Status 2026-11-11:** the native display is NativeGopDisplay alone. It carries its own `AppleParavirtGPU` nub, the default
> ADT has `/arm-io/gop-display` (`make-piano-adt.py --no-gop-display` leaves it out), and it is in the native kext list
> (`configs/k27-native-kexts.txt`). Build and run: `tooling/k27/build-native-image.sh`, then
> `tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img`. Measured: nub, display started on the boot
> framebuffer, root and Data unchanged.
>
> **2026-11-12 (built, host-tested, KC links; not run on the tablet yet):** the copy is a composition of the swap's layers (§5a):
> WindowServer's cursor (layer 1) is drawn over the frame, and a cursor-only swap redraws only its two rectangles. Against the
> flicker and white bands at the login of 2026-11-11: a frame the GPU has not finished is never copied (the previous one stays,
> the ticks retry), and a swap completes only once its own job is done, so a surface goes back to WindowServer only after the
> copy of the frame that replaced it. All on by default; `native-gop-display-cursor=0` and `native-gop-display-defer=0` give the
> old behaviour back (the fence rule stays).

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
   (an `IOTimerEventSource` at `refresh-hz`; there is no hardware vblank) the newest queued swap is programmed. Its layers go to
   the copy thread as a job (§5a), and the swap completes once that job is done (`native-gop-display-defer`).

## 3. Boot args

| Arg | Default | Effect |
|---|---|---|
| `native-gop-display=0` / `=1` | the ADT marker (1 in `configs/k27-gop-display.toml`) | 0: NativeGopDisplay leaves the nub (no display); 1: it takes it even on a node without the marker |
| `native-gop-display-hz=N` | ADT `refresh-hz`, else 60 | rate told to WindowServer and of the VBL timer (30..240). The panel stays at ABL's 120 Hz |
| `native-gop-display-mm=WxH` | ADT, else 239x163 | physical size, so the density and macOS's HiDPI ("looks like 1600x1068") modes |
| `native-gop-display-bars=1` | off | 8 colour bars instead of the solid colour until the first frame (left to right: white, yellow, cyan, green, magenta, red, blue, black) to check channel order |
| `native-gop-display-refresh=0\|1\|2` | 1 | the refresh copies of the frame on screen on ticks 1, 2, 4, 8, 16 and 30 after a swap (for a GPU that finishes after the swap): 0 none; 1 all six after a frame no GPU fence covered (no PianoA830, or the wait gave up), only the last (tick 30) after one it did; 2 all six after every frame (the default before 2026-11-12) |
| `native-gop-display-cursor=0\|1\|2` | 1 | layer 1, WindowServer's cursor (§5a): 0 not drawn (before 2026-11-12: the pointer is invisible); 1 its glyph rectangle (IOSurface's origin and extents) blended at the layer's origin; 2 the whole cursor surface (should the extents ever be wrong) |
| `native-gop-display-defer=0\|1\|2` | 2 | when a swap completes (§5a): 0 at once (before 2026-11-12: the copy thread may still read a surface WindowServer draws into again); 1 at once unless the copy thread reads a surface the swap takes off the screen, then once that read is done; 2 once the swap's own job is done (flip mode always uses 2) |
| `native-gop-display-gpu-fence-ms=N` | 1000 | the hang guard of the fence wait (10..10000): a job whose swap's GPU fence has not passed by then is not drawn (the previous frame stays), its swap completes, and the ticks retry until the fence passes. Before 2026-11-12: 50 ms, then copied anyway |
| `native-gop-display-vbl=0` | 1 | the VBL tick (60 Hz) runs always, as before 2026-11-12. With 1 it stops while the display is off (`piano-display-power` false) once nothing is queued, waiting, refreshed or retried: a submitted swap starts it again, so does the display's power-on (line `display off: the VBL tick stopped, nothing queued or to refresh (stop N; ...)`). `docs/power.md` "Idle wakeups by driver" |
| `native-gop-display-thumb=N` | 30 | a thumbnail of the frame on the panel (see §5) on the first frame and then every N s; 0: none |
| `native-gop-display-copy=BITS` | 0 | the copy's cost (§6 risk 3): bit 0 skips the refresh copies of a frame whose copy waited for the GPU's fence and saw it retired (the last one, tick 30, still runs): since 2026-11-12 that is the default (`native-gop-display-refresh=1`), and bit 0 turns `=2` back into it; bit 1 maps surfaces cacheable and cleans/invalidates by VA exactly the rows (or row spans) each job reads, after its fence wait. Bit 1 not run on the tablet yet; the periodic `copy mode ...` line says what it cost |
| `piano-backlight=2` | off | (PianoBacklight's argument) each display power change is also handed to PianoBacklight ([backlight.md](backlight.md)) |
| `piano-dpu-pcc=1\|2` | off | (PianoDpu's, [dpu.md](dpu.md) §4.4) `swap_set_color_matrix` and `set_gamma_table` log each new matrix / table (raw words: B1d-0) and hand it to PianoDpu's PCC; without it both stay stubs |
| `piano-dpu-panel=1` | off | (PianoDpu's, §4.5) each display power change also goes to PianoDpu (`PianoDpuPanel`: the panel's light sleep), after the backlight's request |
| `piano-dpu-refresh=auto` | off | (PianoDpu's, §4.3) each frame shown is reported to PianoDpu (`PianoDpuSwap`: its 60/120 Hz policy) |
| `piano-dp=1` + `piano-dp-fake=S` / `piano-dp-fake-boot=1` | off | (PianoDp's D6, [displayport.md](displayport.md) §8.1) the nub reports 2 display ports and a second instance (`NativeGopDisplayExternal`, lines `NativeGopDisplay-ext:`) with a RAM surface, hot-plugged S s after its start (or connected from it); `-unplug`, `-cycles`, `-mode`, `-mm`, `-primary` shape it. No hardware, no `piano-display-power` of its own; the panel's instance is unchanged. Drivers that find this display by class may find the second one: D6 runs without them |

**The display power for other drivers** (2026-11-11, `docs/policy.md` §5.6; built and linked, not run): every power change call
publishes `piano-display-power` (boolean: on while both the system and WindowServer want the display on), whatever the boot
arguments, and a change of it sends `kIOMessageServicePropertyChange` (argument: the property's name) to the service's interested
clients (PianoPolicy, PianoKeyboard's `piano-keyboard-blank=1`). Before WindowServer's first call the property is absent, as before.
Nothing else changed: the backlight callout still needs `piano-backlight=2`, the DPU's `piano-dpu-panel=1`. The clients' handlers
run inside the framebuffer's gated power change, so they only record it (none may call back into this service there).
Since 2026-11-12 the panel's instance also publishes `piano-display-power-messages` = true at its start, before it registers: the
capability its readers check (PianoPolicy listens to the messages and drops its 5 s display tick when it is there; an older
NativeGopDisplay without it is polled as before, `docs/policy.md` §4). D6's second instance publishes neither property.

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
NativeGopDisplay: composition: layer 0 the frame, layer 1 the cursor blended (its glyph rectangle, premultiplied alpha); a swap
                  completes once its frame or cursor is on the panel; refresh copies after frames no GPU fence covered (the last
                  only after covered ones); a frame is copied only once the GPU retired its fence (hang guard 1000 ms)
NativeGopDisplay: started, display 3200x2136 at 60 Hz, 239x163 mm, with refresh copies
NativeGopDisplay: display on                                                      <- system and API power states both on
NativeGopDisplay: first swap submitted
NativeGopDisplay: swap layers 0x7 first seen (swap 1)                             (each new combination once)
NativeGopDisplay: layer 0: 3200x2136 'BGRA' (swap 1): the frame, composed at (0, 0)   (each new kind of layer once: 'BGRA', 'RGhA', 'b3a8')
NativeGopDisplay: layer 1 off (swap 1): the cursor hidden
NativeGopDisplay: layer 2 off (swap 1)
NativeGopDisplay: first frame (swap N); boot console released from the screen
NativeGopDisplay: surface 3200x2136 row ... offset ... format ... memory ...
NativeGopDisplay: first copy 3200x2136 format ... into the framebuffer in N us
NativeGopDisplay: N swaps submitted, N shown, ..., last frame P% non-black mean #RRGGBB centre #RRGGBB; N copies mean N us (... convert N write N) ...
                                                                                  (every ~10 s while swaps arrive)
NativeGopDisplay: thumb frame N: 3200x2136 'BGRA', P% non-black, mean #RRGGBB, centre #RRGGBB, 80x27 cells of 40x79 px, ramp " .:-=+*#%@"
NativeGopDisplay: thumb 00 |....80 characters, one per cell: its mean luma, dark to bright, as " .:-=+*#%@"....|   (27 rows)
                                                                                  (on the first frame, then every native-gop-display-thumb s)
NativeGopDisplay: layer 1: 128x128 'BGRA' at (X, Y) (swap N): the cursor, blended over the frame at that origin (premultiplied alpha)
NativeGopDisplay: cursor: surface 128x128, glyph X,Y WxH, hot spot (HX, HY) (origin-and-extents words ...); its pixel (0, 0) at
                  (X, Y) on the panel                                              (the first few cursor shapes)
NativeGopDisplay: cursor: N updates of its old and new rectangles, mean N us max N us, last at (X, Y)       (in the 10 s block)
NativeGopDisplay: N swaps completed once the copy thread was done (native-gop-display-defer=2), mean N us max N us after they were
                  programmed
NativeGopDisplay: N refresh copies not needed (their frame's fence wait saw the GPU done)
NativeGopDisplay: N jobs left undrawn (the GPU had not finished their frame within 1000 ms: the previous one stayed), N tick
                  retries, N refresh copies cut short by a swap                   (only when any)
NativeGopDisplay: the GPU had not finished a frame after 1000 ms (fence N): not copied, the previous frame stays; retried at the
                  next ticks (N so far)                                           (the first 8, then every 100th)
```

The "last frame" figures and the thumbnail come from every 4th pixel of every 4th row as written to the panel, after conversion, in
any source format. "Non-black" means a channel above 8 of 255. Before 2026-11-11 the line said `last frame N lit centre X`, and that
was counted only for converted formats ('RGhA', 'b3a8'): on a 'BGRA' display it always read 0, meaning "not measured", not black.
Before 2026-11-12 the thumbnail and the panel showed only the main layer (a cursor layer, `swap layers 0x3: ... 1: 128x128`, was
not copied); since then both show the composition, cursor included.

Reading a run: up to `started, display ...` is the kernel side alone. `first swap submitted` means WindowServer composited
a frame. A WindowServer that exits about 30 s after `started` with no swap is the Metal wait (§6.1).

Failure lines: `IOMobileFramebufferLegacy::start failed`, `createDisplayAttributes failed (...)`, `swap_wait(...) = 0x...`
(the first 20), `a frame|a cursor surface WxH format 0x... row ... is not read` (the first 8), `no work-loop source for the deferred
completions: every swap completes at once, flip mode off`. The probe's refusal texts still end in "...'s display takes the nub" (the
VM-era display kext's name, kept in the binary); natively no other display is linked, so the nub then has none.

## 5a. Layers, the cursor and the copy race (2026-11-12)

**What the logs of 2026-11-11 showed** (`logs/usb-console/20261111-232732`, `-234057`, `20261112-000758`, `-003828`, grep
`NativeGopDisplay`): WindowServer on the A830 swaps the cursor as a layer of its own. `swap layers 0x2: 1: 128x128 0x42475241`
(cursor-only swaps, one per pointer move, hundreds a minute), `0x3: 0: 3200x2136 ... 1: 128x128` (a frame with the cursor),
`0x3`/`0x2` with layer 1 empty (the cursor hidden, as while typing), `0x7` with layers 1 and 2 empty (its first swap) and `0x5` with
layers 0 and 2 empty (right before `display off`). The driver copied only the largest surface, so the pointer never reached the panel.
At the same time every swap completed at once while the copy thread (about 20 ms a copy, plus up to six refresh copies after each
frame) could still be reading the surface the swap had just taken off the screen; the family hands such a surface back to WindowServer
at the completion, and WindowServer draws its next frame into it: the copy then reads a frame being drawn (horizontal bands, white
where the GPU had not drawn yet in sysmem mode) until the next copy. Typing at the login is the worst case: a few swaps a second, each
followed by refresh copies, so a keystroke's swap often lands while a refresh copy reads the previous frame. Fence waits that gave up
after 50 ms (`6 gave up after 50 ms` at the login, more during the desktop's first heavy frames) copied frames still being drawn: the
garbled phase.

**The approach: compose the layers in the driver** (not "advertise no hardware cursor"). The capability WindowServer would read lives
in user space (SkyLight, IOMobileFramebuffer.framework), which this repository does not reverse-engineer; the VM-era note that a
missing `CursorPlane` property makes WindowServer draw the cursor into the frame is contradicted by these logs (the cursor layer
came without one). AppleParavirtDisplay, the reference of every offset here, has a host cursor too: the family's swap carries the
cursor layer whatever the driver says. Composing is in this driver's hands, host-testable, and works the same for the copy and as
the fallback of flip mode.

**The ABI, derived the documented way.** The existing offsets (`kSwap*`) are AppleParavirtDisplay's reads in the k27 collection. For
the cursor's place the same function was read again (`kc27.bin`, 27.0.1 26A434, the k27 tree's `scripts/kc-disasm.py`; the
addresses are kc27's): `AppleParavirtDisplay::program_hardware_gated(bool)` (0xfffffe000869d4f8) tests bit 1 of +984, then
`ldur w2, [x8, #97]` and `ldur w3, [x8, #101]` (+0x28, +0x2c) and calls `program_hardware_cursor_gated(IOSurface *, int, int)` with
layer 1's surface (+2952) and those two words. That function calls `IOSurface::getImageOriginAndExtents()` (24 bytes through x8:
the glyph's rectangle x, y, width, height in the surface and the hot spot x, y, checked to lie inside it), stores the hot spot, and
hands hot spot + words to `program_hardware_cursor_position_gated(bool, unsigned short, unsigned short)`, the host cursor's hot-spot
position; a null layer-1 surface calls it with false (hidden). So +97/+101 (unaligned s32) are where the cursor surface's pixel
(0, 0) goes on the panel, and the glyph rectangle is the part to draw. `kSwapLayer1X/Y` and the `getImageOriginAndExtents` import
record this in `NativeGopDisplay.cpp`. No position is known for layers 2 and 3 (AppleParavirtDisplay reads none; none was seen with
a surface): they are logged, not composed.

**The composition** (`GopCompose.h`, used by the kernel and by the host tests alike):

- A swap's mask says which layers it changes; a set bit with a surface shows it, a set bit without one turns the layer off, a clear
  bit leaves the layer as it is (so a cursor-only swap keeps the frame). Layer 0 is the frame at (0, 0) (converted from 'BGRA',
  'RGhA' or 'b3a8' as before; black where it has no pixel or is off: the `0x5` swap before display sleep shows black); layer 1, a
  'BGRA' surface, is blended over it with premultiplied alpha (`src + dst * (255 - a) / 255`, saturated), its glyph rectangle at the
  layer's origin, clipped at the panel's edges (the origin may be negative or past the edge).
- A job is a snapshot of the layers (references held). A frame's job composes the whole panel row by row (each source row read once
  in one run, converted, the cursor's span blended, the row written at once). A cursor-only job redraws only the rectangle where the
  cursor was drawn and the one where it is now (one box when they overlap or are close, else the two): about 2 x 128 rows of 128
  pixels instead of 27 MB. The copy thread keeps what the panel shows (flipped or not, where the cursor was drawn), so a merged job
  still erases the old cursor.
- The copy thread holds one waiting job: the newest state wins (jobs it had no time for are merged into the next, the larger kind
  kept), and a refresh never displaces a swap's job.

**The race, closed** (`native-gop-display-defer=2`, the default): a swap completes only once its own job is done (from the work
loop, `jobDoneAction`), so the surface it replaces goes back to WindowServer only after the copy of the frame that replaced it: every
read of the old surface (its own copy, its refresh copies, a cursor move over it) came before. A refresh copy in the way of a swap's
job stops at its next row (it re-reads the frame on screen, and the swap's job draws a newer state), so a keystroke's swap does not
wait for a whole refresh copy. With `=1` a swap completes at once unless the job the copy thread is drawing reads a surface the swap
takes off the screen (the old frame, the old cursor image); the check and the offer are one step under the lock, so the copy thread
cannot pick up a waiting job that reads the released surface in between (the offer merges it into this swap's job, which reads only
what stays on screen). Refresh copies read only the frame on layer 0, which the family keeps on screen until a later swap replaces
it, and none is asked for while a swap waits. Flip mode has always used `=2` (the surface it replaces is free once the new one latched).
The cost: a swap completes one copy (about 20 ms) after it is programmed, instead of at once; WindowServer draws at most as fast as
the copies go (every frame it draws is shown; none is skipped).

**Fences: never a frame the GPU has not finished.** A job waits for its swap's GPU fence (the newest fence the GPU had handed out
when the swap was programmed) and is drawn only once the GPU retired it. Up to the hang guard (`native-gop-display-gpu-fence-ms`,
1000 ms): a job still unfinished then is not drawn at all (the previous frame stays on the panel; `the GPU had not finished a frame
...`), its swap completes so WindowServer is not held longer, and every tick asks for the whole panel again, drawn once the fence
has passed (a non-blocking check). Before 2026-11-12 the wait gave up after 50 ms and copied the frame as it was: the white bands. A
GPU that does not answer (no PianoA830, or `NotReady` after a device loss) gives no fence to go by: drawn as without fences, as
before. A frame whose fence wait saw the GPU done needs only the last refresh copy (tick 30, a safety net); a frame drawn without a
fence gets all six. Refresh copies are fence-checked too. The cached-read option (`native-gop-display-copy` bit 1) invalidates
exactly the rows a job reads, after its fence wait.

**Flip mode** (`native-gop-display-flip=1`, PianoDpu): the DPU has no cursor pipe yet, so a frame is offered to the DPU only while no
cursor is on the panel; with one, the frame is composed into the boot framebuffer and the boot framebuffer put back on the panel
(logged once: `flip mode: a cursor is on the panel: ...`), and cursor moves then redraw their rectangles there. When the cursor hides,
the next frame flips again. A cursor showing up over a flipped frame composes the whole panel first. A second DPU pipe for the cursor
(dpu.md §4.2.6) is later.

**Host tests** (`package/NativeGopDisplay/tests/gop_host.cpp`, run by `tooling/k27/build-native-gop-display.sh` before the build,
565 checks): rectangles and clamping, the cursor's damage; `round(a*b/255)` exhaustively, the blend against a reference over 200,000
pixels, saturation; all 65536 half floats, the extended-range table, every base format; the glyph rectangle from the six words
(missing, empty, outside, cut, hot spot outside); the cursor off each edge, far away, partly on; 600 random scenes of composed rows and
damage rectangles against a per-pixel reference (every base format, narrow bases, no base, cursor sub-rectangles); the layer rules;
the slot, merges, the completion rule in each mode and the refresh abort; the panel plans in copy and flip mode; the refresh ticks
and policies; the fence verdict (a frame or a refresh never drawn before its fence passed); and a
simulation of WindowServer (two or three framebuffers, the least recently used free one drawn, a GPU that finishes after the swap, a
moving, hiding, image-changing cursor crossing the panel's edges), the family (queue, current swap, completion handing surfaces back),
the VBL timer, the fences and the copy thread (20 ms frames, 0.5 ms cursor rectangles, refresh copies that a swap cuts short after
the rows they got to). With `defer=1` and `=2`, with either refresh policy, no copy ever reads a surface while WindowServer draws it,
none reads a frame the GPU is drawing, with `=2` no swap completes before its job, and the panel equals the reference composition at
every idle moment and at the end; with `defer=0` (before) the same workload with two framebuffers tears (8 torn reads in the first
seed): the race of 2026-11-11 reproduced. A slow GPU (frames of 30-90 ms against a 50 ms guard) leaves jobs undrawn, the ticks retry,
no frame in progress is ever read, and the finished frame ends on the panel. Flip mode never leaves a flipped frame on the panel while a
cursor is on it.

**The next attended run** (the desktop run of gpu-userspace.md, "The next desktop run (2026-11-12)": no argument of this
section is needed, all is default):

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=1200 \
PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-power=1 piano-keyboard=2 piano-keyboard-gestures=1 piano-backlight=2 piano-backlight-keys=1 exec-log=1 stderr-log=1 piano-gpu-hang-ms=10000" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-attend5 && cp -R out/ram out/ram-attend5
tooling/native/native-loop.sh out/ram-attend5/boot-piano-macos-ram-A-bundled.img
```

What the owner should see: the pointer at the login screen (an arrow over the wallpaper; it follows the cover's touchpad and
the click lands under its tip, so the password field can be clicked); typing the password without the screen flickering and
without white bands; the login transition without a garbled phase (a frame the GPU has not finished stays off the panel, so a
slow frame shows as a short pause, not as bands). The console: `composition: ...` once, `layer 1: 128x128 'BGRA' at (X, Y) ...:
the cursor, blended ...`, `cursor: surface 128x128, glyph ..., hot spot (HX, HY) ...` (record the hot spot and whether clicks
land under the tip), in each 10 s block `cursor: N updates ... mean N us` (expected well under 1 ms), `N swaps completed once the
copy thread was done ..., mean N us` (about one copy, ~20-40 ms), the fence line's `0 gave up after 1000 ms`, and `jobs left undrawn` only
if the GPU stalled. If the pointer is off by a fixed offset, its hot spot is the likely amount (risk 9); drawn only in part:
`native-gop-display-cursor=2`; the desktop at about 10 frames a second: `native-gop-display-defer=1`, then `=0` (risk 10), each
added to `PIANO_EXTRA_ARGS`.

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
   - *Since 2026-11-12, default:* bit 0's rule (`native-gop-display-refresh=1`; the 10 s block's `N refresh copies not needed`
     line counts what it saves), and a pointer move costs only its two rectangles (§5a; the `cursor: N updates ... mean N us`
     line), not a frame copy.
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
9. **The cursor's place** (§5a). The origin words come from AppleParavirtDisplay's code, and their meaning (where the surface's pixel
   (0, 0) goes) from the arithmetic there (hot spot + words = the host's hot-spot position). Should the pointer be drawn off from
   where clicks land by its hot spot, the `cursor: ... hot spot (HX, HY) ...` line gives the offset to correct; should it be drawn
   only in part or with a frame of garbage, `native-gop-display-cursor=2` draws the whole surface. Panel pixels are assumed (the
   cursor surface is 128x128 at the 2x scale, and the swap carries no scale for it).
10. **Deferred completions.** A swap completes one copy (about 20 ms, plus its fence wait) after it is programmed. WindowServer's
    behaviour with such a swap (it asks `swap_wait` without blocking) has not been seen on the tablet; flip mode has always worked this
    way but has not run either. If the desktop drops to about 10 frames a second, `native-gop-display-defer=1` (waits only while a
    read is in the way) and then `=0` tell whether this is the cause (`=0` brings the race back). A pointer move waits behind a
    running frame copy (up to ~20 ms) and behind its fence wait (the newest GPU work at the swap, whoever submitted it).
11. **Layers 2 and 3** are not composed (no position is known). None was seen with a surface; one would be logged once
    (`layer N: ... not composed ...`).
12. **A GPU that hangs.** A frame is then held back for the hang guard (1000 ms) per swap, and the panel keeps the last finished
    frame until the GPU's recovery retires the fences; the pointer stops with it.

## 7. Files

- `package/NativeGopDisplay/{NativeGopDisplay.cpp,UPSTREAM,LICENSE}` (BSD-2-Clause-Patent; provenance in `UPSTREAM`)
- `package/NativeGopDisplay/GopCompose.h` (the composition and the swap pipeline, portable) and `tests/gop_host.cpp` (its host tests)
- `tooling/k27/kext/NativeGopDisplay.json`: personality `IOClass NativeGopDisplay`, `IOProviderClass AppleParavirtGPU`,
  `IOProbeScore 5000`; D6's second personality `NativeGopDisplayExternal` (its own `IOMatchCategory`, `NativeGopDisplayRole`
  "piano-dp-fake"; refused without `piano-dp-fake`); vtable check of all 483 IOMobileFramebufferLegacy slots
- `package/PianoDp/DpFake.h` (included): D6's arguments and plug schedule, host-tested in PianoDp's `tests/dp_host.cpp`
- `tooling/k27/build-native-gop-display.sh`
- `tooling/adt/make-piano-adt.py` (`--no-gop-display` leaves the node out), `configs/k27-gop-display.toml`
- `tooling/bootimg/build-ram-images.py`: `PIANO_EXTRA_ARGS`

## 8. Licence note

The VM-era display kext (`UPSTREAM`) and GopFramebuffer are BSD-2-Clause-Patent. Code was taken from the former only. No GPL
code, and no Linux code, was read into this package. The IOMFB declarations are generated from the collection's symbols
at build time, not copied. The composition (2026-11-12) is new code; the two layer-1 offsets and the extents getter were read
from AppleParavirtDisplay's code in the collection the same way as the earlier offsets (§5a).
