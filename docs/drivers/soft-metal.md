# A Metal device without a GPU: WindowServer on the CPU (NativeSoftGpu, NativeSoftMetal)

The desktop needs WindowServer, WindowServer needs a Metal device, and piano has no GPU driver yet ([gpu.md](gpu.md)). These two
packages give it a device that renders nothing, so that SkyLight composites in software and Core Animation draws the display with its
own software renderer. The frames reach the panel through [NativeGopDisplay](native-gop-display.md)'s CPU copy.

- `package/NativeSoftGpu`: a kernel `IOAccelerator` on IOResources whose only job is to carry `MetalPluginName`. No user client, no
  memory, no hardware. Boot argument `native-soft-gpu=0` leaves it out.
- `package/NativeSoftMetal`: the plugin that IOGPU loads into each Metal client from `/Library/GPUBundles`. Its device answers
  `acceleratorPort` 0 and refuses every command queue, buffer, texture and library.

## 1. What macOS needs, and where it was read

Read on the build Mac (macOS 26.6, Darwin 25.6) in `SkyLight` and `QuartzCore` of the dyld shared cache; 27.0.1 was not read, so every
line below is "26.6 says". The tablet runs 27.0.1.

| Step | What the code does | Symbol |
|---|---|---|
| WindowServer starts | waits up to 30 s for `MTLCopyAllDevices` to be non-empty, else `abort()` | `WSSystemCanCompositeWithMetal`, "No suitable Metal devices present after %llu seconds." |
| it picks the compositor | the first device's `-acceleratorPort`; **0 means the software compositor** | `WSGetDefaultMetalAccelerator`, `WSGetCompositorForAccelerator`, `activate_compositor_sw`, "Software compositor activated." |
| Core Animation's display | asks the first device for a command queue; with none, `AccelServer::renderer()` is nil and the update is rendered by `CA::OGL::SWContext` | `CAMetalContextCreate`, `CA::WindowServer::Server::sw_renderer`, `-[CAWindowServerDisplay beginExternalUpdate:usingSoftwareRenderer:]` |

So the device must exist, report port 0, and have no queue. No defaults key, environment variable or boot argument reaches this: the
compositor is chosen from the device, and `WSMetalCompositorEnabled` is only switched afterwards by a debug-options call.

**Every selector must be answered.** `_MTLDevice`, the base class a plugin's device must have, implements most of the device protocols
but not all: on 26.6, 33 of `MTLDevice`'s 139 and 48 of `MTLDeviceSPI`'s 507 are the driver's own, among them `maxBufferLength`,
`newTextureWithDescriptor:`, `recommendedMaxWorkingSetSize` and the limits SkyLight reads for its device dump
(`maxColorAttachments`, `maxVertexAttributes`, ...). An unanswered selector is an unrecognized selector, and its exception aborts the
process with nothing on stderr.

## 2. The 2026-11-11 run, and what it means

With the first version (a small hand-written method list) WindowServer started, took the device and died:

```
NativeSoftGpu: Metal accelerator registered on IOResources (MetalPluginName NativeSoftMetal ...)
NativeGopDisplay: display on                      <- WindowServer opened the display
NativeSoftMetal: WindowServer: device NativeSoftMetal (software, no GPU), registry ID 0x10000012c, accelerator port 0, ...
NativeSoftMetal: WindowServer: stubbed maxBufferLength
(fd1) Didn't find ADM service
AMFI: Denying core dump for pid 138 (WindowServer)
exit: pid 138 WindowServer status 0x6 reason 2:6          <- SIGABRT, then relaunched, again and again
```

Reading of that run:
- The contract works as far as it goes: the accelerator was found, the bundle was trusted and loaded, the device was made with port 0,
  and WindowServer got past the Metal wait and opened IOMFB. Any theory of the abort has to start after that.
- `stubbed maxBufferLength` is the last thing from the plugin, and it is exactly the first of the driver-owned selectors. The next one
  it asked for was not in that version's list, and SkyLight's device dump asks for a dozen of them in a row.
- An unrecognized selector matches every other detail: `abort()` (status 0x6) rather than a fault, nothing on stderr (the exception's
  message goes to os_log), and a core dump requested. **This is the most likely cause, and it is what this change addresses.**
- "Didn't find ADM service" is a display-manager lookup, logged on ordinary Macs too; not the cause.
- The run where WindowServer lived about 80 s makes a timeout a competing theory. Two in SkyLight could do it: the display readiness
  watchdog (`ReadinessTelemetry::mark_unready`, "[ Watchdog ] display %x has become stuck after %.5fs of unreadiness"; it only reports,
  and `WSWatchdogCheckRunning` first opens `com.apple.windowserver.watchdog` shared memory, which the disabled `watchdogd` will not have
  made) and the scheduler assert in `update_display_callback` ("CADeferLockAcquire was attempted to be called before releasing the
  lock."). Both log through os_log, hence §4's log job.
- Also found: `SLCADisplay::present_update` and `::render_update` have sub-paths that, with a non-Metal compositor, log
  "TODO: software compositor support." and skip. They are sub-paths (a detached or flattened layer), not the whole present, and they
  only log. If the desktop comes up with pieces missing, this is where to look.

## 3. What the plugin does now

- Answers **every** selector of `MTLDevice` and `MTLDeviceSPI`, at run time from the protocols themselves (so it is 27.0.1's own list):
  the values in `kValues` where a plausible one matters, a zero of the right shape otherwise (integers, floats, and the `{QQ}` and
  `{QQQ}` structures those protocols return), each named once on stderr.
- Anything still unknown goes to `-forwardInvocation:`, which logs it and returns zeros. **Nothing throws.**
- Plausible limits instead of zeros: `maxBufferLength` 256 MiB, `maxThreadsPerThreadgroup` 512x512x512, `maxThreadgroupMemoryLength`
  32 KiB, `maxColorAttachments` 8, the vertex and fragment counts, alignments 256, `recommendedMaxWorkingSetSize` two thirds of RAM.
- An **abort reporter**, installed when the bundle loads: an uncaught-exception handler, a C++ terminate handler and handlers for
  `SIGABRT`, `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGTRAP`, `SIGFPE`. Each prints the reason and up to 24 frames as `<image>+<offset>
  <nearest exported symbol>` on stderr, and the signal handler re-raises so the process still dies as it would have. This is what names
  the next cause without the 27.0.1 symbols. Checked on the build Mac against a deliberate exception and a deliberate `abort()`.
  Before the stack it prints every loaded image's crash-reporter message (`__DATA,__crash_info`), where SkyLight, `assert()` and libobjc
  leave their text before `abort()` (checked with a deliberate `assert`). That is the abort reason even when os_log redacts it.
- **8-bit framebuffers** in WindowServer (§8): Core Animation's debug options `CA_FORCE_FP16_FRAMEBUFFER` and
  `CA_FORCE_CIF10_FRAMEBUFFER` are found by name (`CAGetDebugOptionEnvVariableName`), reported, and cleared at load and once a second
  for a minute; the `CAEnableDeepFramebuffer` preference is set false for the process. Every debug option that is on is listed once.
  Checked on the build Mac with `CA_FORCE_FP16_FRAMEBUFFER=1` in the environment: option 124 found and cleared.

Boot arguments (read through `kern.bootargs`, so none of them needs a restaging):

| Argument | Default | Effect |
|---|---|---|
| `soft-metal=0` | on | no device anywhere: as if the plugin were missing |
| `soft-metal-all=1` | off | a device in every process that asks, not only WindowServer |
| `soft-metal-stubs=0` | on | only `kValues` and `_MTLDevice`; the rest goes to forwarding, so `-respondsToSelector:` stays NO for it (a client that asks first then takes its other path) |
| `soft-metal-family=N` | 0 | `-supportsFamily:`/`-supportsFeatureSet:` answer YES up to N, for a client that insists on a GPU family |
| `soft-metal-report=0` | on | no abort reporter |
| `soft-metal-fb8=0` | on | leave Core Animation's framebuffer format alone (the options are still listed) |

## 4. Build, stage and run

```sh
tooling/k27/build-native-soft-gpu.sh                 # the kext -> out/native-kexts + out/k27/kext/NativeSoftGpu.json
tooling/k27/build-soft-metal-plugin.sh               # the plugin -> out/soft-metal/NativeSoftMetal.bundle (arm64e, ad hoc signed)
python3 tooling/k27/stage-soft-metal.py \
  --log-stream 'process == "WindowServer" OR process == "loginwindow"'
# -> out/soft-metal/{scratch-data.img,kernel.macho.trustcache}: the bundle in /Library/GPUBundles, its CDHash in the trust cache, and
#    the seed's one boot job extended with a `log show --last 25s` loop every 20 s (rewritten in place, so it stays root's; a
#    `log stream` stopped after its first 72 lines on 2026-11-11, while each `log show` exits and flushes)
PIANO_EXTRA_KEXTS="$PWD/out/k27/kext/NativeSoftGpu.json" \
PIANO_SEED=out/soft-metal/scratch-data.img PIANO_TRUST=out/soft-metal/kernel.macho.trustcache \
PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="exec-log=1 stderr-log=1" tooling/k27/build-native-image.sh
tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img
```

`PIANO_EXTRA_KEXTS` must be an absolute path (the KC builder changes directory). `configs/k27-native-kexts.txt` is not changed: the
accelerator is an experiment, and `PIANO_KEXT_LIST` or `PIANO_EXTRA_KEXTS` is how it is linked.

The log job needs `logd`. If it is not running the job fails and the boot is otherwise unchanged; the abort reporter still works, since
it writes to stderr from inside WindowServer.

## 5. Reading the run

```sh
grep -E "NativeSoftGpu|NativeSoftMetal|NativeGopDisplay|WindowServer" logs/native-runs/<ts>-loop/xnu-console.txt
```

In order, what each line means:

1. `NativeSoftGpu: Metal accelerator registered on IOResources (MetalPluginName NativeSoftMetal, ...)` — the kext is up.
2. `NativeSoftMetal: WindowServer: abort reporter installed` — the bundle was trusted and loaded.
3. `NativeSoftMetal: WindowServer: N selectors stubbed, M left to forwarding` — N is 27.0.1's count of driver-owned selectors (64 on
   26.6). M should be 0.
4. `NativeSoftMetal: WindowServer: device made, registry ID ..., port 0, no queue: software compositing`.
5. `NativeSoftMetal: WindowServer: zero <selector>` / `forwarded <selector>` / `unknown shape <selector>` — what WindowServer wanted
   from a GPU. The survey of this run is the work list for the next one.
6. **Success:** `NativeGopDisplay: display on`, then `first swap submitted`, `swap layers ...`, `first frame (swap N)`, then the
   periodic `N swaps submitted, N shown, ... copies mean N us`. A frame on the panel is `first frame`.

If it aborts again, the reporter says why, and that decides the next step:

| What the reporter prints | Reading | Next |
|---|---|---|
| `uncaught NSInvalidArgumentException` + `reason: -[NativeSoftMetalDeviceObject ...]: unrecognized selector` | a selector outside both protocols | add it to `kValues`; the reason names it |
| `uncaught ...` from SkyLight or QuartzCore with a Metal-sounding reason | a client wanted real GPU work | the `zero`/`forwarded` lines just before name the selector; decide whether it can be answered |
| `SIGABRT` with `#3 libsystem_c.dylib+... abort` under `SkyLight+0x...` | a SkyLight abort | its text is the `crash message (SkyLight)` line just above the stack; os_log has it too, but a plain `%s` there shows as `<private>` |
| `crash message (SkyLight): Unimplemented pixel format of N used in WSCompositeDestinationCreateWithIOSurface.` | the software compositor was handed a framebuffer it cannot write (§8) | N as a four-char code: 1380411457 `'RGhA'`, 1647534392 `'b3a8'`, 1999908961 `'w40a'` (`'BGRA'` is 1111970369 and is accepted); see §8 for which lever |
| `SIGSEGV` inside `Metal` or `IOGPU` | an `_MTLDevice` method ran on the uninitialized object | answer that selector ourselves (`kValues`) so the base class's never runs |
| nothing from the reporter, WindowServer still dies | it was killed from outside, not aborting itself | look at `exec:`/`exit:` lines and the log stream |

## 6. Files

- `package/NativeSoftGpu/{NativeSoftGpu.cpp,UPSTREAM,LICENSE}`, `tooling/k27/kext/NativeSoftGpu.json`,
  `tooling/k27/build-native-soft-gpu.sh`
- `package/NativeSoftMetal/{NativeSoftMetalPlugin.mm,frameworks/IOGPU.framework/IOGPU.tbd,UPSTREAM,LICENSE}`,
  `tooling/k27/build-soft-metal-plugin.sh`
- `tooling/k27/stage-soft-metal.py` (seed and trust cache; `--boot-run`, `--log-stream`)
- `tooling/k27/build-native-image.sh`: `PIANO_SEED`, `PIANO_TRUST`

## 7. What this is not

A software Metal *implementation*. Nothing is rendered by the GPU or by a CPU rasterizer of ours: the drawing is SkyLight's and Core
Animation's own software paths, which are there for machines without a usable accelerator. Expect it to be slow, and expect anything
that insists on Metal (Safari's compositing, video, some effects) not to draw. The real driver is the plan in
`droidvm-build/piano-patches/gpu-native-plan.md`.

## Measured 2026-11-11 (d306a16a, 300 s)

Abort reporter installed; 64 selectors stubbed, 0 forwarded; device made (port 0, no queue); NativeGopDisplay "display on". Every
WindowServer after the first aborts in SkyLight `WSCompositeDestinationCreateWithIOSurface` called from
`WS::Displays::SLCADisplay::render_update` (`UpdateDisplays` ← `CGXUpdateDisplay` ← `update_display_callback`), a plain `abort()`
(no exception). The os_log stream showed `Display 1 current mode (0 x 0) [0 0], preferred mode (0 x 0)` from CoreAnimation, then
stopped after ~6 s (the `log` job's output). Next: the composite destination's requirements (texture from IOSurface? display mode?).

## 8. The abort in WSCompositeDestinationCreateWithIOSurface (read 2026-11-11, macOS 26.6)

`WSCompositeDestinationCreateWithIOSurface` has two ways to die:
- `assert(surface != nullptr)`. That prints "Assertion failed" on stderr, which the run did not show, so it is not this one.
- **An unsupported pixel format:** `asprintf("Unimplemented pixel format of %i used in %s.")`, logged with a plain `%s` (shown as
  `<private>`), stored as SkyLight's crash-reporter message, then `abort()`. This matches the run.

It takes only `'BGRA'`, `'RGBA'`, 32ARGB (0x20), `'RGfA'` (float32), `'R10k'`, `'b64a'` and `'l10r'`. The half-float `'RGhA'` and the
10-bit `'b3a8'` and `'w40a'` all abort. The Metal compositor writes those through Metal textures (`WSCompositeDestinationCreateWithMetalTexture`).
The software compositor goes through this function. So the fix is not in the device. `newTextureWithDescriptor:iosurface:plane:` is
not on this path at all.

The surface is the display's framebuffer, and Core Animation picks its format in `CA::WindowServer::IOMFBDisplay::framebuffer_pixel_format()`:
- `'RGhA'` when debug option `CA_FORCE_FP16_FRAMEBUFFER` is on (26.6: option 124), or for a display with EDR headroom
  (`Display::edr_headroom()` above a threshold), or with a display capability bit set;
- `'w40a'` when `CA_FORCE_CIF10_FRAMEBUFFER` is on (125);
- the 10-bit forms for HDR modes or a wide gamut (the ADT already says `artwork-display-gamut` sRGB);
- else `'BGRA'`.

NativeGopDisplay publishes one 8-bit SDR sRGB colour element, no luminance and no EDR properties (`SupportsPQEOTF` false). So the EDR
branch is unlikely, and the likeliest source is `CA_FORCE_FP16_FRAMEBUFFER`. NativeGopDisplay's own comment ties `'RGhA'` to the
ADT's `vmm-present`, which main must keep at 1 (with 0, watchdogd exits and launchd panics). This candidate therefore works inside
WindowServer: it clears the two forcing options by name, sets `CAEnableDeepFramebuffer` false, and logs every debug option that is
on. It also prints the crash message, which names the format.

**Reading the next run:**
- **No abort, and `first swap submitted`:** solved. The `CA option … cleared` line says which option it was.
- **Abort, `crash message (SkyLight): Unimplemented pixel format of 1380411457 …`, and a `… cleared` line:** the format was decided
  before the clear. The next lever is earlier: the option set before WindowServer runs.
- **Same abort, no option on:** the EDR or capability branch decided it. The next lever is NativeGopDisplay (what makes
  `edr_headroom()` nonzero or sets the bit). The log loop's Core Animation lines for the display ("current mode", brightness) are the
  evidence to read.
- **Another N:** use the table in §5.

`Display 1 current mode (0 x 0)` was logged early, before any mode was set. It is a second thing to watch in the log loop. It is not
the abort: that is decided by the format alone.

## Measured 2026-11-11 (d6601e49, 300 s)

The crash annotation names it: `Unimplemented pixel format of 1380411457 used in WSCompositeDestinationCreateWithIOSurface`
(`'RGhA'`, half float). No FP16/CIF10 debug option was on; the only CoreAnimation option on was 203 `CA_HDR10_SET_EDR_SCALE`. So the
FP16 framebuffer comes from the EDR or display-capability branch of `framebuffer_pixel_format()`: next, the display's properties.

## 9. Where 'RGhA' comes from: the virtual-machine branch (read 2026-11-11, macOS 26.6)

`IOMFBDisplay::framebuffer_pixel_format()` returns `'RGhA'` in more cases than §8 listed. After the EDR, debug-option and
capability checks, it also returns it **whenever Core Animation considers the device virtualized** (`CADeviceIsVirtualized`, inlined:
MobileGestalt's bool answer, and on chip ID 0x8103 the `kern.hv_vmm_present` sysctl, which the kernel reads from the ADT's
`/defaults/vmm-present`). In that case there is no exit to `'BGRA'`; the only other one, `CA_FORCE_CIF10_FRAMEBUFFER`, gives `'w40a'`,
which SkyLight's software compositor rejects as well.

So, against the measured run:
- `CA_HDR10_SET_EDR_SCALE` is not read by `framebuffer_pixel_format()` (on 26.6 it reads options 124, 125 and 150 only). Clearing it
  would not change the format.
- NativeGopDisplay's properties are not the trigger. It already publishes 8-bit SDR sRGB with no luminance, PQ or EDR, and the
  virtual-machine branch decides alone.
- The ADT says `vmm-present = 1`. Natively that is not true (the tablet is not a VM), and it is what gives WindowServer half-float
  framebuffers. In the VM, Metal composited them; the software compositor cannot.

**Next candidate: the truthful value.** A test ADT with `vmm-present = 0` (`make-piano-adt.py --vmm-present 0`; the shared
`configs/k27-adt.toml` keeps 1), with the same plugin and seed. The known cost, measured earlier: watchdogd runs and exits (no
watchdog device), and launchd panics about 150 s in. WindowServer's first display updates came well before that in every run, so one
run answers both questions:
- does WindowServer submit a swap with 8-bit framebuffers? (`NativeGopDisplay: first swap submitted`, no `crash message (SkyLight)`)
- what does watchdogd fail on before it exits? (`sc-trace=watchdogd`: its failing system calls, with paths)

The seed has no MobileGestalt cache (checked), so the new answer is computed on the first boot of a fresh seed.

```sh
python3 tooling/adt/make-piano-adt.py out/adt/piano-k27-vmm0.adt --unique-chip-id 47554e5941480001 --data --trust --vmm-present 0
PIANO_ADT=out/adt/piano-k27-vmm0.adt PIANO_EXTRA_KEXTS="$PWD/out/k27/kext/NativeSoftGpu.json" \
PIANO_SEED=out/soft-metal/scratch-data.img PIANO_TRUST=out/soft-metal/kernel.macho.trustcache PIANO_RESET_AFTER=300 \
PIANO_EXTRA_ARGS="exec-log=1 stderr-log=1 sc-trace=watchdogd" tooling/k27/build-native-image.sh
tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img
```

If the swaps come and the panic is watchdogd's, the remaining work is a watchdog for watchdogd natively (piano has one: k27 0056
already pets Gunyah's) or keeping watchdogd from running. That is a separate driver task, not part of this one.

## Measured 2026-11-11 05:41 (first frames)

ADT `vmm-present = 0` (`make-piano-adt.py --vmm-present 0`) plus the stock XNU boot argument `panic_on_crash_disabled=1`
(kern_exit.c `proc_should_trigger_panic`: watchdogd, which has no hardware watchdog natively, exits without a panic), NativeSoftGpu
and the staged seed: WindowServer composites in software into NativeGopDisplay. `38 swaps submitted, 20 shown, 0 dropped`, copies
about 38 ms each (prepare 2.3, read 33.7, write 1.4 ms); `swap layers 0x3: 0: 3200x2136 BGRA 1: 128x128 BGRA` (the cursor). WindowServer
still aborts now and then (status 0x6; the console ring wrapped, so the messages were lost). What is on the panel is not known yet
(next: a frame thumbnail in the log).
