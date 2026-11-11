# Video: the Iris video core natively (PianoVideo) and VideoToolbox through the VM-era paravirtual decoder

Status, 2026-11-11: **level 1 built and host-tested** (`package/PianoVideo`, `piano-video=1`, in the kext list, inert without the
argument); not run on the tablet. Levels 2+ are design. Template: `README.md`. Backlog item B10 (`../BACKLOG-hardware.md`).

## 1. Purpose and gate

Natively every video is decoded by the CPU today (VideoToolbox's software decoders: the machine has none of Apple's decoder
hardware). The SoC's video core decodes H.264, HEVC and VP9 at its lowest clock for every common stream (§4.5), and encodes H.264
and HEVC. Gate: battery life (owner priority) and daily use (QuickTime, Safari, wallpapers, thumbnails);
not a login gate. Ubuntu uses it: H.264/HEVC/VP9 decode and H.264/H.265 encode verified at 1080p and 4K, AV1 in software
(`xiaomipad-8pro-mainline/docs/STATUS.md`, `README.en.md`; the core lists AV1, but a third-party piano report has AV1 decode failing
at submit: `unixnu-research/hackios-redmi-k90-annibale/docs/analysis/2026-11-05-video-decode.md` §4.1). The macOS side is proven:
in the VM, VideoToolbox decoded H.264 and HEVC (including the 4096x2160 P010 Aerial) on Ubuntu's Iris through a paravirtual decoder
(`droidvm-build/PROGRESS-thin-macos.md`, issue 4, 2026-11-10).

## 2. Hardware facts

Sources: the piano boot DT (`out/inputs/piano-boot.dtb`, "DT"); the Ubuntu piano kernel (`unixnu-research/ubuntu-ref/kernel`, piano/dev
7f8d6bed, GPL: facts only): iris (`drivers/media/platform/qcom/iris`), videocc/gcc (`drivers/clk/qcom/*-sm8750.c`), qcom_scm,
interconnect `sm8750.c`; the firmware pin (`xiaomipad-8pro-mainline/firmware/piano-iris.json`). Every offset is in
`package/PianoVideo/VideoHw.h`.

| Item | Value | Source | State |
|---|---|---|---|
| Core | `aa00000` (0xf0000), `qcom,sm8750-iris`, VPU 3.5, HFI gen2, 4 VPP pipes, ≤ 16 sessions, ≤ 2x 8K macroblocks per frame; decode H.264/HEVC/VP9/AV1 (`iris_fmts_vpu3x_dec`), encode H.264/HEVC | DT, `iris_platform_vpu3x.c` | verified |
| Regions | VCODEC +0 (codec domain; idle words +0x70+4·pipe); CPU_CS +0xa0000 (CTRL_INIT +0x48, CTRL_STATUS +0x4c, QTBL_INFO/ADDR +0x50/54, SFR +0x5c, UC_REGION +0x64/68, H2X int +0x148, IC_SOFTINT +0x150 doorbell, X2RPMH +0x168); WRAPPER +0xb0000 (INTR status/mask +0xc/+0x10, LPI +0x54..0x60, SPARE_0 +0x78, CORE_POWER_STATUS +0x80); WRAPPER_TZ +0xc0000 (CPU status +0x10 WFI); AON +0xe0000 (NOC LPI) | iris register defines, `iris_vpu3x.c` | verified (source) |
| Interrupt | SPI 174 level (INTID 206): A2H message bit 2, watchdog bit 3 | DT, `iris_vpu_common.c` | verified |
| Clocks | GCC video AHB `0x132004` + XO `0x132038` (Linux keeps both on), AXI0 `0x132018` (codec), AXI1 `0x132028` (controller); VIDEO_CC `aaf0000`: PLL0 Taycan ELU (L 0x25 + alpha 0x8000 = 720 MHz), `mvs0_clk_src` +0x8000 (720..1890 MHz from PLL0) and divider +0x809c (/3) = the OPP clock; branches MVS0C +0x804c, MVS0C free-run +0x805c, MVS0 +0x807c, MVS0 free-run +0x808c; AHB/XO/sleep +0x80a4/d4/f8 (kept on by Linux) | DT, videocc, gcc | verified (source) |
| GDSCs | MVS0C +0x8034 (controller, "venus"), MVS0 +0x8068 (codec, "vcodec0", child of MVS0C, hardware-controlled once running) | videocc | verified |
| Rails, OPPs | MMCX + MXC (VIDEO_CC itself sits on both, at low_svs); OPPs 240/338/420/444/533/570/630 MHz, 240 = MXC svs + MMCX low_svs_d1 | DT | verified |
| Bus | `cpu-cfg` (APPSS → VENUS_CFG, Linux 1 MB/s) and `video-mem` (`qnm_video_mvp` → EBI1: BCM **MM1** + SH0/MC0/ACV); Linux's DDR table: 1080p30 294, 1080p60 567, 4K30 826, 4K60 1608 MB/s. The display rides MM0, so nothing keeps MM1 up for video | DT, interconnect, iris | verified (source); MM1 at XNU: needs-boot |
| SMMU | apps SMMU `15000000` (DT `dma-coherent`), streams **0x1940, 0x1947** mask 0 (one Linux domain); the core is `dma-coherent`; device addresses below **0xe0000000** (`dma_mask`) | DT, iris | verified |
| Content protection | after authentication, SCM MP_VIDEO_VAR (`0x42000c08`) with CP `[0, 0x25800000)`, non-pixel `[0x01000000, 0x25800000)` | `iris_firmware.c`, scm | verified (source) |
| Firmware | `qcom/sm8750/vpu35_4v.mbn` (DT `firmware-name`), 2 409 160 bytes, signed ELF32, the stock odm_a `firmware/vpu35_4v.mbn` (OS3.0.303.0); Linux's generic `qcom/vpu/vpu35_p4.mbn` is not piano's (vendor-signed) | DT, pin, K90 note | verified (pin); generic one refused: inferred |
| Carveout | `video@9c300000`, 8 MiB, no-map (the node's memory-region), outside XNU's DRAM window | DT | verified |
| PAS | ID **9**: init_image, mem_setup, segments, auth_and_reset; suspend/resume SET_REMOTE_STATE(0/1, 0) (already in Sm8750Scm); unload PAS shutdown 9 | `iris_firmware.c` | verified (source) |
| Thermal | zone `video` = TSENS2 (`c22a000`) sensor 15: hot 120 °C, critical 125 °C (PianoThermal reads it) | DT | verified |
| State at XNU | expected cold (nothing before XNU runs video); SMMU entries for 0x1940/0x1947, free groups and banks, IDR0.CTTW | level 1 | **needs-boot** |

## 3. Ubuntu as the specification

- **Core init** (`iris_core.c:iris_core_init`): queues, power, firmware, boot, HFI. *Queues* (`iris_hfi_queues_init`): a 456-byte
  table (288-byte header, three 56-byte queue headers: command, message, debug), three 1 MiB queues of variable-size packets (indices
  in 32-bit words, first word = size), a 4 KiB SFR whose first word is its size; write-combined, device-addressed. *Power*
  (`iris_vpu_power_on`): bandwidth votes, controller (MVS0C GDSC, AXI1, MVS0C free-run, MVS0C), codec (MVS0 GDSC, AXI0, MVS0
  free-run, MVS0), the OPP rate, WRAPPER +0x88 = 0, A2H/watchdog unmasked. *Firmware* (`iris_fw_load`): the MDT into the carveout,
  PAS 9, MP_VIDEO_VAR. *Boot* (`iris_vpu_boot_firmware`): UC_REGION address and size (1 MiB aligned), QTBL address + enable, SFR,
  SPARE_0 = 1 (VPU 3.5), CTRL_INIT = 1, SCIACMDARG3 = 1, poll CTRL_STATUS (≤ 1000 x 50–100 µs; error 0x4 = bad UC region),
  H2X interrupt on, X2RPMH = 0; then the codec GDSC in hardware mode.
- **HFI gen2** (`iris_hfi_gen2_packet.h`, `_defines.h`): a 32-byte header {size, session, header id, 4 reserved, packet count} and
  32-byte packets {size, type, flags, payload info, port, packet id, 2 reserved} + payload. Host flags: interrupt 1, response 2,
  non-discardable 4, get-property 8; firmware flags: success 1, information 2, session error 4, system error 8. Ports: none 0,
  bitstream 1, raw 2. Commands 0x0100000x (INIT 1, OPEN 3, CLOSE 4, START 5, STOP 6, DRAIN 7, RESUME 8, BUFFER 9, SUBSCRIBE_MODE b,
  SETTINGS_CHANGE c, PAUSE 0x11), properties 0x03000xxx, session errors 0x0400000x, watchdog 0x05000001, information (DRAIN_LAST,
  PSC_LAST) 0x0600000x. A buffer is 64 bytes {type, index, IOVA (4 KiB aligned), offset, size, data offset, data size, timestamp ns,
  flags}. A command is written to the command queue, then IC_SOFTINT rings the core; answers raise SPI 174.
- **Handshake** (`iris_hfi_gen2_command.c`): INIT (payload `HFI_VIDEO_ARCH_LX`, plus the UBWC properties of `ubwc_config.c`'s
  SM8750 entry: 8 channels, HBB 16, swizzle levels 2 and 3, bank spreading), then IMAGE_VERSION as a get-property (the answer is
  the firmware's version string), then inter-frame power collapse; the reply within 1 s.
- **A decode session** (`iris_vdec.c`, `iris_hfi_gen2_command.c`): OPEN (session id) → CODEC → default header → the input
  properties (decode-order output, resolution, crop, bit depth, coded frames, minimum output count, POC type, profile, level,
  colour; per-codec lists in `iris_hfi_gen2.c`) → PERSIST, then BIN/COMV/NON_COMV/LINE/PARTIAL internal buffers (sizes:
  `iris_vpu_buffer.c:iris_vpu33_buf_size`, per codec and resolution) → SUBSCRIBE_MODE + START(bitstream) → bitstream buffers →
  the firmware's SETTINGS_CHANGE (the port-settings change = V4L2 SOURCE_CHANGE) → the output properties (OPB, NV12/P010, linear
  stride/scanline, width aligned to 128 and height to 32) → DPB buffers → START(raw) → raw buffers; buffer-done packets return each
  (LAST/PSC_LAST flags); DRAIN for the end of stream; STOP, CLOSE.
- **Power** (`iris_hfi_common.c`): runtime suspend 1.5 s after the last use: PC prep, wait PC_READY + WFI, SET_REMOTE_STATE(0),
  power off (codec: NoC LPI handshake retried up to 1000 times, bridge reset; controller: X2RPMH masked, CPU NoC / video-ctl NoC /
  debug-bridge LPI, clocks, GDSC, resets). Resume: power on, SET_REMOTE_STATE(1), boot again: **no re-authentication**.
- **Fixes Ubuntu carries** (`ubuntu-history/kernel.md` §1.10), behaviours PianoVideo needs from the start: v0.3.0 unmatched
  timestamp metadata discarded, no interrupt work after removal or during power-off; v0.6.0 decode-order output and larger capture
  pools on gen2 (K:1c507b3d51b9, K:e6537c292a51); v0.10.0 a drain finishes when the last buffer has no payload (K:4d00f8297193),
  encoder key-frame requests (K:1cb49011cf28).
- **Acceptance** (`device/media/iris-check.py`): one bounded H.264 decode, every frame's MD5 against a host `framemd5`: level 5's
  criterion.

## 4. UniXNU template to build on, and the design

| Piece | Base (licence) | Use |
|---|---|---|
| VideoToolbox side | macOS-DroidVM `k27/build-native-video.sh`: UniXNU-droidvm `706338e` `package/DroidVMVTParavirt.cpp`, `DroidVMVTHost.cpp`, `DroidVMVTWire.h` + `patches/droidvm-video` 0001–0017 (BSD-2-Clause-Patent) | vendored unchanged as `package/PianoVideo/vt/` with UPSTREAM (level 6) |
| Its transport | `DroidVMVirtioMedia.c` (virtio-media) | **replaced** by PianoVideo's Iris backend, the same 14 functions |
| PAS, MDT, firmware file | PianoPower `Sm8750Scm.c` (+ MP_VIDEO_VAR), `QcomMdt.c`, `FatVolume.c` + NativeUfs partition reads (BSD) | level 3 |
| Rails, bandwidth | PianoA830 `Sm8750Rpmh` (cmd-db, ARC, BCM encoders; MIT): `mmcx.lvl`, `mxc.lvl`, MM1/SH0/MC0/ACV | level 2 |
| SMMU | PianoWifi/PianoDwc3 `SmmuStream.h` (a free group + bank for a fresh stream, done on this SMMU for 0x1401), dpu.md §4.2's tables | level 4 |
| IRQ, power | `package/common/PianoIrq.h`, `package/include/PianoPowerContract.h` | levels 4, 7 |

The ADSP is not involved (the core runs its own firmware); PianoPower's DSP contract does not apply.

**4.1 The VideoToolbox verdict: reuse it, unchanged above the transport.** The VM's frontend is Apple's own: VideoToolbox's
ParavirtualizedVideoDecoder/Encoder and `VideoToolboxParavirtualizationSupport.framework` in userspace, which open the kernel service
`AppleVideoToolboxParavirtualizationDriver`. `DroidVMVTParavirt.cpp` *is* that service: the user client the framework expects
(selector 0: send buffer + IODataQueue; 1/2: register/deregister a session UUID; 3: send a message with ≤ 5 IOSurface IDs; 4:
drained; notification port 0). `DroidVMVTHost.cpp` is the "Mac host" half: it answers VideoToolbox's `vtpv` messages (decs/encs,
+dec, dsta/dsts, dfrm, dfin, dinv, Dbuf/Demt; big-endian atoms, `DroidVMVTWire.h`) and runs each session as a client of a
**stateful V4L2 codec**. Nothing in either file is virtio-specific: the device side is 14 `extern "C"` functions
(`droidvm_media_start/devices/caps/card/is_up/open/close/ioctl/ioctl_sg/mmap/munmap/set_sink/set_fast`). So **no new user client and
no new userspace**: the native kernel provides those functions with V4L2 stateful-codec semantics over Iris, which is what Linux's
iris driver gave the VM through crosvm's proxy, and the VM's 16 patches (0001–0017) come along (HEVC rated 1000, `dsts`, SEI skipping, HEIC tiles
refused, the 4K P010 direct pool, frame thinning, session lifecycle). The subset DroidVMVTHost uses: ENUM_FMT, ENUM_FRAMESIZES,
G_FMT/S_FMT (MPLANE), REQBUFS/QUERYBUF/QBUF (USERPTR only; QBUF carries the physical page list), STREAMON/OFF, G_CTRL
MIN_BUFFERS_FOR_CAPTURE, S_CTRL display delay, SUBSCRIBE_EVENT (EOS, SOURCE_CHANGE), DECODER_CMD START/STOP (ENCODER_CMD later);
events DQBUF/EVENT/ERROR through the sink in virtio-media's layout; card "Iris Decoder"/"Iris Encoder" (0013 matches the role);
payloads in the 64-bit Linux layouts it already builds. Natively the page lists become SMMU entries (no crosvm copy), and VTHost
copies each decoded frame into the player's IOSurface as in the VM, or decodes in place (its direct mode).
Rejected: an AppleAVD-compatible service (AVD's userspace drives Apple's own hardware), a private VideoToolbox decoder plug-in (its
method tables would need Apple binary reverse engineering), MediaExtension decoders (public, but for formats the system lacks, in
apps that opt in; inferred from Apple's documentation). The one open risk is VideoToolbox's "running in a guest" decision with the
truthful `vmm-present = 0` (open question 1): level V0b answers it in one boot, before any HFI work.

**4.2 Memory: a translating bank of our own.** The core addresses 32 bits below 0xe0000000, and every XNU page is above 4 GiB
(0x8c0000000..0xc00000000), so bypass cannot work: the streams need a stage-1 bank with XNU's tables. It is the same root problem as
audio, sensors (32-bit DSP addresses) and the DPU (32-bit fetch); here it is the easy case: Linux under the same Gunyah programs
translating banks for 0x1940/0x1947 itself and decodes (Ubuntu ships it), and the core is cold, so a fresh group per stream and a
fresh bank replace nothing (the DPU must reprogram its live bank in place). Window: IOVA `[0x80000000, 0xe0000000)` (above the CP
range MP_VIDEO_VAR declares, below the limit): the UC region (queues + SFR, 1 MiB aligned) first, then per-session internal buffers,
then client buffers mapped at QBUF (16 KiB XNU pages = four 4 KiB entries) and kept per buffer index until REQBUFS 0 or close. A 4K
P010 session (6 capture buffers of 26.7 MB, its DPB and internal buffers) needs a few hundred MiB: two fit. Tables: 4 KiB granule,
T0SZ 32, in XNU memory (TCR2 PASize ≥ 36 bits, as dpu.md); the core and this SMMU are `dma-coherent`, so entries are normal
write-back inner-shareable and, if IDR0.CTTW reads 1 (level 1 prints it), walks are coherent; otherwise tables are written uncached
(dpu.md's rule). A coherent data path also means VTHost's copy reads a cached mapping (the DPU's copy reads uncached: 34 ms a frame).
**Coordination:** one portable `package/common/SmmuWindow.h` (table builder, window allocator, map/unmap of page lists, TLBI by VA +
sync, fault readout) for PianoDpu (B1b) and PianoVideo, and one registry with a lock word for free stream-match groups and banks
(PianoWifi already takes a free group for 0x1401: two kexts must never take the same one).

**4.3 Firmware at run time.** The owner's own `vpu35_4v.mbn` (from the tablet's odm_a, or the Ubuntu install's copy in the owner's
backup) goes onto the ESP (`piano/firmware/qcom/sm8750/vpu35_4v.mbn`, a `build-esp.py` option to add) from the ignored `private/`;
PianoVideo reads it through NativeUfs + FatVolume (PianoPower's reader), checks size and SHA-256 against the pin and refuses a
mismatch. Never in this repository or a build output; never disassembled (data for the secure world's loader). Later, an odm_a
reader would serve video, audio and sensors alike. The load is PianoPower's sequence with PAS 9, through **one SCM lock shared**
with PianoPower and PianoA830 (no two secure calls at once: the K90's rule, `charging.md` §5a's stall).

**4.4 The backend's states.** Per instance: the V4L2 state machine (Linux's `iris_state.c/.h` as the specification: deinit, init,
input streaming, output streaming, streaming, error; sub-states first source change, resolution change and its last buffer, drain
and its last buffer, input/output pause) over one HFI session; a core thread owns the
queues (message queue drained on SPI 174, polled while starting), debug queue drained to the log (rate-limited), SFR printed on a
watchdog or system error; a system error closes every session with ERROR events (VideoToolbox then falls back to software) and the
core is recovered by power off + boot (the firmware stays in the carveout).

**4.5 Power** (*inferred* until measured). Linux's own formula (`iris_vpu3x_vpu4x_calculate_frequency`, SM8550-class caps: 200 VPP
cycles per macroblock over 4 pipes, 489 583 firmware cycles per frame) asks ~16 MHz for 1080p30 H.264 and ~120 MHz for 4K60: every
common stream runs at the **lowest OPP, 240 MHz** (MXC svs, MMCX low_svs_d1), with Linux's DDR votes of 294 MB/s (1080p30) to
1608 MB/s (4K60). Software decoding costs, by the K90 analysis's estimate (§3.3 there), 15–60 % of an Oryon core for 1080p30 H.264,
1.5–2x that for HEVC/VP9, ~4x for 4K (4K60 HEVC is not real-time in software).

| Case | Software (CPU) | Iris |
|---|---|---|
| 1080p30 H.264 | ~0.3–0.8 W | core at 240 MHz + 0.3 GB/s DDR: ~0.05–0.15 W; the NV12 copy (3.1 MB a frame, cached) ≈ 1 % of a core |
| 1080p30 HEVC / VP9 | ~0.5–1.5 W | the same as H.264 |
| 4K HEVC, 30–60 fps (the Aerials) | several W, frames dropped at 60 | ~0.15–0.4 W; the P010 copy (26.7 MB a frame) is 0.8–1.6 GB/s each way: the direct mode matters |
| Nothing playing | 0 | 0: collapsed 1.5 s after the last frame, the firmware kept; resume is power on + SET_REMOTE_STATE(1) + boot, no PAS |

Measured as `power.md` §6.4: one 1080p30 H.264 and one HEVC clip looped in QuickTime, fixed brightness, no cable,
`piano-video=0` against `=6`, the mean of PianoPower's 10 s battery lines from minute 2 to 9, and the decoding process's CPU in `top`.

## 5. IOKit family / ABI

Level 1: `PianoVideo : IOService` on IOPlatformExpertDevice, registers nothing; properties `piano-video`, `piano-video-survey` (one
line). Level 6: the vendored transport registers `AppleVideoToolboxParavirtualizationDriver` on IOResources (IOUserClientClass
`AppleVideoToolboxParavirtualizationUserClient`: Apple's names, because the sandbox lets a process open only the user clients it
names) and the descriptor gains `com.apple.iokit.IOSurface`. The vendored files keep their `droidvm_*` symbol names (the native
kext rule checks bundle, name and IOClass, not symbols). PianoVideo joins the power contract: off = every session paused and the core
collapsed, votes dropped; on = nothing until the next session.

## 6. Boot args

| Argument | Default | Meaning |
|---|---|---|
| `piano-video` | 0 | 1 the read-only survey (built); later 2 power on/off, 3 + firmware (PAS), 4 + SMMU and HFI handshake, 5 + the embedded H.264 self-test, 6 + the VideoToolbox service (§8) |
| `piano-video-cc` | 1 | 0: VIDEO_CC and the core are not read (the fallback if a load there stalls) |
| `piano-video-smmu` | 2 | 0 skip the SMMU; 1 its global pages; 2 also the banks of matching groups |
| `piano-video-carveout` | 0 | 1: also load the first 32 bytes of the carveout `9c300000` (memory XNU does not own) |
| `piano-video-delay=S`, `-repeat=S`, `-samples=N` | 0, 0, 3 | wait first; read the clock words again every S s, N times |
| later: `piano-video-codecs=MASK`, `piano-video-direct`, `piano-video-idle-ms` | 3 (H.264, HEVC), 0, 1500 | which codecs `decs` offers (4 = VP9); decode into IOSurfaces; the collapse delay |

## 7. Kernel collection

`tooling/k27/build-native-piano-video.sh` (host tests `tests/video_host.cpp`: the gate at each step, the allowlist of loads, the two
registers never loaded, no word loaded twice, the PLL/RCG/divider/OPP decoders, CTRL_STATUS and the core states, the SMMU scan with
free groups and banks in use; then the objects and the import check: 309 kernel imports, 0 unresolved). PianoVideo is in
`configs/k27-native-kexts.txt`, so every `build-native-image.sh` links it. Link checked 2026-11-11 on kernel 0059 with the whole
list: `com.unixnu.driver.PianoVideo: linked, ... 320 pointers`, `native kext rule: 17 native kexts linked, none from DroidVM`.

## 8. Hardware test plan and the ladder (RAM boots; V0 unattended, the rest attended)

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-video=1" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-video1 && cp -R out/ram out/ram-video1
python3 tooling/native/usb-console.py --capture 1000 &   # then: tooling/native/native-loop.sh out/ram-video1/boot-piano-macos-ram-A-bundled.img
grep -a "PianoVideo" logs/usb-console/<newest>-port0.txt
```

**V0 expected** (nothing changes; `...` = values to record):
```
PianoVideo: start: piano-video=1 cc=1 smmu=2 carveout=0 delay=0 repeat=0 samples=3 (Iris 0xaa00000, VIDEO_CC 0xaaf0000, SPI 174, streams 0x1940/0x1947, carveout 0x9c300000)
PianoVideo: level 1: loads only, nothing written
PianoVideo: stage 1: GCC video branches (read only)
PianoVideo: gcc: video ahb ........ (on|off) xo ........ (..) axi0 ........ (off) axi1 ........ (off)
PianoVideo: stage 2: VIDEO_CC behind GCC's video AHB (read only)
PianoVideo: videocc: pll0 mode ........ (not locked, not running) ... L 0 alpha 0000 -> 0 MHz; mvs0 rcg ... (root off, source xo, ...) ...
PianoVideo: videocc: gdsc mvs0c ........ (off) cfg ..., mvs0 ........ (off...) cfg ...; branches mvs0c ........ (off) ...
PianoVideo: gate: controller GDSC (MVS0C) collapsed: the core is cold and not read (as expected before any video driver)
PianoVideo: stage 3: the core skipped: its gate is closed
PianoVideo: core: not read (controller GDSC (MVS0C) collapsed: ...)
PianoVideo: core: cold: the video core is off (GDSC collapsed or its AHB gated): level 2 powers it from cold, ...
PianoVideo: stage 4: SMMU streams 0x1940-0x194f and their banks (read only)
PianoVideo: SMMU scr0 ... idr0 ... idr1 60000053: N stream-match groups (F free), 83 banks (U in use by valid groups), 128 pages, coherent table walks yes|no
PianoVideo: SMMU streams 0x1940-0x194f: no stream-match entry          (or: group G smr ... s2cr ... (translate, bank B) ...)
PianoVideo: SMMU route: no stream-match entry for 0x1940/0x1947 (...): level 4 takes a free group per stream and a free bank ...
PianoVideo: summary: gcc ahb .. axi0 off axi1 off; videocc read; mvs0c off mvs0 off; pll0 off 0 MHz; core cold; smmu 0x1940 none 0x1947 none free F
PianoVideo: stage 6: done
PianoVideo: done: N MMIO loads, 0 stores; P pages mapped, 0 map failures
```
A hang after `stage 2` (no `videocc:` lines) means VIDEO_CC is not readable at its rails' boot levels: rerun with
`piano-video-cc=0` (GCC alone) and give level 2 the MMCX/MXC votes before the first VIDEO_CC load; a hang after `stage 4` is the
SMMU (`piano-video-smmu=0`; PianoDpu and PianoWifi read the same pages). A `core:` line other than `cold`
(a firmware already running) changes level 3 (shut it down first). Then once more with `piano-video-carveout=1`.

| Level | Arguments | Step | Pass | Effort |
|---|---|---|---|---|
| V0 | `piano-video=1` | survey (built) | the lines above, no hang, `0 stores` | done offline; 1 boot |
| V0b | a test image | the vendored transport + VTHost with a stub backend listing H.264/HEVC and refusing `dsta` | with `vmm-present = 0`: a process opens the service and a `decs` arrives (the transport's open and message counters, logged to the USB console), playback falls back to software cleanly | 1–2 days; 1 boot |
| V1 | `=2` | power: MMCX/MXC + MM1/SH0/MC0/ACV votes, GCC AXI0/1, PLL0 720 MHz, MVS0C then MVS0 GDSC + clocks at 240 MHz; the survey's core readout with the gate open; off in Linux's order | CORE_POWER_STATUS bit 1, sane words, CTRL_STATUS 0; GDSCs off again; 20 cycles; the panel unaffected | 3–5 days; 2–4 boots |
| V2 | `=3` | firmware: the pin from the ESP, MDT, PAS 9 init/mem_setup/segments/auth_and_reset, MP_VIDEO_VAR; PAS shutdown | every SMC `ok`, no stall, 3 repeats | 2–4 days; 2–3 boots |
| V3 | `=4` | SMMU groups + bank, UC region, boot words, CTRL_INIT; INIT + IMAGE_VERSION; PC prep, collapse, resume | init without error, INIT answered with success, the version string, FSR 0; 10 collapse/resume cycles | 1–2 weeks; 4–8 boots |
| V4 | `=5` | one H.264 clip in the kernel (our own 320x240, 10 frames, from ffmpeg's test source), the whole session of §3 | every NV12 frame's MD5 = the Mac's `framemd5`; no SMMU fault; close + collapse | 2–3 weeks (buffer sizes, the state machine); 5–10 boots |
| V5 | `=6` | the V4L2-semantics backend under the vendored VT files | `vtread.js`: `dinv: in 300 out 300 dropped 0`, frames equal to the source, UsingHardwareAcceleratedVideoDecoder true; QuickTime/Safari H.264; HEIC still Apple's; 30 min, no leak | 1–2 weeks; 3–6 boots |
| V6 | `=6` | HEVC Main/Main10 (P010, the Aerial), VP9 (`decs` entry `vp09`) | the VM's HEVC tests on Iris; VP9 if VideoToolbox takes it | 1–2 weeks |
| V7 | — | collapse after 1.5 s, bandwidth by resolution, the s2idle off state, the thermal hook; the A/B of §4.5 | collapse between clips; a measured saving; s2idle with a paused video | ~1 week |
| V8 | `=7` | encode H.264/HEVC through VTHost's encoder half (VPSS buffers, key-frame request) | 1080p and 4K AVAssetWriter output that decodes | 2–3 weeks |
| — | `piano-video-direct=1` | decode into IOSurfaces (NV12, P010) | no copy time at 4K60 | ~1 week |

Total about 10–16 weeks attended; H.264 in VideoToolbox (the first battery payoff) after V5, about 6–9 weeks in.

## 9. Fallback

Without `piano-video` nothing is mapped and VideoToolbox keeps Apple's software decoders. From level 6, a format the core cannot
take is refused at `dsta` (VideoToolbox then decodes in software); a session error or a core watchdog ends the sessions with ERROR
events (software again) and powers the core off. A hang or reset returns to the previous system (RAM boots, `piano-reset-after`).

## 10. Open questions

1. Does VideoToolbox use the paravirtual service natively with `vmm-present = 0`? → V0b. If not: `vmm-present = 1` costs the
   'RGhA' framebuffer (`soft-metal.md`); no other route is open within the rules.
2. The state at XNU time (GCC video AHB, GDSCs, PLL0) and whether VIDEO_CC is readable at the boot rail levels → V0.
3. SMMU: entries for 0x1940..0x194f, free groups and banks, CTTW → V0; a fresh bank accepted under Gunyah → V3.
4. PAS 9 from XNU with the owner's file, and the shared SCM lock → V2.
5. Is the MM1 vote needed (the display uses MM0)? → V1/V3 (a stalled core without it).
6. The internal buffer sizes (`iris_vpu33_buf_size`) re-derived exactly → V4 (INSUFFICIENT_RESOURCES or an SMMU fault otherwise).
7. VP9 through the paravirtual protocol (the VM never offered `vp09`) → V6.
8. Direct decode: does CoreVideo's pool give the core's layout (128-byte rows, 32-line scanlines, chroma right after)? → after V5.
9. VTHost's encoder half was only run against the phone's MediaCodec, never Iris → V8.
10. Protected content (secure sessions, CP buffers): not supported; such players fall back or refuse.

## 11. Licence note

The Linux iris, Venus, clock, SCM, interconnect and SMMU drivers (GPL-2.0) are read as the specification only (offsets, fields,
sequences, sizes); nothing is copied. PianoVideo is BSD-2-Clause-Patent; its shape comes from PianoDpu (this repository). The VT
files to vendor are BSD-2-Clause-Patent (UniXNU-droidvm). `vpu35_4v.mbn` is the owner's data, read at run time, never committed or
disassembled; no Apple binary is reverse engineered (public headers, class and symbol names only). Provenance:
`package/PianoVideo/UPSTREAM`.
