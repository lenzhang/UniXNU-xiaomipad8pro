# Audio: four FS19xx speakers on TDM, DMICs on the VA macro, through the ADSP's AudioReach: PianoAudio

Status, 2026-11-11 (evening): `package/PianoAudio` levels 1-3 are built, host-tested and linked into the native KC (kernel 0059:
"20 native kexts linked, none from DroidVM", PianoAudio 1081 pointers, PianoPower's client functions resolved by name), inert
without `piano-audio`; the RAM image verifies PASS; **nothing of it has run on hardware**. Level 1 is the read-only survey, level 2
the DSP link (one readiness query through PianoPower's client contract), level 3 the macOS "Speaker" and "Mic" devices on a null
transport (silence, no hardware). The pieces of levels 4-10 (the FS19xx core, piano's AudioReach graphs with their sequencer, the
period pump, the speaker-safety chain) are written and host-tested but not called: §7.1. The ladder was renumbered for this (§8).
Template: `README.md`.

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
| LPASS clocks and power | no LPASS clock controller for the application processor in the DT: every macro clock is a **q6prm** clock of the DSP (macro vote 0x66, dcodec vote 0x67, mclks 0x39 TX/VA, 0x40 RX, 0x42 WSA, 0x44 WSA2); the DSP's domains lcx/lmx; LPASS NoCs ag/lpiaon/lpicx. PRM's own clock IDs differ from the DT indices (SHENG: TX core 0x30c, VA core 0x307 on SM8550) | DT, SHENG | verified (DT); piano's PRM IDs: needs-boot (level 4) |
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
| GPR header codec, APM/PRM packet builders (graph open/prepare/start/stop/close, SET_CFG, MEM_MAP, WRITE/READ buffers, PRM clock and hardware votes), their host tests (182 checks) | UniXNU-sheng `package/ShengGpr.{h,c}`, `ShengAudioReach.{h,c}`, `tests/gpr_host.c`, `audioreach_host.c` (00ba909) | BSD-2-Clause-Patent | **levels 2-8, 10**, copied with UPSTREAM (done: `PianoGpr`, `PianoAudioReach`); the speaker device graph changed from I2S sink 0x0700100A to TDM sink 0x0700100E (+ MFC, loggers), the mic graph from codec DMA TX3 to VA TX0 |
| Bring-up order and the period engine: SMMU pass-through, pins, buses, amplifiers, `adsp_apps`, SPF ready, graphs, buffer map, four 10 ms periods in flight, answers copied on the DSP thread and served on its own | sheng `AudioProbe.cpp` (hardware-verified: tone 155/151 periods, capture 200/200, Voice Memos smooth) | BSD | design of levels 2-8 (`AudioSequence`'s plan and pump follow it); its CS35L43/WCD938x/MI2S specifics are not piano's |
| The running DSP and its client interface `piano_adsp_channel()` / `piano_adsp_send()` (sheng's `ShengAdsp.h` renamed: the same interface sheng's audio uses for `adsp_apps`) | `package/PianoPower` `PianoAdsp.h`, PianoQrtr.c (the audio PD's locator entry) | BSD (this repo) | level 2+: now `package/include/PianoAdspContract.h`, imported by name, PianoPower not edited by audio (§10.1) |
| GENI I2C master (FIFO), or the bit-banged I2C of the gauges / keyboard cover | sheng `GeniI2c.{h,c}`; PianoPower / PianoKeyboard soft I2C | BSD | level 5 (bus probe; `Fs19xxBus` takes either) |
| macOS device: `IOAudio2Device` + `IOAudio2DeviceUserClient` shape (registry description, status page and ring memory types, selectors 0-12, hog mode, per-device period engine), UIDs "Speaker"/"Mic" | macOS-DroidVM `DroidVMAudio.cpp` (UniXNU-droidvm e29d087, 477 lines; transport `DroidVMVirtioSnd.c` 382) | BSD-2-Clause-Patent | **level 3** (done, `PianoAudioDevice.cpp`, UPSTREAM; the native rule forbids linking a DroidVM kext, not adopting its code as this repo's package); virtio-snd replaced by `AudioPcm.h` (null now, the DSP at level 8) |
| AudioServerPlugIn `ShengAudio.driver` + `ShengAudioService` (IOUserClient, shared rings, one timeline) | sheng `hal/ShengAudioPlugIn.c`, `ShengAudioService.cpp`, `ShengAudioAbi.h` | BSD | fallback of §5 |
| SM8750 GPR from XNU at EL1 under Gunyah: replies, WRITE_DONE 4100/4100, I2S on gpio121-124, 116 x 128-byte intents, GRAPH_OPEN 296 bytes | hackios-xnu-kernel-k90 `annibale_audio_reach.c` | APSL-2.0 | facts only (no sound confirmed there) |
| Module/parameter IDs, payload layouts (the TDM interface parameter) | linux-msm audioreach-topology (BSD-3-Clause; the Project-SunUEFI submodule is empty here), AudioReach engine headers (BSD-3-Clause-Clear) | BSD | the topology's ids are pinned in `PianoAudioReach.h`; the TDM parameter's layout from Ubuntu's kernel (specification) |

Nothing in scope implements FS19xx, a TDM sink or the VA macro under a BSD licence: those are written fresh from §3.

Taken over on 2026-11-11 (records in `package/PianoAudio/UPSTREAM`): sheng's GPR codec unchanged (`PianoGpr.{h,c}`), sheng's
AudioReach writer with piano's graphs (`PianoAudioReach.{h,c}`), DroidVMAudio's IOAudio2 top half under this repository's names
(`PianoAudioDevice.cpp`); PianoPower's client interface is now the contract `package/include/PianoAdspContract.h` (charging.md §9).

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

**Built (level 3).** `PianoAudioDevice.cpp` is DroidVMAudio's top half: `PianoAudioDevice : IOService` and `PianoAudioUserClient :
IOUserClient`, their metaclasses linked at run time under `IOAudio2Device` / `IOAudio2DeviceUserClient` (the family's subclass
contract is closed and its names are taken), one IOResources nub per device ("piano-audio-out", "piano-audio-in"), published once
IOBSD is up. Below it, `AudioPcm.h`'s transport interface replaces virtio-snd: `running(output, input)`, `play(stereo, frames)`,
`capture(mono, frames)` and the transport's own latency, called from each device's engine thread one 5 ms period at a time. Today
only the null transport exists (it takes every period and gives silence); level 8's DSP transport is `audio_output_period` (§7.1)
into the ring the period pump drains. The KC builder's vtable check covers subclasses of a kept kext's class; these derive from
kernel classes (compiled against the same headers), so `tooling/k27/check-audio2-family.py` checks instead what the run-time link
relies on, in the KC base: IOAudio2Family is kept, defines both classes, and their `superClass` pointers are exactly
`IOService::gMetaClass` and `IOUserClient::gMetaClass` (chained fixups of 26A434's `kc27.bin`). It passes.

## 6. Boot args

| Boot arg | Default | Effect |
|---|---|---|
| `piano-audio=N` | 0 (probe() declines, nothing mapped) | 1: the read-only survey; 2: also the DSP link (§8.2); 3: also the macOS devices on the null transport (§8.3); >= 4: not implemented, runs 1-3 |
| `piano-audio-smmu=0` | 1 | skip the SMMU stream scan (global page only) |
| `piano-audio-delay=S` | 0 | wait S seconds before the survey |
| `piano-audio-repeat=S`, `piano-audio-samples=N` | 0, 3 | resample the pins every S seconds, N times (one summary line each) |
| `piano-audio-dsp=0` | 1 | level >= 2 without the DSP link (e.g. to look at the macOS devices alone) |
| `piano-audio-dsp-wait=S` | 300 | give the DSP link up after S seconds without an answer |
| `piano-audio-settle=S` | 20 | the charger domain up this long (in its current generation) before audio's first packet |
| `piano-audio-device=0` | 1 | level >= 3 without the macOS devices |

The DSP link needs `piano-power=2` in the same boot (PianoPower starts the DSP; PianoAudio never starts or stops it): without it the
level logs "the DSP does not run in this boot" and registers nothing. The macOS devices do not need the DSP.

## 7. Build, kernel collection

`tooling/k27/build-native-piano-audio.sh`: the seven host tests (below), the objects in `out/native-kexts/PianoAudio` (12), the
import check (381 kernel imports, 3 library imports from PianoPower: `piano_adsp_channel`, `piano_adsp_send`, `piano_adsp_health`;
0 unresolved) and `check-audio2-family.py` (§5). Descriptor `tooling/k27/kext/PianoAudio.json` (`com.unixnu.driver.PianoAudio`),
which names `com.unixnu.driver.PianoPower` among its libraries; listed in `configs/k27-native-kexts.txt` after PianoPower, so the
KC resolves the contract's functions by name to PianoPower's copy; `tooling/k27/build-native-image.sh` links it into every native
KC (inert). Measured 2026-11-11: `PIANO_A830_METAL=0 tooling/k27/build-native-image.sh` links 20 native kexts (with PianoSensors, the other contract client, and PianoBluetooth), none from DroidVM,
PianoAudio 1081 pointers; `verify-ram-images.py` PASS.

### 7.1 The code (host-tested; levels 1-3 run it on the tablet, the rest waits for its level)

| Piece | Files | What it does | Host test (checks) |
|---|---|---|---|
| Level 1 survey | `AudioSurvey.h`, `AudioHw.h` | §8.1 | `audio_host.cpp` |
| GPR codec | `PianoGpr.{h,c}` (sheng's) | the 24-byte header, decode with its bounds, basic results; the APM readiness probe (one query in flight, retries, give-up) | `gpr_host.c` (37) |
| AudioReach packets | `PianoAudioReach.{h,c}` | GRAPH_OPEN of the four piano subgraphs (§3: device 0x4005 LOG 0x6050 -> MFC 0x6051 -> TDM 0x6052; stream 0x4001 WR_SH 0x6001 (out port 1) -> DEC -> CNV -> LOG 0x6004 -> 0x6050; mic 0x4008 codec DMA 0x6080 (VA, TX0) -> LOG 0x6081; capture 0x4003 LOG -> MFC -> CNV -> ENC -> RD_SH 0x6024); SET_CFG of the loggers (0x19ab/0x19af), the MFC (48 kHz, 32 bit, FL FR LS RS), the TDM sink (`PARAM_ID_TDM_INTF_CFG` 0x0800101B: LPAIF 0, interface 1, internal sync, data out, mask 0xf, 4 x 32, sync mode 1, delay 1; HW endpoint 48000/32/4; frame size factor 1), the decoder/converter, the WR_SH media format; PREPARE/START/STOP/CLOSE; MEM_MAP/UNMAP; WRITE/READ V2; PRM clock (SEC_TDM_IBIT 0x202 at 6.144 MHz) and votes; the DSP address (32-bit physical with the stream index above; XNU memory refused); the answer parser. Packet sizes 68-504 bytes (capture GRAPH_OPEN 504, stream 456, device 392): all within 512 | `audioreach_host.c` (276; GRAPH_START and the PRM clock request byte for byte) |
| Sequencer | `AudioSequence.{h,c}` | §10.1's rule as code: one command at a time, each answer checked (port, domain, token, opcode, status, handle), the first error unwinds what was done (and a step that may have half run) last first, without the DSP only the local undos; piano's speaker plan (clock, device graph, stream graph, map, prepare x2, start x2, amplifiers on; stop = its exact reverse, amplifiers first while the bit clock runs); the period pump (N periods in flight, error statuses, unknown slots, late periods) | `sequence_host.c` (1796: an error, a timeout and an abort at every step against a DSP model) |
| DSP link | `AudioDspLink.{h,c}`, `AudioDsp.cpp` | the guard on PianoPower's health (§10.1), the receive inbox, level 2's exchange | `dsplink_host.c` (386) |
| FS19xx | `Fs19xx.{h,c}`, `AudioSha256.{h,c}` | 8-bit register / 16-bit big-endian I/O through the owner's bus; probe (DEVID polled 100 ms, 0x29xx only); the `fs19xx.fsm` container (size, CRC16, per-address blocks, lists) without copying it; the units (whole, field, delay capped 100 ms, wait) through the safety filter (PWDN and the DAC mute kept, gain <= 192); configure (soft reset, coefficient, common table, music scene, shut down); start (48 kHz / 6.144 MHz only, the capped gain before the unmute, shut down again on a bus error); mute; shutdown (every write tried); status reads and the group check that stops a stream; the stock preset's published size and SHA-256 | `fs19xx_host.c` (287: a four-amplifier register model flags any write that powers up, unmutes or exceeds the cap outside start; a hostile synthetic preset) |
| Safety chain | `AudioSafety.{h,c}` | stereo/mono -> FL FR LS RS; DC blocker (-3 dB near 38 Hz); gain <= 0 dB; fade-in from silence at every start, fade-out to silence; clamp at a ceiling <= -1 dBFS (compile time), -20 dBFS by default; the 1 kHz test tone (<= -20 dBFS, <= 3 s, 20 ms ramps); the stream guard (DSP error, late period, amplifier status, 5 s watchdog); the PCM ring | `pcm_host.c` (no random full-scale input crosses any ceiling) |
| macOS devices | `PianoAudioDevice.cpp`, `AudioPcm.{h,c}` | §5 | KC link + `check-audio2-family.py` |

## 8. Bring-up ladder, test plan and effort

RAM boots only, cable to the Mac, owner present, `piano-reset-after` as the backstop. Each level adds writes only after the previous
level's log was read. On SM8750 the LPASS clocks are DSP (PRM) resources, so the DSP link comes before the clocks; the speaker
"link" is TDM, which only clocks while the device graph runs, so link and graph open are one level. Effort = agent work + owner boots.

Renumbered 2026-11-11: the macOS devices moved down to level 3 on a null transport, so the HAL side is tested before any audio
write to the hardware; the hardware levels moved up by one (old 3-6 are now 4-7, old 7 is 8 = the devices on the DSP transport).

| Level | What | Pass | Effort |
|---|---|---|---|
| 1 (built) | survey: TLMM (bus, resets, interrupts, TDM pins), SMMU 0x1001/0x1041 | §8.1's log; nothing written | done; 1 boot |
| 2 (built) DSP link | with `piano-power=2`: `adsp_apps` registered (answering the DSP's open), PianoPower's health watched, `APM_CMD_GET_SPF_STATE` (§8.2) | "dsp: APM ready (status 1)"; no other command; charging unaffected | done (host); 1-2 boots |
| 3 (built) macOS devices | "Speaker" and "Mic" IOAudio2Devices on the null transport (§8.3) | the HAL lists both; `say`/`afplay` run to the end (silently); the engines' counters move | done (host); 1 boot |
| 4 LPASS clocks/power | PRM through the sequencer: the TDM bit clock 6.144 MHz (`AR_CLK_SEC_TDM_IBIT` 0x202) requested and released; then macro + dcodec votes and the VA core mclk (piano's PRM IDs) and the VA/TX macro and LPI version registers (now clocked) | PRM answers 0 for request and release; plausible block IDs | packets done; 2-4 days; 1-2 boots |
| 5 amplifiers | bus pins as level 1 found them (the hub's GENI engine or bit-banged), resets released, `fs19xx_probe` x 4; the preset (the owner's own odm_a copy, §10.3: `audio_preset_is_stock`, `fsm_find`), `fs19xx_configure` x 4 (powered down) | DEVID 0x2900 x 4, `fsm_check` OK, filtered 0, STATUS consistent, PWDN set on all four | core done; bus + loader 1 week; 2-4 boots |
| 6 TDM link + graphs | TDM pins muxed (`sec-tdm-default-state`), the speaker plan (`ar_speaker_steps`) with `amps_on` a no-op (amplifiers stay down) and a silent buffer | every command answers 0; the unwind answers 0 too; the amplifiers' PLLS/CLKS bits move while the device graph runs | packets + sequencer done; 1 week; 2-3 boots |
| 7 test tone | a DSP-addressable period buffer (§10.4), the plan with `amps_on` = `fs19xx_start(.., 160, muted)` then unmute after 4 silent periods; `audio_tone` (1 kHz, -20 dBFS, 3 s) through the chain and the pump; the guard (5 s watchdog, `fs19xx_group_check` every 100 ms) | WRITE_DONE for every period, STATUS equal on all four, audible on all four (owner, slot order by ear), PWDN after | pieces done; 3-5 days; 2-3 boots |
| 8 macOS device on the DSP | §5 B with the DSP transport: `audio_output_period` into the ring, the pump draining it, the plan on the transport's `running()` | `say` and `afplay` play | 1 week; 2-3 boots |
| 9 volume/mute | HAL volume -> `audio_chain_set_gain` (<= 0 dB) at fixed amplifier gain <= 192; mute = fade-out, zeros, then PWDN after 1 s; the ceiling raised from -20 dBFS (at most -1 dBFS) | volume keys work; no click at start/stop; the caps hold at maximum | 3-5 days; 1-2 boots |
| 10 microphones | PRM votes for the VA macro, LPI DMIC pins (DMIC0/DMIC2), VA macro DMIC clock divider 4, DEC0/DEC1, gain, high-pass; capture graphs (packets done: VA TX0 codec DMA, five-module stream); the first ~75 frames of a start transient dropped; the Mic device on the DSP transport | `tools/mic-capture-check.py` criteria (live, not clipped, >= -60 dBFS, non-flat) on a recording; Voice Memos | 1-2 weeks; 3-4 boots |
| 11 USB-C / Bluetooth | wired: USB host mode + a USB audio class path (option A's family) or the analog switch if a codec exists; Bluetooth with its own page | later | - |

Totals: about 5-8 weeks of agent work left and 15-25 owner boots to speakers, volume and microphones in macOS.

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
PianoAudio: verdict: all four amplifiers are held in shutdown (reset driven low): level 5 must release them (reset high) before a bus probe
PianoAudio: stage 4: survey done
PianoAudio: survey done: N MMIO loads, 0 stores; 15 pages mapped, 0 map failures
```

Reset letters: H held (driven low), R released (driven high), U undriven (input), O other function. Read from it: who holds the
amplifiers in reset and where (the verdict line), whether the hub pins are muxed with pull-ups and idle (then level 5 can use the
hub's engine; otherwise bit-banged I2C), the i2s1 function number if any boot stage set it (else level 6 takes it from the SoC's
pinctrl table), and what the firmware left for streams 0x1001/0x1041 (pass-through bypass as on sheng, a hypervisor bank, or
nothing: this decides level 7's addressing). A hang right after "stage 2" means the SMMU global page is not readable from EL1:
rerun with `piano-audio-smmu=0` (the TLMM part reads the same pages PianoPower reads on every boot).

### 8.2 Level 2: the DSP link (next tablet test)

Charged battery, cable attached, owner present; PianoPower's writes off for the first run:

```sh
PIANO_EXTRA_ARGS="piano-usb=2 piano-power=2 piano-power-write=0 piano-audio=2" PIANO_RESET_AFTER=600 tooling/k27/build-native-image.sh
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json                                 # PASS
```

Expected, after level 1's lines (times vary; the DSP opens `adsp_apps` early, PianoPower's GLINK keeps that open until we register):

```
PianoAudio: stage 5: the DSP link: adsp_apps, PianoPower's health, the APM readiness query (nothing else is sent)
PianoAudio: dsp: channel adsp_apps registered (20 x 512-byte intents; the DSP opens it, we answer)
PianoAudio: dsp: channel adsp_apps not up (PianoPower stage S, dsp_up 0, charger_pd 0 gen 0, link gen 0, quiet 0)   (until the DSP runs)
PianoAudio: dsp: guard wait
PianoAudio: dsp: channel adsp_apps up (PianoPower stage 20, dsp_up 1, charger_pd 0x1fffffff gen 1, link gen 0, quiet 0)
PianoAudio: dsp: guard run                                                            (20 s after the charger domain came up)
PianoAudio: dsp: APM_CMD_GET_SPF_STATE 1 sent (so far: 0 answers, last state 0, 0 refused, 0 timeouts)
PianoAudio: dsp: APM ready (status 1) after 1 queries, N ms after the first; inbox dropped 0
PianoAudio: stage 6: the DSP link: APM ready
```

Pass: "APM ready"; PianoPower's own lines unchanged against a run without `piano-audio` (charger readings, no `fatal`, link
generation 0, quiet raised and released as before). Read from a failure: "not ready after 10 queries" (the audio PD not up: the
locator's `msm/adsp/audio_pd` line in PianoPower's log), "no answer within 300 s" with the channel up (the APM does not answer port
1: the ports question of §10.10), "stopped for the rest of the boot: ..." (the reason and the crash string; then charging is the
first thing to check). `ioreg -l | grep piano-audio-dsp` shows the summary.

### 8.3 Level 3: the macOS devices on the null transport (next tablet test, independent of the DSP)

```sh
PIANO_EXTRA_ARGS="piano-usb=2 piano-audio=3 piano-audio-dsp=0" tooling/k27/build-native-image.sh   # or with level 2's arguments too
```

Expected: `PianoAudio: devices: Speaker and Mic as IOAudio2Devices on the null transport (silence: nothing reaches the hardware)`.
Then, in the booted system: `ioreg -l | grep -E '"device UID"|piano-audio-transport'` (Speaker, Mic, null); `system_profiler
SPAudioDataType` lists both; Sound settings offers "Speaker" as output and "Mic" as input; `say hello` and `afplay` of a short file
run to the end in their real time (silently); Voice Memos records silence. `piano_audio_dev_periods`/`_played` (kernel globals, in
the post-mortem) grow while something plays and stop with it. A failure line `none (stage -2: ...)` means IOAudio2Family's classes
were not found at run time (the static check passed, so the family kext did not load: `kextstat | grep IOAudio2`). Side benefit,
known from DroidVMAudio: with a default output device WebKit's media player works.

## 9. Fallback

`piano-audio=0` (the default): nothing runs, macOS has no audio device (as today); the HAL and audiomxd idle. Sound meanwhile:
Ubuntu (reboot), or later AirPlay/Bluetooth from macOS.

## 10. Open questions and risks

1. **Sharing the DSP with the charger firmware.** One DSP image hosts `charger_pd` and `audio_pd`; PianoPower owns the DSP's life
   (firmware, PAS, SMP2P, GLINK, QRTR, the locator). Risk: a malformed graph that crashes the DSP takes the charger path down with
   it and nothing restarts it natively (no SSR): charging stops until reboot (and the PMIC is not reset by a warm reset,
   `charging.md` §6). So: host-tested packets only, one command at a time with its answer checked, stop at the first error or
   SMP2P fatal bit, first runs with `piano-power-write=0` on a charged battery. What audio must not do: start, stop or restart the
   DSP, vote its rails, or touch RPMh, AOSS or PAS; no function of the contract offers it, and PianoAudio calls no other.
   **The PianoPower calls audio uses** (`package/include/PianoAdspContract.h`, on main since 2026-11-11; the descriptor names
   `com.unixnu.driver.PianoPower`, the KC resolves them by name):

   | Call | Where | How |
   |---|---|---|
   | `piano_adsp_channel("adsp_apps", 512, 20, 0, &client, ctx)` | level 2 (`AudioDsp.cpp`), once per boot | answering the DSP's open (it opens `adsp_apps` early; GLINK keeps that open for a late registration); 20 x 512-byte intents as the DT's `qcom,intents` |
   | `piano_adsp_send(ch, packet, n)` | every DSP command | one GPR packet (24-504 bytes, under `PIANO_ADSP_MAX_MESSAGE` 1056); -1 (queue full) is retried at the next tick, never waited out |
   | `PianoAdspClient.state` / `.receive` | PianoPower's DSP thread | `state` stores a flag; `receive` copies the packet into the inbox (16 x 512) and returns: no lock, no sleep |
   | `piano_adsp_health(&h)` | every 20 ms tick | the guard: wait until `dsp_up`, our channel up and `charger_pd` 0x1fffffff for 20 s in its current generation, hold while `quiet`; off for the boot at `fatal`, a new `fatal_reports`, a `crash` string, another `version`, and once started a new `link_generation` or `charger_generation` |

   Not needed by audio: the QRTR client ports (GPR rides its own GLINK channel), `piano_adsp_quiet()` (read through the health).
   Asked of nobody yet, shared with the sensors (`sensors.md` §10.1 D): one owner and one fixed range of DSP-addressable low memory
   (§10.4). A DSP-down event is the health's link generation; nothing else is needed from PianoPower for levels 2-10.
2. **Speaker damage.** No speaker protection runs (stock runs one on the TDM feedback; Ubuntu caps instead). The rules, now code
   (§7.1): amplifier gain a compile-time cap of 192 (`FS19XX_GAIN_CAP`, a build error above 215), the preset's own power, mute and
   gain writes filtered, tests at 160 with tones <= -20 dBFS and <= 3 s (`audio_tone_init` refuses more), a 5 s watchdog
   (`audio_guard`), start muted (the plan powers the amplifiers last, `fs19xx_start(.., muted)`, the chain fades in from silence),
   stop = fade-out, zeros, then the amplifiers shut down before the graphs stop (the plan's unwind order); any DSP error, late
   period or amplifier status change trips the guard; a DC blocker and a digital ceiling (-20 dBFS until level 9, never above
   -1 dBFS); no DSP volume module (the upstream one reaches +18 dB). The HAL's Apple speaker chain (option B) is not trusted as a
   limiter. Heat: no thermal driver; sessions attended.
3. **The amplifier preset.** `fs19xx.fsm` (TDM slot registers, per-speaker tables) is the stock file of odm_a: read at run time
   from the owner's own tablet, never committed, never disassembled. The parser (`fsm_check`/`fsm_find`) and the identity check
   (`audio_preset_is_stock`: 4348 bytes and the SHA-256 Ubuntu publishes for OS3.0.303.0.WPYCNXM) are done; a file that is not the
   stock one is refused unless the owner overrides it (level 5's boot argument, to add). Open: where the bytes come from. odm_a is
   an Android filesystem image, not FAT: either a read-only reader for it, or the installer copies the owner's own file onto the ESP
   (FAT, readable through NativeUfs + FatVolume, as PianoPower's firmware; PianoWifi carries its own FatVolume copy). (level 5)
4. **A DSP-addressable buffer.** The DSP takes 32-bit addresses with the stream index above (`ar_dsp_address`; the plan refuses
   anything else); XNU's memory is above 4 GiB on piano. Candidates: low DRAM nobody uses after ExitBootServices (UEFI's DXE heap
   or "Kernel" region), or a translating SMMU bank for 0x1001 (on sheng that reset the board: the hypervisor's banks). Level 1
   shows the firmware's entries; level 7 decides, with the sensors (one owner of that range). (needs-boot)
5. **PRM clock IDs and the TDM parameter.** The TDM bit clock is PRM's `SEC_TDM_IBIT` 0x202 (the DT's index 37 = 0x25), from
   Ubuntu's q6prm; the TDM parameter `PARAM_ID_TDM_INTF_CFG` 0x0800101B and its field order from Ubuntu's audioreach (both
   specification only). The VA macro's PRM IDs are still sheng's SM8550 values. (levels 4, 6)
6. The i2s1 function number of gpio121-124 and whether the I2C hub's engine (its GCC clocks are not in this repo yet) or a
   bit-banged bus serves level 5 (`Fs19xxBus` takes either). (level 1 reads the pins; source read)
7. The DMIC supply (no regulator in the VA macro's DT node) and the 150 Hz high-pass / start transient (SunUEFI's notes). (level 10)
8. Option B's private ABI: pinned to this macOS 27 build; an OS update means re-checking DroidVMAudio's selectors, or option C.
9. USB-C wired audio: whether a WCD939x is fitted (stock symbols say maybe, the DT says no) and USB host mode first. (later)
10. **The GPR source ports.** Ubuntu's piano kernel sends APM commands from port 1, PRM requests from port 2 and data from dynamic
    ports (0x10000000 up); sheng (SM8550) and the K90 (SM8750) used 0x2001 for everything; Project-SunUEFI's patch says its piano
    ADSP answered only ports 3 (APM) and 7 (PRM) and dropped port 1. PianoAudio follows Ubuntu (the one stack that plays on this
    tablet) and keeps the ports parameters of every builder; level 2's "no answer with the channel up" is the symptom. (level 2)
11. **The TDM sync mode.** Value 1 (long sync in the TDM API) is Ubuntu's guess between the stock ACDB's two entries (1 and 0);
    `ArTdm.sync_mode` is a field. First suspect if the amplifiers' CLKS/PLLS bits do not settle at level 6.
12. **Packet sizes.** The capture stream's GRAPH_OPEN is 504 of GPR's 512 bytes; any added module needs a second packet or a
   larger intent (the DSP's receive intents for `adsp_apps` are its own; the K90 saw 128-byte intents on its firmware, which
   PianoPower's GLINK would have to split or request). (levels 6, 10)

## 11. Licence note

This repository's packages are BSD-2-Clause-Patent. Linux (q6apm, audioreach, gpr, q6prm, lpass-va-macro, the FS19xx driver, the
piano machine driver) and Project-SunUEFI's kernel patches are GPL-2.0: specification only (behaviour, IDs and register positions
as facts); no code, comment or table is copied. hackios-xnu-kernel-k90 is APSL-2.0: facts only. UniXNU-sheng and
UniXNU-droidvm (DroidVMAudio) are BSD-2-Clause-Patent and are reused with `UPSTREAM` records; AudioReach's topology macros and
engine headers (BSD-3-Clause / BSD-3-Clause-Clear) supply IDs and layouts with attribution. No Qualcomm or FourSemi binary is
reverse engineered. The DSP firmware, the amplifier preset and any topology or ACDB file are data read at run time from the
owner's own tablet (or a staged copy under the ignored `out/`), never committed; the host tests use a synthetic preset container
with made-up register data and models of the DSP and the amplifiers, no recorded vendor data.
