# GPU userspace on the A830: the VM's Metal plugin, Turnip on Adreno830's ABI v2

Status 2026-11-11: the macOS login screen ran on the A830 with this userspace at 07:45 (end of this file); nobody has logged in yet.
The changes for the first desktop session and tonight's run are in [Desktop readiness, 2026-11-11](#desktop-readiness-2026-11-11). The
kernel half (package/PianoA830: board inputs, firmware, bring-up) is in [gpu.md](gpu.md). Plan: the local plan `gpu-native-plan.md` (not published)
§4 steps 2 and 3.

## 1. Purpose

WindowServer and every other Metal client get a real Metal device on piano's Adreno 830. The userspace is the one that drew the macOS 27
desktop in the VM (`NativeAdrenoMetal.bundle`: plugin, frontend, Rust bridge, metal2vulkan, Turnip). Only Turnip's transport changes.
In the VM, Turnip's darwin backend reached the host GPU through virtio-gpu (the `dwv` backend, accelerator open type 0x4149).
Natively it uses its NativeAdreno backend (open type 0x4147) on Adreno830's user client, as on the OnePlus 13T.

| Piece | Natively | Source |
|---|---|---|
| `Contents/MacOS/NativeAdrenoMetal` (plugin + frontend + bridge) | the VM's plugin and bridge, its frontend with one patch, plus an abort reporter, linked here (CDHash `9c42e844…` since the evening of 2026-11-11; the login run had the VM's binary, `ea549d13…`). The objects are compiled without objc_msgSend selector stubs, which makes the link reproducible | the VM-era plugin (`overlay/metal` of the k27 tree), UniXNU-turnip `drivers/AdrenoMetal` + `tooling/metal/turnip-a830/frontend/`, `tooling/metal/turnip-a830/abort_reporter.mm`, bridge `out/metal/bridge` |
| `Contents/MacOS/libvulkan_freedreno.dylib` (Turnip) | rebuilt: the VM's Mesa tree, `NativeAdrenoABI.h` v2, 6 patches (CDHash `5efbe639…` with 0006; `205e665f…` with 0004/0005, `031df58a…` with 3, `eaa367a2…` with 2 in the login run; cold builds byte-identical) | Mesa `cecc0e9` + 23 VM-era patches (`tooling/metal/UPSTREAM`) + `tooling/metal/turnip-a830/mesa/` |
| `Contents/Info.plist` | the VM's (`org.droidvm.NativeAdrenoMetal`, a legacy bundle identifier kept as staged; principal class `NativeAdrenoMetalDevice`) | |

The Turnip patches (`tooling/metal/turnip-a830/mesa/`):
- `0001-turnip-darwin-native-a830.patch`:
  - The virtio-gpu backend runs only with `TU_DARWIN_VIRTGPU=1` (`=probe`: the VM's board probe). So no Metal process opens the
    accelerator with type 0x4149.
  - A `dsb sy` before every Submit and at the end of `vkEndCommandBuffer`. Command streams are CPU writes into write-combined BO
    views, possibly recorded on another core. This is the 13T's fix D1 on the same A830.
  - `gpu_id` 830 for chip `0x44050001`. The decode formula would give 6850.
- `0002-turnip-darwin-tu-debug-file.patch`: without `TU_DEBUG`, the first line of `/private/var/ngpu/debug/tu-debug` stands in for
  it. A system daemon's environment comes from a launchd job on the read-only system volume.
- `0003-turnip-darwin-failures-on-stderr.patch`: a release build's `vk_log` is silent, so nothing of a lost device or a refused
  allocation reached any log. A lost device's reason is now written once per device and queue (`MESA: error: device lost
  (tu_knl_darwin.cc:<line>): <what> failed: 0x…`), and every failing kernel method the first time per method and status, then at the
  10th, 100th, … (`TU: error: NativeAdreno: kernel BoAlloc failed: 0x… (1 time)`). The answers the backend expects are left out.
- `0004-turnip-darwin-max-bo-size-limits.patch`: the kernel refuses a BO or import past Query's `max_bo_bytes` (256 MiB) with
  BadArgument, while Turnip advertised 4 GiB: an 8192x8192 RGBA8 texture reached the kernel and its client saw out-of-memory. Now
  `maxMemoryAllocationSize`, `maxBufferSize` and images' `maxResourceSize` are capped at it, and a larger BO is refused before any
  kernel call (`NativeAdreno: a BO of N MiB is past the kernel's per-object limit of 256 MiB (maxMemoryAllocationSize); refused (1
  time)`). Metal follows: `MTLDevice.maxBufferLength` is 256 MiB (was 4095), a buffer of exactly that is made, one byte more and an
  8192x8192 RGBA8 texture are refused by the frontend without a kernel call (`check-turnip-a830-host.py`, plugin_host section 5).
- `0005-turnip-darwin-tu-debug-boot-arg.patch`: `piano-tu-debug=<flags>` on the boot line (read through `kern.bootargs`) stands in for
  `TU_DEBUG` and wins over the seed's `tu-debug` file; `piano-tu-debug=none` means none at all (Turnip picks GMEM or sysmem per
  render pass). WindowServer says which: `TU: info: TU_DEBUG=sysmem from /private/var/ngpu/debug/tu-debug` or `... from boot
  argument piano-tu-debug`. The sysmem/GMEM A/B is two boot lines over one seed (below).
- `0006-turnip-darwin-device-window.patch` (2026-11-12): the enumeration probe's connection reads GetDeviceInfo's GPU window and closes;
  the device's own connection can land in another slot with another window (the first desktop, run attend4: `Installer Progress` and
  `MenuBarAgent` in slot 5, every BoMap BadArgument, no Metal device). The device now takes its own connection's window before its
  address heap is seeded, and says so once: `TU: error: NativeAdreno: this device's GPU window is 0x... + 0x..., the enumeration
  probe's was 0x... + 0x... (another slot); the device takes its own` (not an error: a note that it happened). gpu.md §8, "The first
  desktop's address space".

The frontend patch (`tooling/metal/turnip-a830/frontend/0001-…patch`, applied after the k27 tree's `scripts/piano-frontend-patch.py`): the
stderr copy of its `NBLOG` lines is rate-limited per call site (8 lines, then the 16th, 32nd, … with the count); `no device answer`
goes to the unified log only; `computeCommandEncoderWithDispatchType:` with a concurrent type returns the serial encoder instead of
failing the command buffer. The abort reporter (`abort_reporter.mm`, ours) is NativeSoftMetal's, see the desktop section.

ABI: `NativeAdrenoABI.h` from `package/PianoA830/` when the kernel side has it, else Adreno830 `1a13210`'s
(sha256 `20ac9298…70d2`, pinned in the build script; a different header is used but noted). The backend also uses one name v2
lacks, `NativeAdrenoParamsOwnedCoherent` (Adreno740's v2 flag for IO-coherent BOs, value 2), added by
`tooling/metal/turnip-a830/NativeAdrenoABICompat.h`. Adreno830 never sets it, so CPU views of BOs stay write-combined.

## 2. Build, check, stage

```sh
tooling/metal/build-turnip-a830.sh           # -> out/turnip-a830/NativeAdrenoMetal.bundle (about 50 s cold on the M1 Pro, seconds after)
python3 tooling/metal/check-turnip-a830-host.py   # the offline ABI handshake and the plugin checks (§3), about a minute
# staging alone (build-native-image.sh does it by itself when PianoA830 is linked):
python3 tooling/k27/stage-soft-metal.py --out out/turnip-a830/stage \
  --file out/turnip-a830/tu-debug:private/var/ngpu/debug/tu-debug out/turnip-a830/NativeAdrenoMetal.bundle
```

`tooling/k27/build-native-image.sh` builds and stages this whenever `PianoA830` is linked, either from the list or from
`PIANO_EXTRA_KEXTS`. `PIANO_A830_METAL=0|1` overrides that. The result goes to `out/turnip-a830/stage/{scratch-data.img,kernel.macho.trustcache}`,
and the image takes those files unless `PIANO_SEED` / `PIANO_TRUST` name others. The staging is redone only when the input seed, the
input trust cache, the stage script, the bundle's files or `PIANO_A830_TU_DEBUG` change (`out/turnip-a830/stage/stamp`).
Builds are byte for byte the same, cold ones included: Mesa's generators run with `PYTHONHASHSEED=0`, and the plugin's objects are
compiled without objc_msgSend selector stubs (ld-1267 orders the stubs it makes differently from link to link; without them ten links
were identical). So the staged seed keeps its identity across rebuilds (the login run of 2026-11-11 had the VM's binary).
`PIANO_A830_TU_DEBUG` defaults to `sysmem` (no GMEM rendering, the plan's first bring-up setting); `PIANO_A830_TU_DEBUG=` stages none.
NativeSoftGpu and PianoA830 together are refused: Metal takes the first accelerator and SkyLight the first device's port.

Measured 2026-11-11: the staged seed is 124.6 MB packed (the input was 84.0 MB), so the macOS RAM image grows by about 40 MB to about 245 MB.
Check that ABL's `max-download-size` takes it before the first run.

## 3. The offline ABI handshake (`check-turnip-a830-host.py`)

The bundle's own arm64e Turnip runs on this Mac against Adreno830's host copy of its kernel stack. That is
`tests/hostsim/host_gpu_service.cpp` with `HOSTSIM_GPU=a830`: the real GPU-context bring-up, VM, ring, submit and user-client code
over software SMMU and CP peers, the A830's device facts and the render server's slot-0 window. The host copy is reached through its
IOKit shim (`DYLD_INSERT_LIBRARIES`). The shim answers as the tablet must: service `AGXAccelerator`, open type 0x4147 only, every
method through the NTNL tunnel of one admitted selector. PAC is active for arm64e processes on this Mac, so the pointer-authentication
ABI is exercised too. No GPU is used. The software CP retires every submission without executing it, so no pixel is checked.

Result 2026-11-11, all pass:
- `vulkan_smoke`, with 16 KiB pages and with 4 KiB pages (the tablet's): device `Adreno (TM) 830` (vendor 0x5143, device
  0x44050001), a 1 MiB BO mapped through the kernel's descriptor, a fill submitted and retired, 0 kernel objects left. Trace: two
  `IOServiceOpen(AGXAccelerator, 0x4147)` (the enumeration probe is closed again, then the device opens), no 0x4149, methods
  Query, GetDeviceInfo, BoAlloc, BoMap, BoCpuMap, Submit, WaitFence, GetStatus.
- `deferred_bo`: the normal mode and 6 error modes (busy, fence, free, lease, view, map cleanup). Two of the test's expectations
  are the 13T Turnip's policies, which this Turnip does not share:
  - the BO cache keeps a freed small BO, so mode `normal` runs with the cache off;
  - the deferred-free queue grows instead of waiting for the GPU at 256 entries; `pressure` is checked for exactly that.
- `plugin_smoke` (AdrenoMetal's `NativeTurnipSmoke.h`): a fill, a compute pipeline and a render pass, compiled by ir3 for the A830.
- `plugin_host` (`tooling/metal/turnip-a830/plugin_host.mm`), with both page sizes: the staged `NativeAdrenoMetal` itself. Its frontend
  opens the device on this Turnip. The bridge's AIR fixtures are translated (AIR → SPIR-V → NIR → ir3), then a compute dispatch, a
  render pass into an IOSurface and a blit run. All 3 command buffers complete, with 3 kernel submissions and 1 BoImport (the path of
  WindowServer's framebuffers).
- Added in the evening (all pass): `plugin_host` also runs a desktop session's extras (a heap with placed resources, a 4x MSAA pass
  with resolve, a concurrent compute encoder, a shared event signalled by a command buffer, 24 panel-sized 3200x2136 IOSurfaces
  rendered into and released one after another with no kernel object left behind, an 8192x4096 texture), also as a small-slot client
  (`plugin_host-slot4`: `HOSTSIM_SLOT=4`, a 124 MiB window, so every import's GPU range must come back), and since 2026-11-12 with
  the device's connection in another slot than the probe's (`plugin_host-reslot`: `HOSTSIM_SLOT=4 HOSTSIM_SLOT_REOPEN=5`; Turnip 0006
  passes it, the 0001-0005 Turnip fails it as the tablet did); `abort_reporter-abort`,
  `-exception` and `-fault` (`reporter_host.mm`): the plugin's abort reporter reports and the process still dies with SIGABRT, SIGABRT
  and SIGSEGV.

Kits: Adreno830 `1a13210` is written against ArmSmmu4K `e923329` and AdrenoPM4 `282606b`. The check fetches both as GitHub archives
into `out/turnip-a830/kits`. The standalone ArmSmmu4K checkout in `unixnu-research` is too new: its arena takes page callbacks, from
`48dbd6c` on.

## 4. What the kernel side (PianoA830) must provide

Read from Turnip's darwin backend, the plugin and the frontend. The hostsim already models items 1-5.

1. **Discovery.** An `IOAccelerator` subclass (IOGraphicsFamily, macOS) whose C++ class is named **`AGXAccelerator`**.
   - Turnip finds it with `IOServiceMatching("AGXAccelerator")`, because the plugin sets `TU_DARWIN_SERVICE=AGXAccelerator`.
   - Its personality carries `MetalPluginName = NativeAdrenoMetal` and `MetalPluginClassName = NativeAdrenoMetalDevice`, as the VM-era GPU
     kext's descriptor does (`IOMatchCategory IOAccelerator`, vtable check against IOAccelerator's 272 slots).
   - It is registered only when the GPU is up: before then, WindowServer must find no accelerator, or a software one.
   - Only one accelerator may be in the KC.
2. **Connections.** `newUserClient` with type **0x4147** gives one NativeAdreno client, whose user client class is
   `AGXDeviceUserClient` (the name the macOS sandbox profiles admit).
   - Every Metal process opens twice: Turnip's enumeration opens and closes, then the device opens. The slot must be free again
     when `IOServiceClose` returns. The VM-era native log's three `0xe00002d5` refusals were this.
   - Refuse any other type without taking a slot: 0x4149 only comes with `TU_DARWIN_VIRTGPU=1`, and the frontend's display-hold and
     surface-sync connections look for `IOAcceleratorES`, which macOS has not got.
   - Render server: **WindowServer** in place of backboardd, at all six sites in Adreno830's `NativeAdrenoPublish.cpp` (slot 0 and
     its window). The macOS client lists (Dock, Finder, loginwindow, SecurityAgent, ControlCenter, NotificationCenter,
     iconservicesagent, WebKit's GPU process) replace the iOS ones.
3. **Tunnel.** The struct method's input is {`0x4e544e4c` 'NTNL', ABI method, payload} on any admitted selector, and the output is the
   method's exact record. Method `0xffffffff` with no payload answers success. Turnip probes selectors 0..255 and keeps the first
   that answers.
4. **Methods (ABI v2).**
   - Query must answer `abi_version` **2** (else Turnip fails with `NativeAdreno query failed: 0xe00002c7`) and flag
     `NativeAdrenoParamsHostImport`. Without that flag no IOSurface can be imported, so WindowServer has no framebuffer.
   - GetDeviceInfo: chip `0x44050001`, GMEM 12 MiB at `0x4000000`, UCHE trap base `0x1FFFFFFFFF000`, the client window as
     `va_start`/`va_size`, highest bank bit 16, swizzle 6, macrotile 1, UBWC 5, flag `ClientIova`. These are the hostsim's values;
     the board's must match the hardware.
   - BoAlloc (4 KiB multiples) and BoMap (at the client's iova; permissions 1 or 3, writeback 0, shareability 2).
   - BoCpuMap gives a token for `IOConnectMapMemory64(..., kIOMapAnywhere | kIOMapWriteCombineCache)`, which must be honoured as WC.
     BoCpuUnmap releases it.
   - BoFree: `kIOReturnBusy` means try again later.
   - BoImport pins the caller's own 4 KiB-aligned pages, for IOSurfaces. The frontend cleans and invalidates each surface's
     range with `sys_dcache_flush` at commit and completion.
   - Submit takes at most 128 IBs on queue 0. `kIOReturnBusy` means the ring is full, and Turnip then waits for its oldest fence.
   - WaitFence takes slices of at most 100 ms and returns `kIOReturnTimeout`.
   - GetStatus reports faults and suspends; `kIOReturnUnsupported` is accepted as "no counters".
   - GetTimestamp reads the GPU always-on counter. Set/GetMetadata are also called.
   - Queues 11/12 are not used.
5. **Ordering.** Turnip does not CPU-wait for a sync already resolved to a ring fence, so jobs run in submission order on one ring.
   Each job's prologue flushes and invalidates the GPU caches and waits for idle (`NativeAdrenoRing::emitPrologue`).
6. **Display (later).** NativeGopDisplay's CPU copy of a GPU-written framebuffer must wait for the fence of the work that wrote it.
   Adreno740's `native_adreno_fences()` export, which Adreno830 lacks (plan §3), is the way to do that. The copy must also invalidate
   the CPU's lines of those pages, because GPU writes are not coherent.
7. **Not needed now:** virtio-gpu methods, display holds and surface-sync tickets (§6), and GPU-copy present (`fb-gpu`, `0x7000`–`0x7005`).

## 5. Test on the tablet (main session)

```sh
# PianoA830 linked (configs/k27-native-kexts.txt or PIANO_EXTRA_KEXTS), NativeSoftGpu not; the GPU's bring-up arguments are
# the kernel side's (gpu.md §6: piano-gpu=1, the stop=N ladder, trace and status)
PIANO_EXTRA_KEXTS="$PWD/out/k27/kext/PianoA830.json" PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=300 \
PIANO_EXTRA_ARGS="exec-log=1 stderr-log=1 panic_on_crash_disabled=1 piano-gpu=1" tooling/k27/build-native-image.sh
tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img
grep -E "NativeAdrenoMetal|NativeAdreno|native-metal|NativeGopDisplay: (thumb|first|[0-9]+ swaps)|crash message|exit: pid [0-9]+ WindowServer" \
  logs/native-runs/<ts>-loop/xnu-console.txt
```

The success lines, in order (`stderr-log=1` keeps the first 159 bytes of each write):
1. The kernel side's own publication line, and its log of WindowServer reading `MetalPluginName`.
2. `NativeAdrenoMetal: WindowServer: stubbed <frontend class> <selector>`: a survey of the IOGPU selectors clients ask for, harmless.
3. **`NativeAdrenoMetal: WindowServer: device Adreno (TM) 830 (raytracing disabled)`**: the plugin, the bridge and Turnip are up on
   the kernel client. The same line follows for loginwindow, SecurityAgent and the agents.
4. `NativeGopDisplay: first swap submitted`, `swap layers 0x…: 0: 3200x2136 BGRA`, `first frame (swap N)`.
5. **`NativeGopDisplay: thumb frame N: 3200x2136 'BGRA', P% non-black, mean #RRGGBB, centre #RRGGBB`** with P well above 0. The
   login window is the wallpaper with the user tile and password field: expect most cells non-blank and a non-black mean. The
   27-row character picture shows the field's bright band in the lower middle.
6. No `crash message (SkyLight)`, no CoreAnimation `Assertion failed`, and no repeated `exit: pid N WindowServer`. The software
   path's backdrop assertion and RenderBox abort ([soft-metal.md](soft-metal.md) §11-12) do not apply: Core Animation renders on Metal.

| Line | Reading |
|---|---|
| `NativeAdrenoMetal: <proc>: no device: … IOServiceOpen(AGXAccelerator, 0x4147) failed: 0x…` | the kernel refused the open: `0xe00002d5` busy (no slot, or the probe's not yet freed), `0xe00002e2` not permitted or `0xe00002c1` not privileged (the open type, or the sandbox) |
| `… no external-method selector is admitted for this process` | no selector answered the tunnel probe |
| `… NativeAdreno query failed: 0xe00002c7` | Query's `abi_version` is not 2 |
| `NativeAdreno: tu_physical_device_init failed` | GetDeviceInfo: a chip id or GMEM Turnip does not know |
| `TU: error: NativeAdreno: kernel BoAlloc failed: 0x… (N times)` (also BoMap, BoImport, …), with the kernel's `PianoA830: gpu call failed: <proc>[pid] slot N BoAlloc -> 0x…` | memory or a refused argument; BoImport refused means WindowServer cannot render into its framebuffers. Turnip 0003 and the kernel's event lines (2026-11-11); before them these were silent |
| `MESA: error: device lost (tu_knl_darwin.cc:N): submit failed: 0x…` / `wait fence failed: 0x…` / `GPU faulted or hung` | device lost for that process: the kernel's `gpu recovery` and `gpu hang snapshot` lines say why (Turnip 0003; silent before) |
| `NativeAdrenoMetal: <proc>: no Turnip beside the plugin` | the bundle is incomplete in the seed |

## 6. Performance to expect

- **GPU**: WindowServer composites on the A830. With the annibale bring-up profile the DCVS stays at or below 342 MHz (Adreno830
  README). A 3200x2136 login frame (wallpaper, blur, a few layers) is a few milliseconds of GPU time at that clock, and `TU_DEBUG=sysmem`
  costs bandwidth. For reference, the VM drew the desktop with this userspace on the same GPU, at Linux's clocks, at 60-96 frames a
  second for a heavy window drag (kc283/kc282 in the plugin's notes).
- **Present**: NativeGopDisplay's CPU copy measured 38 ms a frame at 3200x2136 (read 33.7 ms) in the software runs. It stays the
  limit, so expect about 25 frames a second shown, whatever the GPU does. GPU-copy present (plan step 7, `fb-gpu=1`) or DPU flips
  lift it.
- **First boot**: shader translation and compilation for every client (about 40 s cold in the VM). Caches go to
  `<user cache>/com.apple.metal/<bundle>/m2v` and persist with `data-persist=2`.

## 7. CPU fallback (lavapipe): not built

The first version of this task asked for Mesa's CPU Vulkan driver behind the same plugin. It is not a near-free by-product of this
build, and it was set aside when the owner made the GPU route the priority:
- **LLVM.** llvmpipe needs it, and Homebrew's LLVM is arm64 only, while every image in an arm64e process must be arm64e. That means
  a from-source LLVM 22.1.8 (the newest major Mesa `cecc0e9` guards for) built `-arch arm64e`, AArch64 target only: about 30-60 min
  on this M1 Pro.
- **Pointer authentication at the JIT boundary.** arm64e C code signs and authenticates function pointers, while llvmpipe's JIT code
  calls raw ones. Three places need patches in gallivm: symbols resolved for the JIT (`dlsym` answers signed pointers), constant
  function pointers (`lp_build_const_func_pointer`), and JIT functions handed to C callers (`gallivm_jit_function`), with JIT-to-JIT
  tables kept raw.
- **The loader name.** The runtime opens the ICD by path and looks up `vkGetInstanceProcAddr` (`ash::Entry::load_from`). Lavapipe
  exports only `vk_icdGetInstanceProcAddr`, so it needs the same `__APPLE__` export Turnip carries in `tu_device.cc`.
- **JIT memory.** WindowServer is signed with library validation, not the hardened runtime, so mprotect-to-RX JIT pages are allowed
  on macOS. Sandboxed clients (SecurityAgent) were not checked.
- **Speed.** About 1-2 Gpixel/s of bilinear composition on 8 cores gives 30-80 ms per 3200x2136 login frame with blur, before the
  38 ms present.

To switch the plugin's ICD for a CPU fallback:
- put the driver at `Contents/MacOS/libvulkan_freedreno.dylib`: the plugin's `turnipPath()` takes that fixed name, and the bundle's
  CDHashes go into the trust cache as now;
- the kernel side needs an accelerator named `AGXAccelerator` that publishes `MetalPluginName NativeAdrenoMetal` without a GPU (a
  NativeSoftGpu variant);
- the device then reports the accelerator's port, so SkyLight takes its Metal compositor.

## 8. Open items

1. `max-download-size` of piano's ABL against the 245 MB image (§2).
2. The real Metal client count under macOS against the 32 slots (plan risk R4): the login screen had 7 Metal processes and 6 live
   connections; after the login the kernel's `PianoA830: gpu clients N (peak P, …)` lines give it (desktop section).
3. Surface sync: not needed for the desktop, decided 2026-11-11 (desktop section). The frontend's surface tickets (op `0x7200+11`)
   need an `IOAcceleratorES` service and open type 0x4149, which macOS never had: the VM's desktop ran without them too. Their known
   user is WebKit's canvases (putImageData read back half-drawn). If canvas artifacts appear, the kernel side should serve them on its
   own connection type and the frontend should look for `AGXAccelerator`.
4. `TU_DEBUG=sysmem` first. Drop it (`PIANO_A830_TU_DEBUG=`) once frames are right, and compare frame times.

## 9. Licences

- Mesa: MIT.
- Adreno830, ArmSmmu4K, AdrenoPM4 and AdrenoMetal: MIT.
- The plugin (`NativeAdrenoMetalPlugin.mm`): BSD-2-Clause-Patent.
- The bridge: metal2vulkan, GPL-2.0-or-later OR LGPL-3.0-or-later. It is linked statically into the plugin. If the bundle is ever
  published, honour the LGPL's terms for static linking.
- Binaries are never committed. Provenance of every input: `tooling/metal/UPSTREAM`.

## Measured 2026-11-11 07:45: the login screen on the A830

PianoA830 (piano-gpu=2, piano-gpu-delay=20) brought the GPU up S1..S10 (an earlier full run: SMMU, FWMEM, GMU-HFI, CP, ZAP, fence
self-test, METAL publish, DCVS, "GPU RUN 160M", 44-53 C). With Turnip on ABI v2 staged (this file's build) and the ADT's
vmm-present 0 (8-bit framebuffers) plus panic_on_crash_disabled=1, WindowServer composited the macOS login screen (date and clock,
the user's avatar, the password field) on the GPU and NativeGopDisplay showed it on the panel (camera, ~3.5 min after fastboot
boot). Open: the console mailbox at 0x9cb00000 was overwritten in that run (no log); the display's present path has no GPU fence
yet; no input to log in (touch not done; the buttons only). Later the same day: the mailbox was decayed, not overwritten, and decodes
(gpu.md §8); NativeGopDisplay waits for the GPU's fence (gpu.md §8); a USB HID keyboard from the Mac and the keyboard cover give input.
The run's log, read for the desktop: next section.

## Desktop readiness, 2026-11-11

Offline work (no boot) so that the first desktop session after a login on the A830 works and can be read from the console. The
input of tonight's run is a login: a password typed from the Mac (`piano-usb-hid=1`) or the keyboard cover.

### What the login run showed (07:44:43, 300 s)

The run's console mailbox decodes with the fixed reader (`tooling/native/xnu-log.py decode logs/native-runs/20261111-074440-loop/dump.bin`,
5653 lines, gpu.md §8). Bits are still wrong after the vote (threshold 3 of 11), so a word is sometimes garbled and the counts below
come from fuzzy matching (± a few lines). No USB console was recorded for it. The plugin's lines are there because the image had
`exec-log=1 stderr-log=1`.

| Line (on the console) | Count | Where / reading |
|---|---|---|
| `NativeAdrenoMetal: <proc>: device Adreno (TM) 830 (raytracing disabled)` | 6 + 1 | WindowServer, Installer Progress, loginwindow, SecurityAgent, iconservicesagent, VTDecoderXPCService, and MenuBarAgent (its line garbled). The meter says `conn 6/6`: 6 connections live at most, no slot refused (`nos 0`) |
| `native-metal: no device answer: the device has no answer for <selector>` | ~24 | 3-6 per client (maxVertexAmplificationCount, supportsComputeCompressedTextureWrite, supportsConditionalLoadStore, gpuCoreCount, maxComputeThreadgroupMemory, isLargeMRTSupported). Harmless (`harmlessProblem`); the frontend patch keeps them off the console now |
| `native-metal: heap N bytes type 0 options 0x220 -> N` | 11 | WindowServer and SecurityAgent; an information line per heap. Rate-limited now |
| `native-metal: surface texture refused: 4096x2160 YCbCr format 501/503 converts on the CPU` | 4 (the frontend's own cap) | WindowServer, from about the time the login screen appears: the wallpaper video's frames. Only formats 500/505 (4:2:0) have the planes-only path (`METAL2VULKAN_YCBCR_PLANES_ONLY`); these Core Animation converts on the CPU. As in the VM |
| `native-metal: binary archive refused: BinaryArchive(InvalidFile …RenderBox…)` | 1 | SecurityAgent: pipelines compile from source. Harmless, as in the VM |
| `NativeAdrenoMetal: SecurityAgent: stubbed NBTexture allocatedSize` | 1 | the stub survey; answers 0 |
| `setBackgroundGPUPriority:2` / `setGPUPriority:1` | 2 | iconservicesagent; kept, nothing schedules by them |
| `PianoA830: <proc> opens the accelerator, type 0x5: 0xe00002c7`, `type 0x4147: 0` | 1, 11 | IOGPU's type-5 open is refused without taking a slot; two 0x4147 opens per Metal process (enumeration, device) |
| `PianoA830: row3 IRQ 0 wait 3838 to 1850 late 1330 …` (at the end) | — | `to`: WaitFence calls that ended with kIOReturnTimeout, zero-timeout status polls included; `late`: completions seen after a poll slice (polled fences, annibale). Not errors |
| `PianoA830: row0 GPU RUN 160M/342M … f0 h0 … 42-57C` | ~2 a second | no fault and no hang for the whole run; DCVS residency 53 % at 160 MHz, 47 % at 342 MHz |
| `PianoA830: row2 MEM pool 512 used 488 imp 772 … vm 622 sc9 WindowServer:410+86` | — | the 512 MiB BO pool was 95 % used at the login screen. BOs past it come from contiguous or scattered pages (`sc9`: 9 MiB outside the pool), by design. WindowServer: 360-410 MiB own, 86 MiB imported |
| `PianoA830: t gpu meter rs 622 2.5/11.5ms … conn 6/6 nos 0 ovf 0 … pf 0/0/0/0` | 4 (a minute each) | steady state: about 10 render-server jobs a second, 2.5 ms mean and 11.5 ms longest GPU time; no slot refusal, no window overflow, no page fault |
| `NativeGopDisplay: N swaps submitted, …; copies mean 19.7 ms (read 17.4) …, N waits released`, `at most 9-10 frames in a second` | every ~10 s | WindowServer composited about 10 frames a second; every copy waited for its GPU fence and none ran into the 50 ms limit |
| `crash message`, `Assertion failed`, `exit: pid N WindowServer`, `device lost`, `GPU faulted or hung`, `Bo… failed`, `submit failed`, `noslot`, `LOST` | 0 | none. Turnip could not have printed its own failures anyway: a release build's `vk_log` is silent (fixed by Turnip 0003) |
| `AMFI: Denying core dump for pid N (watchdogd)` | ~8 | watchdogd's crash loop (no watchdog driver natively), not the GPU |
| `airportd: AssertMacros: !eachConnection.isLocal`, `VTDecoderXPCService: unrecognised property key` | a few | Wi-Fi and VideoToolbox, not the GPU path |

Timeline. Kernel time T counts from XNU's start, 20 s after the `fastboot boot` command (the loop's "XNU started"). Sources: the
bring-up trace's microsecond stamps, the meter line once a minute after S10, the display's line every ~10 s while swaps arrive, the
1970 calendar of launchd's own lines; ±10 s after T 60 s.

| T | What |
|---|---|
| -20 s | `fastboot boot` (241960 KB, 5.6 s download), ABL, our UEFI, DarwinBoot |
| 0 | XNU starts |
| ~7 s | PianoA830 starts and waits 20 s (`piano-gpu-delay=20`) |
| 27.3-28.4 s | S0..S10 in 1.07 s (S3's 512 MiB pool reservation 1.02 s); AGXAccelerator published |
| ~48 s | launchd (`1970-01-01 07:30:48`) |
| ~65 s | WindowServer (pid 102): MetalPluginName, two 0x4147 opens, `device Adreno (TM) 830`, then GPU jobs (2 in the first meter minute) |
| ~90-100 s | first swaps; frames black (WindowServer up, no login window yet) |
| ~100-145 s | the seed's boot job (settime, bind mounts), Installer Progress, then loginwindow, SecurityAgent, iconservicesagent and VTDecoderXPCService get devices |
| ~150-160 s | the first non-black frame: the login screen (`100.0% non-black, mean #9f9f9f`), ~10 frames a second from then on |
| 300 s | `piano-reset-after` |

What the ~3 min go to, and what to cut: the GPU path is not on the critical path. The accelerator is published 37 s before WindowServer
starts, so `piano-gpu-delay=20` costs nothing here (0 would save nothing either; gpu.md §8 wants one try of it before it becomes the
default). The GPU does 2.5 ms a frame, so `TU_DEBUG=sysmem` is no limit and stays for tonight (GMEM is untested natively). The time goes
to macOS's own boot until WindowServer (~65 s), then ~85 s until loginwindow has its window (boot tasks, the seed's boot job, first-use
shader translation in every process with a cold m2v cache), then the camera. At the login screen the frame rate (~10 a second) is
WindowServer's CPU: the 4096x2160 wallpaper video, decoded in software by VTDecoderXPCService, converted to RGB on the CPU (formats
501/503 above). Nothing is cut tonight.

### After the login: what changes

Metal clients. At the login screen 7 processes had a device. After it, expect the Dock (a large slot), Finder, SystemUIServer, Control
Centre, Notification Centre and its widget renderer (chronod), WindowManager, Spotlight, WallpaperAgent (large), TextInput and
accessibility agents, screencaptureui on a screenshot, Safari's `com.apple.WebKit.GPU` when Safari opens, and background daemons that
use Metal for Core ML and video (media and photo analysis, more VTDecoderXPCService instances): 15-25 live connections. The kernel has
32 slots in the 16 GiB table (`va=16`): slot 0 WindowServer's 1.5 GiB window (446 MiB used at the login screen), slots 1-3 large
(512 MiB: Dock, WallpaperAgent, iconservicesagent), 28 small (448 MiB) for everyone else; a process on neither list gets a small slot
only while more than 4 are free. Each connection holds up to 4096 objects (16384 for WindowServer, 8192 for a large client). Memory
beyond the 512 MiB pool comes from contiguous or scattered pages; one BO is at most 256 MiB (Query's `max_bo_bytes`, which Turnip does
not take as its allocation limit: an 8192x8192 RGBA8 texture is refused with BadArgument, found by the host check).

Metal features the login screen did not use, and where each stands:

| Feature | Natively |
|---|---|
| Blur, backdrop layers (Dock, menu bar, Control Centre) | the login screen already blurs its wallpaper. The desktop's window effects go through MPS: the plugin's fixes from the VM (userDictionary, gpuCoreCount, protocol conformances) are in this plugin |
| IOSurface sharing across processes (window backing stores) | 976 imports at the login screen, read-only imports coherent (`coh 1`); the host check churns 24 panel-sized surfaces with no import left behind, also in a 124 MiB window |
| Argument buffers, heaps | the VM's frontend and bridge, unchanged; SecurityAgent made heaps at the login screen; heaps checked offline |
| MTLSharedEvent | checked offline (signalled by a command buffer, read back) |
| MSAA | 4x with resolve checked offline |
| Compute | MPS's serial encoders as in the VM; a concurrent compute encoder (Core ML, MPSGraph) failed its whole command buffer, now it runs serially (frontend patch) |
| Large textures | 3200x2136 and 4096x2160 at the login screen; 8192x4096 checked offline; above 256 MiB per BO refused |

Surface sync (§8.3): not needed for the desktop. The frontend's tickets need `IOAcceleratorES` and open type 0x4149, which macOS never
had, so the VM's desktop (Dock, Finder, Safari, Settings) ran without them. Their known user is WebKit's canvases (putImageData read
back half-drawn); a CPU reader in the same process already waits for the GPU (`waitForResource`). Not implemented.

The VM's unexplained hang. At 3200x2136 the VM hung about a minute after every login (3 of 3 runs), and its login transition had passes
that ran several seconds at 1.1 GHz, past Linux's 2 s hang check. Here the A830 runs at 160-342 MHz and both watchdogs call 2 s
without progress a hang. A long full-panel pass can therefore be taken for a hang during the transition: the recovery fails that job,
WindowServer loses its device and the session falls back to the login window. Hence `piano-gpu-hang-ms=10000` tonight (the VM's
setting that let the login through). A genuine hang then freezes the panel 10 s before the recovery; two recoveries a minute are
allowed, the third gives the GPU up for the boot.

### What changed

- Kernel (PianoA830, `ADRENO830_MACOS` only): the session's GPU events as `PianoA830: gpu …` lines whatever `piano-gpu-trace` says,
  the first time and then every 10th, 100th, …: `call failed: <proc>[pid] slot N <method> -> 0x… (<IOReturn>)` for every failed call
  but the answers Turnip expects; `open refused: <proc>[pid] -> 0x…: <why>`; `recovery: culprit slot N <proc>; jobs in flight fail: …`,
  `hang snapshot 1/3..3/3` (cause, fences, RBBM, RPTR/WPTR, BR IB1/IB2, the pipes' fault and protect status, the GPU bank's FSR/FAR),
  `recovery done`, `DEVICE LOST`; `page fault at 0x…: <proc>'s window (slot N)`; `clients N (peak P, refused …): 0:WindowServer 446M
  1:Dock 60M …` when the set changes, at most every 10 s. The status rows: at most every 10 s once the GPU runs, at once when row 0's
  state, fault or hang count changes (they were two lines a second). The first 160 opens and 96 MetalPluginName reads are logged.
  `piano-gpu-hang-ms` (gpu.md §6).
- Turnip `0003-turnip-darwin-failures-on-stderr.patch` (§1): device lost and failed kernel calls on stderr.
- Frontend patch (§1): `NBLOG` rate-limited per call site on stderr, `no device answer` off the console, concurrent compute serial.
- Abort reporter (`tooling/metal/turnip-a830/abort_reporter.mm`, ours: NativeSoftMetal's): in WindowServer, Dock, Finder,
  SystemUIServer, ControlCenter, NotificationCenter, WindowManager, Spotlight, loginwindow, SecurityAgent and the other desktop system
  processes, an abort, an uncaught exception, a C++ terminate or a fatal signal prints `NativeAdrenoMetal: <proc>: SIGABRT`, the
  `crash message (<image>)` lines (SkyLight's and assert()'s text), `uncaught <name>`/`reason: …` and the stack as `#N <image>+0x…`,
  then the process ends as it would have (its own handlers chained). `piano-metal-report=0` leaves it out, `=2` puts it in every
  process that loads the plugin. Without it a WindowServer that dies after the login left only `exit: pid N WindowServer status 0x6`.
- `PIANO_A830_LOG=PREDICATE` (build-native-image.sh): the seed's boot job also prints those processes' os_log errors and faults
  (`stage-soft-metal.py --log-errors`), for SkyLight's and Core Animation's reasons that never reach stderr. Off by default.
- Host checks (§3): the desktop extras and the reporter; `run-a830-host-tests.py` adds `macos_events`.

Built (offline, 2026-11-11 evening): Turnip CDHash `031df58ae7cacc6ccb317155eb293afe55b8f632`, plugin `9c42e84423bb5be3ad7f76b4451b6c2f6226a325`
(both twice from scratch, the same); bundle files sha256 `cead9f7c…5222` (Turnip), `d4ffaf27…d4d6` (plugin), `9595a4d2…f86b`
(Info.plist). Staged with `tu-debug` sysmem into the morning's input seed: `scratch-data.img` 124657792 bytes packed, identity
`262446fdd2ce3d8e` (the login run's was `aced5bf9794b42f3`), trust cache sha256 `5768d31c…e291`. A new seed identity: `data-persist=2`'s
store starts over at the first boot of the new image (the Data changes of earlier native boots are dropped; the account is in the seed).
Checks: `check-turnip-a830-host.py` all PASS (17 runs), `run-a830-host-tests.py` upstream 43 and macOS 42 pass with the known
exceptions as before, `macos_lists` and `macos_events` PASS, `check-a830-abi-host.py` PASS, the kext links (20 weak symbols coalesced,
0 unresolved imports). Since then Turnip 0004/0005 (§1): CDHash `205e665f594c6b33c13369cdee1fc4863ce00f68`, the plugin unchanged
(`9c42e844…`); the next `build-native-image.sh` restages the seed (a new identity again).

### Tonight's run

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=1200 \
PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-power=1 exec-log=1 stderr-log=1 piano-gpu-hang-ms=10000" \
  tooling/k27/build-native-image.sh
tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img     # with the USB console recording (piano-usb=2)
```

New against the given command: `exec-log=1 stderr-log=1` (without `stderr-log=1` no line of the plugin, the frontend, Turnip or the
reporter reaches the console; the login run had both) and `piano-gpu-hang-ms=10000`. The command line is 846 characters (DarwinBoot
takes 1023). Optional: `PIANO_A830_LOG='process == "WindowServer" OR process == "Dock" OR process == "Finder" OR process == "loginwindow"'`
in front of it, for the os_log errors of those processes (a new seed again). The installed ESP (`tooling/persist/build-esp.py --profile
daily`) has none of this until it is rebuilt from this tree with `--arg exec-log=1 --arg stderr-log=1 --arg piano-gpu-hang-ms=10000`.
stderr-log costs console lines: about 760 in the login run's 5 minutes (2.5 a second, most of them not the GPU's).

What the run must show, in order:
1. `PianoA830: bring-up done: the GPU runs`, then `fd2: pid N WindowServer: NativeAdrenoMetal: WindowServer: abort reporter installed …`
   and `… device Adreno (TM) 830 (raytracing disabled)`, then `PianoA830: gpu clients 1 (…): 0:WindowServer …M`.
2. The login screen as in the morning: `NativeGopDisplay: thumb frame N: 3200x2136 'BGRA', 100.0% non-black …`, `gpu clients 6 …`.
3. After the password: `loginwindow` stays, new `… device Adreno (TM) 830` lines for Dock, Finder, SystemUIServer, ControlCenter,
   NotificationCenter and the rest, and `gpu clients N (peak P, refused for want of a slot 0): …` lines with 15-25 of them.
4. A desktop thumbnail: a bright menu-bar band in row 00, the Dock's band in the bottom rows, the wallpaper in between; swaps keep
   coming (`N swaps submitted`), `waits released` close to the copies.
5. None of: `gpu call failed`, `gpu open refused`, `gpu recovery`, `gpu hang snapshot`, `gpu DEVICE LOST`, `gpu page fault`,
   `MESA: error: device lost`, `TU: error: NativeAdreno: kernel`, `NativeAdrenoMetal: <proc>: SIGABRT` / `crash message` / `uncaught`,
   `exit: pid N WindowServer`, a growing `f`/`h` count in `row0`.
6. `row0 … NNC`: the temperature with a desktop at 342 MHz (the login run reached 57 C; the DCVS holds 160 MHz from 70 C).

Reading a failure: `exit: pid N <proc> status 0x6` (SIGABRT) or `0xb` (SIGSEGV) comes right after the reporter's lines of that
process, which name the framework; a `gpu call failed` names the process, the ABI method and the IOReturn, and Turnip's `TU: error:
NativeAdreno: kernel <method> failed` / `MESA: error: device lost (…): <reason>` and the frontend's `<proc>: native-metal: <what>
refused: <why>` come from the same process just before or after; a `gpu recovery` line with its three `hang snapshot` lines places a
hang (culprit, where BR's IB1/IB2 stood); `gpu open refused … no free slot, N connections live` is the slot table. A WindowServer
restart puts the session back at the login window; after a third recovery within a minute (`DEVICE LOST`) the GPU is gone until the
next boot.

Open risks:
1. The VM's post-login hang at 3200x2136 (root cause never found) may come back natively; the snapshot lines are what to read then.
2. The wallpaper video costs CPU (software HEVC decode plus Core Animation's CPU conversion of formats 501/503) and held the login
   screen to ~10 frames a second; the same holds for the desktop if its wallpaper is a video: a still wallpaper (System Settings,
   Wallpaper) is the lever.
3. Memory: the BO pool was 95 % used at the login screen; the pool is 1536 MiB now (an eighth of the RAM, gpu.md §8 "The BO
   pool"), and what still does not fit lives on the fallback pages, which no native run has used in volume yet.
4. Thermal: no cooling or charging control (PianoPower read-only); watch `row0`'s temperature, keep the session within the 20 min
   reset.
5. No surface sync: WebKit canvases may show stale or half-drawn pixels.
6. stderr-log=1 puts every process's stderr on the console; a chatty daemon after the login can add lines (the GPU path's own are
   rate-limited).
7. A new seed identity: the store of earlier boots is dropped, and every shader cache is cold on the first boot (first-use stalls per
   client).

### A/B: sysmem against GMEM rendering

The seed stages `tu-debug` = `sysmem` (`PIANO_A830_TU_DEBUG`'s default: no GMEM for the first bring-up). Turnip 0005 lets the boot
line override it, so both images share one seed (one seed identity: a `data-persist=2` store survives the switch):

```sh
A="panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-power=1 exec-log=1 stderr-log=1 piano-gpu-hang-ms=10000"
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=1200 PIANO_EXTRA_ARGS="$A" tooling/k27/build-native-image.sh &&
  rm -rf out/ram-sysmem && cp -R out/ram out/ram-sysmem
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=1200 PIANO_EXTRA_ARGS="$A piano-tu-debug=none" tooling/k27/build-native-image.sh &&
  rm -rf out/ram-gmem && cp -R out/ram out/ram-gmem
tooling/native/native-loop.sh out/ram-sysmem/boot-piano-macos-ram-A-bundled.img   # then out/ram-gmem/...
```

`piano-tu-debug=gmem` instead of `none` forces GMEM on every pass (a stress run once `none` is clean). First check that the mode took:
WindowServer's `TU: info: TU_DEBUG=... from boot argument piano-tu-debug` (or `... (none: Turnip picks GMEM or sysmem per render
pass)`); a sandboxed client that cannot read `kern.bootargs` falls back to the seed's file, i.e. sysmem, so only WindowServer's line
is proof. What GMEM trouble looks like here: GMEM is the GPU address window at 64 MiB (`A830_GMEM_BASE`, 0x4000000..0x4c00000 for
12 MiB; Query's `gmem_base`/`gmem_bytes`, which Turnip takes as they are), so a `gpu page fault at 0x04xxxxxx` names a GMEM address
gone to the SMMU (a wrong base or size); a `gpu recovery` whose `hang snapshot 3/3` has a non-zero BR `prot` word (a protected
register or range the pass touched; RB_GC_GMEM_PROTECT is 0x0C000000 after the secure exit) or BV's status words busy while BR waits
(the binning pipe stuck) is a GMEM pass the hardware refused; `MESA: error: device lost` follows either. Without any of those: tile-shaped garbage or stale tiles in the thumbnails
(`NativeGopDisplay: thumb frame N`: rectangles of a bin's size, typically 96 or 128 pixels, in otherwise sane rows) is a resolve or
load/store-op bug; and the DCVS row's busy share should drop with GMEM on a desktop of overlapping layers, since sysmem renders
every blend through DDR. Same order of checks as tonight's list; a GMEM failure is a reason to stay on sysmem,
not to stop the session.

### The VM's post-login hang, natively

In the VM (virgl to the host's msm, 3200x2136, 2026-11-10/11), about one to two minutes after the login the GPU hung for real (the
host's hangcheck fired with no ring progress, also at 10 s), WindowServer lost its device and the session went back to the login
window; three of three runs at 3200x2136, none at 1600x1068. `TU_DEBUG=sysmem,nocb` did not change it; with the host's GPU
diagnostics or metal2vulkan's pipeline log on it did not happen in the runs tried (timing-sensitive), and the root cause was never
found. The login transition itself had multi-second single passes at 1.1 GHz, which a 2 s hangcheck called hangs: hence
`piano-gpu-hang-ms=10000` here, where the DCVS runs at most 342 MHz. Natively the same hang shows as, in this order: the status
row's `h` count rising, `PianoA830: gpu recovery: culprit slot N <proc>; jobs in flight fail: ...`, `gpu hang snapshot 1/3..3/3`
(cause, the fences, RBBM status, RPTR/WPTR, BR's IB1/IB2, the pipes' status and protect words, the GPU bank's FSR/FAR),
`gpu recovery done`, then in the culprit's process `MESA: error: device lost (tu_knl_darwin.cc:N): ...`; a third recovery within a
minute is `gpu DEVICE LOST` (the GPU stays off until the next boot). A WindowServer that loses its device restarts (`exit: pid N
WindowServer ...`, then the login window). What to keep from such a run: the three snapshot lines (where BR stood: the same IB1 in
every hang points at one pass), the culprit's process, the `clients` line before it, and whether it came with `piano-tu-debug=none`
or with sysmem. Not a hang: a long pass that ends before 10 s (no recovery line); a page fault (its own line, the process's window).

### The next desktop run (2026-11-12)

What changed since attend4: the shared VM's 524288 table ranges, a 2048 MiB BO pool, macOS's window table (WindowServer 3.5 GiB),
Turnip 0006, and the address-space lines (gpu.md §8, "The first desktop's address space"). One image, attend4's boot line plus
`stderr-log=1` (Turnip's 0006 note and its errors reach the console only with it) and `piano-gpu-hang-ms=10000`:

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=1200 \
PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-usb-hid=1 piano-power=1 piano-keyboard=2 piano-keyboard-gestures=1 piano-backlight=2 piano-backlight-keys=1 exec-log=1 stderr-log=1 piano-gpu-hang-ms=10000" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-attend5 && cp -R out/ram out/ram-attend5
tooling/native/native-loop.sh out/ram-attend5/boot-piano-macos-ram-A-bundled.img
```

The new Turnip makes a new seed identity: a `data-persist=2` store of another seed is left alone (Data volatile for this run, as the
store guard does). What to look for: `board: BO pool 2048 MiB` (or `1024` if S3 had to halve it), no `gpu call failed: ... BoMap`;
if one comes, the `gpu BoMap refused ...` line after it names the limit or the argument; the `gpu va/map` lines' ranges and each
window's `pk` against its size, at the login screen and as apps open; in the processes that had slot 5, a `TU: error: NativeAdreno:
this device's GPU window ...` note at most, and a device; NativeGopDisplay's `N gave up after 50 ms` (the flicker's source, gpu.md §8).
