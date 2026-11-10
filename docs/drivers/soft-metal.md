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

Boot arguments (read through `kern.bootargs`, so none of them needs a restaging):

| Argument | Default | Effect |
|---|---|---|
| `soft-metal=0` | on | no device anywhere: as if the plugin were missing |
| `soft-metal-all=1` | off | a device in every process that asks, not only WindowServer |
| `soft-metal-stubs=0` | on | only `kValues` and `_MTLDevice`; the rest goes to forwarding, so `-respondsToSelector:` stays NO for it (a client that asks first then takes its other path) |
| `soft-metal-family=N` | 0 | `-supportsFamily:`/`-supportsFeatureSet:` answer YES up to N, for a client that insists on a GPU family |
| `soft-metal-report=0` | on | no abort reporter |

## 4. Build, stage and run

```sh
tooling/k27/build-native-soft-gpu.sh                 # the kext -> out/native-kexts + out/k27/kext/NativeSoftGpu.json
tooling/k27/build-soft-metal-plugin.sh               # the plugin -> out/soft-metal/NativeSoftMetal.bundle (arm64e, ad hoc signed)
python3 tooling/k27/stage-soft-metal.py \
  --log-stream 'process == "WindowServer" OR process == "loginwindow"'
# -> out/soft-metal/{scratch-data.img,kernel.macho.trustcache}: the bundle in /Library/GPUBundles, its CDHash in the trust cache, and
#    the seed's one boot job extended with `log show --last boot` + `log stream` (rewritten in place, so it stays root's)
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
| `SIGABRT` with `#3 libsystem_c.dylib+... abort` under `SkyLight+0x...` | a SkyLight assert; its text is in the os_log stream | read the `log stream` lines in the same console log |
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
