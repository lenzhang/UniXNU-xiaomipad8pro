# The machine identity (2026-11-12)

What macOS on piano calls itself: the root's `model`, `target-type`, `target-sub-type` and `compatible`, and `/product`'s
`fdr-product-type`, `sub-product-type`, `unique-model` (and the display strings `product-name`, `product-description`,
`product-soc-name`, kept in `configs/k27-adt.toml`). Since the VM era it is Apple's vma2 identity (`VirtualMac2,1`, `VMA2MACOS`,
`VMA2MACOSAP`). This page is the inventory of what depends on it (the kernel, the kernel collections, macOS 27.0.1's userland,
this repository), the candidates for another one, the build switch, and what a boot with another identity must show.

Sources: the native kernel collection (`out/k27/kc-k27-native.bin`) and Apple's kernelcaches (the vma2 27.0.1 base `kc27.bin`, the
27.3 `kernelcache.release.mac17j`): their `__PRELINK_INFO` (Info.plists, matching dictionaries) and strings; the k27 XNU source;
the installed macOS 27.0.1 (26A434) on the tablet, read only: its dyld shared cache's strings mapped to images with the cache's
`.map`, the system volume's files, `sysctl`, `ioreg`, `system_profiler`; the Data seed (mounted read-only). Behaviour, strings and
symbols only, no disassembly. "verified" = read in those sources or on the tablet; "inferred" = not yet seen on a boot.

## 1. The result in short

- **No kext matches on the identity.** Not one personality in the native collection (Apple's 55 bundles or our 25 kexts) matches on the
  root's model, target-type or compatible; ours match `IOPlatformExpertDevice` by class, the platform expert is ArmDTPlatform
  (`IONameMatch` `device-tree`, the root node's name). The vma2 base's own platform expert (AppleVirtualPlatformARMPE, compatible
  `AppleVirtualPlatformARM`) and the 27.3 kernelcache's (AppleARMPE, compatible `AppleARM`) are not linked or do not match. The
  note in `configs/k27-adt.toml` and `bringup-debugging.md` §5.3 that "the vma2 kernel collection's kexts match on it" holds for
  the VM-era collection only (verified).
- **"Is this a virtual machine" is not the model.** About 65 frameworks and daemons ask `kern.hv_vmm_present` (ADT `/defaults
  vmm-present`), which the desktop images already set to 0 (the tablet reads `kern.hv_vmm_present: 0`). The model string
  `VirtualMac2,1` itself appears in a handful of places (ProductKit's product tables, MobileDevice's device map, CoreTypes' icons,
  warmd, configd's databases).
- **So the identity is a userland choice**, with a few kernel constraints (section 3). The kernel-side values (chip-id,
  `/chosen` security values, `vmm-present`, `serial-number`) stay as they are in every candidate.

## 2. Inventory

### 2.1 The kernel (k27, xnu-12377)

| Value | Read by | Effect | State |
|---|---|---|---|
| root `model` | `IODTPlatformExpert::getMachineName` | `hw.model` and `hw.product` (arm64: both the root model) | verified (source, tablet) |
| root `compatible[0]` | `getModelName` ("AAPL," stripped) | `hw.target` (today `xiaomi,piano`, the DTS's first entry) | verified |
| root `target-type` | `gTargetTypeBuffer[16]` (pe_init.c), `hw.targettype`, `hw.jetsam_properties_product_type` (fallback) | `PE_device_is_simulated()` is true when it contains `sim`; `J236c` enables a CPU tunable; panic info | verified (source); tablet: `hw.targettype: VMA2MACOS` |
| `/product unique-model`, `sub-product-type` | `gUniqueDeviceTargetTypeBuffer[16]`, `gUniqueDeviceModelTypeBuffer[32]` | panic log / panic info only | verified (source) |
| `/product product-soc-name` | `machdep.cpu.brand_string` | System Information's Chip | the display-names change (`configs/k27-adt.toml`) |
| `/defaults vmm-present` | `kern.hv_vmm_present` | the userland's VM test (2.3) | verified |
| `/chosen chip-id` (0x8011), `board-id` (absent), `security-domain`, `certificate-*`, `effective-*` | AppleImage4 / Image4 (chip environments incl. `CHIP_VMA2*`), AMFI | trust caches, cryptexes, image4 checks | verified (strings): **not touched by any candidate** |

### 2.2 The kernel collections

| Collection | Identity matching | Identity read in code | State |
|---|---|---|---|
| native (`kc-k27-native.bin`: kc-keep.txt + DiskImages + their closure, 55 Apple bundles, 7 of them KPI pseudo-kexts, + this repository's) | none: Apple's personalities match IOResources, IOMedia, IOHIDInterface, `defaults`, child-node names (`scaler,s5l8960x`, `iommu-mapper`, `audio-data,hdmi`); ours `IOPlatformExpertDevice` by class, `paravirtualizedgraphics,gpu`, IOResources | IOgPTPPlugin: `target-type` must exist (`targetTypeStr != nullptr`), then its `TemperatureSensor` table (J274 ... J493, "PMU tdevN") names a HID temperature sensor for clock drift; no entry for VMA2MACOS, K114c or Piano. AppleM2ScalerCSCDriver reads `target-type` (inert: no scaler node). Image4/AppleImage4/AMFI/sandbox/IOCryptoAccelerator: chip-id, board-id, `kern.hv_vmm_present` | verified |
| vma2 base `kc27.bin` (VM era) | AppleVirtualPlatformARMPE on root compatible `AppleVirtualPlatformARM`; the paravirtual kexts on virtio IDs | - | verified; not in native boots (`check-native-kexts.py`) |
| 27.3 `kernelcache.release.mac17j` | AppleARMPE on root compatible `AppleARM` (the 27.3 profile's compatible `K114cAP` leaves it out, ArmDTPlatform stays the platform expert) | AppleTopCaseDriverV2: per-model personalities `Mac MTP Actuator/MT/Keyboard - <model>` with `ModelNumber` (Mac14,2 ... Mac18,1, Mac17,11 among them) on `IOHIDInterface` with `Transport` FIFO: only an Apple internal keyboard/trackpad (MTP) would meet them; IOgPTPPlugin's same table (no K114c) | verified (Info.plists) |
| this repository's kexts | `IOPlatformExpertDevice` by class | none reads model, target-type or compatible (grep) | verified |

### 2.3 macOS 27.0.1's userland (the tablet's system volume and shared cache)

| What | Who (images whose strings name it) | Effect / note |
|---|---|---|
| `kern.hv_vmm_present` | ~65: SkyLight, QuartzCore, CoreVideo, MediaToolbox, libMobileGestalt, ApplePushService, IDSFoundation, AuthKit, AppleAccount, DeviceIdentity, FindMyBase, MobileActivationMacOS, SoftwareUpdateCoreSupport, OSInstaller, ControlCenter, Sharing, AirPlaySupport, CoreML/Espresso/ANE, Hypervisor, Virtualization, WebGPU, ... (sharingd, akd, identityservicesd, tccd, storekitagent: `isVirtualMachine`) | the VM test: the identity's VM-ness. 0 natively already |
| the literal `VirtualMac2,1` | ProductKit, ProductKitCore (model → product tables), MobileDevice (device map: VirtualMac2,1-2,3, Mac14,2, MacFamily20,1), CoreTypes Info.plist (`com.apple.virtual-machine` icon for model codes `VirtualMac`, `VMWare`), warmd (`VirtualMac`) | names, pictures and icons; nothing else found |
| ProductKit's model table | knows Mac14,2, MacBookAir10,1, Mac15,3-15,14, Mac16,1-16,13, Mac17,2-17,9, Mac17,14-17,16, Mac18,5, iPad8,6, VirtualMac2,1 | **Mac17,10-17,13 (so Mac17,11, K114c) are unknown to 27.0.1**; an unknown model takes the fallback (System Information shows `product-name`) |
| `/product` values | libMobileGestalt: `target-sub-type`, `unique-model`, `sub-product-type`, `compatible-app-variant`, `compatible-device-fallback`, `product-description`, `thin-bezel`, `artwork-device-subtype`, `graphics-featureset-class`, `oled-display`, `board-id`; AppleDeviceQuerySupport: `fdr-product-type`, `hw.product` | MobileGestalt's answers (device class, idiom, App Store variant `MacFamily20,1`, the iPad an iOS app sees: the kernel answers `hw.machine` `iPad8,6` to iOS processes, `compatible-device-fallback` agrees) |
| `hw.target` (root compatible[0]) | libramrod, Bootability, OSPersonalization, MSUDataAccessor, SoftwareUpdateCoreSupport, SUDocAssetsUI, AssistantServices, CoreUtils, dyld | restore/update/personalisation paths mostly |
| `hw.targettype` (target-type) | BrightnessControl, perfdata, ProactiveSupport, coreroutine, DistributedEvaluation, libramrod | BrightnessControl per target (inferred: tables for Apple panels; none for VMA2MACOS today) |
| `hw.product` / `hw.model` | SoftwareUpdateCoreSupport, libauthinstall, libamsupport, BiomeFoundation, AppleDeviceQuerySupport, Bootability; System Information, ProductKit (IORegistry `model`) | Software Update's catalog query and TSS (2.5) |
| per-target / per-model tables | AboutSettings, MonitorPanel, Portrait (J413 ...); BiometricKitUI, MicroLocationDaemon (Mac15,4, Mac16,2, ...) | a real model's entry brings its UI assumptions (About picture, built-in display options, Touch ID UI) |
| `board-id` (`/chosen`, absent here) | LoginUIKit ("No board-id entry was available"), CoreDisplay/IOKit (`board-ids`), libpmenergy, PowerlogCore, GPUInfo, MobileAssetDaemon, SoftwareUpdateCoreSupport, OSPersonalization, libAudioDSPCore, VoiceProcessor, libImage4 | unchanged by every candidate (an absent board-id is today's state) |
| system-volume files by model | CoreTypes icons, `/System/Library/Displays/.../Overrides/Models` pictures, jetsam (`com.apple.jetsamproperties.Mac.plist`: one file for every Mac, no per-target keys), IOPlatformPluginFamily (Intel only) | pictures; jetsam unaffected |
| Data volume, written at runtime | configd `preferences.plist` and InterfaceNamer `NetworkInterfaces.plist` carry `Model` = hw.model; a file made on another model is moved aside (`preferences-<model>.plist`, `NetworkInterfaces.plist-<model>`) and a new one started; ComputerName / LocalHostName / SMB `NetBIOSName` default from the model and product name | **verified on the installed store**: the seed's files (made on iPad7,1) were moved aside at its first VirtualMac2,1 boot (2026-11-11 20:31). A new identity does the same once: network services new, and the interface database empty, so en0/en1 wait for InterfaceNamer's IOKit-quiet timeout (240 s) unless a model-less database is staged (desk-dev does: `tooling/k27/desk-dev-NetworkInterfaces.plist`) |
| the Data seed | no MobileGestalt cache, no eligibility cache, no `Model` other than the seed era's iPad7,1 | RAM images with any identity start clean; the installed store may hold a MobileGestalt cache made under VirtualMac2,1 (root-only, not read: inferred risk, delete it at the first boot of a new identity) |

### 2.4 What the tablet shows today (installed system, 27.0.1)

`hw.model` / `hw.product` VirtualMac2,1, `hw.target` xiaomi,piano, `hw.targettype` VMA2MACOS, `kern.hv_vmm_present` 0;
System Information: Model Identifier VirtualMac2,1, Chip Unknown (before the display-names change: Model Name "Apple Virtual
Machine 1"); host name Apple-Virtual-Machine-1.local; `pmset -g batt` only AC Power (no InternalBattery on that boot's profile),
`pmset -g cap` without `lowpowermode`.

### 2.5 This repository

- `configs/k27-adt.toml` (the values), `board.toml` (comments), `tooling/adt/make-piano-adt.py` (merges them);
  `logs/adt/piano-k27-adt.txt` (a dump).
- `tooling/k27/desk-dev-NetworkInterfaces.plist` has no `Model` on purpose (InterfaceNamer keeps a model-less database).
- Comments only: `package/PianoPower/PianoBattery.cpp`, `package/NativeGopDisplay/NativeGopDisplay.cpp`, the UPSTREAM files of
  ARMPlatformStandIn / PianoUsbHostStandIn, `tooling/kc/build-kc-k27.sh` ("vma2" = the KC base, not the ADT identity).
- `docs/policy.md` (Low Power Mode "not offered on VirtualMac2,1": inferred, unverified), `el1-native-plan.md` (plan R8).
- No code reads the model. No J-number or Mac model in code.

## 3. Constraints on any identity (the kernel-relevant part)

- Keep chip-id 0x8011, `/chosen` `certificate-*` / `effective-*` / `amfi-only-platform-code`, `/defaults vmm-present`, the root
  `serial-number` (the Data seed was made with it), `compatible-app-variant` MacFamily20,1.
- `target-type`: present (IOgPTPPlugin), at most 15 characters (`gTargetTypeBuffer[16]`), never containing `sim`
  (`PE_device_is_simulated`), not `J236c`. `unique-model` at most 15, `sub-product-type` at most 31 characters.
- Root `compatible`: the identity's entry first (hw.target), never `AppleARM` or `AppleVirtualPlatformARM` (Apple's platform
  experts would match), the DTS's entries after it.
- No `iPad` anywhere in the model string: the VM era's `iPad7,1` identity made apsd abort (vma2 fixed it); a name such as
  `XiaomiPad8Pro,1` contains `iPad`.

## 4. Candidates

| | vma2 (today) | A `piano` | B a real portable (e.g. Mac14,2 / J413) | C the 27.3 line (Mac17,11 / K114c) | C-light `k114c` |
|---|---|---|---|---|---|
| model / target-type / sub-type / compatible[0] | VirtualMac2,1 / VMA2MACOS / VMA2MACOSAP / xiaomi,piano | Piano1,1 / Piano / PianoAP / PianoAP | Mac14,2 / J413 / J413AP / J413AP | Mac17,11 / K114c / K114cAP / K114cAP | as C, on 27.0.1 |
| kernel side (section 3) | - | unchanged | unchanged | new KC base, new kernel line | unchanged |
| KC | vma2 base | same | same | mac17j base, the profile's roots | same as today |
| userland | 27.0.1 | 27.0.1 | 27.0.1 | 27.3 (26C5035r) | 27.0.1 |
| System Information | the product-name; Model Identifier VirtualMac2,1 | product-name; Piano1,1 | "MacBook Air (M2, 2022)" and its picture from ProductKit (a false product) | 27.3's table: MacBook Pro (14-inch ...) if it knows Mac17,11 (inferred), not the product-name | product-name; Mac17,11 (unknown to 27.0.1) |
| Software Update | VM assets | none for an unknown model (inferred) | real Mac14,2 updates offered; preparing fails at personalisation (no SEP, no nonce), after a multi-GB download (inferred) | real Mac17,11 updates | real Mac17,11 updates (27.x) offered (inferred) |
| risks | the name | apsd and other daemons on an unknown model (the iPad7,1 lesson; must be seen on a boot); generic icons | per-model UI (About, MonitorPanel's built-in display, Touch ID UI tables), IOgPTPPlugin looks for "PMU tdev5" (absent), Apple ID/iCloud sees a real Mac model without SEP attestation | all of B's for Mac17,11 on 27.3, plus the cost below | B's Software Update and Apple ID risks; ProductKit fallback |
| cost | none | ADT only (this switch) | ADT only | a new line (below) | ADT only (this switch) |

**C's cost** (UniXNU `profiles/os/macos-27.3-26C5035r`, run by UniXNU-sheng on SM8550): the Mac17,11 kernelcache as the KC base
with its roots list (AppleARMPlatform, ApplePMGR, AppleARMWatchdogTimer, IOGPUFamily, IOMobileGraphicsFamily, the USB device and
host stacks, IO80211Family, IOBluetoothFamily, AppleTopCase, EndpointSecurity, ...: the 27.0.1 native collection keeps 16 roots (kc-keep.txt)
+ DiskImages), xnu-12377 rebuilt as `target = "macosx"` with the macOS series (Darwin 26 KPIs, shared-region size, ...); every
native kext rebuilt and its Apple ABIs re-derived against the 27.3 collection (IOMobileFramebuffer for NativeGopDisplay, IOGPU /
IOAccelerator for PianoA830, IOHID, IONetworking/Skywalk, IOAudio2); the Metal plugin's frontend against 27.3's Metal; the OS,
SystemOS and App cryptex images of 26C5035r on linux_rootfs; a new Data seed (the 27.3 template with the accounts), so the
installed store (seeded from the 27.0.1 image) is not resumed: a new store, the owner's files moved over by hand. The
native-kext rule and `check-native-kexts.py` hold unchanged. The identity part of C is the small part; it can be tried first on
27.0.1 as C-light.

**C-light** (the 27.0.1 vma2 KC base with the Mac17,11/K114c ADT identity) is plausible: nothing in the native collection matches
on the identity (2.2), so kext matching does not change; what changes is userland, as for B, except that 27.0.1 does not know
Mac17,11 (2.3), so its names fall back to product-name.

## 5. The switch

`configs/k27-identity/NAME.toml` holds an identity's model values (`["root"."string"]`, `["product"."string"]`); vma2 is
`configs/k27-adt.toml` itself. `tooling/adt/set-identity.py IN OUT --identity NAME` applies one to a built ADT (everything else byte
for byte; the round trip of the input is checked) and marks the root with `unixnu-identity = NAME`; `--show` prints an ADT's
identity.

- `PIANO_IDENTITY=piano|k114c tooling/k27/build-native-image.sh`: out/ram and out/ram-dump take `out/adt/piano-k27-id-NAME.adt`;
  `out/adt/piano-k27.adt` (the ESP's) keeps vma2.
- `DESK_IDENTITY=piano|k114c tooling/k27/build-desktop-images.sh NAME...`: the images go to `out/ram-NAME-IDENTITY/` (the dump
  image keeps the input ADT).
- `tooling/persist/build-esp.py` refuses an ADT with the mark unless `PIANO_IDENTITY_INSTALL=1`.

Default: vma2, nothing changes.

## 6. What a boot with another identity must show

Images (2026-11-12 23:05, built with this switch from one snapshot of the main checkout's KC, ADT and Turnip seed as
`DESK_RTC_OFFSET=1794397906 DESK_IDENTITY=<id> tooling/k27/build-desktop-images.sh desk-dev`; desk-dev's profile and ssh key, Data
volatile (`data-persist=0`), `piano-reset-after=3600`, command line 954/959 bytes, `verify-ram-images.py` PASS; out/ is not
committed):

| Image (out/) | Identity | boot image sha256 |
|---|---|---|
| `ram-desk-dev-id-vma2` | vma2, the control (same inputs) | `0b3aeff179860b3e...` |
| `ram-desk-dev-id-piano` | A | `f5a3e6d1bef484c4...` |
| `ram-desk-dev-id-k114c` | C-light | `1b31e069e43606e6...` |
| `ram-desk-dev-id-dump` | the loop's dump image | `ac55eaf9165ffb5a...` |

On the boot, from the Mac (`tooling/native/desk-ssh.sh`), after the owner's login (`desk-login.sh`):

| # | Check | Command / where | Pass |
|---|---|---|---|
| 1 | boots, login window, desktop after login | console; webcam | as vma2 desk-dev |
| 2 | the identity | `sysctl hw.model hw.target hw.targettype hw.product kern.hv_vmm_present`; `ioreg -c IOPlatformExpertDevice -d 1 -r \| grep -E 'model\|target\|compatible\|unixnu-identity'` | the overlay's values, vmm 0 |
| 3 | names | `system_profiler SPHardwareDataType`; `scutil --get ComputerName; scutil --get LocalHostName` | Model Name = product-name, Model Identifier = the model, Chip = product-soc-name |
| 4 | WindowServer, loginwindow, Dock, Finder stable 10 min | `pgrep -l -x WindowServer loginwindow Dock Finder` at login and 10 min later | same PIDs |
| 5 | no new crash loops | `ls -t /Library/Logs/DiagnosticReports \| head -30` (and `~/Library/Logs/DiagnosticReports`) after 10 min; `launchctl list \| awk '$2 != 0 && $2 != "-"'` | nothing vma2 desk-dev lacks; in particular no apsd, akd, identityservicesd, sharingd, softwareupdated, MobileGestalt-helper report |
| 6 | apsd | `launchctl print system/com.apple.apsd \| grep -E 'state\|pid\|last exit'` | running (or as on vma2) |
| 7 | configd's databases | `defaults read /Library/Preferences/SystemConfiguration/preferences Model`; `ls /Library/Preferences/SystemConfiguration`; the console's `desk-dev: usb net` line time | the new model; en1 up as fast as on vma2 (the staged model-less database) |
| 8 | battery | `pmset -g batt`, `pmset -g cap`, `ioreg -rc AppleSmartBattery \| grep -E 'CurrentCapacity\|IsCharging\|ExternalConnected'`; the menu bar (webcam) | as vma2; note whether `lowpowermode` appears (docs/policy.md's open question) |
| 9 | brightness, keyboard | the brightness keys (PianoBacklight's log lines), typing in Terminal, no Keyboard Setup Assistant | as vma2 |
| 10 | Software Update (only with the Mac's Internet Sharing up) | `softwareupdate --list 2>&1 \| head` | piano: nothing; k114c: note what is offered, install nothing |

## 7. Recommendation

1. Keep vma2 on the installed system until an identity's RAM boot passes section 6.
2. On the 27.0.1 line, boot A (`piano`) and C-light (`k114c`) once each (no other change in the images). A is the honest one
   (nothing claims to be an Apple product, Software Update finds nothing); its open risk is a daemon that refuses an unknown model
   (check 5/6). C-light previews the 27.3 line's identity on the hardware and finds its userland effects early, but is a real
   Apple model on 27.0.1 (Software Update offers real updates: keep it off).
3. For the 27.3 line (C): the identity costs nothing kernel-side either; what remains to decide there is whether Mac17,11's
   Apple names should show (27.3 likely knows Mac17,11, so System Information would say MacBook Pro rather than the product-name:
   check 27.3's ProductKit), or whether that line too takes an own identity (A's values work unchanged: nothing in the mac17j
   collection's matching depends on the model either).
4. Moving the installed system to a new identity, when it is chosen: on its first boot configd starts new network
   preferences and interface database (2.3: stage a model-less NetworkInterfaces.plist as desk-dev does, or accept one 240 s
   wait), delete the store's MobileGestalt cache once, and turn automatic Software Update checks off for an Apple model.

## 8. The 240 s network wait under k114c (2026-11-13) and its cause, AppleImage4

**Seen (the coordinator, installed ESP v3.3d with k114c, 01:33-01:45):** configd's InterfaceNamer logs `waiting for quiet`, then
4 minutes later `timed out waiting for IOKit to quiesce`, and only then names the interfaces (the USB network among them).
`ioreg`: `/defaults` (IOPlatformDevice) `busy 1` for the whole uptime, its parents ArmDTPlatform and K114cAP too; its child
AppleImage4 `!registered, !matched, active, busy 0`.

**Not the identity (verified, logs).** The registry was never quiet on vma2 either: every vma2 desk-dev boot of 2026-11-12
(19:40-20:27, `logs/usb-console/20261112-*`) has `desk-dev: iokit busy (configd 260+ s): defaults (IOPlatformDevice) busy 1`, and
the k114c RAM desk-dev boots of 01:02 and 01:24 the same. What hid it: InterfaceNamer names an interface that is in its database at
once and waits for the quiet only for a new one. desk-dev stages a model-less database (`named after 0s` on both identities); the
installed store had one with `Model` VirtualMac2,1. Under Mac17,11 that database is moved aside (section 2.3), en0/en1 are new, and
they wait for a quiet that never comes: the 240 s timeout.

**The cause (verified: the call sequence in 27.0.1's AppleImage4, strings and the resolved stubs; no code copied).**
`AppleImage4::start` finalizes its expert (`kmod_expert_finalize`, once), which calls `IOService::serviceMatching("IOAESAccelerator")`
and `IOService::waitForMatchingService(that, UINT64_MAX)`: it waits without a timeout for a registered IOAESAccelerator, then
finalizes the supervisor and registers. Natively there has been none since 2026-11-11, when NativeAES (UniXNU's SoftwareAES, the VM
era's AES accelerator) was dropped as unused. So AppleImage4's start never returns, its provider `/defaults` stays busy, and IOKit
never goes quiet, on every identity. The same sequence is in 27.3's AppleImage4 (`kernelcache.release.mac17j`), so the 27.3 line
(`configs/k273-adt.toml`, the same native kext list) has it too. No ADT property changes it (the wait is unconditional), and the
root `compatible` plays no part.

**The fix:** `package/SoftwareAES` back in `configs/k27-native-kexts.txt` (UniXNU's SoftwareAES 4a45326, UPSTREAM): NativeAES, an
IOAESAccelerator doing AES-CBC in software, matched on IOResources/IOBSD, registered after its known-answer tests. It also serves
the device key, selector 2212, with a fixed software key: once finalized, AppleImage4 entangles the nonce slots it resets with that
key and panics if the accelerator refuses it (the VM era's finding; the NativeAES-era native boot of 2026-11-11 01:33 got through
this step: `Image4: resetting: pdmg ...`, no panic). Its instance size (0x120) is the one both collections' IOCryptoAcceleratorFamily
gives IOAESAccelerator (their metaclass constructors).

**The boot's check (desk-dev, any identity):**

| Question | Command / line | Pass |
|---|---|---|
| the accelerator is there | console `SoftwareAES: NativeAES registered as the IOAESAccelerator ...`; `ioreg -r -c NativeAES -d 1` | the line; one NativeAES, registered |
| AppleImage4 finished | `ioreg -w0 -r -n defaults -d 2 \| grep -E 'defaults\|AppleImage4'` | `defaults` `busy 0`; AppleImage4 `registered, matched` |
| nonce entanglement | console `SoftwareAES: device key 2212 asked for ...`; no `panic: encryption failed` | (the line may be absent: only when AppleImage4 resets slots) |
| the registry goes quiet | console `desk-dev: iokit busy (configd 260+ s): none` | `none` |
| the interfaces without the staged database (`DESK_DEV_NETDB=0` image) | console `desk-dev: usb net: en1 is ours (named after Ns)`; on the tablet `ifconfig -l` | N well under 240 (seconds after configd starts), en0 and en1 listed |

Images (2026-11-13 02:10, this branch on main 9db4334: the KC rebuilt with SoftwareAES, kernel 0065, `PIANO_GPU=2
PIANO_VMM_PRESENT=0`, `NativeAES vs IOAESAccelerator: 0 problem slots`, 26 native kexts; main's Turnip seed; desk-dev,
`DESK_RTC_OFFSET=1794397906`, Data volatile, command line 959 bytes, `verify-ram-images.py` PASS; out/ is not committed):

| Image (out/) | Identity | Interface database | boot image sha256 |
|---|---|---|---|
| `ram-desk-dev-aes-k114c` | k114c | none (`DESK_DEV_NETDB=0`): the quiet test | `cf369a40ef1a2a61...` |
| `ram-desk-dev-aes-k114c-netdb` | k114c | staged, as desk-dev before | `0e61978a0bb61cc2...` |
| `ram-desk-dev-aes-vma2` | vma2 | staged | `f2eb4c849585fcfc...` |
| `ram-desk-dev-aes-dump` | the loop's dump image | - | `b37c9c421a2a34d1...` |
