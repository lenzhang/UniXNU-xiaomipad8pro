# Audio: four FS19xx speakers on TDM, DMICs on the VA macro, through the ADSP's AudioReach: PianoAudio

Status: design + level 1 code, 2026-11-11. `package/PianoAudio` level 1 (read-only survey) is built, host-tested and linked into the
native KC (kernel 0059: "12 native kexts linked, none from DroidVM", PianoAudio 341 pointers), inert without `piano-audio`; a RAM
image with §8.1's arguments verifies PASS; **not run on hardware**. Levels 2 and up are design only. Template: `README.md`.

## 1. Purpose and gate

Speakers and microphones for native macOS. Gates no login milestone (plan §4(d).6: "Wi-Fi, Bluetooth, audio, sensors, later");
it is the last piece of a tablet that can replace Ubuntu for media and calls. Today the native boot runs `coreaudiod` and
`audiomxd` (11-11 07:26 run: pids 282, 292, the HAL's driver helpers) but the HAL finds no device.

The finding of this page: on piano the whole path is DSP-driven and the DSP is already up natively (PianoPower level 2), every
protocol piece has a hardware-verified BSD donor (UniXNU-sheng's GPR/AudioReach on SM8550; the VM's DroidVMAudio for the macOS
device on this very macOS 27), and the piano-specific work is small but risky: the four FS19xx amplifiers (bus, reset, the vendor
preset), the TDM sink instead of sheng's I2S, a DMA buffer the DSP can address, and speaker safety without any speaker protection.

## 2. Hardware facts

Sources: DT = `out/inputs/piano-boot.dtb` (Ubuntu's running DT); UBU = `xiaomipad-8pro-mainline` `device/audio/` (README, m4, UCM,
`card.json`), `firmware/piano-audio.json`, `tools/speaker-playback-check.py`; INV = `droidvm-build/piano-patches/ubuntu-hardware-inventory.md`
§8; SHENG = UniXNU-sheng `porting.toml` area "audio" (SM8550, same DSP family, same hypervisor); RUN = `logs/native-runs/20261111-072646-loop`
(PianoPower level 2). All register values of level 1 are in `package/PianoAudio/AudioHw.h`.

| Item | Value | Source | State |
|---|---|---|---|
| Speakers | **four**, each with its own **FourSemi FS19xx** smart amplifier (`foursemi,fs19xx`), I2C 0x34 bottom left, 0x35 top left, 0x36 bottom right, 0x37 top right (landscape, camera up; bottom pair "confirm by ear" in UBU) | DT, UBU | verified (DT) |
| Amplifier bus | `i2c@980000`, the I2C hub's SE0 (`qcom,geni-i2c-master-hub`, wrapper `geniqup@9c0000`), 400 kHz, only these four devices; pins gpio64/gpio65 `i2chub0_se0`, 2 mA, pull-up | DT | verified (DT); state at hand-over: needs-boot (level 1) |
| Amplifier lines | reset (active low): 0x34 gpio128, 0x35 gpio129, 0x36 gpio107, 0x37 gpio108; interrupt (level low): gpio66, 84, 81, 67; no supply in the DT | DT | verified (DT); levels: needs-boot (level 1) |
| Amplifier identity | DEVID (reg 0x03) 0x2900 on all four; STATUS (0x00) bits BOVDS, PLLS, OTPDS, OVDS, UVDS, OCDS, CLKS, OTWDS, BOPS, BOLDS, INDS, VDDPS; gain 0.375 dB steps, 255 max | UBU | verified (source) |
| Amplifier preset | `foursemi/piano/fs19xx.fsm`, 4348 bytes, from the stock **odm_a** partition: per-amplifier register tables (TDM slot registers 0x1a/0x1c, 0xbd/0xbe, scene masks), CRC16; the same file on every unit of the build, no per-unit calibration (the stock driver reports re25 0) | `firmware/piano-audio.json` | verified (metadata); read at run time only, never committed |
| Speaker link | LPAIF **secondary TDM** (`SECONDARY_TDM_RX_0` = q6apm-lpass-dais dai 40): 48 kHz, 4 x 32-bit slots, mask 0xf, bit clock 6.144 MHz from the SoC (internal sync), long sync, data delay 1; slot 0 TL, 1 TR, 2 BL, 3 BR; the DSP's TDM sink module 0x0700100E on LPAIF interface 1; bit clock = q6prm clock 0x25 (the DT's `bclk` of dai@28) | DT, UBU | verified (DT, source); the sync mode is UBU's one guess |
| TDM pins | main TLMM gpio121 `i2s1_sck`, 122 `i2s1_data0`, 123 `i2s1_ws`, 124 `i2s1_data1`, 8 mA (the sound node's pinctrl) | DT | verified (DT); mux at hand-over: needs-boot (level 1) |
| SoundWire / WSA / RX | WSA macro `0x6b00000` + SoundWire `0x6b10000`, WSA2 `0x6aa0000` + `0x6ab0000`, RX macro `0x6ac0000` + `0x6ad0000`, TX SoundWire `0x7630000`: all `disabled`. **No SoundWire and no WSA88xx on piano's speaker path** (Project-SunUEFI's early WSA884x guess was replaced by FS19xx on TDM) | DT, SUN | verified |
| Microphones | DMICs on the **VA macro** `codec@7660000` (okay); four DMIC pin pairs on LPASS LPI pins gpio6/7, 8/9, 12/13, 17/18, 4 mA; `qcom,dmic-sample-rate` 2.4 MHz (divider 4); capture `VA_CODEC_DMA_TX_0` (dai 110), 2 ch 48 kHz; UCM: DEC0 <- DMIC0 (side top), DEC1 <- DMIC2 (side bottom), gain 124 (+40 dB); DMIC4 top, DMIC6 back unused. TX macro `0x6ae0000` okay (Linux's aux device) | DT, UBU | verified (DT); four mics: inferred (stock ODM mixer overlay via UBU) |
| LPASS clocks and power | no LPASS clock controller for the application processor in the DT: every macro clock is a **q6prm** clock of the DSP (macro vote 0x66, dcodec vote 0x67, mclks 0x39 TX/VA, 0x40 RX, 0x42 WSA, 0x44 WSA2); the DSP's domains lcx/lmx; LPASS NoCs ag/lpiaon/lpicx. PRM's own clock IDs differ from the DT indices (SHENG: TX core 0x30c, VA core 0x307 on SM8550) | DT, SHENG | verified (DT); piano's PRM IDs: needs-boot (level 3) |
| DSP services | `remoteproc@6800000` (PianoPower boots it); GLINK edge `lpass` (pid 2); `gpr` on channel `adsp_apps`, domain 2; services 1 q6apm (protection domain `avs/audio` in `msm/adsp/audio_pd`), 2 q6prm | DT | verified (DT) |
| Audio PD natively | with PianoPower level 2 the DSP **opened `adsp_apps`** (nobody serves it yet) next to glink_ssr, pcie_drv, mhi_sat, LOOPBACK_CTL_LPASS, fastrpcglink-apps-dsp; the locator answered tms/servreg, pddump, pdr; `msm/adsp/audio_pd` -> `avs/audio` is in PianoQrtr.c's domain table | RUN line 1173 | **verified (hardware)** |
| DMA | the q6apm dais' SMMU streams `0x1001` (mask 0x80) and `0x1041` (mask 0x20); SHENG: the DSP takes a **32-bit** address with the stream index (1) in bits 32 and up, the audio streams stay in SMMU pass-through, their context banks are the hypervisor's (programming one reset the board), buffers in low DRAM mapped write-combined. Piano's XNU window is 0x8c0000000 and up: no kernel allocation is DSP-addressable | DT, SHENG, `el1-hyp-notes.md` | verified (DT) / inferred (SHENG); entries: needs-boot (level 1) |
| USB-C audio | Type-C mux DIOO DIO4485 (`dioo,dio4485` 0x42 on `i2c@a8c000`, mode + orientation switch; Android's DT names fsa4480); the stock overlay's symbols name a WCD939x reset and `wcd_usbss`, the running DT has no WCD codec; Ubuntu offers no wired audio | DT, INV | verified (DT) / unknown (a WCD939x fitted?) |
| Bluetooth audio | WCN7860 Bluetooth (Ubuntu: audio works) | INV | later (a Bluetooth page) |

## 3. Ubuntu as the specification

Kernel `yzddmr6/linux-sm8750-piano` `piano/v1.1.0` (GPL-2.0, behaviour only): `sound/soc/qcom/qdsp6/q6apm.c`, `audioreach.c`
(graph open/prepare/start, the TDM interface configuration of module 0x0700100E, the WR_SH data commands), `q6apm-lpass-dais.c`,
`q6prm.c` / `q6prm-clocks.c`, `topology.c`; `drivers/soc/qcom/apr.c` (GPR); `sound/soc/codecs/lpass-va-macro.c` (DMIC clock,
decimators, the forced 150 Hz high-pass); the FS19xx codec driver (also Project-SunUEFI `0005-audio.patch`: preset parse, reset,
power-up order); the piano machine driver (TDM slots, `set_sysclk` of the bit clock). The order (ASoC's, inferred): DSP graph
prepared, bit clock on, amplifiers configured and powered up once the stream runs, powered down after (reset low on unbind).

Topology (BSD-3-Clause, `device/audio/Xiaomi-Pad-8-Pro.m4`, `card.json`): playback MultiMedia1 1-4 ch S16 48 kHz -> stream subgraph
0x4001 (WR_SH, PCM decoder, logger; **no DSP volume**, the upstream one reaches +18 dB) -> device subgraph 0x4005 (logger, MFC,
TDM sink, instance base 0x6050); capture `VA_CODEC_DMA_TX_0` device subgraph 0x4008 -> stream 0x4003 (2 ch). The one bug found on
piano: the WR_SH output port must be 1 (with 0 the DSP rejected GRAPH_OPEN with error 1). Gain lives in the amplifiers: UCM 192
(-23.6 dB), kernel cap 215 (-15 dB; `card.json` now records 231 after v1.1.0's "maximum volume increased"), unattended tests 160 (-35.6 dB) with tones of at most -20 dBFS for 3 s and a 5 s watchdog.
Status (v1.1.0 README): all four speakers play, internal microphones record; "no speaker protection; keep the volume cap".

## 4. UniXNU templates, and what can be borrowed

| Piece | Where | Licence | Use |
|---|---|---|---|
| GPR header codec, APM/PRM packet builders (graph open/prepare/start/stop/close, SET_CFG, MEM_MAP, WRITE/READ buffers, PRM clock and hardware votes), their host tests (182 checks) | UniXNU-sheng `package/ShengGpr.{h,c}`, `ShengAudioReach.{h,c}`, `tests/gpr_host.c`, `audioreach_host.c` (00ba909) | BSD-2-Clause-Patent | **levels 2-6, 9**, copied with UPSTREAM; the speaker device graph changes from I2S sink 0x0700100A to TDM sink 0x0700100E (+ MFC), the mic graph from codec DMA TX3 to VA TX0 |
| Bring-up order and the period engine: SMMU pass-through, pins, buses, amplifiers, `adsp_apps`, SPF ready, graphs, buffer map, four 10 ms periods in flight, answers copied on the DSP thread and served on its own | sheng `AudioProbe.cpp` (hardware-verified: tone 155/151 periods, capture 200/200, Voice Memos smooth) | BSD | design of levels 2-6; its CS35L43/WCD938x/MI2S specifics are not piano's |
| The running DSP and its client interface `piano_adsp_channel()` / `piano_adsp_send()` (sheng's `ShengAdsp.h` renamed: the same interface sheng's audio uses for `adsp_apps`) | `package/PianoPower` `PianoAdsp.h`, PianoQrtr.c (the audio PD's locator entry) | BSD (this repo) | level 2+: imported by name, **PianoPower unchanged** (§10.1) |
| GENI I2C master (FIFO), or the bit-banged I2C of the gauges / keyboard cover | sheng `GeniI2c.{h,c}`; PianoPower / PianoKeyboard soft I2C | BSD | level 4 (bus probe) |
| macOS device: `IOAudio2Device` + `IOAudio2DeviceUserClient` shape (registry description, status page and ring memory types, selectors 0-12, hog mode, per-device period engine), UIDs "Speaker"/"Mic" | macOS-DroidVM `DroidVMAudio.cpp` (UniXNU-droidvm e29d087, 477 lines; transport `DroidVMVirtioSnd.c` 382) | BSD-2-Clause-Patent | **level 7**: copied into PianoAudio with UPSTREAM (the native rule forbids linking a DroidVM kext, not adopting its code as this repo's package); virtio-snd replaced by the GPR transport |
| AudioServerPlugIn `ShengAudio.driver` + `ShengAudioService` (IOUserClient, shared rings, one timeline) | sheng `hal/ShengAudioPlugIn.c`, `ShengAudioService.cpp`, `ShengAudioAbi.h` | BSD | fallback of §5 |
| SM8750 GPR from XNU at EL1 under Gunyah: replies, WRITE_DONE 4100/4100, I2S on gpio121-124, 116 x 128-byte intents, GRAPH_OPEN 296 bytes | hackios-xnu-kernel-k90 `annibale_audio_reach.c` | APSL-2.0 | facts only (no sound confirmed there) |
| Module/parameter IDs, payload layouts (the TDM interface parameter) | linux-msm audioreach-topology (BSD-3-Clause; the Project-SunUEFI submodule is empty here), AudioReach engine headers (BSD-3-Clause-Clear) | BSD | pinned at level 5 |

Nothing in scope implements FS19xx, a TDM sink or the VA macro under a BSD licence: those are written fresh from §3.

## 5. IOKit family / ABI: the macOS side

Level 1 is a plain `IOService` on `IOPlatformExpertDevice` (`com.apple.kpi.*` only). Later: what does the HAL talk to?

**What the KC offers.** The base vma2 KC (`kc27.bin`, 26A434) has `com.apple.iokit.IOAudioFamily` 800.1 (25 KB executable),
`IOAudio2Family` (a 2.5 KB shell, a dependency of IOAVFamily), `AppleUSBAudio` (needs IOAudioFamily) and `AppleVirtIO` (with
`AppleVirtIOSound`). The native KC keeps 48 kexts: only `IOAudio2Family` (with IOAVFamily) of these; `AppleAudioClockLibs` and
`AppleIISController` are ARMPlatformStandIn's stand-ins. No AppleHDA, no `IOUserAudio`/AudioDriverKit support; `IOUserServer` exists.

| Option | For | Against | Verdict |
|---|---|---|---|
| A. IOAudioFamily driver (`IOAudioDevice`/`IOAudioEngine`, KDK headers) | documented kernel API; AppleUSBAudio would come along | not in the native KC (the keep list is the k27 tree's); whether this build's HAL still drives IOAudio engines is unshown | not now (revisit for USB-C audio) |
| B. **`IOAudio2Device` top half** (DroidVMAudio) under the stock HAL (`HALS_IOA2Manager` in audiomxd/coreaudiod, VirtualAudio's built-in Speaker/Mic classes) | **ran on this macOS 27** in the VM (sound, mic, 2 h soak), and natively it loaded and published Speaker/Mic (11-10/11 runs before the no-DroidVM rule); kernel only: no plugin bundle, no trust-cache entry, no HAL-directory overlay; host time is already 24 MHz natively (k27 0042), so none of sheng's host-clock patching | the user-client ABI is private (DroidVMAudio's notes), tied to this OS build; VirtualAudio's speaker class loads Apple's speaker DSP chain of another product (EQ/limiter, not protection for these speakers) | **recommended** |
| C. AudioServerPlugIn (sheng's) + IOUserClient rings | public plugin API; hardware-verified on iPadOS 27 | natively: the bundle in the Data seed and its CDHash in the trust cache (the GPU plugin's staging), sheng's iOS workarounds (Thunderbolt transport type, the host-clock constant) to re-check on macOS; more parts | fallback if B misbehaves |
| D. AudioDriverKit dext | Apple's current model | needs a dext runtime, sysextd approval and entitlements; no IOUserAudio kernel side found | no |

Under B, PianoAudio publishes "Speaker" (stereo out) and "Mic" (mono or stereo in), 48 kHz S16, as DroidVMAudio does. The kernel
maps stereo to the four 32-bit slots (L -> TL + BL, R -> TR + BR in landscape; rotation-aware mapping later), applies the volume
digitally, and owns every safety limit (§10.2) below whatever the HAL does.

## 6. Boot args

| Boot arg | Default | Effect |
|---|---|---|
| `piano-audio=N` | 0 (probe() declines, nothing mapped) | 1: the read-only survey; >= 2: not implemented, runs level 1 (planned: 2 DSP link, 3 PRM, 4 amplifiers, 5 graphs, 6 tone, 7 macOS device) |
| `piano-audio-smmu=0` | 1 | skip the SMMU stream scan (global page only) |
| `piano-audio-delay=S` | 0 | wait S seconds before the survey |
| `piano-audio-repeat=S`, `piano-audio-samples=N` | 0, 3 | resample the pins every S seconds, N times (one summary line each) |

Levels >= 2 will need `piano-power=2` in the same boot (PianoPower starts the DSP; PianoAudio never starts or stops it).

## 7. Build, kernel collection

`tooling/k27/build-native-piano-audio.sh` (host tests `package/PianoAudio/tests/audio_host.cpp`, the objects in
`out/native-kexts/PianoAudio`, the import check: 308 kernel imports, 0 unresolved), descriptor `tooling/k27/kext/PianoAudio.json`
(`com.unixnu.driver.PianoAudio`), listed in `configs/k27-native-kexts.txt` after PianoPower (so level 2 can import its client
interface by name), so `tooling/k27/build-native-image.sh` links it into every native KC (inert).

## 8. Bring-up ladder, test plan and effort

RAM boots only, cable to the Mac, owner present, `piano-reset-after` as the backstop. Each level adds writes only after the previous
level's log was read. On SM8750 the LPASS clocks are DSP (PRM) resources, so the DSP link comes before the clocks; the speaker
"link" is TDM, which only clocks while the device graph runs, so link and graph open are one level. Effort = agent work + owner boots.

| Level | What | Pass | Effort |
|---|---|---|---|
| 1 (built) | survey: TLMM (bus, resets, interrupts, TDM pins), SMMU 0x1001/0x1041 | the log below; nothing written | done; 1 boot |
| 2 DSP link | with `piano-power=2`: register `adsp_apps` (answer the DSP's open), GPR on sheng's codec, `APM_CMD_GET_SPF_STATE` | "APM ready" (reply status 1); no other command | 2-3 days; 1-2 boots |
| 3 LPASS clocks/power | PRM: macro + dcodec votes, VA core mclk 19.2 MHz (piano's PRM IDs), the TDM bit clock 6.144 MHz; then read the VA/TX macro and LPI version registers (now clocked) | PRM answers 0; plausible block IDs; release answers 0 | 2-4 days; 1-2 boots |
| 4 amplifiers | bus pins as level 1 found them (mux to the hub or bit-banged), resets released, DEVID of 0x34-0x37; then the preset (from the owner's own odm_a copy, §10.3) with PWDN kept set | DEVID 0x2900 x 4, STATUS consistent, amplifiers still powered down | 1-2 weeks; 2-4 boots |
| 5 TDM link + graphs | TDM pins muxed (`sec-tdm-default-state`), device subgraph 0x4005 (logger, MFC, TDM sink 0x0700100E, intf 1, 4 x 32, mask 0xf, long sync, delay 1) and stream subgraph 0x4001 (WR_SH port 1, PCM decoder), open/prepare/start with silence | every command answers 0; the amplifiers' PLLS/CLKS bits report a clock | 1 week; 2-3 boots |
| 6 test tone | a low-DRAM period buffer the DSP can address (§10.4), MEM_MAP, WRITE periods; 1 kHz, -20 dBFS, 3 s, amplifier gain 160, PWDN cleared only while it plays, 5 s watchdog | WRITE_DONE for every period, STATUS equal on all four, audible on all four (owner, slot order by ear) | 3-5 days; 2-3 boots |
| 7 macOS device | §5 B: DroidVMAudio's IOAudio2 top half on the GPR transport, Speaker device | Sound settings lists it; `say` and `afplay` play | 1 week; 2-3 boots |
| 8 volume/mute | HAL volume -> kernel digital gain (<= 0 dB) at fixed amplifier gain <= 192; mute = zeros, then PWDN after 1 s; DC blocker; fade in/out | volume keys work; no click at start/stop; the caps hold at maximum | 3-5 days; 1-2 boots |
| 9 microphones | PRM votes for the VA macro, LPI DMIC pins (DMIC0/DMIC2), VA macro DMIC clock divider 4, DEC0/DEC1, gain, high-pass; capture graph (codec DMA source 0x07001024 on VA TX0, RD_SH); the first ~75 frames of a start transient dropped; macOS Mic device | `tools/mic-capture-check.py` criteria (live, not clipped, >= -60 dBFS, non-flat) on a recording; Voice Memos | 1-2 weeks; 3-4 boots |
| 10 USB-C / Bluetooth | wired: USB host mode + a USB audio class path (option A's family) or the analog switch if a codec exists; Bluetooth with its own page | later | - |

Totals: about 6-9 weeks of agent work and 15-25 owner boots to speakers, volume and microphones in macOS.

### 8.1 Level 1 test image

```sh
PIANO_EXTRA_ARGS="piano-usb=2 piano-audio=1" PIANO_RESET_AFTER=300 tooling/k27/build-native-image.sh   # out/ram/boot-piano-macos-ram-A-bundled.img
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json                                 # PASS
```

Read the lines live with `tooling/native/usb-console.py` (`piano-usb=2`), or `grep PianoAudio` in the run's `xnu-console.txt`.
Adding `piano-power=2 piano-power-write=0` also shows the DSP opening `adsp_apps` again (PianoPower's "open:" line). Expected (the
likely hand-over: nothing has touched audio since reset):

```
PianoAudio: start: piano-audio=1 smmu=1 delay=0 repeat=0 samples=3 (4 x FS19xx on i2c hub 0 0x980000, secondary TDM on gpio121-124)
PianoAudio: level 1: loads only, nothing written
PianoAudio: stage 1: TLMM: the amplifiers' bus, reset and interrupt pins, the TDM pins (read only)
PianoAudio: pin hub0 SDA gpio64: cfg ........ io ........: function N pull up 2 mA output disabled, out 0 in 1   (14 pin lines)
PianoAudio: amp Top Left 0x35 slot 0: reset gpio129 held in shutdown (driven low), function 0, reads 0; irq gpio84 GPIO, reads 1 (active low)   (4 amp lines)
PianoAudio: bus i2c hub 0 (i2c@980000): SDA gpio64 function N, SCL gpio65 function N: both on an alternate function (the hub's), pull-ups on, idle (both high)
PianoAudio: TDM (LPAIF secondary, i2s1): sck 0 data0 0 ws 0 data1 0: all four on GPIO (not muxed: a later level sets sec-tdm-default-state)
PianoAudio: stage 2: SMMU streams 0x1001/0x1041, the DSP's PCM buffers (read only)
PianoAudio: SMMU idr0 ........ idr1 60000053: N stream-match groups, 83 banks, 128 pages
PianoAudio: SMMU stream 0x1001: no stream-match entry (the firmware left none)   (or: group G smr ........ (id 0x1001 mask 0x80) s2cr ........ (bypass, bank B))
PianoAudio: SMMU stream 0x1041: ...
PianoAudio: stage 3: the LPASS blocks: not read (their clocks are the DSP's; nothing here says whether they run)
PianoAudio: LPASS not read: WSA2 macro 0x6aa0000 (DT disabled), WSA2 SoundWire 0x6ab0000 (DT disabled), ..., LPI TLMM 0x7760000 (DT okay)
PianoAudio: summary: resets(TL,TR,BL,BR) HHHH irq 1111 bus hub/idle tdm-muxed 0/4 smmu 0x1001 none 0x1041 none
PianoAudio: verdict: all four amplifiers are held in shutdown (reset driven low): level 2 must release them (reset high) before a bus probe
PianoAudio: stage 4: done
PianoAudio: done: N MMIO loads, 0 stores; 15 pages mapped, 0 map failures
```

Reset letters: H held (driven low), R released (driven high), U undriven (input), O other function. Read from it: who holds the
amplifiers in reset and where (the verdict line), whether the hub pins are muxed with pull-ups and idle (then level 4 can use the
hub's engine; otherwise bit-banged I2C), the i2s1 function number if any boot stage set it (else level 5 takes it from the SoC's
pinctrl table), and what the firmware left for streams 0x1001/0x1041 (pass-through bypass as on sheng, a hypervisor bank, or
nothing: this decides level 6's addressing). A hang right after "stage 2" means the SMMU global page is not readable from EL1:
rerun with `piano-audio-smmu=0` (the TLMM part reads the same pages PianoPower reads on every boot).

## 9. Fallback

`piano-audio=0` (the default): nothing runs, macOS has no audio device (as today); the HAL and audiomxd idle. Sound meanwhile:
Ubuntu (reboot), or later AirPlay/Bluetooth from macOS.

## 10. Open questions and risks

1. **Sharing the DSP with the charger firmware.** One DSP image hosts `charger_pd` and `audio_pd`; PianoPower owns the DSP's life
   (firmware, PAS, SMP2P, GLINK, QRTR, the locator). What audio reuses: the running DSP, the GLINK edge and PianoPower's client
   interface (4 client slots, 2 used by IPCRTR and PMIC_RTR_ADSP_APPS; 6 GLINK channels; messages up to 560 bytes, sheng's GPR
   packets stay within 512; callbacks on the DSP thread, which must not block). What it must not do: start, stop or restart the
   DSP, vote its rails (the DSP holds its own after handover), or touch RPMh. Risk: a malformed graph that crashes the DSP takes
   the charger path down with it and nothing restarts it natively (no SSR): charging stops until reboot (and the PMIC is not reset by a
   warm reset, `charging.md` §6). So: host-tested packets only, one command at a time with its answer checked, stop
   at the first error or SMP2P fatal bit, first runs with `piano-power-write=0` on a charged battery. To agree with PianoPower's
   owner before level 2: importing `piano_adsp_channel`/`piano_adsp_send` by name, and a DSP-down notice beyond the channel's
   state callback.
2. **Speaker damage.** No speaker protection runs (stock runs one on the TDM feedback; Ubuntu caps instead). Hard rules for every
   level: amplifier gain a compile-time cap of 192 (Ubuntu's UCM; never above Ubuntu's 215), tests at 160 with tones <= -20 dBFS
   and <= 3 s, a 5 s watchdog that zeroes the stream, stops the graph and sets PWDN (then reset low); start muted (PWDN until the
   first period, fade in), zeros and PWDN on stop or on any DSP error or late period; a DC blocker (high-pass) before the DSP and
   a digital clamp; no DSP volume module (the upstream one reaches +18 dB). The HAL's Apple speaker chain (option B) is not
   trusted as a limiter. Heat: no thermal driver; sessions attended.
3. **The amplifier preset.** `fs19xx.fsm` (TDM slot registers, per-speaker tables) is the stock file of odm_a: read at run time
   from the owner's own tablet, never committed, never disassembled (it is register data; its container format is the open
   driver's specification). odm_a is an Android filesystem image, not FAT: either a read-only reader for it, or the installer
   copies the owner's own file onto the ESP (FAT, readable through NativeUfs + FatVolume, as PianoPower's firmware). Check CRC16 and
   the size before use; a different stock build may carry a different file. (level 4)
4. **A DSP-addressable buffer.** The DSP takes 32-bit addresses (SHENG, K90); XNU's memory is above 4 GiB on piano. Candidates:
   low DRAM nobody uses after ExitBootServices (UEFI's DXE heap or "Kernel" region, as PianoPower's carveout overlap shows such
   ranges exist), or a translating SMMU bank for 0x1001 (on sheng that reset the board: the hypervisor's banks). Level 1 shows the
   firmware's entries; level 6 decides. (needs-boot)
5. **PRM clock IDs and the TDM parameter.** Piano's PRM IDs (sheng's SM8550 values differ from the DT indices) and the TDM sink's
   interface parameter layout come from the BSD AudioReach headers, checked against Ubuntu's topology tokens 262-269. (level 3, 5)
6. The i2s1 function number of gpio121-124 and whether the I2C hub's engine (its GCC clocks are not in this repo yet) or a
   bit-banged bus serves level 4. (level 1 reads the pins; source read)
7. The DMIC supply (no regulator in the VA macro's DT node) and the 150 Hz high-pass / start transient (SunUEFI's notes). (level 9)
8. Option B's private ABI: pinned to this macOS 27 build; an OS update means re-checking DroidVMAudio's selectors, or option C.
9. USB-C wired audio: whether a WCD939x is fitted (stock symbols say maybe, the DT says no) and USB host mode first. (later)

## 11. Licence note

This repository's packages are BSD-2-Clause-Patent. Linux (q6apm, audioreach, gpr, q6prm, lpass-va-macro, the FS19xx driver, the
piano machine driver) and Project-SunUEFI's kernel patches are GPL-2.0: specification only (behaviour, IDs and register positions
as facts); no code, comment or table is copied. hackios-xnu-kernel-k90 is APSL-2.0: facts only. UniXNU-sheng and
UniXNU-droidvm (DroidVMAudio) are BSD-2-Clause-Patent and are reused with `UPSTREAM` records; AudioReach's topology macros and
engine headers (BSD-3-Clause / BSD-3-Clause-Clear) supply IDs and layouts with attribution. No Qualcomm or FourSemi binary is
reverse engineered. The DSP firmware, the amplifier preset and any topology or ACDB file are data read at run time from the
owner's own tablet (or a staged copy under the ignored `out/`), never committed.
