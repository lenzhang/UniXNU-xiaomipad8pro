# GPU userspace on the A830: the VM's Metal plugin, Turnip on Adreno830's ABI v2

Status: built and checked offline, 2026-11-11. Nothing has run on the tablet yet. The kernel half (package/PianoA830: board inputs,
firmware, bring-up) is a separate task, see [gpu.md](gpu.md). Plan: `droidvm-build/piano-patches/gpu-native-plan.md` §4 steps 2 and 3.

## 1. Purpose

WindowServer and every other Metal client get a real Metal device on piano's Adreno 830. The userspace is the one that drew the macOS 27
desktop in the VM (`NativeAdrenoMetal.bundle`: plugin, frontend, Rust bridge, metal2vulkan, Turnip). Only Turnip's transport changes.
In the VM, Turnip's darwin backend reached the host GPU through virtio-gpu (the `dwv` backend, accelerator open type 0x4149).
Natively it uses its NativeAdreno backend (open type 0x4147) on Adreno830's user client, as on the OnePlus 13T.

| Piece | Natively | Source |
|---|---|---|
| `Contents/MacOS/NativeAdrenoMetal` (plugin + frontend + bridge) | **byte-identical to the VM's** (CDHash `ea549d13…`): its objects rebuild byte for byte, so the VM's linked executable is taken (ld 27106.1 does not link it reproducibly) | macOS-DroidVM `overlay/metal`, UniXNU-turnip `drivers/AdrenoMetal`, bridge `out/metal/bridge` |
| `Contents/MacOS/libvulkan_freedreno.dylib` (Turnip) | rebuilt: the VM's Mesa tree, `NativeAdrenoABI.h` v2, 2 patches (CDHash `eaa367a2…` on 2026-11-11; two cold builds byte-identical) | Droid-VM Mesa `cecc0e9` + 23 VM patches + `tooling/metal/turnip-a830/mesa/` |
| `Contents/Info.plist` | the VM's (`org.droidvm.NativeAdrenoMetal`, principal class `NativeAdrenoMetalDevice`) | |

The Turnip patches (`tooling/metal/turnip-a830/mesa/`):
- `0001-turnip-darwin-native-a830.patch`:
  - The virtio-gpu backend runs only with `TU_DARWIN_VIRTGPU=1` (`=probe`: the VM's board probe). So no Metal process opens the
    accelerator with type 0x4149.
  - A `dsb sy` before every Submit and at the end of `vkEndCommandBuffer`. Command streams are CPU writes into write-combined BO
    views, possibly recorded on another core. This is the 13T's fix D1 on the same A830.
  - `gpu_id` 830 for chip `0x44050001`. The decode formula would give 6850.
- `0002-turnip-darwin-tu-debug-file.patch`: without `TU_DEBUG`, the first line of `/private/var/ngpu/debug/tu-debug` stands in for
  it. A system daemon's environment comes from a launchd job on the read-only system volume.

ABI: `NativeAdrenoABI.h` from `package/PianoA830/` when the kernel side has it, else Adreno830 `1a13210`'s
(sha256 `20ac9298…70d2`, pinned in the build script; a different header is used but noted). The backend also uses one name v2
lacks, `NativeAdrenoParamsOwnedCoherent` (Adreno740's v2 flag for IO-coherent BOs, value 2), added by
`tooling/metal/turnip-a830/NativeAdrenoABICompat.h`. Adreno830 never sets it, so CPU views of BOs stay write-combined.

## 2. Build, check, stage

```sh
tooling/metal/build-turnip-a830.sh           # -> out/turnip-a830/NativeAdrenoMetal.bundle (about 50 s cold on the M1 Pro, seconds after)
python3 tooling/metal/check-turnip-a830-host.py   # the offline ABI handshake (§3), about 15 s
# staging alone (build-native-image.sh does it by itself when PianoA830 is linked):
python3 tooling/k27/stage-soft-metal.py --out out/turnip-a830/stage \
  --file out/turnip-a830/tu-debug:private/var/ngpu/debug/tu-debug out/turnip-a830/NativeAdrenoMetal.bundle
```

`tooling/k27/build-native-image.sh` builds and stages this whenever `PianoA830` is linked, either from the list or from
`PIANO_EXTRA_KEXTS`. `PIANO_A830_METAL=0|1` overrides that. The result goes to `out/turnip-a830/stage/{scratch-data.img,kernel.macho.trustcache}`,
and the image takes those files unless `PIANO_SEED` / `PIANO_TRUST` name others. The staging is redone only when the input seed, the
input trust cache, the stage script, the bundle's files or `PIANO_A830_TU_DEBUG` change (`out/turnip-a830/stage/stamp`).
Builds are byte for byte the same, cold ones included: Mesa's generators run with `PYTHONHASHSEED=0`, and the plugin is the VM's
binary. So the staged seed keeps its identity across rebuilds.
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

Kits: Adreno830 `1a13210` is written against ArmSmmu4K `e923329` and AdrenoPM4 `282606b`. The check fetches both as GitHub archives
into `out/turnip-a830/kits`. The standalone ArmSmmu4K checkout in `unixnu-research` is too new: its arena takes page callbacks, from
`48dbd6c` on.

## 4. What the kernel side (PianoA830) must provide

Read from Turnip's darwin backend, the plugin and the frontend. The hostsim already models items 1-5.

1. **Discovery.** An `IOAccelerator` subclass (IOGraphicsFamily, macOS) whose C++ class is named **`AGXAccelerator`**.
   - Turnip finds it with `IOServiceMatching("AGXAccelerator")`, because the plugin sets `TU_DARWIN_SERVICE=AGXAccelerator`.
   - Its personality carries `MetalPluginName = NativeAdrenoMetal` and `MetalPluginClassName = NativeAdrenoMetalDevice`, as macOS-DroidVM's
     `overlay/kext/DroidVMGpu.json` does (`IOMatchCategory IOAccelerator`, vtable check against IOAccelerator's 272 slots).
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
| `BoAlloc failed` / `BoMap failed` / `BoImport failed: 0x…` | memory; BoImport refused means WindowServer cannot render into its framebuffers |
| `submit failed: 0x…`, `GPU faulted or hung` | device lost: the kernel's fault and status rows say why |
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
2. The real Metal client count under macOS against the 32 slots (plan risk R4): count the `NativeAdrenoMetal: <proc>: device` lines.
3. Surface sync. In the VM, the frontend's surface tickets (op `0x7200+11` through the tunnel) let IOSurface locks wait for the GPU;
   WebKit canvases needed them. Natively they are off (§4.2). If canvas artifacts appear, the kernel side should serve them on its
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
yet; no input to log in (touch not done; the buttons only).
