English | [简体中文](README.zh-CN.md)

# UniXNU-piano

Native macOS 27 on the Xiaomi Pad 8 Pro (`piano`, Snapdragon 8 Elite SM8750), as a [UniXNU](https://github.com/UniXNU/UniXNU)
device repository: https://github.com/lenzhang/UniXNU-xiaomipad8pro (formerly lenzhang/UniXNU-piano; the local
checkout keeps the name UniXNU-piano). The 27.3 line's k273 XNU build (the k27 source tree with `patches/k27` 0053-0076 and
`patches/k273`) runs as the HLOS at EL1, with
Gunyah staying at EL2. Boot chain: ABL → BootShim → Mu-Silicium UEFI (the FD, piano-uefi's `pianoPkg` with this repository's
deltas) → the UniXNU payload and menu → DarwinBoot → XNU. UniXNU core, Mu-Silicium, piano-uefi, DarwinBoot and the SoC module
are pinned dependencies (`unixnu.lock`, `feeds.toml`), not forks.

Related public repositories: [UniXNU-sm8750-common](https://github.com/lenzhang/UniXNU-sm8750-common) is a placeholder for the
SM8750 pieces to be shared later (the code still lives here, and the build pins another SM8750 module); [veil](https://github.com/lenzhang/veil)
is the frozen archive of the earlier Linux + Gunyah guest path. The map of repositories, local trees and rules:
[docs/PROJECT-MAP.md](docs/PROJECT-MAP.md).

## State (2026-11-27)

Native macOS **27.3 (26C5035r)** boots on the tablet from power-on as well as from a RAM image. The current daily
install is **v13.1y12u** (2026-11-27, ESP-only over `v13.1y12q`), with **120 Hz** daily default display refresh (144 Hz
opt-in), **DPU M2** WC cache-clean skip (`piano-dpu-clean-skip=1`), **Night Shift** DSPP PCC routing ON, **Disk M5
write-back** (`data-store-write-back=1`) plus M4 Data concurrent mode, and USB debug network TX flow control on by
default. Native panel sleep (`piano-dpu-panel=0`) and dynamic refresh switching stay off. The machine identifies as
Mac17,11, shown as "Xiaomi Pad 8 Pro" with a
"Snapdragon 8 Elite" chip. Root is on `linux_rootfs`; the owner's Data store is on `linux_data`. High Power is the daily
Energy Mode. The Android-aligned CPU/GPU frequency curve and PD buck charging (9 V) are on by default; the charge pump
stays off.

What works on the installed system today: boot from disk with the owner's data, the display at **120 Hz** with Night Shift
and DPU M2 clean-skip, the native touch
screen, the keyboard cover and its trackpad, Wi-Fi (join at boot, off/on, rejoin after a link loss), speakers and
microphone (including active full-duplex), battery
percentage, 5 V and 9 V PD charging with the 80 % hold, automatic brightness, Metal on the Adreno 830 with paged GPU
memory and 4 GiB GPU windows (Core Image on 4032-pixel photos), Photo Booth and camera recording, hardware video decode in
QuickTime and Safari (H.264 up to 1080p), Safari WebGL 2 and WebGPU, and Chrome with GPU acceleration (WebGL, GPU compositing,
smooth scrolling and video, with its GPU sandbox on) and, since v13.1y12m, Chrome WebGPU without flags (kernel patch 0075, a
47-bit user address space). Since v13.1y12m the daily line also includes the cover and power-key sleep policy, native screenshots
from the display driver and the Bluetooth LE event-mask fix. In RAM qualification, a simulated cover close on the charger locked
the screen without sleeping, and a simulated power-key press slept and woke the system. The physical keys and closing the cover
on battery still need attended checks. The installed system uses a 13 GiB memory window. Bluetooth keyboard and mouse pairing,
charge-pump fast charging, USB host devices and external displays remain open. Work continues on auto-rotate and validating
the install guide on other units.

On November 27, combined volatile-Data RAM qualification **`v131y12u-daily1`** passed all four new daily features together
(168 PASS, 0 code failures, plus `video-v6 --qualify` 13/13), followed by a fresh SHA-256 read-back verified owner Data
store backup and ESP-only install **`v13.1y12u`** (`inst-v131y12u-20261127-094439`, 75 PASS / 0 FAIL, post-install
flags-free Chrome WebGPU 6/6 + Safari WebGPU 52.15 fps, and clean reboot survival). Earlier on November 27, installed
`v13.1y12q` passed 10-minute sustained Metal 3D (59.98 fps at 967–1050 MHz), Bluetooth preflight (32/32), and unattended
zero-TCC microphone + active duplex (`inst-y12q-obs6`), while RAM tests passed Iris H.264 hardware encode at 480p and
1080p (`v131y12u-e1fix1`, `v131y12u-e1-1080p`) and isolated the M5 dark-wake `CTRL=0x1f7` bug as secondary (`v131y12u-m5ab1`,
M5 stays **HOLD**). Host-side 27.3 `powerd` + kernel 0076 analysis and live `ioreg` inspection closed the BH-2 root cause
(`ArmDTPlatform::excludeList()` shadowing `IODTNVRAM` with an `IOPlatformDevice` nub at `IODeviceTree:/options`).
[November 27 results](docs/results-2026-11-27.md) · [BH-2 analysis](docs/drivers/power-bh2-powerd-analysis.md)

Kernel patch 0076 lets the USB debug shell and other updated helpers read the full boot line, including arguments past
the legacy 1024-byte cut.
CI now runs the Adreno830 GPU host suite with pinned public kits. [Afternoon results](docs/results-2026-11-26.md#afternoon-storage-power-cut-tests-120-hz-ci)

### Hardware support

Every status comes from a run on the tablet. "Works (installed)" means verified on the system installed on the
tablet's disk (the build named in the row); "RAM only" means verified on a RAM boot image only.

✅ fully supported · 🟡 partial / in progress · ❌ not supported

| Feature | Status | Verified (build, date) | Note |
| --- | --- | --- | --- |
| Boot from disk, persistent install | ✅ Works (installed) | v13.1y12u, 2026-11-27 | Installed **v13.1y12u** (`inst-v131y12u-20261127-094439`, 75 PASS / 0 FAIL) wrote only changed ESP files after a fresh SHA-256 read-back verified owner Data backup, verified read-back, kept the `v13.1y12q` userland layer untouched, and survived a clean disk reboot with owner Data resumed. Earlier `v13.1y12q` disk boots confirmed +32 s login and `cs-native4k-skip=1` (896 files, 7496 MiB skipped). [Installed v13.1y12u](docs/results-2026-11-27.md#installed-v131y12u-esp-only-update-and-reboot-check) |
| Display | ✅ Works (installed), 120 Hz | installed v13.1y12u, 2026-11-27 · RAM v131y12u-daily1 and m5ab1, 2026-11-27 | Daily **v13.1y12u** runs fixed **120 Hz** dual-link scanout (`120.016 Hz`), **DPU M2** WC clean-skip (`piano-dpu-clean-skip=1`, 6,177 flips with 3 init cleans, 0 underruns, max latch `8.375 ms`), **Night Shift** DSPP0/1 PCC routing ON (`native-gop-display-night-shift=1`), and native screenshots. Combined RAM `v131y12u-daily1` verified unlocked Night Shift `off→warm→off` (0 apply failures, `1.000/0.999/0.999` restore) and 100% M2 skip under 120 Hz with live C920 optical checks (`transient_frames=0`). Native panel sleep stays off (`piano-dpu-panel=0`, M5 **KEEP HOLD** after `v131y12u-m5ab1` cleared `CMD_MODE_EN` contention yet stayed optically dark at `YAVG 21.5`). [Installed v13.1y12u](docs/results-2026-11-27.md#installed-v131y12u-esp-only-update-and-reboot-check) · [Combined RAM qual](docs/results-2026-11-27.md#v131y12u-daily1-2026-11-27) · [M5 wake results](docs/results-2026-11-27.md#v131y12u-m5ab1-2026-11-27) |
| 120 Hz / 144 Hz | 🟡 Works (installed) at 120 Hz; 144 Hz opt-in | installed v13.1y12u, 2026-11-27 · RAM m120c-144, 2026-11-26 | **120 Hz** is the daily default on installed **v13.1y12u** (`piano-dpu-modes=120`, `120.016 Hz` verified on both links, `IOMFBDisplayRefresh` all 6 fields exact, 0 underruns, survives reboot, `OWNER-0748`). **144 Hz** remains opt-in (`144.146 Hz` RAM-verified on `m120c-144`, 0 underruns). High-refresh app presentation cadence (`pipe120` fallback 511/511, 50.71 callbacks/s, zero `presentedTime` on `v131y12t-m120d`) remains **HOLD** for follow-up analysis. [Installed v13.1y12u](docs/results-2026-11-27.md#installed-v131y12u-esp-only-update-and-reboot-check) · [120 Hz RAM probe](docs/results-2026-11-27.md#120-hz-pipe--presented-time-probe-ram-v131y12t-m120d) |
| Touch screen (native) | ✅ Works (installed) | v13.1y12l, 2026-11-26; gestures on RAM touch-native3, 2026-11-23 | A native built-in Touch Screen bound to the display: tap, long press (right click), pan and scroll in scroll views, pinch, rotate, smart magnify, palm rejection, 90 degree axes (44/0 on RAM). Installed: published from boot and acquired by the system. Long use by hand on the native path is pending. |
| Keyboard cover and trackpad | ✅ Works (installed) | owner hands-on, v12, 2026-11-19; v13.1y12l, 2026-11-26 | Typing and the trackpad work; the cover is known to the login window. |
| Wi-Fi | 🟡 Works (installed), partial | v13.1y12l, 2026-11-26; v13.1x, 2026-11-21 | Joins at boot (WPA2, DHCP, DNS); radio off/on from the switch rejoins; a forced link loss rejoins within seconds; country code set. Pending: joining by clicking a network row by hand, auto-rejoin after a hotspot goes away and comes back, and a multi-access-point office network. |
| Bluetooth | ❌ Not yet (in progress) | v13.1y12q, 2026-11-27; owner session on installed v13.1, 2026-11-20 | Installed preflight reported BT_PREFLIGHT READY (32/32 after 48 s), with the controller published and powered on and LE event mask bits 6 and 11 set. Discovery worked in the earlier owner session, but the LE connection timed out (HCI 0x08). Keyboard and mouse pairing awaits the owner's 20-minute hands-on session. Wi-Fi coexistence is under analysis. Bluetooth audio is open. |
| Speakers | ✅ Works (installed) | v13.1u, 2026-11-21 (audible tone); v13.1y12q, 2026-11-27 (`inst-y12q-obs6` active duplex) · RAM v131y12u-ram2, 2026-11-27 | Playback, volume and mute work. Speaker protection at gain 243 and the stock Dolby "Movie" processing run on the DSP. Installs stay silent unless explicitly exercised. On installed `v13.1y12q` (`inst-y12q-obs6`), concurrent `afplay` playback during mic capture completed cleanly (`underruns=0`, `failed=0`, `fallbacks=0`, volume/mute restored). The opt-in silent `spk-probe1` passed two consecutive RAM runs (4/4 amps `ok` at 25.0 °C, powered down). [Installed duplex results](docs/results-2026-11-27.md#installed-microphone-and-active-duplex-inst-y12q-obs6) · [Silent probe results](docs/results-2026-11-27.md#second-silent-speaker-and-emission-free-ir-passes) |
| Microphone | ✅ Works (installed) | installed v13.1y12q (`inst-y12q-obs6`), 2026-11-27 · RAM y12t-obs1, 2026-11-27 · noise reduction on RAM mic-ns4b, 2026-11-23 | Installed audio exposes raw `"Mic"` (default input) and `"Mic (noise reduced)"` (RNNoise). On installed `v13.1y12q` (`inst-y12q-obs6`, zero TCC bypass), direct capture passed quiet floor (`-44.45 dBFS`), exact-zero hardware mute (`0` nonzero samples over `3.13 s`), and active concurrent Speaker+Mic full-duplex (`994.93 Hz`, `28.16 dB SNR`, `-4.26 dBFS`, `0` clipped samples, `63 dB` pre/post contrast, clean cold `mic release`, with a one-time `37 ms` FIFO gap at cold speaker open under `duplex-hold=0`). RAM `y12t-obs1` also passed calibrated −36 dBFS AVFoundation recording and C920 playback. Hands-on QuickTime / Voice Memos GUI prompts remain optional owner checks. [Installed mic & duplex](docs/results-2026-11-27.md#installed-microphone-and-active-duplex-inst-y12q-obs6) · [Mic RAM results](docs/results-2026-11-27.md#calibrated-microphone-and-c920-playback) |
| Cameras: Photo Booth | ✅ Works (installed) | v13.1y12l, 2026-11-26 | Live front preview with no prompt; the effects grid opens. |
| Cameras: QuickTime | 🟡 Partial | v13.1y12l, 2026-11-26 | 10 s H.264 movies from the front and rear cameras record (AVFoundation, the QuickTime High preset). A New Movie Recording saved and played back by hand is pending. Encoding is in software. |
| Cameras: FaceTime | 🟡 Not yet verified | permissions pre-granted, v13.1y12l, 2026-11-26 | Both cameras are listed and FaceTime has Camera and Microphone access; a FaceTime self-view has not been checked. |
| Charging: 5 V | ✅ Works (installed) | v13.1y12u, 2026-11-27 · v13.1y12d, 2026-11-24 | Up to 3 A from a 3 A source (`ICLReadback=3000 mA`, `AppliedFCC=1500 mA`). On the Mac mini 5 V USB-C port, hardware AICL holds `1650 mA` (`IBUS=1644 mA @ 4.72 V`, `7.77 W` in), delivering **`+2.0–2.7 W`** (`+514 to +701 mA`) net positive battery charge awake and with display off + Low Power Mode. [Charging measurements](docs/results-2026-11-27.md#live-5-v-usb-c-charging-and-low-power-check) |
| Charging: PD 9 V (buck) | ✅ Works (installed) | v13.1y12d, 2026-11-24 | On by default. Fixed 9 V PD on the stock charger: 9 V / 1.6 A (about 14 W, the current the charger firmware requests, as on Ubuntu). PPS 9 V (up to 2.5 A) is built but not run. |
| Charging: 80 % hold | ✅ Works | RAM chg-buck6, 2026-11-17; in every installed profile since | Stops at 80 %, resumes at 75 %. |
| Charging: charge pump / MiPPS | ❌ Not yet | first attended try, 2026-11-24 | Off by default. The first attempt stayed at a 100 mA input limit (an unmapped vendor power object); fixed in the build, the next attended test is pending. |
| Battery percentage | ✅ Works (installed) | installed v13.1y12u & RAM v131y12u-daily1, 2026-11-27 | The installed system shows battery percentage and power state. On `m120c-120`, the battery registry reported 122 cycles, 9200 mAh design capacity and 8781 mAh full capacity, while System Information showed “Service Recommended” and omitted Maximum Capacity. Host-side 27.3 `powerd` + kernel 0076 disassembly and live `ioreg` inspection (`OWNER-0748`) closed the root cause: `ArmDTPlatform::excludeList()` returns `"()"` so `IODTPlatformExpert::processTopLevel()` shadows real `IODTNVRAM` with an `IOPlatformDevice` nub at `IODeviceTree:/options` (`kIOReturnUnsupported` → `0xe00002ca`); BH-2 stays opt-in/default-off pending a dedicated kernel build + RAM qual. [BH-2 analysis](docs/drivers/power-bh2-powerd-analysis.md) |
| Sleep and wake | 🟡 Partial | v13.1y12m, 2026-11-26 (installed policy and RAM checks); RAM v131y12t-qual1, 2026-11-27 | The v13.1y12m checks recorded Wi-Fi and USB recovery and return to the lock screen after wake. The installed build arms the cover and power-key policy. The latest RAM qualification passed suspend-to-idle with timer wake and simulated power-key and lid policies. Earlier RAM checks covered charging with the lid closed (locks without suspending) and simulated power-key sleep and wake. The physical power key and closing the cover on battery still need attended checks. |
| Brightness: manual | 🟡 Partial | keys on installed v12, 2026-11-19; slider path on v13.1i | Brightness keys work. The slider path through the system's brightness control works; a hand drag in Control Center is pending. |
| Brightness: automatic | ✅ Works (installed) | v13.1y12l, 2026-11-26 | The light sensor drives automatic brightness; manual moves do not end automatic mode. A real light-change span is pending. |
| GPU: Metal (Adreno 830) | ✅ Works (installed) | v13.1y12l, 2026-11-26 | Our Metal plug-in runs WindowServer, the desktop and apps; ImageIO, Core Image on Metal, MetalKit texture loading and Quick Look thumbnails pass: 30 of 30 graded paths, including the 4032-pixel Core Image and HEIC texture paths that ran out of memory before; Preview opens a 4032-pixel photo. |
| GPU: paged memory | ✅ Works (installed) | v13.1y12l, 2026-11-26 | 4 KiB paged GPU memory (cap 7168 MiB, no fixed pool), no refusals, no page faults; hang recovery by replay on by default. |
| GPU: Android-level frequency curve | ✅ Works (installed) | v13.1y12q, 2026-11-27 · v13.1y12l, 2026-11-26 | Android's 14-level GPU table up to 1100 MHz, with skin and thermal limits and a KGSL-style governor, plus the Android CPU table. The installed 10-minute Metal 3D run averaged 59.98 fps at 967–1050 MHz with 0 GPU faults. FPS measures Metal completions (`presented=0`). [Sustained Metal 3D results](docs/results-2026-11-27.md#sustained-metal-3d-installed-v131y12q) |
| GPU: 4 GiB windows | ✅ Works (installed) | v13.1y12l, 2026-11-26 | 64 client windows of 4 GiB in a 256 GiB aperture, 1 GiB per object, no GPU open refused. Fixes the Core Image 4032-pixel out-of-memory paths (Metal image check 30/30). |
| Safari: WebGL 2 and WebGPU | ✅ Works (installed) | v13.1y12o, 2026-11-26 | WebGL 2 and WebGPU passed again. On v13.1y12m, the WebGPU adapter was "adreno" and the test page ran at about 52 fps with correct readback. |
| Safari: hardware video decode | ✅ Works (installed) | v13.1y12o, 2026-11-26; v13.1y12i, 2026-11-25 | H.264 up to 1920x1088 decodes on the Iris video core. The 1080p test again reported zero dropped frames. |
| Safari: HEVC and 4K | ✅ Works (installed), software | v13.1y12j, 2026-11-25; HEVC replayed on v13.1y12o | HEVC 2160p 600/0 at 29.9 fps and H.264 2160p 591/5 on v13.1y12j, in Apple's software decoders. VP9 and AV1 are not supported. |
| Chrome: GPU acceleration | ✅ Works (installed) | v13.1y12q, 2026-11-27; v13.1y12o, 2026-11-26; earlier tests on v13.1y12m and v13.1y12l | Flags-free WebGL 2 measured 37.89 fps at 3200 × 1738 through ANGLE Metal (Adreno 830, `GraphiteDawnMetal`). Chrome played H.264 and HEVC at 1080p and 4K and VP9 and AV1 at 1080p with 0–1 dropped frames. HEVC went through VideoToolbox. The run recorded zero crashes, PianoA830 failures or sandbox denies. Earlier checks with the GPU sandbox on covered GPU compositing, canvas 2D at 56 fps and scrolling at 60 fps with no frame over 34 ms. Chrome can exhaust the RAM-backed Data disk on RAM boots. |
| Chrome: WebGPU | ✅ Works (installed) | v13.1y12u, 2026-11-27; v13.1y12q, 2026-11-27; v13.1y12o, 2026-11-26 | Verified again on installed **v13.1y12u** (`post-y12u-20261127-100037`): without GPU, WebGPU or sandbox flags, 6/6 contexts opened on `qualcomm` / `adreno-8xx` (`fallback_adapter = false`), with correct compute and exact offscreen and canvas `bgra8unorm` pixel readback, WebGL `37.4 fps`, 0 context/ring/transfer errors, 47-bit user VA (kernel patch 0075), and 17/17 Chrome processes in secure V8 sandbox mode (bucket 0, 1152 GB reservation). |
| Video playback (QuickTime) | ✅ Works (installed) | installed v13.1u/v13.1x, 2026-11-21 · installed v13.1y12u & RAM v131y12u-daily1, 2026-11-27 | AVC High and HEVC Main10 decoded in hardware; about 100 s each with 0 transient or dark frames on the webcam. The VideoToolbox pending-cancel fix (`a426ce70`) is now carried on installed **v13.1y12u** after passing `video-v6 --qualify` 13/13 (`V6_TOOL_EXIT=0`, 4/4 terminal `Demt`, `encs=0`) across consecutive RAM runs including `v131y12u-daily1`. [Combined RAM qual](docs/results-2026-11-27.md#v131y12u-daily1-2026-11-27) |
| Energy modes | 🟡 Partial | v13.1y12l, 2026-11-26 | Low Power, Automatic and High Power (the daily setting) set with `pmset` reach the power and thermal policy. The picker in System Settings is not verified by hand. |
| Login window idle CPU | ✅ Works (installed) | v13.1y12l, 2026-11-26 | Fixed: an audio daemon looped at the login window (about 1.3 cores); it is now held until the login. |
| App launches | ✅ Works (installed) | v13.1y12l, 2026-11-26 | Calculator, TextEdit and Notes open their first window in under 2.5 s. |
| USB debug (console, root shell, ssh) | ✅ Works (installed) | installed v13.1y12q and RAM v131y12t-qual1, 2026-11-27; installed v13.1y12q, 2026-11-26 | USB-C debug console, root shell and SSH over the cable work. Patch 0076 lets the shell read boot arguments past the legacy 1024-byte cut. TX flow control is on by default since v13.1y12q and was active on the November 27 disk check. The November 27 RAM matrix passed 66/66 exact transfers with zero driver drops. |
| OS switcher (Android / macOS) | 🟡 RAM only, read-only | RAM v13.1y12e, 2026-11-24 | The boot-flag sysctls read the flag; writes are refused by design. A full switch has not been run. |
| USB host / OTG | ❌ Not yet | RAM usb-a2d, 2026-11-18 (empty port) | Host mode starts on an empty port; VBUS supply and a real device are not done. |
| External display (USB-C DP) | ❌ Not yet | none | Design only. |
| Auto-rotate and other sensors | ❌ Not yet | RAM v131y12t-qual1, 2026-11-27 | The light sensor works. The RAM check passed the accelerometer stream and the `piano-rotated` boot-argument read past 1024 bytes. Auto-rotate remains off in the daily profile and in analysis. |
| Hardware video encode (Iris VPU 3.5) | 🟡 RAM-only E1 self-test PASS (480p & 1080p) | installed `v13.1y12q` · RAM `v131y12u-e1fix1` and `v131y12u-e1-1080p`, 2026-11-27 | Installed recordings use software encoding; daily `v13.1y12q` keeps `piano-video-enc=0` and empty VT encoder inventory (`encs=0`). Opt-in RAM E1 kernel self-tests passed H.264 hardware encode at **640×480@30** (`v131y12u-e1fix1`, 105/0, `22,439 B`, `314 ms`, PSNR-Y 51.6 dB avg) and **1920×1080@30** (`v131y12u-e1-1080p`, 113/0, 14 entries `56,180,736 B`, `88,882 B`, `358 ms`, PSNR-Y 44.3 dB avg), each followed by same-boot 13/13 AVC decode on the resident Iris core. VideoToolbox encoder registration (E2+) remains **HOLD**. [480p E1](docs/results-2026-11-27.md#v131y12u-e1fix1-2026-11-27) · [1080p E1](docs/results-2026-11-27.md#v131y12u-e1-1080p-2026-11-27) |
| IR blaster, stylus, torch | 🟡 RAM-only IR; torch host-verified (attended LED pending) | RAM `v131y12u-ram2` (IR), RAM T2 `flip4`/`hands1` (torch 20/50 mA), host 400 mA configured + ladder 2026-11-27 | SE15 IR passed two consecutive RAM runs (0 survey stores, 64/64 DMA loopback in 381 µs, GPIO29 dark, 0 sends; emission awaits an attended receiver check). For the rear torch, only 20 mA and 50 mA were ever tried on macOS (`flip4`/`hands1`, 12/12 writes read back, LED stayed dark); on 2026-11-27 the owner approved the 400 mA total (2×200 mA) factory limit (`OWNER-0717`) and set **200 mA per channel (400 mA total)** as the configured torch-on current (not only the ladder top step, `OWNER-0801`), with temperature guards, read-backs and default-off boot behavior preserved (host-verified, attended lit test pending). Stylus remains unsupported. [IR RAM results](docs/results-2026-11-27.md#second-silent-speaker-and-emission-free-ir-passes) · [Torch checklist](docs/drivers/torch.md#attended-t4-ram-qualification-checklist) |
| Memory (16 GB) | 🟡 Partial | installed v13.1y12l and RAM m120c-120/ram16g-hevc1, 2026-11-26 · RAM ram16g-hctl1/tier3w/tctl3, 2026-11-27 | The installed system uses 13 GiB (13310 MiB; 47% free idle, no swap/pressure). The EFI audit classified 319/322 two-MiB slabs as non-runtime (released 0 MiB, awaits owner verification). The 684 MiB mixed HEVC arena passed 24/24 golden 4K Main10 frames on `ram16g-hevc1`; control `ram16g-hctl1` passed the HEVC cap (−131 MiB delta) and isolated the AVC cancel timeout off the mixed arena. `ram16g-tier3w` verified **126 MiB** peak private GPU `region1` residency (1,512 MiB copies, 0 faults/refusals, all slabs returned) alongside tier-OFF control `ram16g-tctl3` (Metal Image 30/30). Daily stays `piano-extra-ram=0`, `piano-gpu-tier=0`. [Control](docs/results-2026-11-27.md#ram16g-hevc-control-ram-ram16g-hctl1) · [GPU tier](docs/results-2026-11-27.md#ram16g-private-gpu-tier-ram-ram16g-tier3w) |
| Internal storage (UFS) | ✅ Works (installed) | installed v13.1y12u, 2026-11-27; scratch-window A/B & power-cut tests, 2026-11-26–27 | Installed **v13.1y12u** runs **M4 concurrent mode** (`mode=concurrent depth=8`) **plus Disk M5 write-back** (`data-store-write-back=1`, `OWNER-0748`, after a fresh SHA-256 read-back verified owner Data backup): live disk boot logged `write_back=1`, `fua=0`, `cached=31,298` (`~27.9 µs` mean write), `flushes=1,311`, `errors=0`, and survived a clean reboot (`cached=3,966`, `flushes=137`, `errors=0`). Earlier A/B measured 4.9×–5.6× random-read IOPS (M4), 1.71×–4.12× write throughput and 42%–58% lower sync p50 (M5-wb), and 32/32 power-cut durability. `data-store-queue=0` stays off (unsuitable). [Installed v13.1y12u](docs/results-2026-11-27.md#installed-v131y12u-esp-only-update-and-reboot-check) · [Scratch-window results](docs/results-2026-11-27.md#disk-m5-scratch-window-ab) |
| Fingerprint, NPU | ❌ Deferred | none | No Secure Enclave (no Touch ID); no Core ML backend on this SoC. |

Details (status definitions, known gaps, showcase): [docs/STATUS.md](docs/STATUS.md).
Tablet results for [November 27](docs/results-2026-11-27.md) and [November 26](docs/results-2026-11-26.md).

**Showcase:** screenshots and short clips of each feature, taken on the tablet itself, are collected in
[docs/STATUS.md#showcase](docs/STATUS.md#showcase) (first native captures from the installed v13.1y12m: About This Mac, Displays, Battery, Finder, Activity Monitor, Metal 3D, Safari video).

A USB-C debug console (`package/PianoDwc3`, `tooling/native/usb-console.py`) and ssh over the cable are available;
the monitor's `reset fastboot` returns the tablet to ABL without buttons. Everyday development uses `fastboot boot`
RAM images, which take priority over the installed copy.

Per-driver pages, boot arguments and test levels: [docs/drivers/README.md](docs/drivers/README.md) and
[docs/PROJECT-MAP.md](docs/PROJECT-MAP.md). Using, updating and recovering the installed system:
[docs/USING.md](docs/USING.md). Earlier public write-ups include
[docs/results-2026-11-19.md](docs/results-2026-11-19.md) and
[docs/results-2026-11-17.md](docs/results-2026-11-17.md).

## Layout

| Path | What |
| --- | --- |
| `patches/k27-tree/` | our delta of the k27 tree on its pinned upstream (macOS-DroidVM): build scripts, KC builder, kernel series 0050-0054 |
| `patches/k27/` | the k27 kernel patches 0053-0076 on top of the k27 source tree (xnu-12377 + the baseline series 0001-0052) |
| `patches/k273/` | the 27.3 line's kernel series on top of `patches/k27` |
| `patches/k27-kc/` | the KC builder's native placement (best fit, relocation-free constants in `__TEXT`), applied by hand to the k27 tree ([docs/kc-space.md](docs/kc-space.md)) |
| `patches/xnu/` | the device series of the UniXNU macOS 27.3 line (empty: none yet) |
| `package/` | the native kexts; each is inert without its boot argument |
| `configs/` | the native kext list (`k27-native-kexts.txt`), the ADT identity and display node (`k27-adt.toml`, `k27-gop-display.toml`), the 27.3 line's kernel config |
| `dts/piano.dts` | piano's hardware for the ADT |
| `.github/`, `scripts/` | CI (checks, monthly dependency check, builds, hand-made releases: [docs/ci.md](docs/ci.md)), `fetch-pins.py` |
| `tooling/` | `k273/build-k273-image.sh` (the 27.3 kexts → kernel collection → ADT → RAM images, `K273_KERNEL`), `k273/build-k273-esp.py` (the installed 27.3 ESP), `kc/` (the KC build), `adt/`, `bootimg/` (RAM images and their verification), `native/` (the test loop, the USB console, input), `persist/` (the installed copy), `firmware/` (the FD) |
| `firmware/` | deltas to piano-uefi's `pianoPkg`, Mu-Silicium and UniXNU |
| `docs/` | bring-up notes, driver pages (`drivers/`), plans and evidence |
| `skills/`, `tooling/repro/` | the agent guide for another unit and its kit: pins of Apple's inputs, self-fetch, root rebuild checks, host check |
| `logs/`, `artifacts/ARTIFACTS.md` | committed verification logs; SHA-256 of built artifacts and inputs |
| `private/`, `out/` | ignored: keys, per-unit calibration; build outputs |

## Build

The whole path for a new unit (requirements, inputs, build, RAM-boot validation, guarded install, rollback, checks) is the
install guide [skills/install-piano-macos/SKILL.md](skills/install-piano-macos/SKILL.md), written for a coding agent
(Chinese: [SKILL.zh-CN.md](skills/install-piano-macos/SKILL.zh-CN.md)). This section is the
summary; the skill is the reference.

### Host requirements

- **A Mac with Apple silicon (M1 or later). Intel Macs are not supported.** Xcode 27's compiler and linker and macOS 27's own
  tools are arm64-only, and two build steps run arm64e programs they compile. The evidence is in the skill (§1.1).
- Tested: macOS 27.2.1 with **Xcode 27.2** (the app, not only the Command Line Tools: the kernel needs the iphoneos SDK and
  `Kernel.framework`; the kernel links with Xcode's `ld-classic`). Homebrew at `/opt/homebrew` with the packages the skill lists
  (§1.2), `fastboot`, Python 3.11 or later, about 90 GiB free disk, `sudo` in an attached Terminal for the root image steps.
- `bash tooling/repro/check-host.sh` reads the host and prints `ok` / `WARN` / `FAIL` / `GAP` per requirement; continue only with
  `0 FAIL`. Run from a native arm64 shell, not under Rosetta.

### Sources and pins

- This repository plus the pinned dependencies of `unixnu.lock` / `feeds.toml` (UniXNU core, Mu-Silicium, piano-uefi,
  DarwinBoot, the SoC module) and the k27 kernel tree. Set `PIANO_K27_TREE` to the tree pinned in `unixnu.lock`.
- **Fetching the pins:** `python3 scripts/fetch-pins.py` fetches every `unixnu.lock` checkout by commit into `$UNIXNU_RESEARCH`
  (`--verify` only checks them; contributed by SorasakiHinaHusband). The k27 kernel tree is upstream
  [macOS-DroidVM](https://github.com/Droid-VM/macOS-DroidVM) at its pin plus our [patches/k27-tree](patches/k27-tree/README.md):
  `tooling/k27/fetch-k27-tree.sh DEST`, then `PIANO_K27_TREE=DEST`.
- Apple's files are never distributed. `tooling/repro/fetch-ipsw.py` fetches the pinned 27.3 IPSW's members from Apple's CDN and
  the AEA keys from Apple, and checks every file against `tooling/repro/pins.json`.
- Some inputs are still local to the original setup (the k27 kernel tree, the base trust bundle, the Data seed, the Turnip tree,
  the write tooling's single-unit pins): the skill's "Known gaps" lists each with its options.

### Build steps

```sh
bash tooling/repro/fetch-and-build.sh --dry-run   # every command it would run; nothing fetched, built or written
bash tooling/repro/fetch-and-build.sh             # host, inputs, systemos, root, kernel, fd, image (or one step by name)
bash tooling/repro/fetch-and-build.sh --check     # each step's outputs against the pins (no network)
```

The steps it runs, also usable one by one:

```sh
bash tooling/k273/prepare-k27-tree.sh          # the 27.3 kernel tree and its relinked kernel; prints K273_KERNEL's path
bash tooling/firmware/build-fd.sh               # the UEFI FD from the unixnu.lock checkouts
K273_KERNEL=<path printed above> bash tooling/k273/build-k273-image.sh   # kexts -> kernel collection -> ADT -> RAM images
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
```

- **Kexts and the kernel collection:** the KC links only this repository's kexts (`configs/k27-native-kexts.txt`);
  `tooling/k27/check-native-kexts.py` enforces it before and after the link. Pass line in the KC log:
  `native kext rule: ... all this repository's`.
- **The root image is generated locally, never shipped.** The 20.9 GB root is the IPSW's OS volume (byte for byte, pinned) plus a
  SystemOS window rebuilt by `tooling/k273/make-systemos.sh` and `make-root-image.sh`, checked by `tooling/repro/verify-root.py`
  against the pinned file-level manifest (skill §3.1).
- Only the 27.3 line is built; the retired 27.0.1 path (`tooling/k27/build-native-image.sh`) is not used. Root placement: a new
  unit starts with the root on `linux_data` (`K273_ROOT_PART=linux_data`, skill §3.2); the reference unit's installed ESP uses
  `build-k273-esp.py --root-part linux_rootfs` ([docs/persistent-boot.md](docs/persistent-boot.md) §13.8).

### CI and releases

The FD, the k27 native kernel and our kexts also build on GitHub's hosted runners ([docs/ci.md](docs/ci.md)): pull requests run the
pin fetch, the self-tests and a privacy scan; releases (tagged by hand after a tablet qualification) carry only our binaries, the
recipes, SHA-256 manifests and the install skill. The root image and the kernel collection are always built on the user's side.

### Size limit of a RAM image

A RAM boot image must be **at most 250,000,000 bytes**. ABL loads the whole image into the UEFI's 256 MiB kernel region; a
294,952,960-byte image stayed in fastboot after the download, while 247,767,040-byte images boot
([docs/k273-line.md](docs/k273-line.md) §4). The cap is `IMAGE_MAX_BYTES` in `tooling/bootimg/build-ram-images.py` (which
shrinks the payload's free space when an image would pass it) and is checked again by `tooling/bootimg/verify-ram-images.py`
(both read `PIANO_IMAGE_MAX_BYTES`; do not raise it).

### RAM boot before any install

Every image is first booted with `fastboot boot` (a RAM boot writes no partition) and must pass there before anything is written
to the disk: the console reaches XNU, the USB console answers, the boot returns to ABL by its reset guard, and for a desktop image
the login window, touch, keyboard cover, ssh, Wi-Fi and battery read correctly (skill §4). Installs then go only through
`tooling/persist/maint-session.py`, with backups, read-back checks and a rollback for each step
([docs/persistent-boot.md](docs/persistent-boot.md), [docs/install-esp-v3.md](docs/install-esp-v3.md)).

More: boot arguments and debugging without a screen in [docs/bringup-debugging.md](docs/bringup-debugging.md) (§4 for what is read
from the k27 tree); what works today in [docs/STATUS.md](docs/STATUS.md).

## Boot and test

Everyday development uses RAM boots: `fastboot boot` of a RAM image, which takes priority over the installed copy, with an
unattended loop that boots a test, resets and reads the test's log back (`tooling/native/native-loop.sh`,
[docs/bringup-debugging.md](docs/bringup-debugging.md) §3). The installed copy ([docs/persistent-boot.md](docs/persistent-boot.md))
writes only the regions that document lists, after verified backups of each, with a read-back check after every write and a
rollback for each step; it never writes firmware partitions. Do not flash boot or firmware partitions by hand.

## The native kext rule

The native kernel collection links only this repository's kexts (`configs/k27-native-kexts.txt`). `tooling/k27/check-native-kexts.py`
enforces it on every build, before and after the link: no descriptor from elsewhere, none of the VM-era paravirtual kexts.

## Where to read next

- [docs/STATUS.md](docs/STATUS.md): what works, per feature, with the build and date it was verified.
- [docs/USING.md](docs/USING.md): using, updating and recovering the installed system.
- [skills/install-piano-macos/SKILL.md](skills/install-piano-macos/SKILL.md): building and installing on another unit (agent guide).
- Driver design pages, boot arguments and qualification notes. [Driver index](docs/drivers/README.md)
- [docs/PROJECT-MAP.md](docs/PROJECT-MAP.md): repositories, local layout, hard rules.
- [docs/bringup-debugging.md](docs/bringup-debugging.md): the boot chain, debugging without a screen, the test loop, rebuilding.
- [docs/BACKLOG-hardware.md](docs/BACKLOG-hardware.md): the hardware still to bring up.
- [docs/kc-space.md](docs/kc-space.md): the kernel collection's room for native kexts, the run-time firmware loader.

## Licence and contents

Code here is BSD-2-Clause-Patent, Copyright (c) 2026 Leo Zhang, unless a package's `LICENSE`, a file's SPDX header or an
`UPSTREAM` file says otherwise (third-party parts keep their own holders and licences).
Vendor firmware, partition images and Apple payloads (kernel collections, trust caches, IPSW-derived files) are never in this
repository; neither are secrets, keys, per-unit calibration, local network or storage details, or machine names.

`scripts/fetch-pins.py` was first written by SorasakiHinaHusband (pull request #2).
